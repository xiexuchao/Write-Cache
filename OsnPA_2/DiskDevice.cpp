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

extern CGlobal* g_pGlobal;

CDiskDevice::CDiskDevice():
CFido(DeviceFilter)
{
}

CDiskDevice::CDiskDevice(VOLUME_TYPE volumeType)
				:CFido(DeviceFilter),
				m_SizeInBlocks(0)
{
	m_AppIrpCount        = 0;
	m_ActiveIrpReadCount = 0;
	m_ActiveIrpWriteCount = 0;

	KeInitializeEvent(&m_HandleIrpEvent,SynchronizationEvent,FALSE);
	KeInitializeSpinLock(&m_ApplicationIrpQLock);
	KeInitializeSpinLock(&m_ActiveIrpQlock);
}

CDiskDevice::~CDiskDevice()
{
}

NTSTATUS CDiskDevice::InsertApplicationIrp(PIRP			pIrp)
{
	IoMarkIrpPending(pIrp);

	KIRQL	oldIrql;
	oldIrql = AcquireCmdListLock();
	m_ApplicationIrpQHead.InsertQTail(pIrp);
	ReleaseCmdListLock(oldIrql);

	InterlockedIncrement(&m_AppIrpCount);
	KdPrint(("OSNPA - INSERT IRPLIST ,m_AppIrpCount= %x,pIrp= %x \n",m_AppIrpCount,pIrp));


	this->SetHandleIrpThreadEvent();

	return STATUS_PENDING;
}

PIRP CDiskDevice::GetFirstApplicationIrp()
{
	CQueue	*pNode;
	PIRP	pIrp;

	KIRQL	oldIrql = AcquireCmdListLock();
	pNode = m_ApplicationIrpQHead.Next();
	if(pNode)
		pIrp = (PIRP) pNode->GetItem();
	else
		pIrp = NULL;
	ReleaseCmdListLock(oldIrql);

	return pIrp;
}

PIRP CDiskDevice::RemoveFirstApplicationIrp()
{
	PIRP	pIrp;

	KIRQL	oldIrql = AcquireCmdListLock();
	pIrp = (PIRP) m_ApplicationIrpQHead.DeQueueHead();
	ReleaseCmdListLock(oldIrql);

	if(pIrp)
		InterlockedDecrement(&m_AppIrpCount);


	return pIrp;
}

VOID CDiskDevice::ClearAppCmdQueue()
{
	PIRP	pIrp;

	KIRQL	oldIrql = AcquireCmdListLock();
	pIrp = (PIRP)m_ApplicationIrpQHead.DeQueueHead();
	while(pIrp)
	{
		DecrementAppCmdCount();
		pIrp->IoStatus.Status = STATUS_DEVICE_BUSY;
		pIrp->IoStatus.Information = 0;
		IoCompleteRequest(pIrp,IO_NO_INCREMENT);
		pIrp = (PIRP)m_ApplicationIrpQHead.DeQueueHead();
	}

	ReleaseCmdListLock(oldIrql);
}

VOID CDiskDevice::ClearActiveQueue()
{
	PIRP	pIrp;

	KIRQL	oldIrql = AcquireActiveIrpListLock();
	pIrp = (PIRP)m_ActiveIrpQHead.DeQueueHead();
	while(pIrp)
	{
		PIO_STACK_LOCATION iostack = IoGetCurrentIrpStackLocation(pIrp);
		if(iostack->MajorFunction == IRP_MJ_WRITE)
		{
			InterlockedDecrement(&m_ActiveIrpWriteCount);
		}
		else if(iostack->MajorFunction = IRP_MJ_READ)
		{
			InterlockedDecrement(&m_ActiveIrpReadCount);
		}
		pIrp->IoStatus.Status = STATUS_DEVICE_BUSY;
		pIrp->IoStatus.Information = 0;
		IoCompleteRequest(pIrp,IO_NO_INCREMENT);
		pIrp = (PIRP)m_ActiveIrpQHead.DeQueueHead();
	}

	ReleaseActiveIrpListLock(oldIrql);
}

