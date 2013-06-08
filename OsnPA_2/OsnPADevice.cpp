#include "stdio.h"
#include <stddef.h>
#include <stdlib.h>

#include "OSNddk.h"
#include "ntdddisk.h"

#include "..\common\Queue.h"
#include "..\common\osnevent.h"
#include "..\..\Applications\common\OSNDefs.h"
#include "..\..\Applications\common\CfgCommon.h"
#include "..\..\Applications\common\OSNCommon.h"
#include "DDOsnPA.h"

#include "DiskDevice.h"
#include "DiskCacheVolume.h"
#include "ActiveIrp.h"
#include "OsnPADevice.h"
#include "global.h"

#include "OsnPAmsg.h"

extern CGlobal* g_pGlobal;

COsnPADevice::COsnPADevice():
CDiskDevice()
{
}

COsnPADevice::COsnPADevice(VOLUME_TYPE volumeType):
m_CacheFlag(FALSE),
m_CacheRole(NO_CACHE),
m_RemoveCacheFlag(FALSE),
m_CacheState(NONE),
m_CachePreState(NONE),
m_pPACacheDevice(NULL),
CDiskDevice(volumeType)
{
	m_VolumeName.Length = 0;
	m_VolumeName.MaximumLength = MAX_DEVICE_NAME_LENGTH*sizeof(WCHAR);
	m_VolumeName.Buffer        = m_VolumeNameBuffer;
	RtlZeroMemory(m_VolumeNameBuffer,MAX_DEVICE_NAME_LENGTH);

	m_ThreadHandle = NULL;
	m_pCacheVolume = NULL;
	m_HandleIrpThreadStart = FALSE;
	m_HandleIrpThreadStop  = TRUE;
	m_IsHandleIrpThreadDie = TRUE;

	m_ReloadSuccess        = TRUE;
	m_CacheDevicePointerUseFlag = FALSE;
	m_ForceRemoveCacheFlag = FALSE;

	m_FlushCache           = FALSE;
	m_FlushBlockNum        = 0;
	m_pBlockBitmap         = NULL;
	m_FlushNext            = TRUE;
	m_FlushIrpOutStandingCount = 0;
	m_FlushBitMapByte      = 0;
	m_FlushBitMapBit       = 0;
	m_FlushSourceBlockNum  = 0; 
	m_FlushWriteFailed     = FALSE;
	m_FlushFinishBit       = 0;
	m_FlushSuccess         = TRUE;

	KeInitializeSpinLock(&m_OsnPACacheSpinLock);
	KeInitializeSpinLock(&m_CacheDeviceSpinLock);
	KeInitializeSpinLock(&m_FlushSpinlock);

	for(ULONG index = 0 ;index <OSNMAX_FLUSH_IO_COUNT;index ++)
	{
		m_FlushInfo[index].m_Bit = 0;
		m_FlushInfo[index].m_Byte = 0;
		m_FlushInfo[index].m_pFlushIrp = NULL;
		m_FlushInfo[index].m_ReadFlushIrpComplete = FALSE;
	}
}

COsnPADevice::~COsnPADevice()
{
	FreeCacheMem();
}

NTSTATUS COsnPADevice::QueryDeviceName()
{

	NTSTATUS		status;

    ULONG           bufferSize = sizeof(MOUNTDEV_NAME) + MAX_DEVICE_NAME_LENGTH * sizeof(WCHAR);
    PMOUNTDEV_NAME  pMountDecName = (PMOUNTDEV_NAME) ExAllocatePoolWithTag(NonPagedPool, bufferSize, OSNTAGPA);   
	if(pMountDecName) 
	{
	    status = IssueIoctlToLowerDevice(IOCTL_MOUNTDEV_QUERY_DEVICE_NAME,
										NULL,
										0,
										pMountDecName,
										bufferSize);


		if(NT_SUCCESS(status))
		{
			RtlCopyMemory(m_VolumeNameBuffer, pMountDecName->Name, pMountDecName->NameLength);
			m_VolumeName.Length = pMountDecName->NameLength;

			KdPrint(("OsnPA - Added volume: %S\n", m_VolumeNameBuffer));
		}

        ExFreePool(pMountDecName);
	}

	return status;
}

VOID     COsnPADevice::QueryDeviceProperty()
{
	NTSTATUS		status;

	//get volume name \device\osn_{....}
	status = QueryDeviceName();
	if(NT_SUCCESS(status))
	{
		KdPrint(("OsnPA - QueryDeviceProperty() returns volume name %S\n", m_VolumeName.Buffer));
	}

		//set volume ID = GUID. Minus "\Device\osn_" prefix
	UNICODE_STRING	guidString;
	guidString.Length			= m_VolumeName.Length - wcslen(OSNVM_DEVICE_PREFIX) * sizeof(WCHAR);
	guidString.MaximumLength	= m_VolumeName.MaximumLength - wcslen(OSNVM_DEVICE_PREFIX) * sizeof(WCHAR);
	guidString.Buffer			= &m_VolumeName.Buffer[wcslen(OSNVM_DEVICE_PREFIX)];
	RtlGUIDFromString(&guidString, &m_VolumeGuid);

	m_SizeInBlocks = InquireDeviceBlockSize();
	KdPrint(("OsnPA - osnPADevice VoluneName is %S,VolumeSize is %I64x .\n",m_VolumeName.Buffer,\
		m_SizeInBlocks));
	
}

