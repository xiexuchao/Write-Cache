#ifndef _GLOBAL_H_
#define _GLOBAL_H_

#include "OSNBaseDevice.h"

#define DELAY_ONE_MICROSECOND      (-10)
#define DELAY_ONE_MILLISECOND      (DELAY_ONE_MICROSECOND*1000)
#define DELAY_ONE_SECOND           (DELAY_ONE_MILLISECOND*1000)

class CGlobal : public COSNBaseDevice
{
public:
	CGlobal(PDRIVER_OBJECT pDriverObject);

	virtual ~CGlobal()
	{
		ExDeleteNPagedLookasideList(&m_NonPagedLookasideList);
		COSNBaseDevice::~COSNBaseDevice();
	}

	NTSTATUS			Initialize(PUNICODE_STRING  pRegistryPath);

	PDEVICE_OBJECT	    GetGlobalDevice(){return GetDeviceObject();}

	NTSTATUS            InsertDeviceToList(COsnPADevice *pDevice);

	NTSTATUS            RemoveDeviceFromList(CQueue *pListNodese);

	BOOLEAN             DeviceRemoveRoutine(COsnPADevice *pOsnDevice);

	COsnPADevice *      GetDeviceFromList(GUID volumeID);

	COsnPADevice *      GetDeviceFromList(PUNICODE_STRING pVolumeName);

	NTSTATUS            InsertCacheInfoToList(PCACHE_INFO pCacheInfo);

	NTSTATUS            RemoveCacheInfoFromList(PCACHE_INFO pCacheInfo);

	PCACHE_INFO         GetCacheInfoFromList(VOLUMEID *pSourceVolumeID);

	PCACHE_INFO         GetCacheInfoFromList(GUID guid);

	PCACHE_INFO         CheckCacheInfoByCache(GUID guid);

	BOOLEAN             ChecCacheInfoBySource(GUID guid);
	
	PCACHE_INFO         GetCacheInfoFromListByCache(VOLUMEID *pCacheVolumeID);

	NTSTATUS            ResetCacheRelation(CDiskDevice *pdevice);

	NTSTATUS            ClearCache(COsnPADevice *pOsnDevice);

	NTSTATUS            QueryCacheRelationFromRegistry(WCHAR *pKeyName,WCHAR *pValue,WCHAR *pSubKey);

	NTSTATUS            DeleteCacheRelationFromRegistry(WCHAR *pSubKey,WCHAR *pValue);

	static NTSTATUS             QueryRegistryStringValue(
		PWSTR	ValueName, 
		ULONG	ValueType,
		PVOID	ValueData,
		ULONG	ValueLength,
		PVOID	Context,
		PVOID	EntryContext);

	friend VOID         OsnPAWorkerThread(PVOID  context);
	
	static NTSTATUS     OSNDeviceIoctl(
		PDEVICE_OBJECT		pTargetDevice, 
		ULONG				DeviceIoctlCode, 
		PVOID				pInputBuffer, 
		ULONG				inputBufferLength, 
		PVOID				pOutputBuffer,
		ULONG				outputBufferLength,
		PIO_STATUS_BLOCK	pIoStatusBlock);

	inline BOOLEAN      GetWorkerThreadStart()
	{
		return m_WorkerThreadStarted;
	}

	inline BOOLEAN      GetWorkerThreadStop()
	{
		return m_WorkerThreadStop;
	}

	inline PKEVENT      GetWorkerThreadEvent()
	{
		return &m_WorkerThreadEvent;
	}

	inline	KIRQL		AcquireDeviceListLock()
	{
		KIRQL	oldIrql;
		KeAcquireSpinLock(&m_OsnPADeviceListLock, &oldIrql);
		return oldIrql;
	}

	inline	void		ReleaseDeviceListLock(KIRQL oldIrql) 
	{
		KeReleaseSpinLock(&m_OsnPADeviceListLock, oldIrql);
	}
	
	inline	KIRQL		AcquireCacheInfoListLock()
	{
		KIRQL	oldIrql;
		KeAcquireSpinLock(&m_OsnPACacheInfoLock, &oldIrql);
		return oldIrql;
	}

	inline	void		ReleaseCacheInfoLock(KIRQL oldIrql) 
	{
		KeReleaseSpinLock(&m_OsnPACacheInfoLock, oldIrql);
	}

	inline CQueue *    GetOSNPADeviceListHeader()
	{
		return &m_OsnPADeviceListHeader;
	}

protected:
	virtual NTSTATUS	OnDispatchIoctl(PIRP pIrp);
	virtual NTSTATUS	OnDispatchInternalIoctl(PIRP pIrp);


private:
	CQueue         m_OsnPADeviceListHeader;

	ULONG          m_DeviceCount;

	KSPIN_LOCK     m_OsnPADeviceListLock;

	CQueue         m_OsnPACacheInfoListHeader;

	ULONG         m_CacheCount;

	KSPIN_LOCK     m_OsnPACacheInfoLock;

	KEVENT         m_WorkerThreadEvent;

	HANDLE         m_WorkerThreadHandle;

	BOOLEAN        m_WorkerThreadStarted;

	BOOLEAN        m_WorkerThreadStop;

	UNICODE_STRING m_RegistryPath;

	NPAGED_LOOKASIDE_LIST       m_NonPagedLookasideList;
};


#endif
