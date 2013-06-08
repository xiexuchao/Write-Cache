#include "stdio.h"
#include <stddef.h>
#include <stdlib.h>

#include "..\common\Queue.h"
#include "..\common\osnevent.h"
#include "..\..\Applications\common\OSNDefs.h"
#include "..\..\Applications\common\CfgCommon.h"
#include "..\..\Applications\common\OSNCommon.h"
#include "DDOsnPA.h"


#include "OSNddk.h"
#include "ntdddisk.h"

#include "DiskDevice.h"
#include "DiskCacheVolume.h"
#include "ActiveIrp.h"
#include "OsnPADevice.h"

#include "global.h"

#include "OsnPAmsg.h"


extern CGlobal* g_pGlobal;

CGlobal::CGlobal(PDRIVER_OBJECT pDriverObject):
COSNBaseDevice(NtStyleDevice)
{		
	KeInitializeSpinLock(&m_OsnPADeviceListLock);
	KeInitializeSpinLock(&m_OsnPACacheInfoLock);
	KeInitializeEvent(&m_WorkerThreadEvent,SynchronizationEvent,FALSE);

	m_pDriverObject         = pDriverObject;
	m_DeviceCount           = 0;
	m_CacheCount            = 0;
	m_WorkerThreadStop      = FALSE;
	m_WorkerThreadStarted   = TRUE;
	m_WorkerThreadHandle    = NULL;
}

NTSTATUS CGlobal::Initialize(PUNICODE_STRING  pRegistryPath)
{
	NTSTATUS   status ;

	m_RegistryPath.Length			= pRegistryPath->Length;
	m_RegistryPath.MaximumLength	= pRegistryPath->MaximumLength;
	m_RegistryPath.Buffer			= (PWSTR) ExAllocatePoolWithTag(NonPagedPool,pRegistryPath->MaximumLength, OSNTAGPA);
	if(m_RegistryPath.Buffer)
	{
		RtlCopyUnicodeString(&m_RegistryPath, pRegistryPath);
	}

	// initialize lookasidelist 
	ExInitializeNPagedLookasideList(&m_NonPagedLookasideList,
		NULL,
		NULL,
		0,
		sizeof(CALLBACKCONTEXT),
		OSNTAGPA,
		0);

	m_WorkerThreadStop = FALSE;
	status = PsCreateSystemThread(&m_WorkerThreadHandle,		// will get thread handle
		(ACCESS_MASK)0L,			// access attributes (0 for driver-created threads)
		NULL,						// obj attributes (NULL for driver-created threads)
		NULL,						// process handle (NULL for driver-created threads)
		NULL,						// client ID (NULL for driver-created threads)
		(PKSTART_ROUTINE) OsnPAWorkerThread,		// function to call
		this);						//  start context(parameter passed to new thread)  ?
	if(!NT_SUCCESS(status))
	{
		KdPrint(("OSNPA - Failed to Create worker thread ,status = %x\n",status));
		return status ;
	}
	m_WorkerThreadStarted = TRUE;

	return status;
}

NTSTATUS CGlobal::InsertDeviceToList(COsnPADevice *pDevice)
{
	KIRQL       irql;
	bool        isSuccess = true;
	NTSTATUS    status = STATUS_UNSUCCESSFUL;

	if(!pDevice)
		return status;

	irql = AcquireDeviceListLock();

	isSuccess = m_OsnPADeviceListHeader.InsertQTail(pDevice);
	if(isSuccess)
	{
		status = STATUS_SUCCESS;
		m_DeviceCount++;
	}

	ReleaseDeviceListLock(irql);

	KdPrint(("OSNPA - Insert device to list ,device = %x\n",pDevice));
	return status;
}

NTSTATUS CGlobal::RemoveDeviceFromList(CQueue *pListNode)
{
	NTSTATUS   status = STATUS_UNSUCCESSFUL;
	KIRQL      irql;

	if(!pListNode)
		return status;

	irql = AcquireDeviceListLock();

	pListNode->Remove();
	m_DeviceCount--;
	delete pListNode;

	ReleaseDeviceListLock(irql);

	return STATUS_SUCCESS;
}