ULONGLONG  COsnPADevice::InquireDeviceBlockSize()
{

	NTSTATUS				status;
	IO_STATUS_BLOCK			IoStatusBlock;

	PARTITION_INFORMATION	partitionInfo;
	RtlZeroMemory(&partitionInfo, sizeof(PARTITION_INFORMATION));
	status = CGlobal::OSNDeviceIoctl(GetTargetDeviceObject(), 
										IOCTL_DISK_GET_PARTITION_INFO, 
										NULL, 
										0, 
										&partitionInfo,
										sizeof(PARTITION_INFORMATION),
										&IoStatusBlock);

	if(!NT_SUCCESS(status))
		return 0;

	ULONGLONG sizeInBlocks = (partitionInfo.PartitionLength.QuadPart / BLOCK_SIZE);

	return sizeInBlocks;
}

NTSTATUS   COsnPADevice::SetCacheProtect(COsnPADevice *pPACacheDevice,ULONG  unitsize,BOOLEAN newConfig)
{
	if(!pPACacheDevice)
	{
		return STATUS_UNSUCCESSFUL;
	}

	KIRQL         oldIrql;
	NTSTATUS      status;
	PCACHE_INFO   pCacheInfo;

	m_pCacheVolume = new CDiskCache();
	if(!m_pCacheVolume)
	{
		return STATUS_UNSUCCESSFUL;
	}

	pCacheInfo = (PCACHE_INFO)ExAllocatePoolWithTag(NonPagedPool,sizeof(CACHE_INFO),OSNTAGPA);
	if(!pCacheInfo)
	{
		KdPrint(("OSNPA - Failed to allocate memory for Cacheinfo.\n"));
		return STATUS_UNSUCCESSFUL;
	}
	else
	{
		RtlCopyMemory(&pCacheInfo->m_SourceVolumeID.SAN_VolumeID.m_VolumeGuid,this->GetVolumeGUID(),sizeof(GUID));
		RtlCopyMemory(&pCacheInfo->m_CacheVolumeID.SAN_VolumeID.m_VolumeGuid,pPACacheDevice->GetVolumeGUID(),sizeof(GUID));
	}

	status = m_pCacheVolume->Initialize((VOLUMEID *)&m_VolumeGuid,(VOLUMEID *)pPACacheDevice->GetVolumeGUID(),\
		unitsize,newConfig);
	if(!NT_SUCCESS(status))
	{
		KdPrint(("OSNPA - Failed to Initialize cache volume ,status = %x\n",status));
		ExFreePool(pCacheInfo);
		delete m_pCacheVolume;
		return status;
	}

	status = InitializeHandleIrpThread();
	if(!NT_SUCCESS(status))
	{
		KdPrint(("OSNPA - Failed to Initialize handle irp thread ,status = %x\n",status));
		ExFreePool(pCacheInfo);
		delete m_pCacheVolume;
		return status;
	}

	oldIrql = AcquireCacheLock();
	m_CacheFlag = TRUE;
	m_CacheRole = CACHE_SOURCE;
	m_CacheState = UP;
	m_CachePreState = UP;
	ReleaseCacheLock(oldIrql);

	oldIrql = pPACacheDevice->AcquireCacheLock();
	pPACacheDevice->SetCacheFlag(TRUE);
	pPACacheDevice->SetCacheRole(CACHE_TARGET);
	pPACacheDevice->ReleaseCacheLock(oldIrql);

	m_pPACacheDevice = pPACacheDevice;
	

	if(!g_pGlobal->ChecCacheInfoBySource(pCacheInfo->m_SourceVolumeID.SAN_VolumeID.m_VolumeGuid))
	{
		g_pGlobal->InsertCacheInfoToList(pCacheInfo);
	}
	else
	{
		ExFreePool(pCacheInfo);
	}


	return status;
}

VOID   COsnPADevice::FreeCacheMem()
{
	if(m_pCacheVolume)
	{
		delete m_pCacheVolume;
		m_pCacheVolume = NULL;
	}
}

NTSTATUS   COsnPADevice::InitializeHandleIrpThread()
{
	NTSTATUS status ;
	if(!m_HandleIrpThreadStart)
	{
		m_HandleIrpThreadStop = FALSE;
		m_IsHandleIrpThreadDie = FALSE;
		status = PsCreateSystemThread(&m_ThreadHandle,(ACCESS_MASK)0L,
			NULL,
			NULL,
			NULL,
			(PKSTART_ROUTINE)OSNPAHandleIrpWorkThread,
			this);
		if (!NT_SUCCESS(status) )	
		{
			KdPrint(("COSNPA - Initialize -- Couldn't start device %x handle irp worker thread\n",this));
			return status;
		}

		ZwClose(m_ThreadHandle);
		m_HandleIrpThreadStart  = TRUE;
		KdPrint(("OSNPA - success initalize handle irp worker\n"));
	}

	return STATUS_SUCCESS;
}

