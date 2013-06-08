#include "stdio.h"
#include <stddef.h>
#include <stdlib.h>

#include "OSNddk.h"
#include "ntdddisk.h"

#include "..\common\Queue.h"
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

CActiveIrp::CActiveIrp(PIRP pIrp,CDiskDevice *pSourceVolume,
	ACTIVE_IRP_STATE state,BOOLEAN readOrWrite):
m_pIrp(pIrp),
m_pSourceVolume(pSourceVolume),
m_ActiveState(state),
m_ReadOrWrite(readOrWrite),
m_Information(0),
m_SubIoCount(0),
m_Status(STATUS_SUCCESS)
{
	m_BlockOffset = 0;
	m_BlockLength = 0;
	m_SendToLocal = FALSE;
	m_BlockNum    = 0;
	m_CompeleteTimes = 0;

	RtlZeroMemory(&m_BitMapClr,sizeof(BITMAP_CLR));
}

CActiveIrp::~CActiveIrp()
{
}

NTSTATUS  CActiveIrp::SetBlockAndLength()
{
	ULONGLONG	activeIrpOffset;
	ULONG	    activeIrpLength;
	PIO_STACK_LOCATION  pActiveIrpStack = IoGetCurrentIrpStackLocation(m_pIrp);

	if(m_ReadOrWrite)
	{
		activeIrpOffset = pActiveIrpStack->Parameters.Read.ByteOffset.QuadPart / BLOCK_SIZE;
		activeIrpLength = pActiveIrpStack->Parameters.Read.Length / BLOCK_SIZE;
	}
	else
	{
		activeIrpOffset = pActiveIrpStack->Parameters.Write.ByteOffset.QuadPart / BLOCK_SIZE;
		activeIrpLength = pActiveIrpStack->Parameters.Write.Length / BLOCK_SIZE;
	}

	m_BlockOffset = activeIrpOffset;
	m_BlockLength = activeIrpLength;

	return STATUS_SUCCESS;
}

BOOLEAN   CActiveIrp::CheckOverlap(PIRP  pIrp, BOOLEAN ReadOrWrite)
{
	LONGLONG	irpOffset;
	ULONG		irpLength;


	PIO_STACK_LOCATION  pIrpStack = IoGetCurrentIrpStackLocation(pIrp);

	if(ReadOrWrite)
	{
		irpOffset = pIrpStack->Parameters.Read.ByteOffset.QuadPart / BLOCK_SIZE;
		irpLength = pIrpStack->Parameters.Read.Length / BLOCK_SIZE;
	}
	else
	{
		irpOffset = pIrpStack->Parameters.Write.ByteOffset.QuadPart / BLOCK_SIZE;
		irpLength = pIrpStack->Parameters.Write.Length / BLOCK_SIZE;
	}



	if((m_BlockOffset >irpOffset+irpLength) ||
		(irpOffset>m_BlockOffset + m_BlockLength))
		return FALSE;
	else
		return TRUE;
}

BOOLEAN   CActiveIrp::CheckOverlap(LONGLONG blockOffset,ULONG blockLength)
{
	if((m_BlockOffset > blockOffset+blockLength) ||
		(blockOffset>m_BlockOffset + m_BlockLength))
		return FALSE;
	else 
		return TRUE;
}

NTSTATUS  CActiveIrp::ProcessActiveIrp()
{
	NTSTATUS	status;

	switch (m_ActiveState)
	{
	case STATE_WRITE_NEW:
		{
			KdPrint(("OSNPA - Process STATE_WRITE_NEW ,IRP = %x.\n",m_pIrp));
			status = ProcessWriteIrp();
			break;
		}
	case STATE_READ_NEW:
		{
			KdPrint(("OSNPA - Process STATE_READ_NEW,IRP = %x .\n",m_pIrp));
			status = ProcessReadIrp();
			break;
		}
	case STATE_READ_PENDING:
		{
			status = QueryReadStatus();
			break;
		}
	case STATE_WRITE_PENDING:
		{
		//	KdPrint(("OSNPA - Process PENDING ,IRP = %x.\n",m_pIrp));
		//	status = QueryWriteStatus();
			status = STATUS_PENDING;
			break;
		}
	case STATE_WRITE_FAILED:
		{
			status = RecoveryBitmap();
			CompeleteWriteIrp();
			break;
		}
	case STATE_READ_COMPLETED:
	case STATE_WRITE_COMPLETED:
		{
			KdPrint(("OSNPA - Process COMPLETED\n"));
			if(m_SubIoCount <= 0)
			{
				status = STATUS_MORE_PROCESSING_REQUIRED;
			}
			else
			{
				status = STATUS_PENDING;
				KdPrint(("OSNPA - THERE IS M_SUBIOCOUNT = %x \n",m_SubIoCount));
			}
			
			break;
		}
	default:
		status = STATUS_SUCCESS;
		break;
	}
	
	return status;
}

NTSTATUS  CActiveIrp::DeviceOFFline(ACTIVE_IRP_STATE state)
{
	m_pIrp->IoStatus.Status = STATUS_DEVICE_OFF_LINE;
	m_pIrp->IoStatus.Information = 0;
	IoCompleteRequest(m_pIrp, IO_NO_INCREMENT);
	SetActiveState(state);

	return STATUS_DEVICE_OFF_LINE;
}

