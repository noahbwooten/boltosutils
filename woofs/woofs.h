//
//  woofs.h
//  boltosutils
//
//  Created by Noah Wooten on 5/5/26.
//

#ifndef _WOOFS_H
#define _WOOFS_H

/*
 aarch64 shims
 */

#define WORD64 unsigned long
#define WORD32 unsigned int
#define WORD16 unsigned short
#define WORD8 unsigned char
#define BYTE unsigned char
#define STR signed short
#define PSTR signed short*
#define PCSTR const signed short*

#define WOOFS_MAGIC             0xFFFA123112000001ULL

#define WOOFS_ERROR_NONE            0x0000000000000000ULL
#define WOOFS_ERROR_DIR_NOT_FOUND   0x0000000000000001ULL
#define WOOFS_ERROR_FILE_NOT_FOUND  0x0000000000000002ULL
#define WOOFS_ERROR_DISK_FULL       0x0000000000000003ULL
#define WOOFS_ERROR_MULTISPAN_FULL  0x0000000000000004ULL
#define WOOFS_ERROR_BAD_MAGIC       0x0000000000000005ULL
#define WOOFS_ERROR_ALREADY_EXISTS  0x0000000000000006ULL
#define WOOFS_ERROR_INVALID_ARGS    0x0000000000000007ULL
#define WOOFS_ERROR_OUT_OF_MEMORY   0x0000000000000008ULL

typedef void(*DiskRead_t)(void* Buffer, WORD64 Address, WORD64 Size);
typedef void(*DiskWrite_t)(void* Buffer, WORD64 Address, WORD64 Size);

typedef struct _WOOFS_HEAD {
    WORD64 Magic;

    WORD64 RelativeBase;        /* context-only: partition offset on disk, set by WoofsMount */
    WORD64 SerialNumber[2];

    WORD64 FirstIndex;          /* FS-relative address of first WOOFS_INDEX page */
    WORD64 FirstFile;           /* FS-relative address of first WOOFS_FILE */
    WORD64 FirstDirectory;      /* FS-relative address of first WOOFS_DIRECTORY (always root) */
    WORD64 FirstGaps;           /* FS-relative address of first WOOFS_GAP_INDEX page */

    WORD64 NextAvailableFileId;
    WORD64 NextAvailableDirectoryId;
    WORD64 HighestAddress;      /* next unallocated FS-relative byte; grows as filesystem grows */
    WORD64 PartitionSize;       /* total partition size in bytes, set by WoofsFormat */

    WORD64 LastError;           /* context-only: last error code, set by each operation */
    DiskRead_t  ReadFn;         /* context-only: set by WoofsMount */
    DiskWrite_t WriteFn;        /* context-only: set by WoofsMount */

    WORD64 Reserved[60];

    union {
        WORD64 FlagsRaw[2];
        struct {
            WORD64 Reserved0 : 64;
            WORD64 Reserved1 : 64;
        };
    } Flags;
} WOOFS_HEAD, *PWOOFS_HEAD;

typedef struct _WOOFS_INDEX_ITEM {
    WORD64 FilePathHash;    /* WoofsHashString64 of full path, e.g. L"/tmp/test.txt" */
    WORD64 FileLocation;    /* FS-relative address of the WOOFS_FILE or WOOFS_DIRECTORY */
    union {
        WORD64 FlagRaw;
        struct {
            WORD64 IsDirectory : 1;
            WORD64 Reserved    : 63;
        };
    };
} WOOFS_INDEX_ITEM, *PWOOFS_INDEX_ITEM;

typedef struct _WOOFS_INDEX {
    WOOFS_INDEX_ITEM Items[512];
    WORD64 Previous, Me, Next;
} WOOFS_INDEX, *PWOOFS_INDEX;

typedef struct _WOOFS_DIRECTORY {
    STR    DirectoryName[64];
    WORD64 PathHash;            /* WoofsHashString64 of local DirectoryName */
    WORD64 ParentDirectory;     /* parent DirectoryId; 0xFFFFFFFFFFFFFFFF for root */
    WORD64 DirectoryId;

    struct {
        WORD16 UserOrGroupID;
        union {
            WORD32 FlagRaw;
            struct {
                WORD32 IDRepresentsGroup : 1;
                WORD32 Read              : 1;
                WORD32 Write             : 1;
                WORD32 Execute           : 1;
                WORD32 InUse             : 1;
                WORD32 Reserved          : 27;
            };
        } Flags;
    } SecurityInformation[8];   /* not implemented */

    WORD64 SecurityExtension;
    WORD64 Reserved[16];

    WORD64 FirstFile;           /* FS-relative address of first WOOFS_FILE in this directory */
    WORD64 AbsolutePrevious, Me, AbsoluteNext;
    WORD64 DirPrevious, DirNext; /* sibling directories within the same parent */
} WOOFS_DIRECTORY, *PWOOFS_DIRECTORY;