NTSTATUS  COsnPADevice::StopHandleIrpThread()
{
	NTSTATUS status = STATUS_SUCCESS;
	PVOID	pThreadObj = NULL;
	KIRQL   oldIrql;

	if(m_HandleIrpThreadStart)
	{		
		if(!m_HandleIrpThreadStop)
		{
			m_HandleIrpThreadStop = TRUE;
			SetHandleIrpThreadEvent();
		}

		if(m_IsHandleIrpThreadDie)
			m_HandleIrpThreadStart = FALSE;
		else
			return STATUS_UNSUCCESSFUL;

		KdPrint(("OSNPA -success stop handle irp thread\n"));
	}

	return status;
}

BOOLEAN	  COsnPADevice::ProcessIrp()
{
	BOOLEAN workDone = ProcessActiveIrps();

	PIRP	pFirstIrp	= NULL;
	CQueue *pNode		= NULL;
	LONGLONG	blockOffset;
	ULONG		blockLength;

	KIRQL	oldIrql = AcquireCmdListLock();
	pNode			= m_ApplicationIrpQHead.Next();
	if(pNode)
		pFirstIrp	= (PIRP) pNode->GetItem();
	ReleaseCmdListLock(oldIrql);

	if(!pFirstIrp)
		return workDone;

	PIRP pIrp = NULL;
	while(pNode)
	{
		pIrp = (PIRP) pNode->GetItem();
		PIO_STACK_LOCATION  pIrpStack		= IoGetCurrentIrpStackLocation(pIrp);
		ASSERT(pIrpStack->MajorFunction == IRP_MJ_WRITE ||
			pIrpStack->MajorFunction == IRP_MJ_READ);

		if(pIrpStack->MajorFunction == IRP_MJ_WRITE)
		{
			blockOffset = pIrpStack->Parameters.Write.ByteOffset.QuadPart ;
			blockLength = pIrpStack->Parameters.Write.Length;
			if(GetFlushCache())
			{
				if(CheckFlushOverlap(blockOffset,blockLength))
				{
					SetFlushNext(TRUE);
					if(GetFlushIrpCount() != 0)
					{
						KdPrint(("OSNPA - FLUSH OVERLAP \n"));
						break;
					}
				}
			}
			if(!OverlapWithActiveIrps(pIrp,FALSE))
			{
				CQueue	*pNextNode = NULL;
				KIRQL	oldIrql = AcquireCmdListLock();
				pNextNode = pNode->Next();
				pNode->Remove();
				delete pNode;
				ReleaseCmdListLock(oldIrql);

				if(pIrp)
					InterlockedDecrement(&m_AppIrpCount);
				KdPrint(("OSNPA - m_appIrpCount  = %x ,INSERT WRITER IRP = %x\n",m_AppIrpCount,pIrp));
				InsertActiveIrpQTail(pIrp, STATE_WRITE_NEW);


				pNode = pNextNode;

				//			KdPrint(("osncdp - application Irp 0x%x inserted into active irp queue, offset=0x%x\n", pIrp, blockOffset));

				workDone = TRUE;
				continue;		
			}
			else
			{
				KdPrint(("OSNPA - WIRTE IRP OVERLAP \n"));
				break;
			}
		}
		else
		{
			blockOffset = pIrpStack->Parameters.Read.ByteOffset.QuadPart ;
			blockLength = pIrpStack->Parameters.Read.Length ;
			if(GetFlushCache())
			{
				if(CheckFlushOverlap(blockOffset,blockLength))
				{
					SetFlushNext(TRUE);
					if(GetFlushIrpCount() != 0)
					{
						KdPrint(("OSNPA - FLUSH OVERLAP \n"));
						break;
					}
				}
			}
			if(!OverlapWithActiveIrps(pIrp,TRUE))
			{
				CQueue	*pNextNode = NULL;
				KIRQL	oldIrql = AcquireCmdListLock();
				pNextNode = pNode->Next();
				pNode->Remove();
				delete pNode;
				ReleaseCmdListLock(oldIrql);

				if(pIrp)
					InterlockedDecrement(&m_AppIrpCount);
				KdPrint(("OSNPA - m_appIrpCount  = %x INSERT READ IRP\n",m_AppIrpCount));
				InsertActiveIrpQTail(pIrp, STATE_READ_NEW);

				pNode = pNextNode;

				//			KdPrint(("osncdp - application Irp 0x%x inserted into active irp queue, offset=0x%x\n", pIrp, blockOffset));

				workDone = TRUE;
				continue;		
			}
			else
			{
				KdPrint(("OSNPA - READ IRP OVERLAP \n"));
				break;
			}
		}

		SetHandleIrpThreadEvent();
		KIRQL	oldIrql = AcquireCmdListLock();
		pNode = pNode->Next();
		ReleaseCmdListLock(oldIrql);
	}

	return workDone;
}

BOOLEAN    COsnPADevice::CheckFlushOverlap(LONGLONG blockOffset,ULONG blockLength)
{
	LONGLONG flushoffset = ((LONGLONG)m_FlushSourceBlockNum) * m_pCacheVolume->GetOsnDskCH()->Cache.BlockSize;
	ULONG    flushlength = m_pCacheVolume->GetOsnDskCH()->Cache.BlockSize;
	if((flushoffset > blockOffset+ blockLength) ||
		(blockOffset > flushoffset+flushlength))
		return FALSE;
	else
		return TRUE;
}

NTSTATUS  COsnPADevice::ProcessReadIrp(PIRP pIrp)
{
	NTSTATUS   status;

	status = PassThrough(pIrp);
	return status;
}