NTSTATUS  CActiveIrp::ProcessWriteIrp()
{
	NTSTATUS   status;
	PIO_STACK_LOCATION  pIrpStack;

	LARGE_INTEGER  blockOffset  ;
	ULONG  blockLength = 0;
	ULONG  relLength   = 0;
	LARGE_INTEGER  tempOffset   ;
	ULONG   tempLength  = 0;

	pIrpStack    = IoGetCurrentIrpStackLocation(m_pIrp);
	blockOffset  = pIrpStack->Parameters.Write.ByteOffset;
	blockLength  = pIrpStack->Parameters.Write.Length;
	KdPrint(("OSNPA - M_IRP = %x WRITE ORIGNAL BLOCKOFFSET = %I64x ,BLOCKLENGTH = %x \n",m_pIrp,blockOffset.QuadPart,blockLength));

	tempOffset.QuadPart = blockOffset.QuadPart;
	tempLength = blockLength;

	COsnPADevice  *pSouceDevice = (COsnPADevice *)m_pSourceVolume;
	COsnPADevice  *pCacheDevice = (COsnPADevice *)pSouceDevice->GetCacheDevice();
	CDiskCache    *pDiskCache   = pSouceDevice->GetDiskCache();
	if(!pCacheDevice || !pDiskCache)
	{
		KdPrint(("OSNPA - CACHE DEVICE IS OFFLINE OR NO CACHE INFO \n "));
		DeviceOFFline(STATE_WRITE_COMPLETED);
	//	pSouceDevice->SetCacheState(CACHE_DOWN);
	//	pSouceDevice->SetReloadFlag(FALSE);
		return STATUS_DEVICE_OFF_LINE;
	}

	relLength  =  (blockOffset.QuadPart % pDiskCache->GetOsnDskCH()->Cache.BlockSize) + blockLength;
	if(relLength <= pDiskCache->GetOsnDskCH()->Cache.BlockSize)
	{
		IncrementSubIrpCount();
		m_BitMapClr.Separate = FALSE;
		status = WriteProcess(tempOffset,tempLength,0,TRUE);
		if(!NT_SUCCESS(status))
		{
			QueryWriteStatus();
		}
	}
	else
	{
		KdPrint(("OSNPA - NEED SEPARATE TWO WRITE IRP \n"));
		IncrementSubIrpCount();
		IncrementSubIrpCount();

		m_BitMapClr.Separate = TRUE;
		tempLength = pDiskCache->GetOsnDskCH()->Cache.BlockSize - (blockOffset.QuadPart % pDiskCache->GetOsnDskCH()->Cache.BlockSize);
		status = WriteProcess(tempOffset,tempLength,0,TRUE);
		if(!NT_SUCCESS(status))
		{
			IncrementSubIrpCount();
			QueryWriteStatus();
			return status;
		}

		tempOffset.QuadPart = blockOffset.QuadPart;
		relLength           = pDiskCache->GetOsnDskCH()->Cache.BlockSize - (blockOffset.QuadPart % pDiskCache->GetOsnDskCH()->Cache.BlockSize);
		tempOffset.QuadPart += relLength;
		tempLength          = blockLength - relLength;
		status = WriteProcess(tempOffset,tempLength,relLength,FALSE);
		if(!NT_SUCCESS(status))
		{
			QueryWriteStatus();
		}
	}

	return status;
}

NTSTATUS  CActiveIrp::WriteProcess(LARGE_INTEGER tempOffset,ULONG   tempLength,ULONG relLength,BOOLEAN first)
{
	GET_BLOCK_STATE    ret ;
	NTSTATUS           status;
	COsnPADevice  *pSouceDevice = (COsnPADevice *)m_pSourceVolume;
	CDiskCache    *pDiskCache   = pSouceDevice->GetDiskCache();

	if(first)
	{
		ret = pDiskCache->GetBlockInCache(tempOffset.QuadPart,tempLength,FALSE,&m_BitMapClr.BitMapSeg[0]);
	}
	else
	{
		ret = pDiskCache->GetBlockInCache(tempOffset.QuadPart,tempLength,FALSE,&m_BitMapClr.BitMapSeg[1]);
	}
	switch(ret)
	{
	case 	STATE_SUCCESS:
		{
			KdPrint(("OSNPA - M_IRP = %x,WRITE TO CACHE BLOCKOFFSET = %I64x,BLOCKLENGTH = %x \n",m_pIrp,tempOffset.QuadPart,tempLength));
			status = WriteToCache(m_pIrp,tempOffset,tempLength,relLength);
			if(!NT_SUCCESS(status))
			{
				//DecrementSubIrpCount();
				m_Status = status;
				m_Information = 0;
				//m_pIrp->IoStatus.Status = status;
				//m_pIrp->IoStatus.Information = 0;
				//IoCompleteRequest(m_pIrp,IO_NO_INCREMENT);
				//SetActiveState(STATE_WRITE_COMPLETED);
			}
			break;
		}
	case     STATE_FAILED:
		{
			KdPrint(("OSNPA - M_IRP = %x, FAILED TO GET CHACHE DETAIL \n",m_pIrp));
			//DecrementSubIrpCount();
			//m_pIrp->IoStatus.Status		= STATUS_UNSUCCESSFUL;
			//m_pIrp->IoStatus.Information	= 0;
			//IoCompleteRequest(m_pIrp, IO_NO_INCREMENT);
			//SetActiveState(STATE_WRITE_COMPLETED);
			status = STATUS_UNSUCCESSFUL;
			m_Status = status;
			m_Information = 0;
			break;
		}

	case      STATE_NO_ENOUGH_SPACE:
		{
			KdPrint(("OSNPA - M_IRP = %x, THERE IS NO ENOUGH SPACE IN CACHE \n",m_pIrp));

			status = WriteToOriginal(m_pIrp,tempOffset,tempLength,relLength);
			if(!NT_SUCCESS(status))
			{
				//DecrementSubIrpCount();
				//m_pIrp->IoStatus.Status = status;
				//m_pIrp->IoStatus.Information = 0;
				//IoCompleteRequest(m_pIrp,IO_NO_INCREMENT);
				//SetActiveState(STATE_WRITE_COMPLETED);
				m_Status = status;
				m_Information = 0;
			}
			break;
		}
	}

	return status;
}