NTSTATUS CDiskDevice::SynchReadWriteLocalBlocks(
	ULONG		    MajorFunction,
	PVOID			pBuffer,
	PLARGE_INTEGER	pStartingOffset,
	ULONG			length)
{
	ASSERT(pBuffer);

	if(!pBuffer)
		return STATUS_INVALID_PARAMETER;

	if(GetPnpState() != StateStarted)
		return STATUS_DEVICE_OFF_LINE;

	ASSERT(MajorFunction==IRP_MJ_READ || MajorFunction==IRP_MJ_WRITE);

	NTSTATUS		status;
	KEVENT			event;
	IO_STATUS_BLOCK	ioStatusBlock;

	PIRP			pIrp;

	KeInitializeEvent(&event,NotificationEvent,FALSE);

	pIrp = IoBuildSynchronousFsdRequest(MajorFunction,
		GetTargetDeviceObject(),
		pBuffer,
		length,
		pStartingOffset,
		&event,
		&ioStatusBlock);
	if(!pIrp)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	status = IoCallDriver(GetTargetDeviceObject(), pIrp);

	if (status == STATUS_PENDING) 
	{ 
		KeWaitForSingleObject(&event, Suspended, KernelMode, FALSE, NULL); 
		return ioStatusBlock.Status;
	} 

	return status;
}

NTSTATUS CDiskDevice::OnDispatchInternalIoctl(PIRP pIrp)
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
		break;
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

void     CDiskDevice::SyncFilterWithTarget()
{
	#define FILTER_DEVICE_PROPOGATE_FLAGS            0
	#define FILTER_DEVICE_PROPOGATE_CHARACTERISTICS (FILE_REMOVABLE_MEDIA |  \
                                                 FILE_READ_ONLY_DEVICE | \
                                                 FILE_FLOPPY_DISKETTE    \
                                                 )

	ULONG                   propFlags;

    propFlags = GetNextLowerDevice()->Flags & FILTER_DEVICE_PROPOGATE_FLAGS;
    GetDeviceObject()->Flags |= propFlags;

    propFlags = GetNextLowerDevice()->Characteristics & FILTER_DEVICE_PROPOGATE_CHARACTERISTICS;
    GetDeviceObject()->Characteristics |= propFlags;
}

NTSTATUS CDiskDevice::StartNotification(PIRP pIrp)
{
	KdPrint(("OsnPA - enter StartNotification.\n"));

	SyncFilterWithTarget();

	//add here if you need to get the underlying disk level
        //
    ((COsnPADevice *) this)->QueryDeviceProperty();
	g_pGlobal->ResetCacheRelation(this);
	
	return STATUS_SUCCESS;
}

NTSTATUS CDiskDevice::StopNotification()
{
	KdPrint(("OsnPA - enter StopNotification.\n"));
	NTSTATUS status=STATUS_SUCCESS;
	

	return status;
}

NTSTATUS CDiskDevice::RemovalNotification()
{
	KdPrint(("OsnPA - enter RemovalNotification.\n"));

	//add here to perform removal cleanup
	//
	//

	

	//	
	CFido::RemovalNotification();
	return STATUS_SUCCESS;
}

NTSTATUS CDiskDevice::OnDispatchReadWrite(PIRP pIrp)
{
	PIO_STACK_LOCATION currentIrpStack = IoGetCurrentIrpStackLocation(pIrp);

	if(currentIrpStack->MajorFunction == IRP_MJ_READ && currentIrpStack->Parameters.Read.Length< BLOCK_SIZE)
	{
		return OsnCompleteIrp(pIrp, STATUS_SUCCESS);
	}

	if(currentIrpStack->MajorFunction == IRP_MJ_WRITE &&
		currentIrpStack->Parameters.Write.Length <BLOCK_SIZE)
	{
		return OsnCompleteIrp(pIrp, STATUS_SUCCESS);
	}

	if(GetPnpState() != StateStarted)
		return PassThrough(pIrp);

	COsnPADevice *pOsnPADevice = (COsnPADevice *)this;
	if(!pOsnPADevice->GetCacheFlag())
	{
		return PassThrough(pIrp);
	}

	if(pOsnPADevice->GetCacheRole() == CACHE_TARGET )
	{
		if(currentIrpStack->MajorFunction == IRP_MJ_WRITE)
			return OsnCompleteIrp(pIrp,STATUS_SUCCESS);
		else if(currentIrpStack->MajorFunction == IRP_MJ_READ)
			return PassThrough(pIrp);
	}

	NTSTATUS	status = pOsnPADevice->InsertApplicationIrp(pIrp);	
	if(status!=STATUS_PENDING)
	{
		KdPrint(("OsnPA - InsertApplicationCmd() returns status=0x%x\n",status));
		pIrp->IoStatus.Status = status;
		pIrp->IoStatus.Information=0;
		IoCompleteRequest(pIrp, IO_NO_INCREMENT);
		return status;
	}

	return status;

}