VOID     COsnPADevice::GetFlushInfoFromBitMap(ULONG &bitCount, ULONG &emptyCount)
{
	ULONG  startingByte  = 0;
	ULONG  startingBit   = 0;
	ULONG  endByte       = 0;
	ULONG  endBit        = 0;
	bitCount             = 0;
	emptyCount           = 0;

	startingByte = m_FlushBitMapByte;
	endByte      = m_pCacheVolume->GetOsnDskCH()->Cache.BlockHdSize-1;

	if(!m_pBlockBitmap)
	{
		return ;
	}

	for(ULONG ByteIndex =  startingByte ;ByteIndex <= endByte ;ByteIndex++)
	{
		startingBit = 0;
		if(ByteIndex == startingByte)
			startingBit = m_FlushBitMapBit;

		for(ULONG  bitindex = startingBit;bitindex <BIT_PER_BYTE;bitindex++)
		{
			if((m_pBlockBitmap->BitMap[ByteIndex] &(0x1<<bitindex)) !=0)
			{
				if(bitCount >= OSNMAX_FLUSH_IO_BIT_NUM)
					return ;

				bitCount++;
			}
			else
			{
				if(bitCount == 0)
				{
					emptyCount++;
					continue;
				}
				else
					return ;
			}
		}
	}
}

VOID      COsnPADevice::UpdateFlushBitmap(ULONG flushBitCount)
{
	ULONG byte = 0;
	ULONG bit  = 0;

	byte  = (flushBitCount) / BIT_PER_BYTE;
	bit   = (flushBitCount) % BIT_PER_BYTE;

	m_FlushBitMapByte += byte;
	m_FlushBitMapBit  += bit;
	if(m_FlushBitMapBit /BIT_PER_BYTE)
	{
		m_FlushBitMapByte++;
		m_FlushBitMapBit -= BIT_PER_BYTE;
	}
	KdPrint(("OSNPA - UPDATE BLOCK FLUSH BYTE = %x,BIT = %x \n",m_FlushBitMapByte,m_FlushBitMapBit));
}

NTSTATUS     COsnPADevice::FlushBlockFinish()
{
	NTSTATUS status;
	m_pCacheVolume->DeleteBitMapFromMem(m_FlushBlockNum);
	status = m_pCacheVolume->ReadAndWriteNewBat(m_FlushSourceBlockNum,m_FlushBlockNum);

	return status;
}

VOID      COsnPADevice::ClearCacheInfo()
{
	NTSTATUS        status;
	PCACHE_INFO     pCacheInfo = NULL;
	COsnPADevice    *pSourceDevice = NULL;
	COsnPADevice    *pCacheDevice  = NULL;
	UNICODE_STRING  guidstring;

	if(CACHE_TARGET == GetCacheRole())
	{
		pCacheDevice = this;
		pCacheInfo   = g_pGlobal->CheckCacheInfoByCache(pCacheDevice->GetGUID());
		if(pCacheInfo)
		{
			pSourceDevice = g_pGlobal->GetDeviceFromList(pCacheInfo->m_SourceVolumeID.SAN_VolumeID.m_VolumeGuid);
			if(pSourceDevice)
			{
				status = pSourceDevice->StopHandleIrpThread();
				if(!NT_SUCCESS(status))
					return ;
				pSourceDevice->ClearAppCmdQueue();
				pSourceDevice->ClearActiveQueue();
			}
		}
	}
	else if(CACHE_SOURCE == GetCacheRole())
	{
		pSourceDevice  = this;
		pCacheInfo     = g_pGlobal->GetCacheInfoFromList(pSourceDevice->GetGUID());
		if(pCacheInfo)
		{
			pCacheDevice = g_pGlobal->GetDeviceFromList(pCacheInfo->m_CacheVolumeID.SAN_VolumeID.m_VolumeGuid);
		}
	}

	if(pSourceDevice)
	{
		RtlStringFromGUID(pSourceDevice->GetGUID(),&guidstring);
		status = g_pGlobal->DeleteCacheRelationFromRegistry(OSN_CACHE_RELATION,guidstring.Buffer);
		if(!NT_SUCCESS(status))
		{
			KdPrint(("OSNPA - FAILED TO DELETE CACHE REL FORM REG ,STATUS = %x \n",status));
		}
		else
			KdPrint(("OSNPA - SUCCESS DELETE CACHE REL FORM REG  \n"));

		pSourceDevice->ReSetDeviceInfo();
	}

	if(pCacheDevice)
	{
		pCacheDevice->ReSetDeviceInfo();
	}

	if(pCacheInfo)
	{
		g_pGlobal->RemoveCacheInfoFromList(pCacheInfo);
	}
}

VOID     COsnPADevice::PrepareForFlush()
{
	m_FlushSuccess    = TRUE;

	m_FlushBlockNum        = 0;
	if(m_pBlockBitmap)
	{
		ExFreePool(m_pBlockBitmap);
		m_pBlockBitmap         = NULL;
	}
	m_FlushNext            = TRUE;
	m_FlushIrpOutStandingCount = 0;
	m_FlushBitMapByte      = 0;
	m_FlushBitMapBit       = 0;
	m_FlushSourceBlockNum  = 0; 
	m_FlushWriteFailed     = FALSE;
	m_FlushFinishBit       = 0;
}