COsnPADevice *  CGlobal::GetDeviceFromList(PUNICODE_STRING pVolumeName)
{
	PUNICODE_STRING		pLongVolumeName;
	UNICODE_STRING		osnVolumeName;
	CQueue              *pListNode;
	COsnPADevice        *pOsnPADevice = NULL;
	COsnPADevice        *pTempDevice  = NULL;
	KIRQL               irql;

	irql = AcquireDeviceListLock();
	pListNode = m_OsnPADeviceListHeader.Next();
	while(pListNode)
	{
		pTempDevice = (COsnPADevice *)pListNode->GetItem();
		if(pTempDevice && pTempDevice->GetPnpState() != StateRemoved )
		{
			//have to remove "\\Device\"
			pLongVolumeName = pTempDevice->GetVolumeName();

			osnVolumeName.Length			= pLongVolumeName->Length - wcslen(DEVICE_PREFIX) * sizeof(WCHAR);
			osnVolumeName.MaximumLength		= pLongVolumeName->MaximumLength - wcslen(DEVICE_PREFIX) * sizeof(WCHAR);
			osnVolumeName.Buffer			= &(pLongVolumeName->Buffer[wcslen(DEVICE_PREFIX)]);

			KdPrint(("OsnPA - Available %S, length=%d.\n", osnVolumeName.Buffer, osnVolumeName.Length));

			if(RtlCompareUnicodeString(&osnVolumeName, pVolumeName, false) == 0)	
			{
				KdPrint(("OsnPA - Found Snapshot Volume!\n"));
				pOsnPADevice = pTempDevice;
				break;
			}
		}
		pListNode = pListNode->Next();
	}
	ReleaseDeviceListLock(irql);

	return pOsnPADevice;
}

COsnPADevice *  CGlobal::GetDeviceFromList(GUID volumeID)
{
	CQueue         *pListNode;
	COsnPADevice   *pOsnPADevice = NULL;
	COsnPADevice   *pTempDevice  = NULL;
	KIRQL          irql;

	irql = AcquireDeviceListLock();
	pListNode = m_OsnPADeviceListHeader.Next();
	while(pListNode)
	{
		pTempDevice = (COsnPADevice *)pListNode->GetItem();
		if(pTempDevice && pTempDevice->GetPnpState() != StateRemoved)
		{
			if(RtlCompareMemory(pTempDevice->GetVolumeGUID(),&volumeID,sizeof(GUID)) == sizeof(GUID))
			{
				pOsnPADevice = pTempDevice;
				break;
			}
		}
		pListNode = pListNode->Next();
	}
	ReleaseDeviceListLock(irql);

	return pOsnPADevice;
}

NTSTATUS CGlobal::InsertCacheInfoToList(PCACHE_INFO pCacheInfo)
{
	KIRQL       irql;
	bool        isSuccess = true;
	NTSTATUS    status = STATUS_UNSUCCESSFUL;

	if(!pCacheInfo)
		return status;

	irql = AcquireCacheInfoListLock();

	isSuccess = m_OsnPACacheInfoListHeader.InsertQTail(pCacheInfo);
	if(isSuccess)
	{
		m_CacheCount++;
		status = STATUS_SUCCESS;
	}

	ReleaseCacheInfoLock(irql);

	KdPrint(("OSNPA - Insert cacheinfo to list ,device = %x\n",pCacheInfo));
	return status;
}

NTSTATUS  CGlobal::RemoveCacheInfoFromList(PCACHE_INFO pCacheInfo)
{
	CQueue    *pListNode = NULL;
	PCACHE_INFO pTempCacheInfo = NULL;
	KIRQL      oldIrql;

	oldIrql = AcquireCacheInfoListLock();
	pListNode = m_OsnPACacheInfoListHeader.Next();
	while(pListNode)
	{
		pTempCacheInfo = (PCACHE_INFO)pListNode->GetItem();
		if(pTempCacheInfo && RtlCompareMemory(pCacheInfo,pTempCacheInfo,sizeof(CACHE_INFO)) == sizeof(CACHE_INFO))
		{
			pListNode->Remove();
			m_CacheCount--;
			ReleaseCacheInfoLock(oldIrql);
			delete pListNode;
			ExFreePool(pTempCacheInfo);

			return STATUS_SUCCESS;
		}
		pListNode = pListNode->Next();
	}

	ReleaseCacheInfoLock(oldIrql);
	return STATUS_UNSUCCESSFUL;
}

NTSTATUS   CGlobal::ClearCache(COsnPADevice *pOsnDevice)
{
	NTSTATUS     status = STATUS_SUCCESS;

	status = pOsnDevice->StopHandleIrpThread();
	if(!NT_SUCCESS(status))
		return status;

	if(pOsnDevice->GetAppCmdCount() != 0 ||
		pOsnDevice->GetAppReadCmdCount()!= 0 ||
		pOsnDevice->GetAppWriteCmdCount()!= 0 ||
		pOsnDevice->GetFlushIrpCount() != 0)
		return status;

	for(ULONG i=0;i<OSNMAX_FLUSH_IO_COUNT;i++)
	{
		if(pOsnDevice->GetFlushIrp(i)!=NULL)
		{
			KdPrint(("OSNPA: FLUSH io %d\n.",i));
			return status;
		}
	}

	pOsnDevice->ClearCacheInfo();
	pOsnDevice->ClearAppCmdQueue();
	pOsnDevice->ClearActiveQueue();

	return status;
}

