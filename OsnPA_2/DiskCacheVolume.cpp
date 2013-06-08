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

CDiskCache::CDiskCache()
{
	m_pOsnDskHD = NULL;
	m_pOsnDskCh = NULL;
	m_pSourceDevice = NULL;
	m_pCacheDevice  = NULL;
	m_TotalBatCount = 0;
	m_BatBlockSize  = OSNBATMAP_BLOCK_SIZE;
	m_BlockUseBitmap  = NULL;
	m_BitmapInMemCount = 0;
	m_pBatBlock        = NULL;
	m_TotalBlockHaveUsed = 0;

	KeInitializeSpinLock(&m_BitmapListLock);
}

CDiskCache::~CDiskCache()
{
	if(m_pOsnDskHD)
	{
		ExFreePool(m_pOsnDskHD);
		m_pOsnDskHD = NULL;
	}
	if(m_pOsnDskCh)
	{
		ExFreePool(m_pOsnDskCh);
		m_pOsnDskCh = NULL;
	}

	FreeBatMap();

	if(m_BlockUseBitmap)
	{
		ExFreePool(m_BlockUseBitmap);
		m_BlockUseBitmap = NULL;
	}
}

/*++
par unit is save as blocks;
--*/
NTSTATUS CDiskCache::Initialize(VOLUMEID *pSourceVolumeId,VOLUMEID * pCacheVolumeId,ULONG unit,BOOLEAN newConfig)
{
	const ULONG hdmagicsize = sizeof(OSNDSKHD_MAGIC); 
	const ULONG chmagicsize = sizeof(OSNDSKCH_MAGIC);
	CHAR  magic[hdmagicsize]  = OSNDSKHD_MAGIC;
	CHAR  chmagic[chmagicsize] = OSNDSKCH_MAGIC;
	NTSTATUS status;
	LARGE_INTEGER offset;
	ULONG    BlockUseTotal = 0;

	if(pSourceVolumeId == NULL || pCacheVolumeId == NULL)
	{
		return STATUS_UNSUCCESSFUL;
	}

	m_pCacheDevice = (CDiskDevice *)g_pGlobal->GetDeviceFromList(pCacheVolumeId->SAN_VolumeID.m_VolumeGuid);
	m_pSourceDevice = (CDiskDevice *)g_pGlobal->GetDeviceFromList(pSourceVolumeId->SAN_VolumeID.m_VolumeGuid);
	if(!m_pCacheDevice || !m_pSourceDevice)
	{
		return STATUS_UNSUCCESSFUL;
	}

	RtlCopyMemory(&m_SourceVolumeID,pSourceVolumeId,sizeof(VOLUMEID));
	RtlCopyMemory(&m_CacheVolumeID,pCacheVolumeId,sizeof(VOLUMEID));

	m_pOsnDskHD = (POSNDSKHD)ExAllocatePoolWithTag(NonPagedPool,sizeof(OSNDSKHD),OSNTAGPA);
	m_pOsnDskCh = (POSNDSKCH)ExAllocatePoolWithTag(NonPagedPool,sizeof(OSNDSKCH),OSNTAGPA);
	if(!m_pOsnDskHD || !m_pOsnDskCh)
	{
		return STATUS_UNSUCCESSFUL;
	}

	RtlZeroMemory(m_pOsnDskHD,sizeof(OSNDSKHD));
	RtlZeroMemory(m_pOsnDskCh,sizeof(OSNDSKCH));

	if(!newConfig)
	{
		offset.QuadPart = OSNDSKHD_OFFSET;
		offset.QuadPart *= BLOCK_SIZE;
		status = m_pCacheDevice->SynchReadWriteLocalBlocks(
			IRP_MJ_READ,
			(PVOID)m_pOsnDskHD,
			&offset,
			OSNDSKHD_SIZE);
		if(!NT_SUCCESS(status))
		{
			ExFreePool(m_pOsnDskHD);
			ExFreePool(m_pOsnDskCh);
			m_pOsnDskHD = NULL;
			m_pOsnDskCh = NULL;
			KdPrint(("OSNPA - read dskhd failed ,status = %x\n",status));
			return status;
		}

		if(RtlCompareMemory(&m_pOsnDskHD->Header.Magic,magic,sizeof(m_pOsnDskHD->Header.Magic)) != sizeof(m_pOsnDskHD->Header.Magic))
		{
			ExFreePool(m_pOsnDskHD);
			ExFreePool(m_pOsnDskCh);
			m_pOsnDskHD = NULL;
			m_pOsnDskCh = NULL;
			KdPrint(("OSNPA - DSHHD IS CHANGED \n"));
			return STATUS_UNSUCCESSFUL;
		}

		offset.QuadPart = OSNDSKCH_OFFSET;
		offset.QuadPart *= BLOCK_SIZE;
		status = m_pCacheDevice->SynchReadWriteLocalBlocks(
			IRP_MJ_READ,
			(PVOID)m_pOsnDskCh,
			&offset,
			OSNDSKCH_SIZE);
		if(!NT_SUCCESS(status))
		{
			ExFreePool(m_pOsnDskHD);
			ExFreePool(m_pOsnDskCh);
			m_pOsnDskHD = NULL;
			m_pOsnDskCh = NULL;
			KdPrint(("OSNPA - read dskch failed ,status = %x\n",status));
			return status;
		}
		if(RtlCompareMemory(&m_pOsnDskCh->Cache.Magic,chmagic,sizeof(m_pOsnDskCh->Cache.Magic)) != sizeof(m_pOsnDskCh->Cache.Magic))
		{
			ExFreePool(m_pOsnDskHD);
			ExFreePool(m_pOsnDskCh);
			m_pOsnDskHD = NULL;
			m_pOsnDskCh = NULL;
			KdPrint(("OSNPA - DSHCH IS CHANGED \n"));
			return STATUS_UNSUCCESSFUL;
		}
	}
	else
	{
		ULONG  unitsize            = unit;
	    ULONGLONG  BatTotalSize        = 0;
		ULONG      index               = 1;
		if(unitsize > OSNBLOCK_MAX_DATA_SIZE)
			unitsize = OSNBLOCK_MAX_DATA_SIZE;

		RtlCopyMemory(&m_pOsnDskHD->Header.Magic,magic,sizeof(m_pOsnDskHD->Header.Magic));
		m_pOsnDskHD->Header.Version = OSNDSKHD_VERSION;
		m_pOsnDskHD->Header.Type    = OSNDSKHD_TYPE;
		m_pOsnDskHD->Header.SourceId      = pSourceVolumeId->SAN_VolumeID.m_VolumeGuid;
		m_pOsnDskHD->Header.CacheId       = pCacheVolumeId->SAN_VolumeID.m_VolumeGuid;
		m_pOsnDskHD->Header.CacheSize     = m_pCacheDevice->GetSizeInBlocks()*BLOCK_SIZE;
		m_pOsnDskHD->Header.SourceSize    = m_pSourceDevice->GetSizeInBlocks()*BLOCK_SIZE;

		RtlCopyMemory(&m_pOsnDskCh->Cache.Magic,chmagic,sizeof(m_pOsnDskCh->Cache.Magic));
		m_pOsnDskCh->Cache.Version     = OSNDSKCH_VERSION;
		BatTotalSize                   = m_pSourceDevice->GetSizeInBlocks()*BLOCK_SIZE/unitsize;
		BatTotalSize                  *= OSNBAT_PER_UNIT;
		while(BatTotalSize > index*OSNBAT_MIN_SIZE)
		{
			index++;
		}
		BatTotalSize = index*OSNBAT_MIN_SIZE;
		m_pOsnDskCh->Cache.DskChSize   = BatTotalSize;
		m_pOsnDskCh->Cache.BlockSize   = unitsize;
		m_pOsnDskCh->Cache.BlockHdSize = unitsize/(BIT_PER_BYTE*BLOCK_SIZE);
		m_pOsnDskCh->Cache.StartingOffset = m_pOsnDskCh->Cache.DskChSize + OSNDSKHD_SIZE+OSNDSKCH_SIZE;
		m_pOsnDskCh->Cache.NumberBlocks   = (m_pOsnDskHD->Header.CacheSize-m_pOsnDskCh->Cache.StartingOffset)/ \
			(m_pOsnDskCh->Cache.BlockSize+m_pOsnDskCh->Cache.BlockHdSize);
		//if last block is satisfy with BlockSize ,we don't user it;
		//if((m_pOsnDskHD->Header.CacheSize-m_pOsnDskCh->Cache.StartingOffset)%m_pOsnDskCh->Cache.BlockSize !=0)
		//{
		//	m_pOsnDskCh->Cache.NumberBlocks++;
		//}

		offset.QuadPart = OSNDSKHD_OFFSET;
		offset.QuadPart *= BLOCK_SIZE;
		status = m_pCacheDevice->SynchReadWriteLocalBlocks(
			IRP_MJ_WRITE,
			(PVOID)m_pOsnDskHD,
			&offset,
			OSNDSKHD_SIZE);
		if(!NT_SUCCESS(status))
		{
			ExFreePool(m_pOsnDskHD);
			ExFreePool(m_pOsnDskCh);
			m_pOsnDskHD = NULL;
			m_pOsnDskCh = NULL;
			KdPrint(("OSNPA - write dskhd failed ,status = %x\n",status));
			return status;
		}

		offset.QuadPart = OSNDSKCH_OFFSET;
		offset.QuadPart *= BLOCK_SIZE;
		status = m_pCacheDevice->SynchReadWriteLocalBlocks(
			IRP_MJ_WRITE,
			(PVOID)m_pOsnDskCh,
			&offset,
			OSNDSKCH_SIZE);
		if(!NT_SUCCESS(status))
		{
			ExFreePool(m_pOsnDskHD);
			ExFreePool(m_pOsnDskCh);
			m_pOsnDskHD = NULL;
			m_pOsnDskCh = NULL;
			KdPrint(("OSNPA - write dskch failed ,status = %x\n",status));
			return status;
		}
	}

	BlockUseTotal = (ULONG)(m_pOsnDskCh->Cache.NumberBlocks /BIT_PER_BYTE)+1;
	if(m_pOsnDskCh->Cache.NumberBlocks % BIT_PER_BYTE)
		BlockUseTotal++;
	m_BlockUseBitmap = (PBLOCK_USE_BITMAP)ExAllocatePoolWithTag(NonPagedPool,sizeof(BLOCK_USE_BITMAP)+BlockUseTotal,OSNTAGPA);
	if(!m_BlockUseBitmap)
	{
		KdPrint(("OSNPA - FAILED TO ALLOC USE BITMPA .\n"));
		ExFreePool(m_pOsnDskHD);
		ExFreePool(m_pOsnDskCh);
		m_pOsnDskHD = NULL;
		m_pOsnDskCh = NULL;
		return STATUS_UNSUCCESSFUL;
	}
	RtlZeroMemory(m_BlockUseBitmap,sizeof(BLOCK_USE_BITMAP)+BlockUseTotal);

	status = InitBatMap(newConfig);
	if(!NT_SUCCESS(status))
	{
		ExFreePool(m_pOsnDskHD);
		ExFreePool(m_pOsnDskCh);
		m_pOsnDskHD = NULL;
		m_pOsnDskCh = NULL;
		KdPrint(("OSNPA - FAILED TO INIT BATMAP ,status = %x\n",status));
		return status;
	}

	return STATUS_SUCCESS;
}