NTSTATUS  CActiveIrp::WriteToOriginal(PIRP pIrp,LARGE_INTEGER offset,ULONG length, ULONG RelLength)
{
	PIRP   newIrp = NULL;
	ULONGLONG  blockoffset = offset.QuadPart/BLOCK_SIZE;
	COsnPADevice *pSourceDevice = (COsnPADevice *)m_pSourceVolume;
	COsnPADevice *pCacehDevice  = (COsnPADevice *)pSourceDevice->GetCacheDevice();

	newIrp = IoAllocateIrp(pSourceDevice->GetTargetDeviceObject()->StackSize+1,FALSE);
	if(!newIrp)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	newIrp->AssociatedIrp.MasterIrp = pIrp;
	//to set the top stack location for itself
	IoSetNextIrpStackLocation(newIrp);

	PIO_STACK_LOCATION  pIrpStack = IoGetCurrentIrpStackLocation(pIrp);
	PIO_STACK_LOCATION	pNewIrpCurStack = IoGetCurrentIrpStackLocation(newIrp);
	*pNewIrpCurStack = *pIrpStack;
	pNewIrpCurStack->Parameters.Write.ByteOffset = offset;
	pNewIrpCurStack->Parameters.Write.Length     = length;

	IoCopyCurrentIrpStackLocationToNext(newIrp);

	if(blockoffset >pSourceDevice->GetSizeInBlocks())
	{
		IoFreeIrp(newIrp);
		KdPrint(("ONSPA - offset is too big\n"));
		return STATUS_INVALID_PARAMETER;
	}

	PUCHAR	pSystemBuffer = (PUCHAR) MmGetSystemAddressForMdlSafe(pIrp->MdlAddress,
		NormalPagePriority)+RelLength;

	ASSERT(MmGetMdlByteCount(pIrp->MdlAddress) >= (RelLength+length));
	PMDL	pNewMdl = IoAllocateMdl(pSystemBuffer,
		length,
		FALSE,
		FALSE,
		NULL);
	pNewMdl->Next = NULL;
	MmBuildMdlForNonPagedPool(pNewMdl);

	ASSERT(MmGetMdlByteCount(pNewMdl) == length);
	newIrp->MdlAddress = pNewMdl;

	IoSetCompletionRoutine(newIrp,
		WriteCacheCompletionRoutine,
		this,
		true,
		true,
		true);

	NTSTATUS  status = pSourceDevice->AcquireRemoveLock(newIrp);
	if(!NT_SUCCESS(status))
	{
		IoFreeIrp(newIrp);
		IoFreeMdl(pNewMdl);
		return status;
	}

	SetActiveState(STATE_WRITE_PENDING);
	status = IoCallDriver(pSourceDevice->GetTargetDeviceObject(),newIrp);
	pSourceDevice->ReleaseRemoveLock(newIrp);
	return status;
}

NTSTATUS  CActiveIrp::WriteToCache(PIRP pIrp,LARGE_INTEGER offset,ULONG length,ULONG RelLength)
{
	PIRP   newIrp = NULL;
	ULONGLONG  blockoffset = offset.QuadPart/BLOCK_SIZE;
	COsnPADevice *pSourceDevice = (COsnPADevice *)m_pSourceVolume;
	COsnPADevice *pCacehDevice  = (COsnPADevice *)pSourceDevice->GetCacheDevice();

	newIrp = IoAllocateIrp(pCacehDevice->GetTargetDeviceObject()->StackSize+1,FALSE);
	if(!newIrp)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	newIrp->AssociatedIrp.MasterIrp = pIrp;
	//to set the top stack location for itself
	IoSetNextIrpStackLocation(newIrp);

	PIO_STACK_LOCATION  pIrpStack = IoGetCurrentIrpStackLocation(pIrp);
	PIO_STACK_LOCATION	pNewIrpCurStack = IoGetCurrentIrpStackLocation(newIrp);
	*pNewIrpCurStack = *pIrpStack;
	pNewIrpCurStack->Parameters.Write.ByteOffset = offset;
	pNewIrpCurStack->Parameters.Write.Length     = length;
	
	IoCopyCurrentIrpStackLocationToNext(newIrp);

	if(blockoffset >pCacehDevice->GetSizeInBlocks())
	{
		IoFreeIrp(newIrp);
		KdPrint(("ONSPA - offset is too big\n"));
		return STATUS_INVALID_PARAMETER;
	}

	PUCHAR	pSystemBuffer = (PUCHAR) MmGetSystemAddressForMdlSafe(pIrp->MdlAddress,
														NormalPagePriority)+RelLength;

	ASSERT(MmGetMdlByteCount(pIrp->MdlAddress) >= (RelLength+length));
	PMDL	pNewMdl = IoAllocateMdl(pSystemBuffer,
									length,
									FALSE,
									FALSE,
									NULL);
	pNewMdl->Next = NULL;
	MmBuildMdlForNonPagedPool(pNewMdl);

	ASSERT(MmGetMdlByteCount(pNewMdl) == length);
	newIrp->MdlAddress = pNewMdl;

	IoSetCompletionRoutine(newIrp,
							WriteCacheCompletionRoutine,
							this,
							true,
							true,
							true);

	NTSTATUS  status = pCacehDevice->AcquireRemoveLock(newIrp);
	if(!NT_SUCCESS(status))
	{
		IoFreeIrp(newIrp);
		IoFreeMdl(pNewMdl);
		return status;
	}

	SetActiveState(STATE_WRITE_PENDING);
	status = IoCallDriver(pCacehDevice->GetTargetDeviceObject(),newIrp);
	pCacehDevice->ReleaseRemoveLock(newIrp);
	return status;
}