PCACHE_INFO CGlobal::CheckCacheInfoByCache(GUID guid)
{
	CQueue    *pListNode = NULL;
	PCACHE_INFO pTempCacheInfo = NULL;
	KIRQL      oldIrql;

	oldIrql = AcquireCacheInfoListLock();
	pListNode = m_OsnPACacheInfoListHeader.Next();
	while(pListNode)
	{
		pTempCacheInfo = (PCACHE_INFO)pListNode->GetItem();
		if(pTempCacheInfo)
		{
			if(pTempCacheInfo->m_CacheVolumeID.SAN_VolumeID.m_VolumeGuid == guid)
			{
				ReleaseCacheInfoLock(oldIrql);
				return pTempCacheInfo;
			}
		}
		pListNode = pListNode->Next();
	}

	ReleaseCacheInfoLock(oldIrql);
	return pTempCacheInfo;
}

BOOLEAN     CGlobal::ChecCacheInfoBySource(GUID guid)
{
		CQueue    *pListNode = NULL;
	PCACHE_INFO pTempCacheInfo = NULL;
	KIRQL      oldIrql;

	oldIrql = AcquireCacheInfoListLock();
	pListNode = m_OsnPACacheInfoListHeader.Next();
	while(pListNode)
	{
		pTempCacheInfo = (PCACHE_INFO)pListNode->GetItem();
		if(pTempCacheInfo)
		{
			if(pTempCacheInfo->m_SourceVolumeID.SAN_VolumeID.m_VolumeGuid == guid)
			{
				ReleaseCacheInfoLock(oldIrql);
				return TRUE;
			}
		}
		pListNode = pListNode->Next();
	}

	ReleaseCacheInfoLock(oldIrql);
	return FALSE;
}

PCACHE_INFO  CGlobal::GetCacheInfoFromList(VOLUMEID *pSourceVolumeID)
{
	KIRQL      oldIrql;
	CQueue    *pListNode = NULL;
	PCACHE_INFO pTempCacheInfo = NULL;
	PCACHE_INFO pCacheInfo     = NULL;

	oldIrql = AcquireCacheInfoListLock();
	pListNode = m_OsnPACacheInfoListHeader.Next();
	while(pListNode)
	{
		pTempCacheInfo = (PCACHE_INFO)pListNode->GetItem();
		if(pTempCacheInfo)
		{
			if(RtlCompareMemory(&pTempCacheInfo->m_SourceVolumeID,pSourceVolumeID,sizeof(VOLUMEID)) == sizeof(VOLUMEID))
			{
				pCacheInfo = pTempCacheInfo;
				break;
			}
		}
		pListNode = pListNode->Next();
	}
	ReleaseCacheInfoLock(oldIrql);

	return pCacheInfo;
}

PCACHE_INFO  CGlobal::GetCacheInfoFromList(GUID guid)
{
	KIRQL      oldIrql;
	CQueue    *pListNode = NULL;
	PCACHE_INFO pTempCacheInfo = NULL;
	PCACHE_INFO pCacheInfo     = NULL;

	oldIrql = AcquireCacheInfoListLock();
	pListNode = m_OsnPACacheInfoListHeader.Next();
	while(pListNode)
	{
		pTempCacheInfo = (PCACHE_INFO)pListNode->GetItem();
		if(pTempCacheInfo)
		{
			if(RtlCompareMemory(&pTempCacheInfo->m_SourceVolumeID.SAN_VolumeID.m_VolumeGuid,&guid,sizeof(GUID)) == sizeof(GUID))
			{
				pCacheInfo = pTempCacheInfo;
				break;
			}
		}

		pListNode = pListNode->Next();
	}
	ReleaseCacheInfoLock(oldIrql);

	return pCacheInfo;
}

PCACHE_INFO  CGlobal::GetCacheInfoFromListByCache(VOLUMEID *pCacheVolumeID)
{
	KIRQL      oldIrql;
	CQueue    *pListNode = NULL;
	PCACHE_INFO pTempCacheInfo = NULL;
	PCACHE_INFO pCacheInfo     = NULL;

	oldIrql = AcquireCacheInfoListLock();
	pListNode = m_OsnPACacheInfoListHeader.Next();
	while(pListNode)
	{
		pTempCacheInfo = (PCACHE_INFO)pListNode->GetItem();
		if(pTempCacheInfo)
		{
			if(RtlCompareMemory(&pTempCacheInfo->m_CacheVolumeID,pCacheVolumeID,sizeof(VOLUMEID)) == sizeof(VOLUMEID))
			{
				pCacheInfo = pTempCacheInfo;
				break;
			}
		}

		pListNode = pListNode->Next();
	}
	ReleaseCacheInfoLock(oldIrql);

	return pCacheInfo;
}