NTSTATUS  CDiskCache::InitBatMap(BOOLEAN newConfig)
{
	LARGE_INTEGER   offset;
	NTSTATUS        status;
	ULONG           index = 0;

	if(!m_pOsnDskCh || !m_pOsnDskHD)
	{
		return STATUS_UNSUCCESSFUL;
	}

	if(!m_pCacheDevice || !m_pSourceDevice)
	{
		return STATUS_UNSUCCESSFUL;
	}

	m_TotalBatCount = (ULONG)(m_pOsnDskCh->Cache.DskChSize/OSNBATMAP_BLOCK_SIZE);


	m_pBatBlock = (PBAT_BLOCK)ExAllocatePoolWithTag(NonPagedPool,OSNBATMAP_BLOCK_SIZE+sizeof(BAT_BLOCK),OSNTAGPA);
	if(!m_pBatBlock)
	{
		KdPrint(("OSNPA - Failed to alloc paged pool for bat block index =%x\n",index));
		return STATUS_UNSUCCESSFUL;
	}

	RtlZeroMemory(m_pBatBlock,OSNBATMAP_BLOCK_SIZE+sizeof(BAT_BLOCK));


	if(!newConfig)
	{
		for(index = 0 ;index < m_TotalBatCount;index++)
		{
			offset.QuadPart = index*OSNBATMAP_BLOCK_SIZE+OSNDSKHD_SIZE+OSNDSKCH_SIZE;
			status = m_pCacheDevice->SynchReadWriteLocalBlocks(
				IRP_MJ_READ,
				m_pBatBlock->Address,
				&offset,
				OSNBATMAP_BLOCK_SIZE);
			if(!NT_SUCCESS(status))
			{
				KdPrint(("OSNPA - FAILED TO READ BATMAP ,status = %x\n",status));
				FreeBatMap();
				return status;
			}
			else
			{
				m_pBatBlock->CurNum = index;
				InitBlockUseBitMap(m_pBatBlock->Address,OSNBATMAP_BLOCK_SIZE);
			}
		}
	}
	else
	{
		for(index = 0;index<m_TotalBatCount;index++)
		{
			offset.QuadPart = index*OSNBATMAP_BLOCK_SIZE+OSNDSKHD_SIZE+OSNDSKCH_SIZE;
			status = m_pCacheDevice->SynchReadWriteLocalBlocks(
				IRP_MJ_WRITE,
				m_pBatBlock->Address,
				&offset,
				OSNBATMAP_BLOCK_SIZE);
			if(!NT_SUCCESS(status))
			{
				KdPrint(("OSNPA - FAILED TO WRITE BATBLOCK TO DISK ,STATUS = %x\n",status));
				FreeBatMap();
				return status;
			}
		}
	}

	return STATUS_SUCCESS;
}