typedef struct _WOOFS_FILE {
    STR    FileName[64];
    WORD64 FileId;
    WORD64 FilePathHash;        /* WoofsHashString64 of local FileName */
    WORD64 ParentDirectory;     /* parent DirectoryId */

    WORD64 FileSize;
    WORD64 RawData;             /* FS-relative address of contiguous data; 0 if MultispanUsed */

    struct {
        WORD64 EntryLocation;   /* FS-relative address of this span's data */
        WORD64 EntrySize;       /* byte count; 0 = unused entry (sentinel) */
    } MultispanEntry[16];

    WORD64 EncryptionInformation[4];

    union {
        WORD64 FlagRaw;
        struct {
            WORD64 Hidden       : 1;
            WORD64 System       : 1;
            WORD64 MultispanUsed: 1;
            WORD64 Reserved     : 61;
        };
    } Flags;

    struct {
        WORD16 UserOrGroupID;
        union {
            WORD32 FlagRaw;
            struct {
                WORD32 IDRepresentsGroup : 1;
                WORD32 Read              : 1;
                WORD32 Write             : 1;
                WORD32 Execute           : 1;
                WORD32 InUse             : 1;
                WORD32 Reserved          : 27;
            };
        } Flags;
    } SecurityInformation[8];   /* not implemented */

    WORD64 SecurityExtension;
    WORD64 FileExtension;

    WORD64 Reserved[32];
    WORD64 AbsolutePrevious, Me, AbsoluteNext;
    WORD64 DirPrevious, DirNext; /* files within the same directory */
} WOOFS_FILE, *PWOOFS_FILE;

typedef struct _WOOFS_GAP_ITEMS {
    WORD64 ChunkStart;  /* FS-relative address of free block */
    WORD64 ChunkSize;   /* size in bytes; 0 = unused entry (sentinel) */
} WOOFS_GAP_ITEMS, *PWOOFS_GAP_ITEMS;

typedef struct _WOOFS_GAP_INDEX {
    WOOFS_GAP_ITEMS Gaps[128];
    WORD64 Previous, Me, Next;
} WOOFS_GAP_INDEX, *PWOOFS_GAP_INDEX;

WORD64 WoofsHashString64(PSTR String);

void WoofsFormat(DiskRead_t ReadFn, DiskWrite_t WriteFn, WORD64 PartitionOffset, WORD64 PartitionSize);
void WoofsMount(DiskRead_t ReadFn, DiskWrite_t WriteFn, WORD64 PartitionOffset, PWOOFS_HEAD* OutFilesystem);
void WoofsShutdown(PWOOFS_HEAD Filesystem);
WORD64 WoofsGetLastError(PWOOFS_HEAD Filesystem);

WORD64 WoofsCreateDirectory(PWOOFS_HEAD Filesystem, PSTR Path);
WORD64 WoofsCreateFile(PWOOFS_HEAD Filesystem, PSTR Path);
void   WoofsDeleteFile(PWOOFS_HEAD Filesystem, WORD64 FileId);

WORD64 WoofsGetTotalFileCount(PWOOFS_HEAD Filesystem);
WORD64 WoofsGetFileIdByName(PWOOFS_HEAD Filesystem, PSTR Path);
PSTR   WoofsGetFileNameById(PWOOFS_HEAD Filesystem, WORD64 FileId);
PSTR   WoofsGetFilePathById(PWOOFS_HEAD Filesystem, WORD64 FileId);
WORD64 WoofsGetFileSize(PWOOFS_HEAD Filesystem, WORD64 FileId);

void WoofsReadFile(PWOOFS_HEAD Filesystem, WORD64 FileId, void* Buffer, WORD64 FileOffset, WORD64 AmountToCopy);
void WoofsWriteFile(PWOOFS_HEAD Filesystem, WORD64 FileId, void* Buffer, WORD64 FileOffset, WORD64 AmountToWrite);

void WoofsRefactorFilesystem(PWOOFS_HEAD Filesystem);

#endif