BOOLEAN  CDiskDevice::ProcessActiveIrps()
{
	BOOLEAN	workDone = FALSE;

	NTSTATUS	status;

	//loop over m_ActiveIrpQHead
	CActiveIrp	*pActiveIrp;
	CQueue      *pNextNode = NULL;
	CQueue		*pNode = m_ActiveIrpQHead.Next();
	while(pNode)
	{
		pActiveIrp = (CActiveIrp *) (pNode->GetItem());
		status = pActiveIrp->ProcessActiveIrp();

		if(status != STATUS_PENDING)
		{
			workDone = TRUE;
		}

		if(status == STATUS_MORE_PROCESSING_REQUIRED)
		{
			//it means this Irp has been completed (RemoveActiveIrp)
			if(pActiveIrp->GetReadOrWriteIrp())
				InterlockedDecrement(&m_ActiveIrpReadCount);
			else
				InterlockedDecrement(&m_ActiveIrpWriteCount);
			KdPrint(("OSNPA - DEL m_ActiveIrpReadCount = %x \n",m_ActiveIrpReadCount));
			KdPrint(("OSNPA  - DEL m_ActiveIrpWriteCount = %x \n",m_ActiveIrpWriteCount));
			KIRQL  irql = AcquireActiveIrpListLock();
			pNextNode = pNode->Next();
			delete pActiveIrp;
			pActiveIrp = NULL;
			pNode->Remove();
			delete pNode;
			ReleaseActiveIrpListLock(irql);

			pNode = pNextNode;
			this->SetHandleIrpThreadEvent();
			continue;
		}

		if(!NT_SUCCESS(status))
		{
			KdPrint(("OSNPA - active irp do failed,status= %x \n",status));
		}

		this->SetHandleIrpThreadEvent();
		pNode = pNode->Next();
	}

	return workDone;
}

VOID     CDiskDevice::InsertActiveIrpQTail(PIRP pIrp,ACTIVE_IRP_STATE state)
{
	BOOLEAN readOrWrite = FALSE;
	
    ASSERT(m_ActiveIrpReadCount >=0 && m_ActiveIrpWriteCount >=0);

	readOrWrite = ((state == STATE_READ_NEW)? TRUE:FALSE);
	if(readOrWrite)
		InterlockedIncrement(&m_ActiveIrpReadCount);
	else
		InterlockedIncrement(&m_ActiveIrpWriteCount);

	KdPrint(("OSNPA - INSERT m_ActiveIrpReadCount= %x \n",m_ActiveIrpReadCount));
	KdPrint(("OSNPA - INSERT m_ActiveIrpWriteCount = %x \n",m_ActiveIrpWriteCount));
	KdPrint(("OSNPA - INSERT m_Irp = %x \n",pIrp));

	CActiveIrp	*pActiveIrp = new CActiveIrp(pIrp, this, state,readOrWrite);
	pActiveIrp->SetBlockAndLength();

	KIRQL  irql = AcquireActiveIrpListLock();
	m_ActiveIrpQHead.InsertQTail(pActiveIrp);
	ReleaseActiveIrpListLock(irql);
}

BOOLEAN  CDiskDevice::OverlapWithActiveIrps(PIRP pIrp, BOOLEAN ReadOrWrite)
{
	CActiveIrp	*pActiveIrp;
	
	KIRQL  irql = AcquireActiveIrpListLock();
	CQueue		*pNode = m_ActiveIrpQHead.Next();
	while(pNode)
	{
		pActiveIrp = (CActiveIrp *) (pNode->GetItem());
		if(pActiveIrp->CheckOverlap(pIrp, ReadOrWrite)) // check irp is having overlap irp in the activeQueue or not having
		{
			ReleaseActiveIrpListLock(irql);
			return TRUE;
		}

		pNode = pNode->Next();
	}
	ReleaseActiveIrpListLock(irql);

	return FALSE;
}

BOOLEAN  CDiskDevice::FlushCheckOverlap(LONGLONG blockoffset,ULONG blockLength)
{
	CActiveIrp	*pActiveIrp;

	KIRQL  irql = AcquireActiveIrpListLock();
	CQueue		*pNode = m_ActiveIrpQHead.Next();
	while(pNode)
	{
		pActiveIrp = (CActiveIrp *) (pNode->GetItem());
		if(pActiveIrp->CheckOverlap(blockoffset, blockLength)) // check irp is having overlap irp in the activeQueue or not having
		{
			ReleaseActiveIrpListLock(irql);
			return TRUE;
		}

		pNode = pNode->Next();
	}
	ReleaseActiveIrpListLock(irql);

	return FALSE;
}