NTSTATUS CGlobal::OnDispatchIoctl(PIRP pIrp)
{
	PIO_STACK_LOCATION currentIrpStack = IoGetCurrentIrpStackLocation(pIrp);

	PVOID	pInBuffer		= pIrp->AssociatedIrp.SystemBuffer;
	ULONG	inBufferSize	= currentIrpStack->Parameters.DeviceIoControl.InputBufferLength;
	ULONG	outBufferSize	= currentIrpStack->Parameters.DeviceIoControl.OutputBufferLength;

	NTSTATUS	status = STATUS_SUCCESS;
	ULONG		retBytes = 0;
	CACHE_INFO  cacheInfo = {0};


	switch (currentIrpStack->Parameters.DeviceIoControl.IoControlCode) 
	{
	case IOCTL_OSNPA_SET_VOLUME_CACHE:
		{
			if(inBufferSize != sizeof(CACHE_INFO))
			{
				KdPrint(("OSNPA - IOCTL_OSNPA_SET_VOLUME_CACHE param.\n"));
				status = STATUS_INVALID_PARAMETER;
				break;
			}

			RtlCopyMemory(&cacheInfo,pInBuffer,sizeof(CACHE_INFO));
			COsnPADevice *pOsnPASource = g_pGlobal->GetDeviceFromList(cacheInfo.m_SourceVolumeID.SAN_VolumeID.m_VolumeGuid);
			if(!pOsnPASource)
			{
				KdPrint(("OSNPA - cant't find the OsnPASource devcie\n"));
				status = STATUS_DEVICE_OFF_LINE;
				break;
			}

			COsnPADevice *pOsnPACache = g_pGlobal->GetDeviceFromList(cacheInfo.m_CacheVolumeID.SAN_VolumeID.m_VolumeGuid);
			if(!pOsnPACache)
			{
				KdPrint(("OSNPA - cant't find the OsnPACache device\n"));
				status = STATUS_DEVICE_OFF_LINE;
				break;
			}

			if(pOsnPASource == pOsnPACache)
			{
				KdPrint(("OSNPA - The PASource == PACache\n"));
				status = STATUS_UNSUCCESSFUL;
				break;
			}

			if(pOsnPASource->GetCacheFlag() || pOsnPACache->GetCacheFlag())
			{
				KdPrint(("OSNPA - PASource or PACache have set Cache.\n"));
				status = STATUS_UNSUCCESSFUL;
				break;
			}

			//this unit will use size in bytes,if application passthough size in blocks ,we should transform
			status = pOsnPASource->SetCacheProtect(pOsnPACache,OSNBLOCK_MAX_DATA_SIZE,TRUE);
			if(NT_SUCCESS(status))
			{
				COSNEventLog::OSNLogEvent(g_pGlobal->GetDriverObject(),OSNPA_CACHE_SET,0,pOsnPASource->GetVolumeName());
			}
			break;
		}
	case IOCTL_OSNPA_REMOVE_VOLUME_CACHE:
		{
			if(inBufferSize != sizeof(CACHE_INFO))
			{
				KdPrint(("OSNPA - IOCTL_OSNPA_REMOVE_VOLUME_CACHE param.\n"));
				status = STATUS_INVALID_PARAMETER;
				break;
			}
			status = STATUS_SUCCESS;

			RtlCopyMemory(&cacheInfo,pInBuffer,sizeof(CACHE_INFO));
			COsnPADevice *pOsnPASource = g_pGlobal->GetDeviceFromList(cacheInfo.m_SourceVolumeID.SAN_VolumeID.m_VolumeGuid);
			COsnPADevice *pOsnPACache = g_pGlobal->GetDeviceFromList(cacheInfo.m_CacheVolumeID.SAN_VolumeID.m_VolumeGuid);
			if(!pOsnPASource)
			{
				KdPrint(("OSNPA - cant't find the OsnPASource devcie\n"));	
				if(pOsnPACache)
				{
					g_pGlobal->ClearCache(pOsnPACache);
				}
				PCACHE_INFO pCacheInfo = g_pGlobal->GetCacheInfoFromList(&cacheInfo.m_SourceVolumeID);
				if(!pCacheInfo)
				{
					pCacheInfo = g_pGlobal->GetCacheInfoFromListByCache(&cacheInfo.m_CacheVolumeID);
				}

				if(pCacheInfo)
				{
					KdPrint(("OSNPA - can't remove pCacheInfo.\n"));
					g_pGlobal->RemoveCacheInfoFromList(pCacheInfo);
				}

				status = STATUS_SUCCESS;
				break;
			}

			if(CACHE_SOURCE != pOsnPASource->GetCacheRole() || FALSE == pOsnPASource->GetCacheFlag())
			{
				KdPrint(("OSNPA  - THIS SOURCE DEVICE IS NOT IN CACHE \n"));
				status = STATUS_SUCCESS;
				break;
			}

			KIRQL  irql = pOsnPASource->AcquireFlushSpinLock();
			pOsnPASource->SetRemoveCacheFlag(TRUE);
			pOsnPASource->SetFlushCacheFlag(TRUE);
			pOsnPASource->PrepareForFlush();
			pOsnPASource->SetCacheState(TRANSFER);
			pOsnPASource->ReleaseFlushSpinlock(irql);

			COSNEventLog::OSNLogEvent(g_pGlobal->GetDriverObject(),OSNPA_CACHE_DELETE,0,pOsnPASource->GetVolumeName());
			break;
		}

	case	IOCTL_OSNPA_GET_CACHE_INFO_STATE:
		{
			if(inBufferSize != sizeof(CACHE_INFO) ||outBufferSize != sizeof(QUERY_CACHE_STATE))
			{
				KdPrint(("OSNPA - THE PARAMETER FORMAT IS NOT RIGHT \n"));
				status = STATUS_INVALID_PARAMETER;
				break;
			}
			
			RtlCopyMemory(&cacheInfo,pInBuffer,sizeof(CACHE_INFO));
			COsnPADevice *pOsnPASource = g_pGlobal->GetDeviceFromList(cacheInfo.m_SourceVolumeID.SAN_VolumeID.m_VolumeGuid);
			if(!pOsnPASource)
			{
				status = STATUS_DEVICE_OFF_LINE;
				KdPrint(("OSNPA - THE SOURCE DEVICE IS OFFLINE \n"));
				break;
			}

			if(!pOsnPASource->GetCacheFlag())
			{
				status = STATUS_UNSUCCESSFUL;
				KdPrint(("OSNPA - NOT CACHE SOURCE \n"));
				break;
			}

			QUERY_CACHE_STATE  queryState;
			queryState.state = pOsnPASource->GetCacheState();

			RtlCopyMemory(pIrp->AssociatedIrp.SystemBuffer,&queryState,sizeof(QUERY_CACHE_STATE));
			status = STATUS_SUCCESS;
			retBytes	= sizeof(QUERY_CACHE_STATE);
			break;
		}
	case	IOCTL_OSNPA_SET_FLUSH_CACHE:
		{
			if(inBufferSize != sizeof(CACHE_INFO))
			{
				KdPrint(("OSNPA - IOCTL_OSNPA_SET_FLUSH_CACHE param.\n"));
				status = STATUS_INVALID_PARAMETER;
				break;
			}

			status = STATUS_SUCCESS;

			RtlCopyMemory(&cacheInfo,pInBuffer,sizeof(CACHE_INFO));
			COsnPADevice *pOsnPASource = g_pGlobal->GetDeviceFromList(cacheInfo.m_SourceVolumeID.SAN_VolumeID.m_VolumeGuid);
			COsnPADevice *pOsnPACache = g_pGlobal->GetDeviceFromList(cacheInfo.m_CacheVolumeID.SAN_VolumeID.m_VolumeGuid);
			if(!pOsnPASource || !pOsnPACache)
			{
				KdPrint(("OSNPA - IOCTL_OSNPA_SET_FLUSH_CACHE FIND DEVICE OFFLINE \n"));
				status = STATUS_DEVICE_OFF_LINE;
				break;
			}

			if(CACHE_SOURCE != pOsnPASource->GetCacheRole() || FALSE == pOsnPASource->GetCacheFlag())
			{
				KdPrint(("OSNPA  - THIS SOURCE DEVICE IS NOT IN CACHE \n"));
				status = STATUS_SUCCESS;
				break;
			}

			KIRQL  irql = pOsnPASource->AcquireFlushSpinLock();
			pOsnPASource->SetFlushCacheFlag(TRUE);
			pOsnPASource->PrepareForFlush();
			pOsnPASource->SetCacheState(TRANSFER);
			pOsnPASource->ReleaseFlushSpinlock(irql);

			COSNEventLog::OSNLogEvent(g_pGlobal->GetDriverObject(),OSNPA_CACHE_FLUSH_BEGIN,0,pOsnPASource->GetVolumeName());
			break;
		}
	default:
		{
			pIrp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
			IoCompleteRequest(pIrp, IO_NO_INCREMENT);

			return STATUS_INVALID_DEVICE_REQUEST;		
		}

	}

	pIrp->IoStatus.Status		= status;
	pIrp->IoStatus.Information	= retBytes;
	IoCompleteRequest(pIrp, IO_NO_INCREMENT);

	return status;		
}