NTSTATUS   CDiskCache::FreeBatMap()
{
	if(m_pBatBlock)
	{
		ExFreePool(m_pBatBlock);
		m_pBatBlock = NULL;
	}

	return STATUS_SUCCESS;
}

NTSTATUS   CDiskCache::InitBlockUseBitMap(unsigned char * address,ULONG size)
{
	ULONG index = 0;
	ULONG blockNum = 0;
	ULONG BlockUseTotal = (ULONG)(m_pOsnDskCh->Cache.NumberBlocks /BIT_PER_BYTE);

	for(index = 0;index <size ;index += sizeof(ULONG) )
	{
		blockNum = *(ULONG *)(address+index);
		if(0 != blockNum)
		{
			if((blockNum/BIT_PER_BYTE) <= BlockUseTotal)
			{
				if((blockNum/BIT_PER_BYTE)>=m_BlockUseBitmap->CurrentByte)
				{
					m_BlockUseBitmap->CurrentByte = blockNum/BIT_PER_BYTE;
					if(blockNum%BIT_PER_BYTE>m_BlockUseBitmap->CurrentBit)
						m_BlockUseBitmap->CurrentBit  = blockNum%BIT_PER_BYTE;
				}

				m_BlockUseBitmap->BitMap[blockNum/BIT_PER_BYTE] |= 1<<(blockNum%BIT_PER_BYTE);
				IncrementTotalBlockHaveCount();
			}
		}
	}

	return STATUS_SUCCESS;
}

