#ifndef _OSN_PA_DEVICE_
#define _OSN_PA_DEVICE_

class COsnPADevice: public CDiskDevice
{
public:
	COsnPADevice();

	COsnPADevice(VOLUME_TYPE volumeType);

	~COsnPADevice();

	NTSTATUS                  QueryDeviceName();

	VOID                      QueryDeviceProperty();

	ULONGLONG                 InquireDeviceBlockSize();

	NTSTATUS                  SetCacheProtect(COsnPADevice *pPACacheDevice,ULONG unit,BOOLEAN newConfig);

	friend  void              OSNPAHandleIrpWorkThread(const PVOID pContext);

	BOOLEAN                   ProcessIrp();

	NTSTATUS                  InitializeHandleIrpThread();

	NTSTATUS                  StopHandleIrpThread();

	NTSTATUS                  ProcessWriteIrp(PIRP pIrp);

	NTSTATUS                  ProcessReadIrp(PIRP pIrp);

	NTSTATUS                  DoFlush();

	VOID                      FreeCacheMem();

	VOID                      GetFlushInfoFromBitMap(ULONG &bitCount, ULONG &emptyCount);

	VOID                      UpdateFlushBitmap(ULONG flushBitCount);

	NTSTATUS                  ReadFromCache(LONGLONG blockOffset,ULONG blockLength,ULONG index);

	NTSTATUS                  FlushWrite(ULONG index);

	VOID                      ResetFlushIndex(ULONG byte,ULONG bit);

	NTSTATUS                  FlushBlockFinish();

	BOOLEAN                   CheckFlushOverlap(LONGLONG blockOffset,ULONG blockLength);

	VOID                      ClearCacheInfo();

	VOID                      ReSetDeviceInfo();

	VOID                      PrepareForFlush();

	static      NTSTATUS      FlushReadCompletionRoutine(PDEVICE_OBJECT	pDeviceObject, 
		PIRP            pIrp, 
		PVOID           pContext);

	static     NTSTATUS       FlusWriteCompletionRoutine(PDEVICE_OBJECT	pDeviceObject, 
		PIRP            pIrp, 
		PVOID           pContext);

	inline     BOOLEAN        GetCacheFlag()
	{
		return m_CacheFlag;
	}

	inline     CACHE_ROLE     GetCacheRole()
	{
		return m_CacheRole;
	}

	inline     VOID           SetCacheFlag(BOOLEAN  flag)
	{
		m_CacheFlag = flag;
	}

	inline     VOID           SetCacheRole(CACHE_ROLE role)
	{
		m_CacheRole = role;
	}

	inline     GUID           GetGUID()
	{
		return    m_VolumeGuid;
	}

	inline     GUID*          GetVolumeGUID()
	{
		return    &m_VolumeGuid;
	}

	inline    PUNICODE_STRING  GetVolumeName()
	{
		return    &m_VolumeName;
	}

	inline	  KIRQL		      AcquireCacheLock()
	{
		KIRQL	oldIrql;
		KeAcquireSpinLock(&m_OsnPACacheSpinLock, &oldIrql);
		return oldIrql;
	}

	inline	  VOID	       	  ReleaseCacheLock(KIRQL oldIrql) 
	{
		KeReleaseSpinLock(&m_OsnPACacheSpinLock, oldIrql);
	}

	inline    BOOLEAN         GetHandleIrpThreadStart()
	{
		return m_HandleIrpThreadStart;
	}

	inline    BOOLEAN         GethandleIrpThreadStop()
	{
		return m_HandleIrpThreadStop;
	}

	inline   VOID             SetIsHandleIrpThreadDie(BOOLEAN flag)
	{
		m_IsHandleIrpThreadDie = flag;
	}

	inline  PKEVENT           GetHandIrpEvent()
	{
		return &m_HandleIrpEvent;
	}

	inline  VOID              SetRemoveCacheFlag(BOOLEAN flag)
	{
		m_RemoveCacheFlag = flag;
	}

	inline  BOOLEAN           GetRemoveCacheFlag()
	{
		return m_RemoveCacheFlag;
	}

	inline  VOID              SetCacheState(CACHE_STATE state)
	{
		m_CacheState = state;
	}

	inline CACHE_STATE        GetCacheState()
	{
		return m_CacheState;
	}

	inline  VOID              SetCachePreState(CACHE_STATE state)
	{
		m_CachePreState = state;
	}

	inline CACHE_STATE        GetCachePreState()
	{
		return m_CachePreState;
	}

	inline CDiskCache*        GetDiskCache()
	{
		return m_pCacheVolume;
	}
	
	inline PVOID             GetCacheDevice()
	{
		return m_pPACacheDevice;
	}

