#ifndef  _ACTIVE_IRP_
#define  _ACTIVE_IRP_

class CActiveIrp :public CNPAllocator
{
public:
	CActiveIrp(PIRP pIrp,CDiskDevice *pSourceVolume,ACTIVE_IRP_STATE state,BOOLEAN readOrWrite);
	~CActiveIrp();

	BOOLEAN                   CheckOverlap(PIRP  pIrp,BOOLEAN ReadOrWrite);

	BOOLEAN                   CheckOverlap(LONGLONG blockOffset,ULONG blockLength);

	NTSTATUS                  ProcessActiveIrp();

	NTSTATUS                  ProcessWriteIrp();

	NTSTATUS                  DeviceOFFline(ACTIVE_IRP_STATE state);

	NTSTATUS                  ReadProcess(LARGE_INTEGER tempOffset,ULONG   tempLength,ULONG relLength);

	NTSTATUS                  WriteProcess(LARGE_INTEGER tempOffset,ULONG   tempLength,ULONG relLength,BOOLEAN first);

	NTSTATUS                  ProcessReadIrp();

	NTSTATUS                  ReadFromCacheOrOrig(ULONG blockNum,LONGLONG tempOffset,ULONG tempLength,ULONG relLength);

	NTSTATUS                  WriteToCache(PIRP pIrp,LARGE_INTEGER offset,ULONG length,ULONG RelLengt);

	NTSTATUS                  WriteToOriginal(PIRP pIrp,LARGE_INTEGER offset,ULONG length, ULONG RelLength);

	NTSTATUS                  ReadFromOrig(LONGLONG offset,ULONG length);

	NTSTATUS                  ReadFormCahce(LONGLONG offset,ULONG length,ULONG relLength);

	NTSTATUS				  QueryReadStatus();

	NTSTATUS                  QueryWriteStatus();

	NTSTATUS                  CompeleteWriteIrp();

	NTSTATUS                  RecoveryBitmap();

	NTSTATUS                  SetBlockAndLength();

	static NTSTATUS           ReadCompletionRoutine(PDEVICE_OBJECT	pDeviceObject, 
		PIRP            pIrp, 
		PVOID           pContext);

	static NTSTATUS           WriteCacheCompletionRoutine(PDEVICE_OBJECT	pDeviceObject, 
		PIRP            pIrp, 
		PVOID           pContext);

	inline  VOID              SetActiveState(ACTIVE_IRP_STATE state)
	{
		m_ActiveState = state;
	}

	inline  BOOLEAN           GetReadOrWriteIrp()
	{
		return m_ReadOrWrite;
	}

	inline VOID              SetInfomation(ULONG info)
	{
		m_Information += info;
	}

	inline ULONG             GetInfomation()
	{
		return m_Information;
	}

	inline LONG              IncrementSubIrpCount()
	{
		return InterlockedIncrement(&m_SubIoCount);
	}

	inline	LONG		    DecrementSubIrpCount()
	{
		return InterlockedDecrement(&m_SubIoCount);
	}

	inline	void		    SetStatus(NTSTATUS status) { m_Status = status; }

	inline  NTSTATUS          GetStatus() { return m_Status ;}

	inline  PVOID           GetSourceDiskDevice()
	{
		return m_pSourceVolume;
	}

private:

	PIRP              m_pIrp;

	CDiskDevice       *m_pSourceVolume;

	ACTIVE_IRP_STATE  m_ActiveState;

	BOOLEAN           m_ReadOrWrite;

	ULONG             m_Information ;

	LONG              m_SubIoCount;

	NTSTATUS	      m_Status;

	LONGLONG          m_BlockOffset;

	ULONG             m_BlockLength;

	BOOLEAN           m_SendToLocal;

	BITMAP_CLR        m_BitMapClr;

	ULONG             m_BlockNum;

	LONG              m_CompeleteTimes;
};


#endif  _ACTIVE_IRP_