NTSTATUS   CDiskCache::CheckBatMap(LONGLONG offset,ULONG length,ULONG *blockNum,BOOLEAN readOrWrite)
{
	NTSTATUS  status;
	ULONG   num = 0;
	ULONG   retNum = 0;
	LARGE_INTEGER  readOffset ;
	ULONGLONG  readLength = 0;
	PBITMAP     pBitmap = NULL;
	CQueue     *tempQueue;
	BOOLEAN     bitInMem = FALSE;

	COsnPADevice *pOsnDevice = (COsnPADevice *)m_pSourceDevice;

	num = (ULONG)(offset / m_pOsnDskCh->Cache.BlockSize);
	if((num*OSNBAT_PER_UNIT) > m_pOsnDskCh->Cache.DskChSize)
	{
		return STATUS_UNSUCCESSFUL;
	}

	status = ReadBatMapFromMem(num,retNum);
	if(!NT_SUCCESS(status))
	{
		KdPrint(("OSNPA - READ BAT MAP FAILED ,STATUS = %x \n",status));
		return status;
	}

	if(0 == retNum)
	{
		if(pOsnDevice->GetFlushCache())
		{
			KdPrint(("OSNPA - FLUSH CACHE AND  BLOCK  NOT IN CACHE \n"));
			*blockNum = 0;
			return STATUS_SUCCESS;
		}
		if(readOrWrite)
		{
			*blockNum = 0;
			return STATUS_UNSUCCESSFUL;
		}

		if(m_BlockUseBitmap)
		{
			KdPrint(("OSNPA - THIS BLOCK HAVN'T ALLOC ,ALLOC NEW BLOCK \n"));
			retNum = GetBitMapNextUnused();
			if(retNum == 0)
			{
				KdPrint(("OSNPA - THERE IS NO BATMAP FOR NEW BLOCK\n"));
				*blockNum = 0;
				return STATUS_SUCCESS;
			}

			status = NewBlock(num,retNum);
			if(!NT_SUCCESS(status))
			{
				*blockNum = 0;
				return STATUS_UNSUCCESSFUL;
			}
		}
		else
			return STATUS_UNSUCCESSFUL;
	}
	else
	{
		tempQueue = m_BitmapListHeader.Next();
		while(tempQueue)
		{
			pBitmap = (PBITMAP)tempQueue->GetItem();
			if(pBitmap->CurNum == retNum)
			{
				bitInMem = TRUE;
				break;
			}
			
			tempQueue = tempQueue->Next();
		}
		if(!bitInMem)
		{
			NewBitmap(retNum,FALSE);
		}
	}

	*blockNum = retNum;
	return STATUS_SUCCESS;
}

NTSTATUS CDiskCache::ReadBatMapFromMem(ULONG num,ULONG &ret)
{
	ULONG           numInCache;
	ULONG           blockNum;
	LARGE_INTEGER   offset;
	NTSTATUS        status;

	numInCache = (num*OSNBAT_PER_UNIT) / OSNBATMAP_BLOCK_SIZE;
	num        =  ((num*OSNBAT_PER_UNIT) % OSNBATMAP_BLOCK_SIZE) ;
	if(numInCache == m_pBatBlock->CurNum)
	{
		ret = *(ULONG *)(m_pBatBlock->Address + num);
	}
	else
	{
		KdPrint(("OSNPA - THE BATMAP IS NOT IN MEM ,READ BAT NUM = %x\n",numInCache));
		offset.QuadPart = numInCache*OSNBAT_PER_UNIT+OSNDSKHD_SIZE+OSNDSKCH_SIZE;
		status = m_pCacheDevice->SynchReadWriteLocalBlocks(
			IRP_MJ_READ,
			m_pBatBlock->Address,
			&offset,
			OSNBATMAP_BLOCK_SIZE);
		if(!NT_SUCCESS(status))
		{
			return status;
		}

		m_pBatBlock->CurNum = numInCache;
		ret = *(ULONG *)(m_pBatBlock->Address + num);
	}

	return STATUS_SUCCESS;
}

NTSTATUS  CDiskCache::NewBlock(ULONG num,ULONG blocknum)
{
	LARGE_INTEGER writeOffset;
	NTSTATUS      status;
	ULONG         numInCache;

	numInCache =  ((num*OSNBAT_PER_UNIT) % OSNBATMAP_BLOCK_SIZE) ;

	*(ULONG *)(m_pBatBlock->Address+numInCache) = blocknum;
	writeOffset.QuadPart = m_pBatBlock->CurNum * OSNBATMAP_BLOCK_SIZE +OSNDSKHD_SIZE+OSNDSKCH_SIZE;
	status = m_pCacheDevice->SynchReadWriteLocalBlocks(
		IRP_MJ_WRITE,
		m_pBatBlock->Address,
		&writeOffset,
		OSNBATMAP_BLOCK_SIZE);
	if(!NT_SUCCESS(status))
	{
		KdPrint(("OSNPA - FAILED TO WRITE BATMAP TO DISK,status = %x\n",status));
		return status;
	}

	KdPrint(("OSNPA - NEW BITMAP FOR BLOCKNUM = %x \n ",blocknum));
	IncrementTotalBlockHaveCount();
	status = NewBitmap(blocknum,TRUE);

	return status;
}