NTSTATUS CGlobal::OnDispatchInternalIoctl(PIRP pIrp)
{
	PIO_STACK_LOCATION currentIrpStack = IoGetCurrentIrpStackLocation(pIrp);

	PVOID	pInBuffer		= pIrp->AssociatedIrp.SystemBuffer;
	ULONG	inBufferSize	= currentIrpStack->Parameters.DeviceIoControl.InputBufferLength;
	ULONG	outBufferSize	= currentIrpStack->Parameters.DeviceIoControl.OutputBufferLength;

	NTSTATUS	status = STATUS_SUCCESS;
	ULONG		retBytes = 0;

	switch (currentIrpStack->Parameters.DeviceIoControl.IoControlCode) 
	{
	case 0:	
	default:
		{
			return COSNBaseDevice::OnDispatchInternalIoctl(pIrp);
		}
	}

	pIrp->IoStatus.Status		= status;
	pIrp->IoStatus.Information	= retBytes;;
	IoCompleteRequest(pIrp, IO_NO_INCREMENT);

	return status;		
}

NTSTATUS CGlobal::OSNDeviceIoctl(
	PDEVICE_OBJECT		pTargetDevice, 
	ULONG				DeviceIoctlCode, 
	PVOID				pInputBuffer, 
	ULONG				inputBufferLength, 
	PVOID				pOutputBuffer,
	ULONG				outputBufferLength,
	PIO_STATUS_BLOCK	pIoStatusBlock)
{
	PIRP					pIrp;
	IO_STATUS_BLOCK			ioStatusBlock;
	KEVENT					Event;
	KeInitializeEvent(&Event, SynchronizationEvent, FALSE);

	NTSTATUS				status;

	pIrp = IoBuildDeviceIoControlRequest(DeviceIoctlCode,
		pTargetDevice,
		pInputBuffer,
		inputBufferLength,
		pOutputBuffer,		
		outputBufferLength,
		FALSE,
		&Event,
		&ioStatusBlock);

	if(!pIrp) 
		return STATUS_INSUFFICIENT_RESOURCES;

	status = IoCallDriver(pTargetDevice, pIrp);
	if(status==STATUS_PENDING)
	{
		KeWaitForSingleObject(&Event, Suspended, KernelMode, FALSE, NULL);
		status = ioStatusBlock.Status;
	}

	if(pIoStatusBlock!=NULL)
	{
		pIoStatusBlock->Status = ioStatusBlock.Status;
		pIoStatusBlock->Information = ioStatusBlock.Information;
	}

	return status;

}

