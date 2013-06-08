#ifndef  _DISK_CACHE_VOLUME_
#define  _DISK_CAHCE_VOLUME_

class CDiskCache:  public CNPAllocator
{
public:
	CDiskCache();
	~CDiskCache();

	NTSTATUS      Initialize(VOLUMEID *pSourceVolumeId,VOLUMEID * pCacheVolumeId,ULONG unit,BOOLEAN newConfigs);
	
	NTSTATUS      InitBatMap(BOOLEAN newConfig);

	NTSTATUS      ReadBatMapFromMem(ULONG num,ULONG &ret);

	NTSTATUS      FreeBatMap();

	NTSTATUS      InitBlockUseBitMap(unsigned char * address,ULONG size);

	VOID          InsertBitmapToList(PVOID item);

	VOID          RemoveLastBitmapFromList();

	NTSTATUS      RemoveBitmapFromList();

	GET_BLOCK_STATE       GetBlockInCache(LONGLONG &offset,ULONG &length,BOOLEAN readOrWrite,PBITMAP_SEG bitmapSec);

	NTSTATUS      CheckBatMap(LONGLONG offset,ULONG length,ULONG *blockNum,BOOLEAN readOrWrite);

	NTSTATUS      CheckBitMap(LONGLONG &offset,ULONG &length,ULONG blockNum,PBITMAP_SEG bitmapSec);

	ULONG         GetBitMapNextUnused();

	VOID          ClearBatMapUsed(ULONG blockNum);

	NTSTATUS      NewBlock(ULONG num,ULONG blocknum);

	NTSTATUS       NewBitmap(ULONG blockNum,BOOLEAN newconfig);

	ULONG         GetBitmapByte(ULONGLONG offset);

	CHAR          GetBitmapBit(ULONGLONG offset);

	PVOID         GetBlockBitmap(ULONG blockNum);

	ULONG         GetNextFlushBlockNum(ULONG &blockNum);

	NTSTATUS      GetBlockBitMap(ULONG blockNum ,PBITMAP pBitMap);

	NTSTATUS      ReadAndWriteNewBat(ULONG sourceBlock,ULONG block);

	VOID          DeleteBitMapFromMem(ULONG blockNum);

	NTSTATUS      RecoveryBitMap(ULONGLONG tempOffset,ULONG tempLength,PBITMAP_SEG pBitMapSeg);


	inline	KIRQL		AcquireBitmapListLock()
	{
		KIRQL	oldIrql;
		KeAcquireSpinLock(&m_BitmapListLock, &oldIrql);
		return oldIrql;
	}

	inline	void		ReleaseBitmapLock(KIRQL oldIrql) 
	{
		KeReleaseSpinLock(&m_BitmapListLock, oldIrql);
	}

	inline  POSNDSKHD   GetOsnDskHD()
	{
		return m_pOsnDskHD;
	}

	inline POSNDSKCH    GetOsnDskCH()
	{
		return m_pOsnDskCh;
	}

	inline LONG              IncrementTotalBlockHaveCount()
	{
		return InterlockedIncrement(&m_TotalBlockHaveUsed);
	}

	inline	LONG		    DecrementTotalBlockHaveCount()
	{
		return InterlockedDecrement(&m_TotalBlockHaveUsed);
	}


private:

	POSNDSKHD      m_pOsnDskHD;
	POSNDSKCH      m_pOsnDskCh;

	ULONG          m_TotalBatCount;
	ULONG          m_BatBlockSize;
	PBAT_BLOCK     m_pBatBlock;

	PBLOCK_USE_BITMAP  m_BlockUseBitmap;

	ULONG          m_BitmapInMemCount;                // the count of bitmap in memory
	CQueue         m_BitmapListHeader;               //the list for bitmap  of block, make it storage in memory
	KSPIN_LOCK     m_BitmapListLock;                //the bitmap list lock

	CDiskDevice   *m_pSourceDevice;                 //source object
	CDiskDevice   *m_pCacheDevice;                  //cache object

	VOLUMEID       m_SourceVolumeID;               //source volume id
	VOLUMEID       m_CacheVolumeID;                //cache volume id

	LONG           m_TotalBlockHaveUsed;          //the num of the cache block used
};


#endif  _DISK_CAHCE_VOLUME_