VOID   COsnPADevice::ReSetDeviceInfo()
{
	m_CacheFlag = FALSE;
	m_CacheRole = NO_CACHE;
	m_RemoveCacheFlag = FALSE;
	m_ForceRemoveCacheFlag = FALSE;
	FreeCacheMem();
	m_ReloadSuccess   = TRUE;
	m_FlushCache   = FALSE;
	PrepareForFlush();

}

NTSTATUS   COsnPADevice::DoFlush()
{
	NTSTATUS   status;
	BOOLEAN    flag = TRUE;
	KIRQL      irql ;
	if(!m_pCacheVolume)
	{
		KdPrint(("OSNPA - CACHE DETAIL IS NULL ,CAN'T FULSH \n"));
		irql = AcquireFlushSpinLock();
		m_FlushCache = FALSE;
		m_FlushSuccess = TRUE;
		if(m_pBlockBitmap)
		{
			ExFreePool(m_pBlockBitmap);
			m_pBlockBitmap = NULL;
		}
		ReleaseFlushSpinlock(irql);

		return STATUS_SUCCESS;
	}

	if(m_FlushNext && m_FlushIrpOutStandingCount == 0)
	{
		if(m_FlushIrpOutStandingCount != 0)
		{
			KdPrint(("OSNPA - FLUSH IRP STANDING COUNT CAN'T ZERO,WAITFOR \n"));
			return STATUS_SUCCESS;
		}

		do
		{
			m_FlushBlockNum = m_pCacheVolume->GetNextFlushBlockNum(m_FlushSourceBlockNum);
			if(m_FlushBlockNum ==0)
			{
				KdPrint(("OSNPA - FLUSH IRP BLOCK IS ZERO,FLUSH compelete \n"));
				if(m_pBlockBitmap)
				{
					ExFreePool(m_pBlockBitmap);
					m_pBlockBitmap = NULL;
				}
				irql = AcquireFlushSpinLock();
				m_FlushCache = FALSE;
				m_FlushSuccess = TRUE;
				SetCacheState(UP);
				ReleaseFlushSpinlock(irql);
				COSNEventLog::OSNLogEvent(g_pGlobal->GetDriverObject(),OSNPA_CACHE_FLUSH_SUCCESS,0,this->GetVolumeName());
				return  STATUS_SUCCESS;
			}
			flag = FlushCheckOverlap(((LONGLONG)m_FlushSourceBlockNum)*m_pCacheVolume->GetOsnDskCH()->Cache.BlockSize,
				m_pCacheVolume->GetOsnDskCH()->Cache.BlockSize);
		}while(flag);

		if(!m_pBlockBitmap)
		{
			m_pBlockBitmap = (PBITMAP)ExAllocatePoolWithTag(NonPagedPool,sizeof(BITMAP)+m_pCacheVolume->GetOsnDskCH()->Cache.BlockHdSize,OSNTAGPA);
			if(!m_pBlockBitmap)
			{
				KdPrint(("OSNPA - NO ENOUGH MEM FOR BLOCKBITMAP \n"));
				irql = AcquireFlushSpinLock();
				m_FlushCache = FALSE;
				m_FlushSuccess = FALSE;
				ReleaseFlushSpinlock(irql);
				return  STATUS_UNSUCCESSFUL;
			}
		}

		status = m_pCacheVolume->GetBlockBitMap(m_FlushBlockNum,m_pBlockBitmap);
		if(!NT_SUCCESS(status))
		{
			KdPrint(("OSNPA - FAILED TO GET BITMAP OF BLOCKNUM %x \n",m_FlushBlockNum));
			return STATUS_UNSUCCESSFUL;
		}

		m_FlushIrpOutStandingCount = 0;
		m_FlushBitMapByte      = 0;
		m_FlushBitMapBit       = 0;
	//	m_FlushSourceBlockNum  = 0; 
		m_FlushWriteFailed     = FALSE;
		m_FlushFinishBit       = 0;
		m_FlushNext            = FALSE;
		KdPrint(("OSNPA - FLUSH IRP NEXT BLOCK NUM = %x \n",m_FlushBlockNum));
	}

	ULONG  allBit  = m_pCacheVolume->GetOsnDskCH()->Cache.BlockHdSize *BIT_PER_BYTE;
	if((m_FlushBitMapByte*BIT_PER_BYTE +m_FlushBitMapBit) >= allBit)
	{
		if(m_FlushFinishBit >= allBit)
		{
			status = FlushBlockFinish();
			if(NT_SUCCESS(status))
			{
				m_FlushNext = TRUE;
			}
			KdPrint(("OSNPA - SUCCESS FLUSH SOURCE BLOCK =  %x \n",m_FlushSourceBlockNum));
		}
		else
		{
			KdPrint(("OSNPA - LAST OUTSTANDING IRP BYTE =%x,BIT = %x \n ",m_FlushBitMapByte,m_FlushBitMapBit));
		}
	}

	ULONG          IoLength     = BLOCK_SIZE;
	LONGLONG      blockOffset  = 0;
	ULONG          flushBitCount  = 0;
	ULONG          flushemptyCount = 0;
	for(ULONG index = 0;index < OSNMAX_FLUSH_IO_COUNT ;index++)
	{
		if(m_FlushInfo[index].m_pFlushIrp == NULL &&
			m_pPACacheDevice->GetPnpState() == StateStarted &&
			GetPnpState() == StateStarted &&
			GetFlushCacheFlag()&&
			!m_FlushNext)
		{
			KIRQL  oldirql = AcquireFlushSpinLock();
			if(m_FlushWriteFailed)
			{
				m_FlushBitMapByte = m_FlushFinishBit /BIT_PER_BYTE;
				m_FlushBitMapBit  = m_FlushFinishBit %BIT_PER_BYTE;
				KdPrint(("OSNPA - reset bitmap m_byte = %x, m_bit = %x \n",m_FlushBitMapByte,m_FlushBitMapBit));
				m_FlushWriteFailed = FALSE;
			}
			ReleaseFlushSpinlock(oldirql);

			if((m_FlushBitMapByte*BIT_PER_BYTE+m_FlushBitMapBit)<= allBit)
			{
				GetFlushInfoFromBitMap(flushBitCount,flushemptyCount);
				UpdateFlushBitmap(flushemptyCount);
				if((m_FlushBitMapByte*BIT_PER_BYTE+m_FlushBitMapBit)>= allBit)
				{
					if(m_FlushIrpOutStandingCount == 0)
						m_FlushFinishBit = m_FlushBitMapByte*BIT_PER_BYTE+m_FlushBitMapBit;
					continue;
				}
				blockOffset  = m_pCacheVolume->GetOsnDskCH()->Cache.StartingOffset;
				blockOffset += (m_FlushBlockNum-1)*(m_pCacheVolume->GetOsnDskCH()->Cache.BlockHdSize+m_pCacheVolume->GetOsnDskCH()->Cache.BlockSize);
				blockOffset += m_pCacheVolume->GetOsnDskCH()->Cache.BlockHdSize;
				blockOffset += (m_FlushBitMapByte*BIT_PER_BYTE + m_FlushBitMapBit)*BLOCK_SIZE;
				IoLength    = flushBitCount*BLOCK_SIZE;
				

				KdPrint(("OSNPA - FLUSH READ BLOCKOFFSET = %I64x ,IOLENGTH = %x \n",blockOffset,IoLength));
				status = ReadFromCache(blockOffset,IoLength,index);
				if(NT_SUCCESS(status))
				{
					UpdateFlushBitmap(flushBitCount);
				}
				else
				{
					KdPrint(("OSNPA - FAILED TO READ FROM CACHE ,STATUS = %x \n",status));
				}
			}
		}
		else if(m_FlushInfo[index].m_ReadFlushIrpComplete)
		{
			status = FlushWrite(index);
		}

		if(!NT_SUCCESS(status))
		{
			KdPrint(("OSNPA - FAILED TO FLUSH IO \n"));
		}
	}

	return status;
}