NTSTATUS    CGlobal::ResetCacheRelation(CDiskDevice *pdevice)
{
	NTSTATUS   status;
	WCHAR      IdentifyName[MAX_DEVICE_NAME_LENGTH];
	UNICODE_STRING  guidString;
	UNICODE_STRING  cacheGuidString;
	GUID            guid;
	KIRQL      oldIrql;
	PCACHE_INFO    pCacheInfo;

	COsnPADevice   *pCacheDevice = NULL;
	COsnPADevice   *pOsnPADevice = (COsnPADevice *)pdevice;
	
	if(!pOsnPADevice->GetCacheFlag() || !pOsnPADevice->GetReloadFlag())
	{
		RtlStringFromGUID(pOsnPADevice->GetGUID(),&guidString);
		status = QueryCacheRelationFromRegistry(guidString.Buffer,IdentifyName,OSN_CACHE_RELATION);
		if(!NT_SUCCESS(status))
		{
			KdPrint(("OSNPA - DEVICE %x,HAVING NO CACHE \n",pOsnPADevice));
			pOsnPADevice->SetReloadFlag(TRUE);
			return status;
		}

		cacheGuidString.Length = wcslen(IdentifyName)*sizeof(WCHAR);
		cacheGuidString.MaximumLength = MAX_DEVICE_NAME_LENGTH*sizeof(WCHAR);
		cacheGuidString.Buffer = IdentifyName;
		RtlGUIDFromString(&cacheGuidString,&guid);
		pCacheDevice = GetDeviceFromList(guid);
		if(!pCacheDevice)
		{
			oldIrql = pOsnPADevice->AcquireCacheLock();
			pOsnPADevice->SetCacheFlag(TRUE);
			pOsnPADevice->SetCacheRole(CACHE_SOURCE);
			pOsnPADevice->SetCacheState(CACHE_DOWN);
			pOsnPADevice->SetReloadFlag(FALSE);
			pOsnPADevice->ReleaseCacheLock(oldIrql);

			KdPrint(("OSNPA - CACHE DEVICE IS NOT READLY,SET CACHE REL WAIT \n"));
			return STATUS_PENDING;
		}

		status = pOsnPADevice->SetCacheProtect(pCacheDevice,OSNBLOCK_MAX_DATA_SIZE,FALSE);
		if(NT_SUCCESS(status))
		{
			pOsnPADevice->SetReloadFlag(TRUE);
			KdPrint(("OSNPA - RESET CAHCE REL SUCCESS ,source deivce = %x \n",pOsnPADevice));
			COSNEventLog::OSNLogEvent(g_pGlobal->GetDriverObject(),OSNPA_CACHE_SET,0,pOsnPADevice->GetVolumeName());
		}
		else
		{
			KdPrint(("OSNPA - RESET CACHE REL FAILED,STATUS = %x \n",status));
			oldIrql = pOsnPADevice->AcquireCacheLock();
			pOsnPADevice->SetCacheFlag(TRUE);
			pOsnPADevice->SetCacheRole(CACHE_SOURCE);
			pOsnPADevice->SetCacheState(NONE);
			pOsnPADevice->SetReloadFlag(FALSE);
			pOsnPADevice->ReleaseCacheLock(oldIrql);
		}

		return status;
	}

	return STATUS_SUCCESS;
}

