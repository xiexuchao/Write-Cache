
#ifndef _DISKDEVICE_H_
#define _DISKDEVICE_H_

#include "OSNddk.h"


#include <initguid.h>
#include <wdmguid.h>
#include <wmistr.h>
#include <wmilib.h>
#include <mountdev.h>
#include <ntddvol.h>

#include "OSNFido.h"


typedef enum VOLUME_TYPE
{
	VOLUME_TYPE_VM,
	VOLUME_TYPE_DM,
	VOLUME_TYPE_TVM,
	VOLUME_TYPE_UNDEFINED
};


class CDiskDevice: public CFido
{
public:

	
	WCHAR					m_StorageManagerName[8];

	//
	//this will remember all the snaps the device is associated
	//
	LARGE_INTEGER			m_size;

	VOLUME_TYPE             m_VolumeType;

	ULONGLONG               m_SizeInBlocks;

	KSPIN_LOCK					m_ApplicationIrpQLock;
	CQueue						m_ApplicationIrpQHead;	
	LONG						m_AppIrpCount;

	KSPIN_LOCK                  m_ActiveIrpQlock;
	CQueue                      m_ActiveIrpQHead;
	LONG                        m_ActiveIrpReadCount;
	LONG                        m_ActiveIrpWriteCount;

	KEVENT                       m_HandleIrpEvent;

public:

	CDiskDevice();

	CDiskDevice(VOLUME_TYPE volumeType);

	~CDiskDevice();

	NTSTATUS		            InsertApplicationIrp(PIRP			pIrp);

	PIRP						GetFirstApplicationIrp();

	PIRP						RemoveFirstApplicationIrp();

	VOID                        ClearAppCmdQueue();

	VOID                        ClearActiveQueue();

	BOOLEAN                     ProcessActiveIrps();

	VOID                        InsertActiveIrpQTail(PIRP pIrp,ACTIVE_IRP_STATE state);

	BOOLEAN                     OverlapWithActiveIrps(PIRP pIrp, BOOLEAN ReadOrWrite);


	NTSTATUS	     SynchReadWriteLocalBlocks(
		ULONG		    MajorFunction,
		PVOID			pBuffer,
		PLARGE_INTEGER	pStartingOffset,
		ULONG			length);

	BOOLEAN  FlushCheckOverlap(LONGLONG blockoffset,ULONG blockLength);

	inline    VOID            SetHandleIrpThreadEvent()
	{
		ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
	//	KdPrint(("OSNPA - m_HandleIrpThreadEvent = %x\n",&m_HandleIrpEvent));
		KeSetEvent( &m_HandleIrpEvent, 0, FALSE );
	}

	inline  PDEVICE_OBJECT        GetTargetDeviceObject()
	{
		return m_pNextLowerDevice;
	}

	inline  ULONGLONG             GetSizeInBlocks()
	{
		return m_SizeInBlocks;
	}

	inline  KIRQL			      AcquireCmdListLock()
	{
		KIRQL	oldIrql;
		KeAcquireSpinLock(&m_ApplicationIrpQLock, &oldIrql);
		return oldIrql;
	}

	inline  void				  ReleaseCmdListLock(KIRQL oldIrql) 
	{
		KeReleaseSpinLock(&m_ApplicationIrpQLock, oldIrql);
	}

	inline  KIRQL			      AcquireActiveIrpListLock()
	{
		KIRQL	oldIrql;
		KeAcquireSpinLock(&m_ActiveIrpQlock, &oldIrql);
		return oldIrql;
	}

	inline  void				  ReleaseActiveIrpListLock(KIRQL oldIrql) 
	{
		KeReleaseSpinLock(&m_ActiveIrpQlock, oldIrql);
	}

	inline  CQueue*               GetApplicationIrpQHead()
	{
		return &m_ApplicationIrpQHead;
	}

	inline LONG                  IncrementAppCmdCount()
	{
		return InterlockedIncrement(&m_AppIrpCount);
	}

	inline	LONG		         DecrementAppCmdCount()
	{
		return InterlockedDecrement(&m_AppIrpCount);
	} 

	inline  LONG                 GetAppCmdCount()
	{
		return m_AppIrpCount;
	}

	inline  LONG                 GetAppReadCmdCount()
	{
		return m_ActiveIrpReadCount;
	}

	inline  LONG                 GetAppWriteCmdCount()
	{
		return m_ActiveIrpWriteCount;
	}


protected: 
	virtual NTSTATUS	OnDispatchReadWrite(PIRP pIrp);
	virtual	NTSTATUS	OnDispatchInternalIoctl(PIRP pIrp);

	virtual NTSTATUS	StartNotification(PIRP pIrp);
	
	virtual NTSTATUS	StopNotification();

	virtual NTSTATUS	RemovalNotification();

	BOOLEAN				ValidateIrp(PIRP pIrp);

private:

	void		SyncFilterWithTarget();

	NTSTATUS	GetTargetDeviceSize();

	NTSTATUS	AssociateCfgVolume();


friend NTSTATUS AddDevice(IN PDRIVER_OBJECT DriverObject, IN PDEVICE_OBJECT pPhysicalDeviceObject);
friend class CGlobal;
};

#endif