NTSTATUS   COsnPADevice::FlushWrite(ULONG index)
{
	NTSTATUS    status ;
	PIRP        pIrp  = m_FlushInfo[index].m_pFlushIrp;

	ASSERT(pIrp);
	ASSERT(pIrp->MdlAddress);

	PIO_STACK_LOCATION	pCurrentIrpStack	= IoGetCurrentIrpStackLocation(pIrp);
	PIO_STACK_LOCATION	pIrpStack			= IoGetNextIrpStackLocation(pIrp);

	ULONGLONG	blockOffset = (ULONGLONG) (pCurrentIrpStack->Parameters.Read.ByteOffset.QuadPart);
	ULONG   blockLength = (ULONG) (pCurrentIrpStack->Parameters.Read.Length);

    status = pIrp->IoStatus.Status;
	if(!NT_SUCCESS(status))
	{
		PVOID	pBuffer = pIrp->MdlAddress->MappedSystemVa;
		ExFreePool(pBuffer);
		IoFreeMdl(pIrp->MdlAddress);

		ResetFlushIndex(m_FlushInfo[index].m_Byte,m_FlushInfo[index].m_Bit);
		IoFreeIrp(m_FlushInfo[index].m_pFlushIrp);
		ASSERT(DecrementFlushIrpCount()>=0);
		m_FlushInfo[index].m_pFlushIrp= NULL;
		m_FlushInfo[index].m_Byte     = 0;
		m_FlushInfo[index].m_Bit      = 0;
		m_FlushInfo[index].m_ReadFlushIrpComplete = FALSE;

		KdPrint(("OSNPA - FALIED TO FLUSH BLOCKOFFSET = %I64x, BLOCKlENGTH = %x \n",blockOffset,blockLength));
		return status;
	}

	blockOffset  -= m_pCacheVolume->GetOsnDskCH()->Cache.StartingOffset;
	blockOffset  -= (m_FlushBlockNum-1)*(m_pCacheVolume->GetOsnDskCH()->Cache.BlockHdSize+m_pCacheVolume->GetOsnDskCH()->Cache.BlockSize);
	blockOffset  -= m_pCacheVolume->GetOsnDskCH()->Cache.BlockHdSize;
	blockOffset   += m_pCacheVolume->GetOsnDskCH()->Cache.BlockSize*m_FlushSourceBlockNum;
	pCurrentIrpStack->Parameters.Write.ByteOffset.QuadPart  = blockOffset;
	pCurrentIrpStack->Parameters.Write.Length  = blockLength;
	m_FlushInfo[index].m_ReadFlushIrpComplete = FALSE;

	*pIrpStack = *pCurrentIrpStack;
	pIrpStack->MajorFunction = IRP_MJ_WRITE;

	IoSetCompletionRoutine(pIrp,
					COsnPADevice::FlusWriteCompletionRoutine,
					this,
					true,
					true,
					true);

	status   =  AcquireRemoveLock();
	if(!NT_SUCCESS(status))
	{
		PVOID	pBuffer = pIrp->MdlAddress->MappedSystemVa;
		ExFreePool(pBuffer);
		IoFreeMdl(pIrp->MdlAddress);
		IoFreeIrp(m_FlushInfo[index].m_pFlushIrp);

		m_FlushInfo[index].m_pFlushIrp = NULL;
		m_FlushInfo[index].m_ReadFlushIrpComplete = FALSE;
		ResetFlushIndex(m_FlushInfo[index].m_Byte,m_FlushInfo[index].m_Bit);
		m_FlushInfo[index].m_Byte = 0;
		m_FlushInfo[index].m_Bit  = 0;

		ASSERT(DecrementFlushIrpCount()>=0);
		return status;
	}

	status = IoCallDriver(GetTargetDeviceObject(),pIrp);
	ReleaseRemoveLock();

	return status;
}