NTSTATUS    CGlobal::QueryCacheRelationFromRegistry(WCHAR *pKeyName,WCHAR *pValue,WCHAR *pSubKey)
{
	RTL_QUERY_REGISTRY_TABLE    QueryTable[2];
    ULONG length =0;
	RtlZeroMemory(&QueryTable[0], sizeof(QueryTable));

	QueryTable[0].QueryRoutine = (PRTL_QUERY_REGISTRY_ROUTINE)QueryRegistryStringValue;
	QueryTable[0].Flags = RTL_QUERY_REGISTRY_REQUIRED;	
	QueryTable[0].Name =  pKeyName;
	QueryTable[0].EntryContext = &length;
	QueryTable[0].DefaultType =0;
	QueryTable[0].DefaultData = NULL;
	QueryTable[0].DefaultLength = 0;

	NTSTATUS status = RtlQueryRegistryValues(RTL_REGISTRY_SERVICES, 
								pSubKey, 
								&QueryTable[0], 
								pValue,
								NULL); 

	if(NT_SUCCESS(status))
	{
		pValue[length/sizeof(WCHAR)]=L'\0';
	}
	else
	{
		KdPrint(("OSNPA -  failed to get CAHCE REL from registry,status =%x\n",status));
	}

	return status;
}

NTSTATUS    CGlobal::DeleteCacheRelationFromRegistry(WCHAR *pSubKey,WCHAR *pValue)
{
	return RtlDeleteRegistryValue(RTL_REGISTRY_SERVICES,
		                            pSubKey,
		                            pValue);
}

//
//if type is REG_SZ or REG_MULTI_SZ, the returned ValueData is in WCHAR
//the last '\0' is included in ValueLength
//

NTSTATUS  CGlobal::QueryRegistryStringValue(PWSTR	ValueName, 
									ULONG	ValueType,
									PVOID	ValueData,
									ULONG	ValueLength,
									PVOID	Context,
									PVOID	EntryContext)
{
	PWCHAR	pData = (PWCHAR) Context;
	PWCHAR	pValue = (PWCHAR) ValueData;
	if(ValueType==REG_SZ || ValueType==REG_MULTI_SZ)
	{
		wcscpy(pData, pValue);
		*(ULONG *) EntryContext = ValueLength;
		return STATUS_SUCCESS;
	}

	RtlCopyMemory(pData, pValue, ValueLength);
	* (ULONG *) EntryContext = ValueLength;
	return STATUS_SUCCESS;
}

BOOLEAN CGlobal::DeviceRemoveRoutine(COsnPADevice *pOsnDevice)
{
	NTSTATUS status;

	if(!pOsnDevice->GetCacheFlag())
		return TRUE;

	COsnPADevice  *pPrimaryDevice = pOsnDevice;
	CACHE_INFO    *pCacheInfo     = NULL;

	if(CACHE_SOURCE  !=  pOsnDevice->GetCacheRole())
	{
		pCacheInfo = CheckCacheInfoByCache(pOsnDevice->GetGUID());
		if(pCacheInfo)
		{
			pPrimaryDevice = GetDeviceFromList(pCacheInfo->m_SourceVolumeID.SAN_VolumeID.m_VolumeGuid);
		}
		else
			pPrimaryDevice = NULL;
	}

	if(pPrimaryDevice == NULL)
	{
		KdPrint(("OSNPA - CAN'T FIND THE SOURCE DEVICE \n"));
		return TRUE;
	}

	status = pOsnDevice->StopHandleIrpThread();
	if(!NT_SUCCESS(status))
	{
		return FALSE;
	}

	pOsnDevice->ClearAppCmdQueue();
	pOsnDevice->ClearActiveQueue();

	if(pOsnDevice->GetAppCmdCount() != 0 ||
		pOsnDevice->GetAppReadCmdCount()!= 0 ||
		pOsnDevice->GetAppWriteCmdCount()!= 0 ||
		pOsnDevice->GetFlushIrpCount() != 0)
		return FALSE;

	if(CACHE_SOURCE !=pOsnDevice->GetCacheRole())
	{
		pPrimaryDevice->SetCacheState(CACHE_DOWN);
		pPrimaryDevice->SetFlushCacheFlag(FALSE);
		pPrimaryDevice->m_pPACacheDevice = NULL;
		pPrimaryDevice->SetReloadFlag(FALSE);
	}
	
	pPrimaryDevice->FreeCacheMem();

	return TRUE;
}