NTSTATUS CDiskCache::NewBitmap(ULONG blockNum,BOOLEAN newconfig)
{
	PBITMAP       pBitmap = NULL;
	NTSTATUS      status;
	LARGE_INTEGER writeOffset;

	pBitmap = (PBITMAP)ExAllocatePoolWithTag(NonPagedPool,m_pOsnDskCh->Cache.BlockHdSize+sizeof(BITMAP),OSNTAGPA);
	if(!pBitmap)
	{
		return STATUS_UNSUCCESSFUL;
	}

	RtlZeroMemory(pBitmap,m_pOsnDskCh->Cache.BlockHdSize+sizeof(BITMAP));
	pBitmap->CurNum = blockNum;
	writeOffset.QuadPart = m_pOsnDskCh->Cache.StartingOffset+(blockNum-1)*(m_pOsnDskCh->Cache.BlockSize+m_pOsnDskCh->Cache.BlockHdSize);
	if(newconfig)
	{
		status = m_pCacheDevice->SynchReadWriteLocalBlocks(
			IRP_MJ_WRITE,
			pBitmap->BitMap,
			&writeOffset,
			m_pOsnDskCh->Cache.BlockHdSize);
	}
	else
	{
		status = m_pCacheDevice->SynchReadWriteLocalBlocks(
			IRP_MJ_READ,
			pBitmap->BitMap,
			&writeOffset,
			m_pOsnDskCh->Cache.BlockHdSize);
	}
	if(!NT_SUCCESS(status))
	{
		ExFreePool(pBitmap);
		return status;
	}

	InsertBitmapToList(pBitmap);

	return status;
}

VOID  CDiskCache::RemoveLastBitmapFromList()
{
	KIRQL   oldIrql ;
	PVOID   item;

	oldIrql = AcquireBitmapListLock();

	item = m_BitmapListHeader.DeQueueTail();
	ExFreePool(item);
	m_BitmapInMemCount--;

	ReleaseBitmapLock(oldIrql);
}

/*
check the list ,if there are having
max num,so delete the last bitmap,and
insert new bitmap to first
*/
VOID  CDiskCache::InsertBitmapToList(PVOID item)
{
	KIRQL   oldIrql ;
	PVOID   pBitmap;

	oldIrql = AcquireBitmapListLock();

	if(m_BitmapInMemCount >= OSNBLOCK_BITMAP_IN_MEM_MAX_COUNT)
	{
		pBitmap= m_BitmapListHeader.DeQueueTail();
		m_BitmapInMemCount--;
		ExFreePool(pBitmap);
	}

	m_BitmapListHeader.InsertQHead(item);
	m_BitmapInMemCount++;
	ReleaseBitmapLock(oldIrql);
}

/*++
num is start from 1,not 0;
	--*/
ULONG CDiskCache::GetBitMapNextUnused()
{
	ULONG  twice = 0;
	ULONG  curByte = 0;
	ULONG   curBit  = 0;
	ULONG  endByte = 0;
	ULONG   endBit;

	endByte       = (ULONG)(m_pOsnDskCh->Cache.NumberBlocks /BIT_PER_BYTE);

	curByte = m_BlockUseBitmap->CurrentByte;
	curBit  = m_BlockUseBitmap->CurrentBit;
	while(true)
	{
		if(curByte == endByte)
		{
			endBit = m_pOsnDskCh->Cache.NumberBlocks %BIT_PER_BYTE;
		}
		else
			endBit = BIT_PER_BYTE-1;

		if(curBit == endBit)
		{
			curBit = 0;
			curByte++;
			if(curByte > endByte)
			{
				curByte = 0;
				curBit = 0;
				twice ++;
				if(twice == 2)
					break;

			}

		}

		if((m_BlockUseBitmap->BitMap[curByte] & (0x1<<curBit)) == 0)
		{
			m_BlockUseBitmap->CurrentByte = curByte;
			m_BlockUseBitmap->CurrentBit  = curBit;
			m_BlockUseBitmap->BitMap[curByte] |= (0x1<<curBit);

			ASSERT((curByte*BIT_PER_BYTE + curBit+1) <= m_pOsnDskCh->Cache.NumberBlocks);
			return curByte*BIT_PER_BYTE + curBit+1;
		}

		curBit++;
	}

	return 0;
}

VOID CDiskCache::ClearBatMapUsed(ULONG blockNum)
{
	ULONG   byte;
	ULONG   bit;

	byte = (blockNum-1) /BIT_PER_BYTE;
	bit  = (blockNum-1) %BIT_PER_BYTE;
	if(m_BlockUseBitmap)
	{
		m_BlockUseBitmap->BitMap[byte] &= ~(0x1<<bit);
	}
}