NTSTATUS       COsnPADevice::FlusWriteCompletionRoutine(PDEVICE_OBJECT	pDeviceObject, 
	PIRP            pIrp, 
	PVOID           pContext)
{
	ULONG         irpIndex = 0;
	COsnPADevice * pOsnPADevice = (COsnPADevice *)pContext;

	for(ULONG index = 0;index <OSNMAX_FLUSH_IO_COUNT;index++)
	{
		if(pOsnPADevice->m_FlushInfo[index].m_pFlushIrp == pIrp)
		{
			irpIndex = index;
			break;
		}
	}

	NTSTATUS status		= pIrp->IoStatus.Status;
	if(!NT_SUCCESS(status))
	{
		KIRQL  irql = pOsnPADevice->AcquireFlushSpinLock();
		pOsnPADevice->SetFlushWriteFailed(TRUE);
		pOsnPADevice->ReleaseFlushSpinlock(irql);
	}
	else
	{
		if(pOsnPADevice->GetFlushFinishBit() < 
			(pOsnPADevice->m_FlushInfo[irpIndex].m_Byte*BIT_PER_BYTE + pOsnPADevice->m_FlushInfo[irpIndex].m_Bit))
		{
			pOsnPADevice->UpdateFinishBit(pOsnPADevice->m_FlushInfo[irpIndex].m_Byte*BIT_PER_BYTE + pOsnPADevice->m_FlushInfo[irpIndex].m_Bit);
			KdPrint(("OSNPA - UPDATE FINISH BLOCK FLUSH BYTE = %x,BIT = %x \n",pOsnPADevice->m_FlushInfo[irpIndex].m_Byte, pOsnPADevice->m_FlushInfo[irpIndex].m_Bit));
		}
	}

	PVOID pBuffer = pIrp->MdlAddress->MappedSystemVa;
	ExFreePool(pBuffer);
	IoFreeMdl(pIrp->MdlAddress);
	IoFreeIrp(pIrp);

	pOsnPADevice->m_FlushInfo[irpIndex].m_pFlushIrp = NULL;
	pOsnPADevice->m_FlushInfo[irpIndex].m_Byte = 0;
	pOsnPADevice->m_FlushInfo[irpIndex].m_Bit  = 0;
	pOsnPADevice->m_FlushInfo[irpIndex].m_ReadFlushIrpComplete = FALSE;
	ASSERT(pOsnPADevice->DecrementFlushIrpCount()>=0);


	return STATUS_MORE_PROCESSING_REQUIRED;
}

VOID   COsnPADevice::ResetFlushIndex(ULONG byte,ULONG bit)
{
	m_FlushBitMapByte = byte;
	m_FlushBitMapBit  = bit;
}