	inline BOOLEAN          GetFlushCacheFlag()
	{
		return m_FlushCache;
	}

	inline VOID             SetFlushCacheFlag(BOOLEAN  flag)
	{
		m_FlushCache = flag;
	}

	inline VOID             SetReloadFlag(BOOLEAN flag)
	{
		m_ReloadSuccess  = flag;
	}

	inline BOOLEAN          GetReloadFlag()
	{
		return m_ReloadSuccess;
	}

	inline	  KIRQL		      AcquireCacheDeviceSpinLock()
	{
		KIRQL	oldIrql;
		KeAcquireSpinLock(&m_CacheDeviceSpinLock, &oldIrql);
		return oldIrql;
	}

	inline	  VOID	       	  ReleaseCacheDeviceLock(KIRQL oldIrql) 
	{
		KeReleaseSpinLock(&m_CacheDeviceSpinLock, oldIrql);
	}

	inline   LONG             IncrementFlushIrpCount()
	{
		return InterlockedIncrement(&m_FlushIrpOutStandingCount);
	}

	inline   LONG             DecrementFlushIrpCount()
	{
		return InterlockedDecrement(&m_FlushIrpOutStandingCount);
	}

	inline   KIRQL            AcquireFlushSpinLock()
	{
		KIRQL	oldIrql;
		KeAcquireSpinLock(&m_FlushSpinlock, &oldIrql);
		return oldIrql;
	}

	inline  VOID              ReleaseFlushSpinlock(KIRQL oldIrql)
	{
		KeReleaseSpinLock(&m_FlushSpinlock, oldIrql);
	}

	inline   VOID             SetFlushWriteFailed(BOOLEAN flag)
	{
		m_FlushWriteFailed  = flag;
	}

	inline  ULONG            GetFlushFinishBit()
	{
		return m_FlushFinishBit;
	}

	inline  VOID            UpdateFinishBit(ULONG bitcount)
	{
		m_FlushFinishBit = bitcount;
	}

	inline  LONG           GetFlushIrpCount()
	{
		return m_FlushIrpOutStandingCount;
	}

	inline  VOID           SetFlushNext(BOOLEAN flag)
	{
		m_FlushNext = flag;
	}

	inline BOOLEAN        GetFlushCache()
	{
		return m_FlushCache;
	}

	inline BOOLEAN         GetFlushSuccess()
	{
		return m_FlushSuccess;
	}

	inline BOOLEAN         GetCacheForceRemoveFlag()
	{
		return m_ForceRemoveCacheFlag;
	}

	inline  VOID           SetCacheForceRemoveFlag(BOOLEAN flag)
	{
		m_ForceRemoveCacheFlag = flag;
	}

	inline PIRP            GetFlushIrp(ULONG index)
	{
		return m_FlushInfo[index].m_pFlushIrp;
	}

public:
	    CDiskDevice *          m_pPACacheDevice;
		BOOLEAN                m_CacheDevicePointerUseFlag;

private:

	GUID                   m_VolumeGuid;
	UNICODE_STRING         m_VolumeName;
	WCHAR                  m_VolumeNameBuffer[MAX_DEVICE_NAME_LENGTH];

	BOOLEAN                m_CacheFlag;
	CACHE_ROLE             m_CacheRole;
	BOOLEAN                m_RemoveCacheFlag;
	BOOLEAN                m_ForceRemoveCacheFlag;
	CACHE_STATE            m_CacheState;
	CACHE_STATE            m_CachePreState;

	KSPIN_LOCK             m_OsnPACacheSpinLock;

	CDiskCache             *m_pCacheVolume;

	HANDLE                  m_ThreadHandle;
	BOOLEAN                 m_HandleIrpThreadStart;
	BOOLEAN                 m_HandleIrpThreadStop;
	BOOLEAN                 m_IsHandleIrpThreadDie;

	BOOLEAN                 m_ReloadSuccess;

	KSPIN_LOCK              m_CacheDeviceSpinLock;

	BOOLEAN                 m_FlushCache;
	BOOLEAN                 m_FlushSuccess;
	BOOLEAN                 m_FlushNext;
	ULONG                   m_FlushBlockNum; 
	ULONG                   m_FlushSourceBlockNum;
	PBITMAP                 m_pBlockBitmap;
	FLUSH_INFO              m_FlushInfo[OSNMAX_FLUSH_IO_COUNT];
	LONG                    m_FlushIrpOutStandingCount;
	ULONG                   m_FlushBitMapByte;
	ULONG                   m_FlushBitMapBit;
	BOOLEAN                 m_FlushWriteFailed;
	KSPIN_LOCK              m_FlushSpinlock;
	ULONG                   m_FlushFinishBit;
};

#endif _OSN_PA_DEVICE_