NTSTATUS   CDiskCache::CheckBitMap(LONGLONG &offset,ULONG &length,ULONG blockNum,PBITMAP_SEG bitmapSec)
{
	NTSTATUS  status ;
	ULONGLONG tempoffset;
	ULONGLONG tempLength;
	PBITMAP   pBitmap;
	LARGE_INTEGER  bitmapOffset;
	CQueue    *pListNode;
	KIRQL      irql;
	ULONG     byteIndex = 0;
	ULONG     bitIndex  = 0;
	ULONG     fisrtbit,lastbit;
	ULONG     StartByte = 0;
	ULONG     EndByte   = 0;
	ULONG     StartBit  = 0;
	ULONG     EndBit    = 0;
	ULONG     numOfSector  = 0;
	ULONG     addressStart = 0;
	BOOLEAN   NotInMem  = FALSE;

	tempoffset = (offset % m_pOsnDskCh->Cache.BlockSize);
	tempLength = length;

	StartByte = GetBitmapByte(tempoffset);
	StartBit  = GetBitmapBit(tempoffset);
	EndByte   = GetBitmapByte(tempoffset+tempLength-1);
	EndBit    = GetBitmapBit(tempoffset+tempLength-1);

	//KdPrint(("OSNPA - FIND BIT FROM BITMAP BEGIN \n"));
	irql = AcquireBitmapListLock();
	pListNode = m_BitmapListHeader.Next();
	while(pListNode)
	{
		pBitmap = (PBITMAP)pListNode->GetItem();
		if(pBitmap)
		{
			if(pBitmap->CurNum == blockNum)
			{
				RtlCopyMemory(bitmapSec->BitMap,&pBitmap[StartByte],(EndByte - StartByte+1));
				for(byteIndex = StartByte;byteIndex<=EndByte;byteIndex++)
				{
					fisrtbit = 0;
					lastbit = BIT_PER_BYTE-1;
					if(byteIndex == StartByte)
						fisrtbit = StartBit;
					if(byteIndex == EndByte)
						lastbit  = EndBit;
					for(bitIndex = fisrtbit;bitIndex<=lastbit;bitIndex++)
					{
						if((pBitmap->BitMap[byteIndex] & (0x1<<bitIndex))==0)
						{
							pBitmap->BitMap[byteIndex] |= (0x1<<bitIndex);
							NotInMem = TRUE;
						}
					}
				}
				break;
			}
		}

		pListNode = pListNode->Next();
	}
	ReleaseBitmapLock(irql);

//	KdPrint(("OSNPA - FIND BIT FROM BITMAP END \n"));
	if(NotInMem)
	{
		numOfSector  = ((EndByte)/BLOCK_SIZE)-((StartByte)/BLOCK_SIZE)+1;
		addressStart = (StartByte/BLOCK_SIZE)*BLOCK_SIZE;
		bitmapOffset.QuadPart = m_pOsnDskCh->Cache.StartingOffset+ (blockNum-1)*(m_pOsnDskCh->Cache.BlockSize+m_pOsnDskCh->Cache.BlockHdSize);
		bitmapOffset.QuadPart += addressStart;
		status = m_pCacheDevice->SynchReadWriteLocalBlocks(
			IRP_MJ_WRITE,
			(&pBitmap->BitMap[addressStart]),
			&bitmapOffset,
			numOfSector*BLOCK_SIZE);
		if(!NT_SUCCESS(status))
		{
			KdPrint(("OSNPA - FAILED TO WRITE BITMAP TO CACHE DISK ,STATUS = %x \n",status));
			return status;
		}
		else
		{
			KdPrint(("OSNPA - BITMAP NOT IN MEM ,READ BLOCK BITMAP NUM = %x  \n",blockNum));
			KdPrint(("OSNPA - BITMAP SCETOR START FROM %x ,NUM OF SECTOR %x \n",addressStart,numOfSector));
			bitmapSec->WriteToDisk = TRUE;
		}
	}

	offset = m_pOsnDskCh->Cache.StartingOffset+ (blockNum-1)*(m_pOsnDskCh->Cache.BlockSize+m_pOsnDskCh->Cache.BlockHdSize);
	offset += m_pOsnDskCh->Cache.BlockHdSize + tempoffset;
	return STATUS_SUCCESS;
}

ULONG   CDiskCache::GetBitmapByte(ULONGLONG offset)
{
	return (ULONG)(offset / (BLOCK_SIZE*BIT_PER_BYTE));
}

CHAR   CDiskCache::GetBitmapBit(ULONGLONG  offset)
{
	ULONG totalBits = (ULONG)(offset/BLOCK_SIZE);
	return (CHAR) (totalBits %BIT_PER_BYTE);
}

GET_BLOCK_STATE    CDiskCache::GetBlockInCache(LONGLONG &offset,ULONG &length,BOOLEAN readOrWrite,PBITMAP_SEG bitmapSec)
{
	ULONGLONG  tempOffset;
	ULONGLONG  tempLength;
	ULONG      blockNum = 0;
	NTSTATUS   status  ;

	status = CheckBatMap(offset,length,&blockNum,readOrWrite);
	if(NT_SUCCESS(status) && blockNum == 0)
	{
		return STATE_NO_ENOUGH_SPACE;
	}

	if(!NT_SUCCESS(status))
	{
		return STATE_FAILED;
	}

	status = CheckBitMap(offset,length,blockNum,bitmapSec);
	if(!NT_SUCCESS(status))
	{
		return STATE_FAILED;
	}

	return STATE_SUCCESS;
}

PVOID  CDiskCache::GetBlockBitmap(ULONG blockNum)
{
	KIRQL      irql;
	CQueue    *pListNode;
	PBITMAP   pBitmap;

	irql = AcquireBitmapListLock();
	pListNode = m_BitmapListHeader.Next();
	while(pListNode)
	{
		pBitmap = (PBITMAP)pListNode->GetItem();
		if(pBitmap->CurNum == blockNum)
		{
			ReleaseBitmapLock(irql);
			return pBitmap;
		}

		pListNode = pListNode->Next();
	}
	ReleaseBitmapLock(irql);

	return NULL;
}