NTSTATUS   COsnPADevice::ReadFromCache(LONGLONG blockOffset,ULONG blockLength,ULONG index)
{
	PIRP             pIrp = NULL;
	NTSTATUS         status;
	PMDL		     pMdl;
	PVOID				pBuffer;
	PIO_STACK_LOCATION	pIrpStack;	
	PIO_STACK_LOCATION	pNextIrpStack;
	LARGE_INTEGER      	byteOffset;
	byteOffset.QuadPart   = blockOffset;

	pBuffer = ExAllocatePoolWithTag(NonPagedPool,blockLength,OSNTAGPA);
	if(!pBuffer)
	{
		KdPrint(("OSNPA - FAIELD TO ALLOC MEM FOR BUFFER \n"));
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	if(!m_pPACacheDevice)
		return STATUS_INSUFFICIENT_RESOURCES;

	pIrp = IoAllocateIrp(m_pPACacheDevice->GetTargetDeviceObject()->StackSize+1,FALSE);
	if(!pIrp)
	{
		ExFreePool(pBuffer);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	IoSetNextIrpStackLocation(pIrp);

	pMdl = IoAllocateMdl(pBuffer,
		blockLength,
		false,
		false,
		pIrp);
	if( !pMdl )
	{
		ExFreePool(pBuffer);
		IoFreeIrp(pIrp);
		pIrp = NULL;
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	MmBuildMdlForNonPagedPool(pMdl);
	pMdl->Next = NULL;	

	pIrpStack = IoGetCurrentIrpStackLocation(pIrp);
	pIrpStack->MajorFunction = IRP_MJ_READ;
	pIrpStack->MinorFunction = 0;
	pIrpStack->Parameters.Read.ByteOffset.QuadPart = byteOffset.QuadPart;
	pIrpStack->Parameters.Read.Length = blockLength;

	pNextIrpStack = IoGetNextIrpStackLocation(pIrp);
	*pNextIrpStack = *pIrpStack;

	m_FlushInfo[index].m_pFlushIrp = pIrp;
	m_FlushInfo[index].m_ReadFlushIrpComplete = FALSE;
	m_FlushInfo[index].m_Byte = m_FlushBitMapByte;
	m_FlushInfo[index].m_Bit  = m_FlushBitMapBit;

	IoSetCompletionRoutine(pIrp,
		COsnPADevice::FlushReadCompletionRoutine,
		this,
		true,
		true,
		true);

	status = m_pPACacheDevice->AcquireRemoveLock();
	if(!NT_SUCCESS(status))
	{
		ExFreePool(pBuffer);
		IoFreeIrp(pIrp);
		pIrp = NULL;
		m_FlushInfo[index].m_pFlushIrp = NULL;
		return status;

	}

	IncrementFlushIrpCount();
	status = IoCallDriver(m_pPACacheDevice->GetTargetDeviceObject(), pIrp);
	m_pPACacheDevice->ReleaseRemoveLock();

	return status;
}

NTSTATUS    COsnPADevice::FlushReadCompletionRoutine(PDEVICE_OBJECT	pDeviceObject, 
	PIRP            pIrp, 
	PVOID           pContext)
{
	ULONG         irpIndex = 0;
	COsnPADevice * pOsnPADevice = (COsnPADevice *)pContext;

	for(ULONG index = 0;index <OSNMAX_FLUSH_IO_COUNT;index++)
	{
		if(pOsnPADevice->m_FlushInfo[index].m_pFlushIrp == pIrp)
		{
			irpIndex = index;
			break;
		}
	}

	if(pOsnPADevice->GetPnpState() != StateStarted)
	{
		KdPrint(("OSNPA - FAILED TO READ DATA FROM CACHE \n"));
		PVOID  pBuffer = pIrp->MdlAddress->MappedSystemVa;
		ExFreePool(pBuffer);
		IoFreeMdl(pIrp->MdlAddress);
		IoFreeIrp(pOsnPADevice->m_FlushInfo[irpIndex].m_pFlushIrp);

		pOsnPADevice->m_FlushInfo[irpIndex].m_pFlushIrp = NULL;
		pOsnPADevice->m_FlushInfo[irpIndex].m_ReadFlushIrpComplete = FALSE;
		pOsnPADevice->m_FlushInfo[irpIndex].m_Byte   = 0;
		pOsnPADevice->m_FlushInfo[irpIndex].m_Bit    = 0;
		ASSERT(pOsnPADevice->DecrementFlushIrpCount()>=0);

	}
	else
	{
		pOsnPADevice->m_FlushInfo[irpIndex].m_ReadFlushIrpComplete = TRUE;
	}

	return STATUS_MORE_PROCESSING_REQUIRED;
}

static void OSNPAHandleIrpWorkThread(const PVOID pContext)
{
	NTSTATUS status = STATUS_SUCCESS;
	BOOLEAN  workDone = FALSE;
	PETHREAD		Pid = PsGetCurrentThread();
	KdPrint(("OSNPA - HANDLE IRR starts with PID=0x%x\n",Pid));

	LONGLONG                oneMilliSecTimeOut = DELAY_ONE_MILLISECOND;
	LARGE_INTEGER           timeout;
	timeout.QuadPart        = oneMilliSecTimeOut;
	PIRP                    pIrp;
	BOOLEAN                 IsEmpty = false;
	KIRQL                   irql  = NULL;
	COsnPADevice     *pOsnPADevice = (COsnPADevice *)pContext;

	while(!pOsnPADevice->GethandleIrpThreadStop())
	{
		ASSERT(KeGetCurrentIrql() < DISPATCH_LEVEL);
		KeWaitForSingleObject(pOsnPADevice->GetHandIrpEvent(),Executive,KernelMode,FALSE,&timeout);

		irql = pOsnPADevice->AcquireCacheDeviceSpinLock();
		pOsnPADevice->m_CacheDevicePointerUseFlag = TRUE;
		pOsnPADevice->ReleaseCacheDeviceLock(irql);

		if(pOsnPADevice->GetFlushCacheFlag())
		{
			pOsnPADevice->DoFlush();
		}

		do
		{
			workDone = pOsnPADevice->ProcessIrp();
		}while(workDone);

		pOsnPADevice->m_CacheDevicePointerUseFlag = FALSE;
	}

	pOsnPADevice->SetIsHandleIrpThreadDie(TRUE);
	pOsnPADevice->ClearAppCmdQueue();
	pOsnPADevice->ClearActiveQueue();
	KdPrint(("OSNPA - terminate handle irp thread \n"));
	PsTerminateSystemThread(STATUS_SUCCESS);
}