static
	VOID OsnPAWorkerThread(PVOID context)
{
	PETHREAD				Pid = PsGetCurrentThread();
	KdPrint(("OSNPA - Start osn PA worker thread,Thread ID = %x\n",Pid));

	LONGLONG                oneMilliSecTimeOut = DELAY_ONE_MILLISECOND;
	LARGE_INTEGER           timeout;
	timeout.QuadPart        = oneMilliSecTimeOut;

	CGlobal                 *pGlobal = (CGlobal *)context;
	CQueue                  *pOSNDeviceList = NULL;
	COsnPADevice            *pOsnPADevice    = NULL;
	KIRQL                   irql = NULL;

	while(!pGlobal->GetWorkerThreadStop())
	{
		ASSERT(KeGetCurrentIrql() < DISPATCH_LEVEL);
		KeWaitForSingleObject(pGlobal->GetWorkerThreadEvent(),Executive,KernelMode,FALSE,&timeout);
		
		pOSNDeviceList = pGlobal->GetOSNPADeviceListHeader()->Next();
		while(pOSNDeviceList)
		{
			pOsnPADevice = (COsnPADevice *)pOSNDeviceList->GetItem();

			irql = pOsnPADevice->AcquireFlushSpinLock();
			if(pOsnPADevice->GetRemoveCacheFlag() && 
				!pOsnPADevice->GetFlushCache())
			{
				if(pOsnPADevice->GetFlushSuccess() || pOsnPADevice->GetCacheForceRemoveFlag())
				{
					g_pGlobal->ClearCache(pOsnPADevice);
					KdPrint(("OSNPA - SUCCESS REMOVE CACHE RELATION \n"));
				}
				else
				{
					pOsnPADevice->SetCacheState(TRANSFER_FAILED);
					pOsnPADevice->SetRemoveCacheFlag(FALSE);
					KdPrint(("OSNPA - FAILED REMOVE CACHE RELATION \n"));
				}
			}
			pOsnPADevice->ReleaseFlushSpinlock(irql);

			if(pOsnPADevice->GetPnpState() == StateRemoved &&
				pOsnPADevice->GetApplicationIrpQHead()->IsEmpty()&&
				!pOsnPADevice->GetDeviceObject())
			{
				if(pGlobal->DeviceRemoveRoutine(pOsnPADevice))
				{
					COsnPADevice *pPrimaryDevice;
					CACHE_INFO *pCacheInfo = pGlobal->CheckCacheInfoByCache(pOsnPADevice->GetGUID());

					if(pOsnPADevice->GetCacheRole() != CACHE_SOURCE &&pCacheInfo != NULL)
					{
						pPrimaryDevice = pGlobal->GetDeviceFromList(pCacheInfo->m_SourceVolumeID.SAN_VolumeID.m_VolumeGuid);
						if(pPrimaryDevice)
						{
							irql = pPrimaryDevice->AcquireCacheDeviceSpinLock();
							if(pPrimaryDevice->m_CacheDevicePointerUseFlag)
							{
								pPrimaryDevice->ReleaseCacheDeviceLock(irql);
								pOSNDeviceList = pGlobal->GetOSNPADeviceListHeader()->Next();
								continue;
							}

							pGlobal->RemoveDeviceFromList(pOSNDeviceList);
							delete pOsnPADevice;
							pPrimaryDevice->ReleaseCacheDeviceLock(irql);
						}
						else
						{
							pGlobal->RemoveDeviceFromList(pOSNDeviceList);
							delete pOsnPADevice;
						}
					}
					else
					{
						pGlobal->RemoveDeviceFromList(pOSNDeviceList);
						delete pOsnPADevice;
					}

					pOSNDeviceList = pGlobal->GetOSNPADeviceListHeader()->Next();
					continue;
				}	
			}

			if(pOsnPADevice->GetPnpState() == StateStarted && 
				pOsnPADevice->GetCacheRole() == CACHE_SOURCE &&
				!pOsnPADevice->GetReloadFlag())
			{
				pGlobal->ResetCacheRelation(pOsnPADevice);
			}

				pOSNDeviceList = pOSNDeviceList->Next();
		}
	}

	KdPrint(("OSNPA - terminate system thread \n"));
	PsTerminateSystemThread(STATUS_SUCCESS);
}