ULONG  CDiskCache::GetNextFlushBlockNum(ULONG &blockNum)
{
	if(0 == m_TotalBlockHaveUsed)
	{
		KdPrint(("OSNPA - THERE IS NO HAVE USED BLOCK \n"));
		return 0;
	}

	NTSTATUS    status;
	ULONG       num        = 0;
	ULONG       blockIndex = 0;
	ULONG       startIndex = 0;
	ULONG       bitIndex   = 0;
	ULONG       secondIndex = 0;
	LARGE_INTEGER  offset  ;
	PBAT_BLOCK  pBatBlock = (PBAT_BLOCK)ExAllocatePoolWithTag(NonPagedPool,sizeof(BAT_BLOCK)+OSNBATMAP_BLOCK_SIZE,OSNTAGPA);
	if(!pBatBlock)
	{
		return 0;
	}

	RtlZeroMemory(pBatBlock,sizeof(BAT_BLOCK)+OSNBATMAP_BLOCK_SIZE);
	startIndex = blockNum*sizeof(ULONG)/OSNBATMAP_BLOCK_SIZE;
	secondIndex = blockNum*sizeof(ULONG) % OSNBATMAP_BLOCK_SIZE;
	for(blockIndex = startIndex;blockIndex<m_TotalBatCount;blockIndex++)
	{
		offset.QuadPart = OSNDSKHD_SIZE+OSNDSKCH_SIZE + blockIndex*OSNBATMAP_BLOCK_SIZE;

		status = m_pCacheDevice->SynchReadWriteLocalBlocks(
			IRP_MJ_READ,
			pBatBlock->Address,
			&offset,
			OSNBATMAP_BLOCK_SIZE);
		if(!NT_SUCCESS(status))
		{
			KdPrint(("OSNPA - FAILED TO READ BATMAP FOR FLUSH ,STATUS = %x\n",status));
			ExFreePool(pBatBlock);
			return status;
		}

		for(bitIndex = secondIndex; bitIndex <=OSNBATMAP_BLOCK_SIZE;bitIndex += sizeof(ULONG))
		{
			//num = *((ULONG *)(pBatBlock->Address+bitIndex*sizeof(ULONG)));
			num = *((ULONG *)(pBatBlock->Address+bitIndex));
			if(num != 0)
			{
				blockNum = blockIndex*OSNBATMAP_BLOCK_SIZE /sizeof(ULONG);
				blockNum += bitIndex /sizeof(ULONG);
				ExFreePool(pBatBlock);
				return num;
			}
		}
	}

	ExFreePool(pBatBlock);
	return 0;
}

NTSTATUS CDiskCache::GetBlockBitMap(ULONG blockNum ,PBITMAP pBitMap)
{
	NTSTATUS   status;
	if(!pBitMap)
	{
		KdPrint(("OSNPA - BITMAP IS NULL \n"));
		return STATUS_UNSUCCESSFUL;
	}

	LARGE_INTEGER  readOffset;

	readOffset.QuadPart = m_pOsnDskCh->Cache.StartingOffset+(blockNum-1)*(m_pOsnDskCh->Cache.BlockSize+m_pOsnDskCh->Cache.BlockHdSize);

	status = m_pCacheDevice->SynchReadWriteLocalBlocks(
		IRP_MJ_READ,
		pBitMap->BitMap,
		&readOffset,
		m_pOsnDskCh->Cache.BlockHdSize);
	if(!NT_SUCCESS(status))
	{
		KdPrint(("OSNPA - FAILED TO READ BITMAP FOR TRANSFER ,STATUS = %x\n",status));
	}

	pBitMap->CurNum = blockNum;
	return status;
}

NTSTATUS      CDiskCache::ReadAndWriteNewBat(ULONG sourceBlock,ULONG block)
{
	NTSTATUS  status ;
	ULONG batBlock ;
	ULONG blockNum;
	LARGE_INTEGER  offset  ;
	PBAT_BLOCK     pBatBlock;
	ULONG          byte;
	ULONG          bit;

	batBlock = sourceBlock*sizeof(ULONG) /OSNBATMAP_BLOCK_SIZE;
	blockNum = sourceBlock*sizeof(ULONG) %OSNBATMAP_BLOCK_SIZE;

	pBatBlock = (PBAT_BLOCK)ExAllocatePoolWithTag(NonPagedPool,sizeof(BAT_BLOCK)+OSNBATMAP_BLOCK_SIZE,OSNTAGPA);
	if(!pBatBlock)
		return STATUS_UNSUCCESSFUL;

	offset.QuadPart =  OSNDSKHD_SIZE+OSNDSKCH_SIZE + batBlock*OSNBATMAP_BLOCK_SIZE;

	status = m_pCacheDevice->SynchReadWriteLocalBlocks(
		IRP_MJ_READ,
		pBatBlock->Address,
		&offset,
		OSNBATMAP_BLOCK_SIZE);
	if(!NT_SUCCESS(status))
	{
		KdPrint(("OSNPA - READ BAT BLOCK FAILED ,STATUS = %x \n",status));
		return status;
	}

	*(ULONG *)(pBatBlock->Address+blockNum) = 0;
	status = m_pCacheDevice->SynchReadWriteLocalBlocks(
		IRP_MJ_WRITE,
		pBatBlock->Address,
		&offset,
		OSNBATMAP_BLOCK_SIZE);
	if(!NT_SUCCESS(status))
	{
		KdPrint(("OSNPA - WRIETE BLOCK FAILED ,STATUS = %x \n",status));
	}
	else
	{
		if(m_pBatBlock)
		{
			if(m_pBatBlock->CurNum == batBlock&&m_pBatBlock)
			{
				*(ULONG *)(m_pBatBlock->Address+blockNum) = 0;
			}
		}
	}

	byte = block /BIT_PER_BYTE;
	bit  = block %BIT_PER_BYTE;
	m_BlockUseBitmap->BitMap[byte] &= ~(0x1<<bit);
	DecrementTotalBlockHaveCount();

	ExFreePool(pBatBlock);

	return status;
}

