#ifndef _DDOSN_PA_H
#define _DDOSN_PA_H

typedef enum CACHE_ROLE
{
	NO_CACHE,
	CACHE_SOURCE,
	CACHE_TARGET
};

typedef enum CACHE_STATE
{
	NONE,
	UP,
	CACHE_DOWN,//CACHE_DOWN
	SOURCE_DOWN,
	TRANSFER,
	TRANSFER_FAILED
};

typedef struct _CACHE_INFO
{
	VOLUMEID   m_SourceVolumeID;
	VOLUMEID   m_CacheVolumeID;
}CACHE_INFO,*PCACHE_INFO;

#define OSNDSKHD_MAGIC			"OsnDskHd"
#define OSNDSKHD_VERSION		1
#define OSNDSKHD_TYPE			1
#define OSNDSKHD_SIZE			512
#define OSNDSKHD_OFFSET         0

#define OSNDSKCH_MAGIC			"OsnDskCh"
#define OSNDSKCH_VERSION		1
#define OSNDSKCH_OFFSET         1
#define OSNDSKCH_SIZE           512
//we use 16M total (one block for OsnDskHd, one block fro OsnDskCh and 16M-2 for OsnBAT)
//#define OSNDSKCH_SIZE			16*1024*1024 - 512
//we max can protect 1PB data
#define OSNBAT_MIN_SIZE         (16*1024*1024)

//we will allocate batmap loop,everytime allocate 8K memory for batmap,it can show 2T block Data if blocksize =256M
#define OSNBATMAP_BLOCK_SIZE    (512*16)
#define OSNBATMAP_BLOCK_IN_MEM  16
#define OSNBAT_PER_UNIT         sizeof(ULONG)

#define OSNBLOCK_MAX_DATA_SIZE		(256*1024*1024)
//#define OSNBLOCK_BITMAP_SIZE	64*1024
//#define OSNBLOCK_TOTAL_SIZE		OSNBLOCK_DATA_SIZE + OSNBLOCK_BITMAP_SIZE
//#define OSNBLOCK_STARTING_OFFSET OSNDSKCH_SIZE + OSNDSKHD_SIZE
#define   OSNBLOCK_BITMAP_IN_MEM_MAX_COUNT   32


#define  OSNMAX_FLUSH_IO_COUNT      16         //MAX FLUSH IO OUTSTANDING
#define  OSNMAX_FLUSH_IO_BIT_NUM    2*1024     //we can flush max 2*1024*512 size data once time;

#define		OSNTAGPA				 'Cnsp'

typedef union _OSNDSKHD{
	struct
	{
		ULONGLONG		Magic;				//¡±OsnDskHd¡±, 
		ULONG			Version;			// Version stamp
		ULONG			Type;				//1 
		GUID			CacheId; 				//buffer volume id
		GUID			SourceId;			//
		ULONGLONG		TimeStamp;				
		ULONGLONG		CacheSize;				//buffer volume size in bytes 
		ULONGLONG		SourceSize;			//in bytes
		ULONG			Checksum;
		ULONG           Reserved[108];         //0
	}Header;

	unsigned char Buffer[512];
}OSNDSKHD, *POSNDSKHD;

typedef union _OsnDskCh
{
	struct
	{
		ULONGLONG		Magic;				// ¡°OsnDskCh¡± 
		ULONG			Version;			// Version stamp
		ULONGLONG		DskChSize;			//OsnDskCh + OsnBAT size 
		ULONG			BlockSize;			//256M bytes
		ULONG			BlockHdSize;		//64K bytes	
		ULONGLONG		StartingOffset; 	//starting offset of osnblocks,
		ULONGLONG		NumberBlocks;		//total number in osnblocks
		ULONG			Checksum;
		ULONG			Reserved[116];		//0	
	}Cache;
	
	unsigned char Buffer[512];
}OSNDSKCH, *POSNDSKCH;

typedef struct _BAT_BLOCK
{
	ULONG     CurNum;
	unsigned char     Address[1];
}BAT_BLOCK,*PBAT_BLOCK;

typedef struct _BITMAP_
{
	ULONG   CurNum;
	unsigned char   BitMap[1];
}BITMAP,*PBITMAP;

typedef enum _WRITE_DETAIL
{
	WRITE_LOCAL,
	WRITE_CACHE
}WRITE_DETAIL;

typedef struct _BITMAP_SEG
{
	BOOLEAN   WriteToDisk;
	unsigned char BitMap[1020];
}BITMAP_SEG,*PBITMAP_SEG;

typedef struct _BITMAP_CLR
{
	BOOLEAN     Separate;
	BITMAP_SEG  BitMapSeg[2];
}BITMAP_CLR,*PBITMAP_CLR;

typedef struct _BLOCK_USE_BITMAP
{
	ULONG CurrentByte;
	ULONG  CurrentBit;
	unsigned char BitMap[1];
}BLOCK_USE_BITMAP,*PBLOCK_USE_BITMAP;

typedef enum _ACTIVE_IRP_STATE 
{
	STATE_WRITE_NEW,
	STATE_WRITE_PENDING,
	STATE_WRITE_FAILED,
	STATE_WRITE_COMPLETED,
	STATE_READ_NEW,
	STATE_READ_PENDING,
	STATE_READ_COMPLETED
}ACTIVE_IRP_STATE;

typedef enum _GET_BLOCK_STATE
{
	STATE_SUCCESS,
	STATE_FAILED,
	STATE_NO_ENOUGH_SPACE
}GET_BLOCK_STATE;

typedef struct _QUERY_CACHE_STATE
{
	CACHE_STATE     state;
}QUERY_CACHE_STATE;

typedef struct _FLUSH_INFO
{
	BOOLEAN     m_ReadFlushIrpComplete;
	ULONG       m_Byte;
	ULONG       m_Bit;
	PIRP        m_pFlushIrp;
}FLUSH_INFO,*PFLUSH_INFO;

typedef ULONG OSNBAT_MAP;

#define OSNBAT_NOT_MAPPED	0xFFFFFFFF

#define OSNPA_DEVICE_TYPE					60008
#define OSNPA_MAKE_IOCTL(t,c)\
        (ULONG)CTL_CODE((t), 0x800+(c), METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_OSNPA_SET_VOLUME_CACHE				OSNPA_MAKE_IOCTL(OSNPA_DEVICE_TYPE,	100)
#define IOCTL_OSNPA_REMOVE_VOLUME_CACHE             OSNPA_MAKE_IOCTL(OSNPA_DEVICE_TYPE,	101)
#define IOCTL_OSNPA_GET_CACHE_INFO_STATE            OSNPA_MAKE_IOCTL(OSNPA_DEVICE_TYPE, 102)
#define IOCTL_OSNPA_SET_FLUSH_CACHE                 OSNPA_MAKE_IOCTL(OSNPA_DEVICE_TYPE, 103)


#define OSN_CACHE_SUBKEY           L"OsnPA\\Parameters"
#define OSN_CACHE_RELATION          L"OsnPA\\Parameters\\Relation"

#endif _DDOSN_PA_H