NTSTATUS  CActiveIrp::WriteCacheCompletionRoutine(PDEVICE_OBJECT	pDeviceObject, 
											                PIRP            pIrp, 
											                PVOID           pContext)
{
	CActiveIrp *pActiveIrp = (CActiveIrp *)pContext;
	CDiskDevice *pSoucreDevice = (CDiskDevice *)(pActiveIrp->GetSourceDiskDevice());
	if (pIrp->PendingReturned) 
	{
		IoMarkIrpPending(pIrp);
	}

	PIRP		pMasterIrp  = pIrp->AssociatedIrp.MasterIrp;

	if(pIrp->MdlAddress)
	{
		IoFreeMdl(pIrp->MdlAddress);
		pIrp->MdlAddress = NULL;
	}

	pActiveIrp->SetInfomation(pIrp->IoStatus.Information);
	if(!NT_SUCCESS(pIrp->IoStatus.Status))
	{
		pActiveIrp->SetStatus(pIrp->IoStatus.Status);
	}

	pActiveIrp->QueryWriteStatus();

	if(pIrp)
	{
		IoFreeIrp(pIrp);
		pIrp = NULL;
	}
	// this have problem we should judge the io is success or not ,if not ,clear bitmap
	if(pSoucreDevice)
		pSoucreDevice->SetHandleIrpThreadEvent();

	return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS  CActiveIrp::ProcessReadIrp()
{
	NTSTATUS  status;
	PIO_STACK_LOCATION  pIrpStack;
	LARGE_INTEGER  blockOffset  ;
	LARGE_INTEGER  tempOffset   ;
	ULONG  blockLength = 0;
	ULONG  relLength   = 0;
	ULONG   tempLength  = 0;



	pIrpStack    = IoGetCurrentIrpStackLocation(m_pIrp);
	blockOffset  = pIrpStack->Parameters.Read.ByteOffset;
	blockLength  = pIrpStack->Parameters.Read.Length;
	KdPrint(("OSNPA - M_IRP = %x, READ ORIGNAL BLOCKOFFSET = %I64x,BLOCKLENGTH = %x \n",m_pIrp,blockOffset,blockLength));

	tempOffset.QuadPart = blockOffset.QuadPart;
	tempLength = blockLength;

	COsnPADevice  *pSouceDevice = (COsnPADevice *)m_pSourceVolume;
	COsnPADevice  *pCacheDevice = (COsnPADevice *)pSouceDevice->GetCacheDevice();
	CDiskCache    *pDiskCache   = pSouceDevice->GetDiskCache();

	if(!pCacheDevice || !pDiskCache)
	{
		KdPrint(("OSNPA - CACHE DEVICE IS OFFLINE OR NO CACHE INFO \n "));
		DeviceOFFline(STATE_READ_COMPLETED);
	//	pSouceDevice->SetCacheState(CACHE_DOWN);
	//	pSouceDevice->SetReloadFlag(FALSE);
		return STATUS_DEVICE_OFF_LINE;
	}

	relLength  =  (blockOffset.QuadPart % pDiskCache->GetOsnDskCH()->Cache.BlockSize) + blockLength;
	if(relLength <= pDiskCache->GetOsnDskCH()->Cache.BlockSize)
	{
		status = ReadProcess(tempOffset,tempLength,0);
	}
	else
	{
		KdPrint(("OSNPA - M_IRP = %x, NEED SEPARATE TWO READ IRP \n"));
		tempLength = pDiskCache->GetOsnDskCH()->Cache.BlockSize - (blockOffset.QuadPart % pDiskCache->GetOsnDskCH()->Cache.BlockSize);
		status = ReadProcess(tempOffset,tempLength,0);
		if(!NT_SUCCESS(status))
		{
			return status;
		}
		
		relLength = pDiskCache->GetOsnDskCH()->Cache.BlockSize - (blockOffset.QuadPart % pDiskCache->GetOsnDskCH()->Cache.BlockSize);
		tempOffset.QuadPart = blockOffset.QuadPart;
		tempOffset.QuadPart += relLength;
		tempLength           = blockLength - relLength;
		status = ReadProcess(tempOffset,tempLength,relLength);
	}

	return status;
}

NTSTATUS  CActiveIrp::ReadProcess(LARGE_INTEGER tempOffset,ULONG   tempLength,ULONG relLength)
{
	NTSTATUS  status;
	ULONG   blockNum    = 0;
	COsnPADevice  *pSouceDevice = (COsnPADevice *)m_pSourceVolume;
	COsnPADevice  *pCacheDevice = (COsnPADevice *)pSouceDevice->GetCacheDevice();
	CDiskCache    *pDiskCache   = pSouceDevice->GetDiskCache();

	status = pDiskCache->CheckBatMap(tempOffset.QuadPart,tempLength,&blockNum,TRUE);
	if(!NT_SUCCESS(status))
	{
		KdPrint(("OSNPA - PASSTHOUGH THIS READ IRP \n"));
		IncrementSubIrpCount();
		status = ReadFromOrig(tempOffset.QuadPart,tempLength);
		if(!NT_SUCCESS(status))
		{
			m_pIrp->IoStatus.Status		= status;
			m_pIrp->IoStatus.Information	= 0;
			IoCompleteRequest(m_pIrp, IO_NO_INCREMENT);
			SetActiveState(STATE_READ_COMPLETED);
			DecrementSubIrpCount();
		}
		return status;
	}

	IncrementSubIrpCount();
	status = ReadFromCacheOrOrig(blockNum,tempOffset.QuadPart,tempLength,relLength);
	if(!NT_SUCCESS(status))
	{
		KdPrint(("OSNPA - FAILED READ IO \n"));
		m_pIrp->IoStatus.Status		= status;
		m_pIrp->IoStatus.Information	= 0;
		IoCompleteRequest(m_pIrp, IO_NO_INCREMENT);
		SetActiveState(STATE_READ_COMPLETED);
	}

	if(DecrementSubIrpCount() == 0 && NT_SUCCESS(status))
	{
	/*	KdPrint(("OSNPA - COMPLETE READ IRP = %x \n",m_pIrp));
		m_pIrp->IoStatus.Status		= m_Status;
		m_pIrp->IoStatus.Information	= m_Information;
		IoCompleteRequest(m_pIrp, IO_NO_INCREMENT);
		SetActiveState(STATE_READ_COMPLETED);*/
	}

	return status;
}

NTSTATUS  CActiveIrp::ReadFromCacheOrOrig(ULONG blockNum,LONGLONG tempOffset,ULONG tempLength,ULONG relLength)
{
	NTSTATUS   status;
	//PIO_STACK_LOCATION   pIrpStack = IoGetCurrentIrpStackLocation(m_pIrp);
	//LARGE_INTEGER   blockOffset       = pIrpStack->Parameters.Read.ByteOffset;
	//blockOffset.QuadPart              += relLength;
	//ULONG        blockLength       = pIrpStack->Parameters.Read.Length;
	//blockLength                   -= relLength;

	LARGE_INTEGER   blockOffset;
	blockOffset.QuadPart = tempOffset;
	ULONG blockLength          = tempLength;
//	LONGLONG    tempOffset = blockOffset.QuadPart;
//	ULONG        tempLength = blockLength;

	COsnPADevice  *pSouceDevice = (COsnPADevice *)m_pSourceVolume;
	CDiskCache    *pDiskCache   = pSouceDevice->GetDiskCache();

	tempOffset              = tempOffset %(pDiskCache->GetOsnDskCH()->Cache.BlockSize);
	ULONG	startingByte	= pDiskCache->GetBitmapByte(tempOffset);
	CHAR	startingBit		= pDiskCache->GetBitmapBit(tempOffset);
	ULONG	endingByte		= pDiskCache->GetBitmapByte(tempOffset + tempLength-1);
	CHAR	endingBit		= pDiskCache->GetBitmapBit(tempOffset + tempLength-1);
	PBITMAP    pBitmap      = (PBITMAP)pDiskCache->GetBlockBitmap(blockNum);
	if(!pBitmap)
	{
		KdPrint(("OSNPA - CAN'T GET BITMAP OF THIS BLOCKNUM \n"));
		return STATUS_UNSUCCESSFUL;
	}

	CHAR		firstBit, lastBit;
	LONG     	subReadOffset        = 0;
	ULONG		subReadLength        = 0;
	ULONG       Cachelen = 0;
	ULONG       OrigLen  = 0;
	ULONG       byteIndex ;

	for (byteIndex = startingByte; byteIndex <= endingByte; byteIndex++)
	{
		firstBit= 0;
		lastBit	= BIT_PER_BYTE - 1;

		if(byteIndex == startingByte)
			firstBit = startingBit;

		if(byteIndex == endingByte)
			lastBit	= endingBit;

		for (CHAR bitIndex = firstBit;  bitIndex <= lastBit; bitIndex++)
		{
			if(pBitmap->BitMap[byteIndex] & (0x1<<bitIndex))
			{
				if(OrigLen >0)
				{
					LONGLONG offset = blockOffset.QuadPart + subReadOffset;
					IncrementSubIrpCount();
					status = ReadFromOrig(offset,OrigLen * BLOCK_SIZE);
					if(!NT_SUCCESS(status))
					{
						DecrementSubIrpCount();
						KdPrint(("OSNPA - FAILED TO READ FROM ORIG BYTE = %x,BIT = %x \n",byteIndex,bitIndex));
						return STATUS_UNSUCCESSFUL;
					}
					subReadOffset += OrigLen*BLOCK_SIZE;
					OrigLen = 0;
				}
				Cachelen++;
				if(byteIndex == endingByte && bitIndex == lastBit)
				{
					LONGLONG  offset =  pDiskCache->GetOsnDskCH()->Cache.StartingOffset + (blockNum-1)*(pDiskCache->GetOsnDskCH()->Cache.BlockSize+pDiskCache->GetOsnDskCH()->Cache.BlockHdSize);
					offset       +=   pDiskCache->GetOsnDskCH()->Cache.BlockHdSize +tempOffset +subReadOffset;
					IncrementSubIrpCount();
					status = ReadFormCahce(offset,Cachelen * BLOCK_SIZE,relLength+subReadOffset);
					if(!NT_SUCCESS(status))
					{
						DecrementSubIrpCount();
						KdPrint(("OSNPA - FAILED TO READ FORM CAHCE BYTE = %x,BIT = %x \n",byteIndex,bitIndex));
						return STATUS_UNSUCCESSFUL;
					}
					subReadOffset += Cachelen*BLOCK_SIZE;
					Cachelen = 0;

					return status;
				}
			}
			else
			{
				if(Cachelen > 0)
				{
					LONGLONG  offset = pDiskCache->GetOsnDskCH()->Cache.StartingOffset + (blockNum-1)*(pDiskCache->GetOsnDskCH()->Cache.BlockSize+pDiskCache->GetOsnDskCH()->Cache.BlockHdSize);
					offset       +=   pDiskCache->GetOsnDskCH()->Cache.BlockHdSize +tempOffset +subReadOffset;
					IncrementSubIrpCount();
					status = ReadFormCahce(offset,Cachelen * BLOCK_SIZE,relLength+subReadOffset);
					if(!NT_SUCCESS(status))
					{
						DecrementSubIrpCount();
						KdPrint(("OSNPA - FAILED TO READ FORM CAHCE BYTE = %x,BIT = %x \n",byteIndex,bitIndex));
						return STATUS_UNSUCCESSFUL;
					}
					subReadOffset += Cachelen*BLOCK_SIZE;
					Cachelen = 0;
				}
				OrigLen++;

				if(byteIndex == endingByte && bitIndex == lastBit)
				{
					LONGLONG offset = blockOffset.QuadPart + subReadOffset;
					IncrementSubIrpCount();
					status = ReadFromOrig(offset,OrigLen * BLOCK_SIZE);
					if(!NT_SUCCESS(status))
					{
						DecrementSubIrpCount();
						KdPrint(("OSNPA - FAILED TO READ FROM ORIG BYTE = %x,BIT = %x \n",byteIndex,bitIndex));
						return STATUS_UNSUCCESSFUL;
					}
					subReadOffset += OrigLen*BLOCK_SIZE;
					OrigLen = 0;

					return status;
				}
			}
		}
	}

	return STATUS_UNSUCCESSFUL;
}

NTSTATUS   CActiveIrp::ReadFromOrig(LONGLONG offset,ULONG length)
{
	NTSTATUS   status;
	if(length == 0)
		return STATUS_SUCCESS;

	PIRP   newIrp = NULL;
	COsnPADevice  *pSouceDevice = (COsnPADevice *)m_pSourceVolume;
	CDiskCache    *pDiskCache   = pSouceDevice->GetDiskCache();

	PIO_STACK_LOCATION   pIrpStack = IoGetCurrentIrpStackLocation(m_pIrp);
	LARGE_INTEGER        blockOffset = pIrpStack->Parameters.Read.ByteOffset;
	ULONG                blockLength = pIrpStack->Parameters.Read.Length;

	newIrp = IoAllocateIrp(pSouceDevice->GetTargetDeviceObject()->StackSize+1,FALSE);
	if(!newIrp)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	newIrp->AssociatedIrp.MasterIrp = m_pIrp;

	//to set the top stack location for itself
	IoSetNextIrpStackLocation(newIrp);

	LONGLONG           orginalOffset = pIrpStack->Parameters.Read.ByteOffset.QuadPart;
	PIO_STACK_LOCATION	pNewIrpCurStack = IoGetCurrentIrpStackLocation(newIrp);
	*pNewIrpCurStack = *pIrpStack;

	pNewIrpCurStack->Parameters.Read.ByteOffset.QuadPart = offset;
	pNewIrpCurStack->Parameters.Read.Length     = length;

	IoCopyCurrentIrpStackLocationToNext(newIrp);

	if(((ULONG)(offset/BLOCK_SIZE)) >pSouceDevice->GetSizeInBlocks())
	{
		IoFreeIrp(newIrp);
		KdPrint(("ONSPA - offset is too big\n"));
		return STATUS_INVALID_PARAMETER;
	}

	PUCHAR	pSystemBuffer = (PUCHAR) MmGetSystemAddressForMdlSafe(m_pIrp->MdlAddress,
		NormalPagePriority)+(offset - orginalOffset);

	KdPrint(("OSNPA - MDL :%x ,offset :%I64x,orginal offset :%I64x ,length :%x\n",
		MmGetMdlByteCount(m_pIrp->MdlAddress),offset , orginalOffset ,length));
	KdPrint(("OSNPA - M_IRP =%x,ReadFromOrig length = %x \n",m_pIrp,length));

	ASSERT(MmGetMdlByteCount(m_pIrp->MdlAddress) >= (offset - orginalOffset +length));
	PMDL	pNewMdl = IoAllocateMdl(pSystemBuffer,
		length,
		FALSE,
		FALSE,
		NULL);
	pNewMdl->Next = NULL;
	MmBuildMdlForNonPagedPool(pNewMdl);

	ASSERT(MmGetMdlByteCount(pNewMdl) == length);
	newIrp->MdlAddress = pNewMdl;

	IoSetCompletionRoutine(newIrp,
		ReadCompletionRoutine,
		this,
		true,
		true,
		true);

	status = pSouceDevice->AcquireRemoveLock(newIrp);
	if(!NT_SUCCESS(status))
	{
		IoFreeIrp(newIrp);
		IoFreeMdl(pNewMdl);
		return status;
	}

	SetActiveState(STATE_READ_PENDING);
	status = IoCallDriver(pSouceDevice->GetTargetDeviceObject(),newIrp);
	pSouceDevice->ReleaseRemoveLock(newIrp);
	return status;
}

NTSTATUS   CActiveIrp::ReadFormCahce(LONGLONG offset,ULONG length,ULONG relLength)
{
	NTSTATUS   status;
	PIRP       newIrp = NULL;
	if(length == 0)
		return STATUS_SUCCESS;
	PIO_STACK_LOCATION   pIrpStack = IoGetCurrentIrpStackLocation(m_pIrp);
	LARGE_INTEGER        blockOffset = pIrpStack->Parameters.Read.ByteOffset;
	ULONG                blockLength = pIrpStack->Parameters.Read.Length;

	COsnPADevice  *pSouceDevice = (COsnPADevice *)m_pSourceVolume;
	CDiskCache    *pDiskCache   = pSouceDevice->GetDiskCache();
	COsnPADevice  *pCacehDevice  = (COsnPADevice *)pSouceDevice->GetCacheDevice();

	newIrp = IoAllocateIrp(pCacehDevice->GetTargetDeviceObject()->StackSize+1,FALSE);
	if(!newIrp)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	newIrp->AssociatedIrp.MasterIrp = m_pIrp;

	//to set the top stack location for itself
	IoSetNextIrpStackLocation(newIrp);

	PIO_STACK_LOCATION	pNewIrpCurStack = IoGetCurrentIrpStackLocation(newIrp);
	*pNewIrpCurStack = *pIrpStack;

	pNewIrpCurStack->Parameters.Read.ByteOffset.QuadPart = offset;
	pNewIrpCurStack->Parameters.Read.Length     = length;

	IoCopyCurrentIrpStackLocationToNext(newIrp);

	if(((ULONG)(offset/BLOCK_SIZE)) >pCacehDevice->GetSizeInBlocks())
	{
		IoFreeIrp(newIrp);
		KdPrint(("ONSPA - offset is too big\n"));
		return STATUS_INVALID_PARAMETER;
	}

	PUCHAR	pSystemBuffer = (PUCHAR) MmGetSystemAddressForMdlSafe(m_pIrp->MdlAddress,
		NormalPagePriority)+relLength;

	KdPrint(("OSNPA - MDL :%x ,offset Mdl :%x \n",MmGetMdlByteCount(m_pIrp->MdlAddress),(length+relLength)));
	KdPrint(("OSNPA - M_IRP =%x,ReadFromCache offset= %I64x,length = %x \n",m_pIrp,offset,length));

	ASSERT(MmGetMdlByteCount(m_pIrp->MdlAddress) >= (length+relLength));
	PMDL	pNewMdl = IoAllocateMdl(pSystemBuffer,
		length,
		FALSE,
		FALSE,
		NULL);
	pNewMdl->Next = NULL;
	MmBuildMdlForNonPagedPool(pNewMdl);

	ASSERT(MmGetMdlByteCount(pNewMdl) == length);
	newIrp->MdlAddress = pNewMdl;

	IoSetCompletionRoutine(newIrp,
		ReadCompletionRoutine,
		this,
		true,
		true,
		true);

	status = pCacehDevice->AcquireRemoveLock(newIrp);
	if(!NT_SUCCESS(status))
	{
		IoFreeIrp(newIrp);
		IoFreeMdl(pNewMdl);
		return status;
	}

	SetActiveState(STATE_READ_PENDING);
	status = IoCallDriver(pCacehDevice->GetTargetDeviceObject(),newIrp);
	pCacehDevice->ReleaseRemoveLock(newIrp);
	return status;
}

NTSTATUS  CActiveIrp::ReadCompletionRoutine(PDEVICE_OBJECT	pDeviceObject, 
	PIRP            pIrp, 
	PVOID           pContext)
{
	CActiveIrp *pActiveIrp = (CActiveIrp *)pContext;
	CDiskDevice *pSoucreDevice = (CDiskDevice *)pActiveIrp->GetSourceDiskDevice();
	if (pIrp->PendingReturned) 
	{
		IoMarkIrpPending(pIrp);
	}

	PIRP		pMasterIrp  = pIrp->AssociatedIrp.MasterIrp;

	if(pIrp->MdlAddress)
	{
		IoFreeMdl(pIrp->MdlAddress);
		pIrp->MdlAddress = NULL;
	}

	pActiveIrp->SetInfomation((ULONG)pIrp->IoStatus.Information);
	if(!NT_SUCCESS(pIrp->IoStatus.Status))
	{
		pActiveIrp->SetStatus(pIrp->IoStatus.Status);
	}
	if(pActiveIrp->DecrementSubIrpCount() == 0)
	{
		//KdPrint(("OSNPA - COMPELETE READ IRP = %x ,INFORMATION = %x \n",pIrp->AssociatedIrp.MasterIrp,
		//	pActiveIrp->GetInfomation()));
		//if(!NT_SUCCESS(pActiveIrp->GetStatus()))
		//	KdPrint(("OSNPA - COMPELETE READ IRP WITH FAILED STATUS = %x \n",pActiveIrp->GetStatus()));
		//pIrp->AssociatedIrp.MasterIrp->IoStatus.Status = pActiveIrp->GetStatus();
		//pIrp->AssociatedIrp.MasterIrp->IoStatus.Information = pActiveIrp->GetInfomation();
		//IoCompleteRequest(pIrp->AssociatedIrp.MasterIrp,IO_NO_INCREMENT);
		//pActiveIrp->SetActiveState(STATE_READ_COMPLETED);
	
	}
	if(pIrp)
	{
		IoFreeIrp(pIrp);
		pIrp = NULL;
	}

	//if(pSoucreDevice)
		//pSoucreDevice->SetHandleIrpThreadEvent();

	return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS  CActiveIrp::QueryReadStatus()
{
	if(m_SubIoCount <= 0)
	{
		KdPrint(("OSNPA - COMPELETE READ IRP = %x ,INFO = %x\n ",m_pIrp,m_Information));
		if(!NT_SUCCESS(m_Status))
		{
			KdPrint(("OSNPA - COMPELETE READ IRP FAILED STATUS = %x \n",m_Status));
		}
		PIO_STACK_LOCATION   pIrpStack = IoGetCurrentIrpStackLocation(m_pIrp);
		if(pIrpStack->Parameters.Read.Length != m_Information)
		{
			KdPrint(("OSNPA -  read error 24 ,m_pIrp=%x, length = %x,m_Information = %x,m_Status = %x\n",
				m_pIrp,pIrpStack->Parameters.Read.Length,m_Information,m_Status));
		}

		m_pIrp->IoStatus.Information = m_Information;
		m_pIrp->IoStatus.Status      = m_Status;

		IoCompleteRequest(m_pIrp, IO_NO_INCREMENT);
		SetActiveState(STATE_READ_COMPLETED);

		return STATUS_MORE_PROCESSING_REQUIRED;
	}

	return STATUS_PENDING;
}

NTSTATUS  CActiveIrp::QueryWriteStatus()
{
	if(DecrementSubIrpCount() == 0)
	{
		KdPrint(("OSNPA - COMPELETE WRITE IRP = %x ,INFO = %x\n ",m_pIrp,m_Information));
		if(!NT_SUCCESS(m_Status))
		{
			SetActiveState(STATE_WRITE_FAILED);
			KdPrint(("OSNPA - COMPELETE WRITE IRP FAILED STATUS = %x \n",m_Status));
			return STATUS_MORE_PROCESSING_REQUIRED;
		}

		return CompeleteWriteIrp();
	}

	return STATUS_PENDING;
}

NTSTATUS   CActiveIrp::CompeleteWriteIrp()
{
	PIO_STACK_LOCATION   pIrpStack = IoGetCurrentIrpStackLocation(m_pIrp);
	if(pIrpStack->Parameters.Write.Length != m_Information)
	{
		KdPrint(("OSNPA -  write error 24 ,m_pIrp=%x, length = %x,m_Information = %x,m_Status = %x\n",
			m_pIrp,pIrpStack->Parameters.Write.Length,m_Information,m_Status));
	}

	m_pIrp->IoStatus.Information = m_Information;
	m_pIrp->IoStatus.Status      = m_Status;
	InterlockedIncrement(&m_CompeleteTimes);
	IoCompleteRequest(m_pIrp, IO_NO_INCREMENT);
	SetActiveState(STATE_WRITE_COMPLETED);

	return STATUS_MORE_PROCESSING_REQUIRED;
}


NTSTATUS  CActiveIrp::RecoveryBitmap()
{
	KdPrint(("OSNPA - Recovery old bitmap to disk\n"));
	ULONGLONG      blockOffset = 0;
	ULONG          blockLength = 0;
	ULONGLONG      tempOffset = 0;
	ULONG          tempLength = 0;
	ULONG          relLength   = 0;

	COsnPADevice  *pSouceDevice = (COsnPADevice *)m_pSourceVolume;
	CDiskCache    *pDiskCache   = pSouceDevice->GetDiskCache();

	blockOffset = m_BlockOffset * BLOCK_SIZE;
	blockLength = m_BlockLength * BLOCK_SIZE;

	if(FALSE == m_BitMapClr.Separate)
	{
		if(m_BitMapClr.BitMapSeg[0].WriteToDisk)
		{
			tempOffset = blockOffset;
			tempLength = blockLength;
			pDiskCache->RecoveryBitMap(tempOffset,tempLength,&m_BitMapClr.BitMapSeg[0]);
		}
	}
	else
	{
		if(m_BitMapClr.BitMapSeg[0].WriteToDisk)
		{
			tempOffset = blockOffset;
			tempLength = pDiskCache->GetOsnDskCH()->Cache.BlockSize - (blockOffset % pDiskCache->GetOsnDskCH()->Cache.BlockSize);
			pDiskCache->RecoveryBitMap(tempOffset,tempLength,&m_BitMapClr.BitMapSeg[0]);
		}

		if(m_BitMapClr.BitMapSeg[1].WriteToDisk)
		{
			tempOffset = blockOffset;
			relLength           = pDiskCache->GetOsnDskCH()->Cache.BlockSize - (blockOffset % pDiskCache->GetOsnDskCH()->Cache.BlockSize);
			tempOffset += relLength;
			tempLength          = blockLength - relLength;
			pDiskCache->RecoveryBitMap(tempOffset,tempLength,&m_BitMapClr.BitMapSeg[1]);
		}
	}

	return STATUS_SUCCESS;
}