VOID          CDiskCache::DeleteBitMapFromMem(ULONG blockNum)
{
	KIRQL      irql;
	CQueue    *pListNode;
	PBITMAP   pBitmap;

	irql = AcquireBitmapListLock();
	pListNode = m_BitmapListHeader.Next();
	while(pListNode)
	{
		pBitmap = (PBITMAP)pListNode->GetItem();
		if(pBitmap->CurNum == blockNum)
		{
			pListNode->Remove();
			ExFreePool(pBitmap);

			//ReleaseBitmapLock(irql);
		}

		pListNode = pListNode->Next();
	}
	ReleaseBitmapLock(irql);
}

NTSTATUS    CDiskCache::RecoveryBitMap(ULONGLONG blockOffset,ULONG blockLength,PBITMAP_SEG pBitMapSeg)
{
	NTSTATUS   status;
	ULONG      blockNum = 0;
	PBITMAP    pBitmap  = NULL;
		LARGE_INTEGER  bitmapOffset;
	KIRQL      irql;
	ULONG     byteIndex = 0;
	ULONG     bitIndex  = 0;
	ULONG     fisrtbit,lastbit;
	ULONG     StartByte = 0;
	ULONG     EndByte   = 0;
	ULONG     StartBit  = 0;
	ULONG     EndBit    = 0;
	ULONGLONG tempoffset;
	ULONGLONG tempLength;
	ULONG     numOfSector  = 0;
	ULONG     addressStart = 0;
	ULONG     length    = 0;

	status = CheckBatMap(blockOffset,blockLength,&blockNum,TRUE);
	if(!NT_SUCCESS(status))
	{
		KdPrint(("OSNPA - THE BAT IS NOT IN BATMAP \n"));
		status = STATUS_SUCCESS;
		return status;
	}

	pBitmap = (PBITMAP)GetBlockBitmap(blockNum);
	if(!pBitmap)
	{
		pBitmap = (PBITMAP)ExAllocatePoolWithTag(NonPagedPool,m_pOsnDskCh->Cache.BlockHdSize+sizeof(BITMAP),OSNTAGPA);
		if(!pBitmap)
		{
			KdPrint(("OSNPA - FAILED TO ALLOC MEMORY FOR BITMAP \n"));
			return STATUS_UNSUCCESSFUL;
		}

		status = GetBlockBitMap(blockNum,pBitmap);
		if(!NT_SUCCESS(status))
		{
			KdPrint(("OSNPA - FAILED TO ALLOC MEMORY FOR BITMAP ,STATUS = %x\n",status));
			return status;
		}
	}

	tempoffset = (blockOffset % m_pOsnDskCh->Cache.BlockSize);
	tempLength = blockLength;

	StartByte = GetBitmapByte(tempoffset);
	StartBit  = GetBitmapBit(tempoffset);
	EndByte   = GetBitmapByte(tempoffset+tempLength-1);
	EndBit    = GetBitmapBit(tempoffset+tempLength-1);

	irql = AcquireBitmapListLock();
	length = 0;
	for(byteIndex = StartByte;byteIndex<=EndByte;byteIndex++,length++)
	{
		fisrtbit = 0;
		lastbit = BIT_PER_BYTE-1;
		if(byteIndex == StartByte)
			fisrtbit = StartBit;
		if(byteIndex == EndByte)
			lastbit  = EndBit;
		for(bitIndex = fisrtbit;bitIndex<=lastbit;bitIndex++)
		{
			if((pBitMapSeg->BitMap[length] & (0x1<<bitIndex))==0)
			{
				pBitmap->BitMap[byteIndex] &= ~(0x1<<bitIndex);
			}
			else
			{
				pBitmap->BitMap[byteIndex] |= (0x1<<bitIndex);
			}
		}
	}
	ReleaseBitmapLock(irql);

	numOfSector  = ((EndByte)/BLOCK_SIZE)-((StartByte)/BLOCK_SIZE)+1;
	addressStart = (StartByte/BLOCK_SIZE)*BLOCK_SIZE;
	bitmapOffset.QuadPart = m_pOsnDskCh->Cache.StartingOffset+ (blockNum-1)*(m_pOsnDskCh->Cache.BlockSize+m_pOsnDskCh->Cache.BlockHdSize);
	bitmapOffset.QuadPart += addressStart;
	status = m_pCacheDevice->SynchReadWriteLocalBlocks(
		IRP_MJ_WRITE,
		(&pBitmap->BitMap[addressStart]),
		&bitmapOffset,
		numOfSector*BLOCK_SIZE);
	if(!NT_SUCCESS(status))
	{
		KdPrint(("OSNPA - FAILED TO CLEAR BITMAP ,STATUS = status\n"));
		return status;
	}

	KdPrint(("OSNPA - SUCCESS CLEAR BITMAP \n"));
	return status;
}