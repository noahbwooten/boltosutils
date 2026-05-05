//
//  woofs.c
//  boltosutils
//
//  Created by Noah Wooten on 5/5/26.
//

#include "woofs.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define HeapAlloc malloc
#define HeapFree free
#define HeapRealloc realloc
#define KsMemSet memset
#define KsMemCpy memcpy

static unsigned long KsStrLen(const STR* s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void KsStrCpy(STR* dst, const STR* src) {
    while ((*dst++ = *src++));
}



/* -------------------------------------------------------------------------
   Low-level I/O
   All ReadFn/WriteFn addresses are absolute disk offsets. Every structure
   address stored in the FS is partition-relative; we add RelativeBase here.
   ------------------------------------------------------------------------- */

static void FsRead(PWOOFS_HEAD Fs, void *Buf, WORD64 FsAddr, WORD64 Size) {
    Fs->ReadFn(Buf, Fs->RelativeBase + FsAddr, Size);
}

static void FsWrite(PWOOFS_HEAD Fs, void *Buf, WORD64 FsAddr, WORD64 Size) {
    Fs->WriteFn(Buf, Fs->RelativeBase + FsAddr, Size);
}

static void FlushHead(PWOOFS_HEAD Fs) {
    FsWrite(Fs, Fs, 0, sizeof(WOOFS_HEAD));
}

/* -------------------------------------------------------------------------
   Gap management
   WOOFS_INDEX (~12 KB) and WOOFS_GAP_INDEX (~2 KB) exceed the 4 KB
   stack-probe threshold for the x86-64 Windows ABI used by UEFI. Every
   function that needs one of these structures heap-allocates it.

   All tracked gaps are guaranteed ≥ GAP_MIN. Fragments smaller than that
   produced by deletions or splits are silently discarded.
   ------------------------------------------------------------------------- */

#define GAP_MIN 4096ULL

static void AddGap(PWOOFS_HEAD Fs, WORD64 ChunkStart, WORD64 ChunkSize) {
    if (ChunkSize < GAP_MIN)
        return;

    PWOOFS_GAP_INDEX Buf = (PWOOFS_GAP_INDEX)HeapAlloc(sizeof(WOOFS_GAP_INDEX));
    WORD64 PageAddr = Fs->FirstGaps;
    WORD64 PrevAddr = 0;

    while (PageAddr) {
        FsRead(Fs, Buf, PageAddr, sizeof(WOOFS_GAP_INDEX));
        for (int I = 0; I < 128; I++) {
            if (Buf->Gaps[I].ChunkSize == 0) {
                Buf->Gaps[I].ChunkStart = ChunkStart;
                Buf->Gaps[I].ChunkSize  = ChunkSize;
                FsWrite(Fs, Buf, PageAddr, sizeof(WOOFS_GAP_INDEX));
                HeapFree(Buf);
                return;
            }
        }
        PrevAddr = PageAddr;
        PageAddr = Buf->Next;
    }

    /* No empty slot — allocate a new gap-index page at the frontier */
    WORD64 NewAddr = Fs->HighestAddress;
    Fs->HighestAddress += sizeof(WOOFS_GAP_INDEX);

    KsMemSet(Buf, 0, sizeof(WOOFS_GAP_INDEX));
    Buf->Gaps[0].ChunkStart = ChunkStart;
    Buf->Gaps[0].ChunkSize  = ChunkSize;
    Buf->Me       = NewAddr;
    Buf->Previous = PrevAddr;
    Buf->Next     = 0;
    FsWrite(Fs, Buf, NewAddr, sizeof(WOOFS_GAP_INDEX));

    if (PrevAddr) {
        FsRead(Fs, Buf, PrevAddr, sizeof(WOOFS_GAP_INDEX));
        Buf->Next = NewAddr;
        FsWrite(Fs, Buf, PrevAddr, sizeof(WOOFS_GAP_INDEX));
    } else {
        Fs->FirstGaps = NewAddr;
    }

    HeapFree(Buf);
    FlushHead(Fs);
}

/* First-fit. Returns FS-relative address of the allocated block, or 0.
   Any leftover ≥ GAP_MIN is returned to the gap list. */
static WORD64 FindGap(PWOOFS_HEAD Fs, WORD64 Size) {
    PWOOFS_GAP_INDEX Buf = (PWOOFS_GAP_INDEX)HeapAlloc(sizeof(WOOFS_GAP_INDEX));
    WORD64 PageAddr = Fs->FirstGaps;

    while (PageAddr) {
        FsRead(Fs, Buf, PageAddr, sizeof(WOOFS_GAP_INDEX));
        for (int I = 0; I < 128; I++) {
            if (Buf->Gaps[I].ChunkSize >= Size) {
                WORD64 Result   = Buf->Gaps[I].ChunkStart;
                WORD64 Leftover = Buf->Gaps[I].ChunkSize - Size;
                Buf->Gaps[I].ChunkStart = 0;
                Buf->Gaps[I].ChunkSize  = 0;
                FsWrite(Fs, Buf, PageAddr, sizeof(WOOFS_GAP_INDEX));
                HeapFree(Buf);
                if (Leftover >= GAP_MIN)
                    AddGap(Fs, Result + Size, Leftover);
                return Result;
            }
        }
        PageAddr = Buf->Next;
    }

    HeapFree(Buf);
    return 0;
}

/* Allocate Size bytes from a gap or from the filesystem frontier. */
static WORD64 AllocBlock(PWOOFS_HEAD Fs, WORD64 Size) {
    WORD64 Addr = FindGap(Fs, Size);
    if (!Addr) {
        Addr = Fs->HighestAddress;
        Fs->HighestAddress += Size;
    }
    return Addr;
}

/* -------------------------------------------------------------------------
   Index management
   FilePathHash == 0 is the empty-slot sentinel; fill-on-insert strategy.
   ------------------------------------------------------------------------- */

static WORD64 SearchIndex(PWOOFS_HEAD Fs, WORD64 PathHash, int *OutIsDir) {
    PWOOFS_INDEX Buf = (PWOOFS_INDEX)HeapAlloc(sizeof(WOOFS_INDEX));
    WORD64 PageAddr = Fs->FirstIndex;

    while (PageAddr) {
        FsRead(Fs, Buf, PageAddr, sizeof(WOOFS_INDEX));
        for (int I = 0; I < 512; I++) {
            if (Buf->Items[I].FilePathHash == PathHash) {
                if (OutIsDir) *OutIsDir = (int)Buf->Items[I].IsDirectory;
                WORD64 Loc = Buf->Items[I].FileLocation;
                HeapFree(Buf);
                return Loc;
            }
        }
        PageAddr = Buf->Next;
    }

    HeapFree(Buf);
    return 0;
}

static void AddIndexEntry(PWOOFS_HEAD Fs, WORD64 PathHash, WORD64 Location, int IsDir) {
    PWOOFS_INDEX Buf = (PWOOFS_INDEX)HeapAlloc(sizeof(WOOFS_INDEX));
    WORD64 PageAddr = Fs->FirstIndex;
    WORD64 PrevAddr = 0;

    while (PageAddr) {
        FsRead(Fs, Buf, PageAddr, sizeof(WOOFS_INDEX));
        for (int I = 0; I < 512; I++) {
            if (Buf->Items[I].FilePathHash == 0) {
                Buf->Items[I].FilePathHash = PathHash;
                Buf->Items[I].FileLocation = Location;
                Buf->Items[I].FlagRaw      = 0;
                Buf->Items[I].IsDirectory  = IsDir ? 1 : 0;
                FsWrite(Fs, Buf, PageAddr, sizeof(WOOFS_INDEX));
                HeapFree(Buf);
                return;
            }
        }
        PrevAddr = PageAddr;
        PageAddr = Buf->Next;
    }

    /* All pages full — allocate a new index page */
    WORD64 NewAddr = Fs->HighestAddress;
    Fs->HighestAddress += sizeof(WOOFS_INDEX);

    KsMemSet(Buf, 0, sizeof(WOOFS_INDEX));
    Buf->Items[0].FilePathHash = PathHash;
    Buf->Items[0].FileLocation = Location;
    Buf->Items[0].IsDirectory  = IsDir ? 1 : 0;
    Buf->Me       = NewAddr;
    Buf->Previous = PrevAddr;
    Buf->Next     = 0;
    FsWrite(Fs, Buf, NewAddr, sizeof(WOOFS_INDEX));

    if (PrevAddr) {
        FsRead(Fs, Buf, PrevAddr, sizeof(WOOFS_INDEX));
        Buf->Next = NewAddr;
        FsWrite(Fs, Buf, PrevAddr, sizeof(WOOFS_INDEX));
    } else {
        Fs->FirstIndex = NewAddr;
    }

    HeapFree(Buf);
    FlushHead(Fs);
}

/* Match by FileLocation so callers don't need to reconstruct the full-path
   hash (WOOFS_FILE only stores the local-name hash). */
static void RemoveIndexByLocation(PWOOFS_HEAD Fs, WORD64 Location) {
    PWOOFS_INDEX Buf = (PWOOFS_INDEX)HeapAlloc(sizeof(WOOFS_INDEX));
    WORD64 PageAddr = Fs->FirstIndex;

    while (PageAddr) {
        FsRead(Fs, Buf, PageAddr, sizeof(WOOFS_INDEX));
        for (int I = 0; I < 512; I++) {
            if (Buf->Items[I].FileLocation == Location) {
                Buf->Items[I].FilePathHash = 0;
                Buf->Items[I].FileLocation = 0;
                Buf->Items[I].FlagRaw      = 0;
                FsWrite(Fs, Buf, PageAddr, sizeof(WOOFS_INDEX));
                HeapFree(Buf);
                return;
            }
        }
        PageAddr = Buf->Next;
    }

    HeapFree(Buf);
}

/* -------------------------------------------------------------------------
   Linked-list lookup helpers
   ------------------------------------------------------------------------- */

static WORD64 FindFileAddrById(PWOOFS_HEAD Fs, WORD64 FileId) {
    WORD64 Addr = Fs->FirstFile;
    WOOFS_FILE F;
    while (Addr) {
        FsRead(Fs, &F, Addr, sizeof(WOOFS_FILE));
        if (F.FileId == FileId) return Addr;
        Addr = F.AbsoluteNext;
    }
    return 0;
}

static WORD64 FindDirAddrById(PWOOFS_HEAD Fs, WORD64 DirId) {
    WORD64 Addr = Fs->FirstDirectory;
    WOOFS_DIRECTORY D;
    while (Addr) {
        FsRead(Fs, &D, Addr, sizeof(WOOFS_DIRECTORY));
        if (D.DirectoryId == DirId) return Addr;
        Addr = D.AbsoluteNext;
    }
    return 0;
}

/* -------------------------------------------------------------------------
   Path helpers (wide strings, STR = short)
   ------------------------------------------------------------------------- */

/* "/opt/test.txt"  → ParentOut="/opt",  NameOut="test.txt"
   "/test.txt"      → ParentOut="/",     NameOut="test.txt"  */
static void ParsePath(PCSTR FullPath, PSTR ParentOut, PSTR NameOut) {
    int Len = KsStrLen(FullPath);
    int LastSlash = 0;
    for (int I = Len - 1; I > 0; I--) {
        if (FullPath[I] == (STR)'/') { LastSlash = I; break; }
    }

    if (LastSlash == 0) {
        ParentOut[0] = (STR)'/';
        ParentOut[1] = 0;
        KsStrCpy(NameOut, (PCSTR)(FullPath + 1));
    } else {
        for (int I = 0; I < LastSlash; I++)
            ParentOut[I] = FullPath[I];
        ParentOut[LastSlash] = 0;
        KsStrCpy(NameOut, (PCSTR)(FullPath + LastSlash + 1));
    }
}

/* -------------------------------------------------------------------------
   Format
   Initial partition layout:
     [0]                 WOOFS_HEAD
     [+HEAD]             WOOFS_INDEX      (first page)
     [+INDEX]            WOOFS_GAP_INDEX  (first page)
     [+GAP_INDEX]        WOOFS_DIRECTORY  (root "/")
     HighestAddress points here.
   ------------------------------------------------------------------------- */

void WoofsFormat(DiskRead_t ReadFn, DiskWrite_t WriteFn,
                 WORD64 PartitionOffset, WORD64 PartitionSize) {
    PWOOFS_HEAD Fs = (PWOOFS_HEAD)HeapAlloc(sizeof(WOOFS_HEAD));
    KsMemSet(Fs, 0, sizeof(WOOFS_HEAD));
    Fs->Magic          = WOOFS_MAGIC;
    Fs->RelativeBase   = PartitionOffset;
    Fs->PartitionSize  = PartitionSize;
    Fs->ReadFn         = ReadFn;
    Fs->WriteFn        = WriteFn;
    Fs->NextAvailableFileId      = 1;
    Fs->NextAvailableDirectoryId = 1;

    WORD64 Cursor = sizeof(WOOFS_HEAD);

    WORD64 IndexAddr    = Cursor; Fs->FirstIndex    = IndexAddr; Cursor += sizeof(WOOFS_INDEX);
    WORD64 GapAddr      = Cursor; Fs->FirstGaps     = GapAddr;   Cursor += sizeof(WOOFS_GAP_INDEX);
    WORD64 RootAddr     = Cursor; Fs->FirstDirectory= RootAddr;  Cursor += sizeof(WOOFS_DIRECTORY);
    Fs->HighestAddress  = Cursor;

    FlushHead(Fs);

    PWOOFS_INDEX IdxPage = (PWOOFS_INDEX)HeapAlloc(sizeof(WOOFS_INDEX));
    KsMemSet(IdxPage, 0, sizeof(WOOFS_INDEX));
    IdxPage->Me = IndexAddr;
    FsWrite(Fs, IdxPage, IndexAddr, sizeof(WOOFS_INDEX));
    HeapFree(IdxPage);

    PWOOFS_GAP_INDEX GapPage = (PWOOFS_GAP_INDEX)HeapAlloc(sizeof(WOOFS_GAP_INDEX));
    KsMemSet(GapPage, 0, sizeof(WOOFS_GAP_INDEX));
    GapPage->Me = GapAddr;
    FsWrite(Fs, GapPage, GapAddr, sizeof(WOOFS_GAP_INDEX));
    HeapFree(GapPage);

    WOOFS_DIRECTORY Root;
    KsMemSet(&Root, 0, sizeof(WOOFS_DIRECTORY));
    Root.DirectoryName[0] = (STR)'/';
    Root.DirectoryName[1] = 0;
    Root.PathHash         = WoofsHashString64(Root.DirectoryName);
    Root.ParentDirectory  = 0xFFFFFFFFFFFFFFFFULL;
    Root.DirectoryId      = Fs->NextAvailableDirectoryId++;
    Root.Me               = RootAddr;
    FsWrite(Fs, &Root, RootAddr, sizeof(WOOFS_DIRECTORY));

    STR RootPath[2] = { (STR)'/', 0 };
    AddIndexEntry(Fs, WoofsHashString64(RootPath), RootAddr, 1);

    FlushHead(Fs);
    HeapFree(Fs);
}

/* -------------------------------------------------------------------------
   Mount / Shutdown
   ------------------------------------------------------------------------- */

void WoofsMount(DiskRead_t ReadFn, DiskWrite_t WriteFn,
                WORD64 PartitionOffset, PWOOFS_HEAD *OutFilesystem) {
    PWOOFS_HEAD Fs = (PWOOFS_HEAD)HeapAlloc(sizeof(WOOFS_HEAD));
    ReadFn(Fs, PartitionOffset, sizeof(WOOFS_HEAD));

    Fs->RelativeBase = PartitionOffset;
    Fs->LastError    = WOOFS_ERROR_NONE;
    Fs->ReadFn       = ReadFn;
    Fs->WriteFn      = WriteFn;

    if (Fs->Magic != WOOFS_MAGIC)
        Fs->LastError = WOOFS_ERROR_BAD_MAGIC;

    *OutFilesystem = Fs;
}

void WoofsShutdown(PWOOFS_HEAD Filesystem) {
    FlushHead(Filesystem);
    HeapFree(Filesystem);
}

WORD64 WoofsGetLastError(PWOOFS_HEAD Filesystem) {
    return Filesystem->LastError;
}

/* -------------------------------------------------------------------------
   Directory creation
   ------------------------------------------------------------------------- */

WORD64 WoofsCreateDirectory(PWOOFS_HEAD Filesystem, PSTR Path) {
    if (!Path || Path[0] != (STR)'/') {
        Filesystem->LastError = WOOFS_ERROR_INVALID_ARGS;
        return 0xFFFFFFFFFFFFFFFFULL;
    }
    if (Path[0] == (STR)'/' && Path[1] == 0) {
        Filesystem->LastError = WOOFS_ERROR_ALREADY_EXISTS;
        return 0xFFFFFFFFFFFFFFFFULL;
    }

    WORD64 FullHash = WoofsHashString64(Path);
    int IsDir;
    if (SearchIndex(Filesystem, FullHash, &IsDir)) {
        Filesystem->LastError = WOOFS_ERROR_ALREADY_EXISTS;
        return 0xFFFFFFFFFFFFFFFFULL;
    }

    STR ParentPath[256], LocalName[64];
    ParsePath((PCSTR)Path, ParentPath, LocalName);

    WORD64 ParentHash = WoofsHashString64(ParentPath);
    WORD64 ParentAddr = SearchIndex(Filesystem, ParentHash, &IsDir);
    if (!ParentAddr || !IsDir) {
        Filesystem->LastError = WOOFS_ERROR_DIR_NOT_FOUND;
        return 0xFFFFFFFFFFFFFFFFULL;
    }
    WOOFS_DIRECTORY Parent;
    FsRead(Filesystem, &Parent, ParentAddr, sizeof(WOOFS_DIRECTORY));

    WORD64 DirAddr = AllocBlock(Filesystem, sizeof(WOOFS_DIRECTORY));

    WOOFS_DIRECTORY Dir;
    KsMemSet(&Dir, 0, sizeof(WOOFS_DIRECTORY));
    KsStrCpy(Dir.DirectoryName, (PCSTR)LocalName);
    Dir.PathHash        = WoofsHashString64(LocalName);
    Dir.ParentDirectory = Parent.DirectoryId;
    Dir.DirectoryId     = Filesystem->NextAvailableDirectoryId++;
    Dir.Me              = DirAddr;

    /* Append to the global directory list */
    WORD64 LastDirAddr = 0, Cur = Filesystem->FirstDirectory;
    WOOFS_DIRECTORY Tmp;
    while (Cur) {
        FsRead(Filesystem, &Tmp, Cur, sizeof(WOOFS_DIRECTORY));
        LastDirAddr = Cur;
        Cur = Tmp.AbsoluteNext;
    }
    if (LastDirAddr) {
        FsRead(Filesystem, &Tmp, LastDirAddr, sizeof(WOOFS_DIRECTORY));
        Tmp.AbsoluteNext     = DirAddr;
        FsWrite(Filesystem, &Tmp, LastDirAddr, sizeof(WOOFS_DIRECTORY));
        Dir.AbsolutePrevious = LastDirAddr;
    } else {
        Filesystem->FirstDirectory = DirAddr;
    }

    /* Link as the last sibling under the same parent */
    WORD64 LastSibAddr = 0;
    Cur = Filesystem->FirstDirectory;
    while (Cur) {
        FsRead(Filesystem, &Tmp, Cur, sizeof(WOOFS_DIRECTORY));
        if (Tmp.ParentDirectory == Parent.DirectoryId && Cur != DirAddr)
            LastSibAddr = Cur;
        Cur = Tmp.AbsoluteNext;
    }
    if (LastSibAddr) {
        FsRead(Filesystem, &Tmp, LastSibAddr, sizeof(WOOFS_DIRECTORY));
        Tmp.DirNext     = DirAddr;
        FsWrite(Filesystem, &Tmp, LastSibAddr, sizeof(WOOFS_DIRECTORY));
        Dir.DirPrevious = LastSibAddr;
    }

    FsWrite(Filesystem, &Dir, DirAddr, sizeof(WOOFS_DIRECTORY));
    AddIndexEntry(Filesystem, FullHash, DirAddr, 1);
    FlushHead(Filesystem);

    Filesystem->LastError = WOOFS_ERROR_NONE;
    return Dir.DirectoryId;
}

/* -------------------------------------------------------------------------
   File creation
   ------------------------------------------------------------------------- */

WORD64 WoofsCreateFile(PWOOFS_HEAD Filesystem, PSTR Path) {
    if (!Path || Path[0] != (STR)'/') {
        Filesystem->LastError = WOOFS_ERROR_INVALID_ARGS;
        return 0xFFFFFFFFFFFFFFFFULL;
    }

    WORD64 FullHash = WoofsHashString64(Path);
    int IsDir;
    if (SearchIndex(Filesystem, FullHash, &IsDir)) {
        Filesystem->LastError = WOOFS_ERROR_ALREADY_EXISTS;
        return 0xFFFFFFFFFFFFFFFFULL;
    }

    STR ParentPath[256], LocalName[64];
    ParsePath((PCSTR)Path, ParentPath, LocalName);

    WORD64 ParentHash = WoofsHashString64(ParentPath);
    WORD64 ParentAddr = SearchIndex(Filesystem, ParentHash, &IsDir);
    if (!ParentAddr || !IsDir) {
        Filesystem->LastError = WOOFS_ERROR_DIR_NOT_FOUND;
        return 0xFFFFFFFFFFFFFFFFULL;
    }
    WOOFS_DIRECTORY Parent;
    FsRead(Filesystem, &Parent, ParentAddr, sizeof(WOOFS_DIRECTORY));

    WORD64 FileAddr = AllocBlock(Filesystem, sizeof(WOOFS_FILE));

    WOOFS_FILE File;
    KsMemSet(&File, 0, sizeof(WOOFS_FILE));
    KsStrCpy(File.FileName, (PCSTR)LocalName);
    File.FileId          = Filesystem->NextAvailableFileId++;
    File.FilePathHash    = WoofsHashString64(LocalName);
    File.ParentDirectory = Parent.DirectoryId;
    File.Me              = FileAddr;

    /* Append to the global file list */
    WORD64 LastFileAddr = 0, Cur = Filesystem->FirstFile;
    WOOFS_FILE Tmp;
    while (Cur) {
        FsRead(Filesystem, &Tmp, Cur, sizeof(WOOFS_FILE));
        LastFileAddr = Cur;
        Cur = Tmp.AbsoluteNext;
    }
    if (LastFileAddr) {
        FsRead(Filesystem, &Tmp, LastFileAddr, sizeof(WOOFS_FILE));
        Tmp.AbsoluteNext      = FileAddr;
        FsWrite(Filesystem, &Tmp, LastFileAddr, sizeof(WOOFS_FILE));
        File.AbsolutePrevious = LastFileAddr;
    } else {
        Filesystem->FirstFile = FileAddr;
    }

    /* Link into the parent directory's file list */
    if (!Parent.FirstFile) {
        Parent.FirstFile = FileAddr;
        FsWrite(Filesystem, &Parent, ParentAddr, sizeof(WOOFS_DIRECTORY));
    } else {
        WORD64 LastDirFileAddr = 0;
        Cur = Parent.FirstFile;
        while (Cur) {
            FsRead(Filesystem, &Tmp, Cur, sizeof(WOOFS_FILE));
            LastDirFileAddr = Cur;
            Cur = Tmp.DirNext;
        }
        FsRead(Filesystem, &Tmp, LastDirFileAddr, sizeof(WOOFS_FILE));
        Tmp.DirNext      = FileAddr;
        FsWrite(Filesystem, &Tmp, LastDirFileAddr, sizeof(WOOFS_FILE));
        File.DirPrevious = LastDirFileAddr;
    }

    FsWrite(Filesystem, &File, FileAddr, sizeof(WOOFS_FILE));
    AddIndexEntry(Filesystem, FullHash, FileAddr, 0);
    FlushHead(Filesystem);

    Filesystem->LastError = WOOFS_ERROR_NONE;
    return File.FileId;
}

/* -------------------------------------------------------------------------
   File growth (internal)
   Updates *File in memory and on disk. FileAddr is the FS-relative address
   of the WOOFS_FILE structure itself.
   ------------------------------------------------------------------------- */

static int GrowFile(PWOOFS_HEAD Fs, WORD64 FileAddr, WOOFS_FILE *File, WORD64 NewSize) {
    WORD64 Extra = NewSize - File->FileSize;

    if (File->FileSize == 0) {
        WORD64 DataAddr = AllocBlock(Fs, NewSize);
        File->RawData   = DataAddr;
        File->FileSize  = NewSize;
        FsWrite(Fs, File, FileAddr, sizeof(WOOFS_FILE));
        FlushHead(Fs);
        return 0;
    }

    if (!File->Flags.MultispanUsed) {
        /* Add a second span and promote to multispan */
        WORD64 ExtAddr = FindGap(Fs, Extra >= GAP_MIN ? Extra : GAP_MIN);
        if (!ExtAddr) {
            ExtAddr = Fs->HighestAddress;
            Fs->HighestAddress += (Extra >= GAP_MIN ? Extra : GAP_MIN);
        }
        File->MultispanEntry[0].EntryLocation = File->RawData;
        File->MultispanEntry[0].EntrySize     = File->FileSize;
        File->MultispanEntry[1].EntryLocation = ExtAddr;
        File->MultispanEntry[1].EntrySize     = Extra;
        File->RawData             = 0;
        File->Flags.MultispanUsed = 1;
        File->FileSize            = NewSize;
        FsWrite(Fs, File, FileAddr, sizeof(WOOFS_FILE));
        FlushHead(Fs);
        return 0;
    }

    /* Count used spans */
    int SpanCount = 0;
    for (int I = 0; I < 16; I++)
        if (File->MultispanEntry[I].EntrySize > 0) SpanCount++;

    if (SpanCount < 16) {
        WORD64 ExtAddr = FindGap(Fs, Extra >= GAP_MIN ? Extra : GAP_MIN);
        if (!ExtAddr) {
            ExtAddr = Fs->HighestAddress;
            Fs->HighestAddress += (Extra >= GAP_MIN ? Extra : GAP_MIN);
        }
        for (int I = 0; I < 16; I++) {
            if (File->MultispanEntry[I].EntrySize == 0) {
                File->MultispanEntry[I].EntryLocation = ExtAddr;
                File->MultispanEntry[I].EntrySize     = Extra;
                break;
            }
        }
        File->FileSize = NewSize;
        FsWrite(Fs, File, FileAddr, sizeof(WOOFS_FILE));
        FlushHead(Fs);
        return 0;
    }

    /* All 16 spans used — consolidate into one contiguous block */
    WORD64 NewDataAddr = FindGap(Fs, NewSize);
    if (!NewDataAddr) {
        NewDataAddr = Fs->HighestAddress;
        Fs->HighestAddress += NewSize;
    }

    void *Buf = HeapAlloc(65536);
    if (!Buf) {
        Fs->LastError = WOOFS_ERROR_OUT_OF_MEMORY;
        return -1;
    }

    WORD64 WriteOff = 0;
    for (int I = 0; I < 16; I++) {
        if (!File->MultispanEntry[I].EntrySize) continue;
        WORD64 Rem = File->MultispanEntry[I].EntrySize, Src = 0;
        while (Rem) {
            WORD64 Chunk = Rem > 65536 ? 65536 : Rem;
            FsRead(Fs, Buf, File->MultispanEntry[I].EntryLocation + Src, Chunk);
            FsWrite(Fs, Buf, NewDataAddr + WriteOff, Chunk);
            Src += Chunk; WriteOff += Chunk; Rem -= Chunk;
        }
        AddGap(Fs, File->MultispanEntry[I].EntryLocation, File->MultispanEntry[I].EntrySize);
    }
    HeapFree(Buf);

    KsMemSet(File->MultispanEntry, 0, sizeof(File->MultispanEntry));
    File->RawData             = NewDataAddr;
    File->Flags.MultispanUsed = 0;
    File->FileSize            = NewSize;
    FsWrite(Fs, File, FileAddr, sizeof(WOOFS_FILE));
    FlushHead(Fs);
    return 0;
}

/* -------------------------------------------------------------------------
   Read / Write
   ------------------------------------------------------------------------- */

void WoofsReadFile(PWOOFS_HEAD Filesystem, WORD64 FileId,
                   void *Buffer, WORD64 FileOffset, WORD64 AmountToCopy) {
    WORD64 FileAddr = FindFileAddrById(Filesystem, FileId);
    if (!FileAddr) { Filesystem->LastError = WOOFS_ERROR_FILE_NOT_FOUND; return; }

    WOOFS_FILE File;
    FsRead(Filesystem, &File, FileAddr, sizeof(WOOFS_FILE));

    if (FileOffset >= File.FileSize) return;
    if (FileOffset + AmountToCopy > File.FileSize)
        AmountToCopy = File.FileSize - FileOffset;

    if (!File.Flags.MultispanUsed) {
        FsRead(Filesystem, Buffer, File.RawData + FileOffset, AmountToCopy);
        return;
    }

    WORD64 LogicalBase = 0, Copied = 0;
    WORD8 *Out = (WORD8*)Buffer;

    for (int I = 0; I < 16 && Copied < AmountToCopy; I++) {
        if (!File.MultispanEntry[I].EntrySize) continue;
        WORD64 SpanEnd = LogicalBase + File.MultispanEntry[I].EntrySize;

        if (FileOffset < SpanEnd && (FileOffset + AmountToCopy) > LogicalBase) {
            WORD64 ReadFrom = (FileOffset > LogicalBase) ? (FileOffset - LogicalBase) : 0;
            WORD64 ToRead   = SpanEnd - (LogicalBase + ReadFrom);
            if (Copied + ToRead > AmountToCopy) ToRead = AmountToCopy - Copied;

            FsRead(Filesystem, Out + Copied,
                   File.MultispanEntry[I].EntryLocation + ReadFrom, ToRead);
            Copied += ToRead; FileOffset += ToRead;
        }
        LogicalBase = SpanEnd;
    }
}

void WoofsWriteFile(PWOOFS_HEAD Filesystem, WORD64 FileId,
                    void *Buffer, WORD64 FileOffset, WORD64 AmountToWrite) {
    WORD64 FileAddr = FindFileAddrById(Filesystem, FileId);
    if (!FileAddr) { Filesystem->LastError = WOOFS_ERROR_FILE_NOT_FOUND; return; }

    WOOFS_FILE File;
    FsRead(Filesystem, &File, FileAddr, sizeof(WOOFS_FILE));

    if (FileOffset + AmountToWrite > File.FileSize)
        if (GrowFile(Filesystem, FileAddr, &File, FileOffset + AmountToWrite) < 0) return;

    if (!File.Flags.MultispanUsed) {
        FsWrite(Filesystem, Buffer, File.RawData + FileOffset, AmountToWrite);
        return;
    }

    WORD64 LogicalBase = 0, Written = 0;
    WORD8 *In = (WORD8*)Buffer;

    for (int I = 0; I < 16 && Written < AmountToWrite; I++) {
        if (!File.MultispanEntry[I].EntrySize) continue;
        WORD64 SpanEnd = LogicalBase + File.MultispanEntry[I].EntrySize;

        if (FileOffset < SpanEnd && (FileOffset + AmountToWrite) > LogicalBase) {
            WORD64 WriteFrom = (FileOffset > LogicalBase) ? (FileOffset - LogicalBase) : 0;
            WORD64 ToWrite   = SpanEnd - (LogicalBase + WriteFrom);
            if (Written + ToWrite > AmountToWrite) ToWrite = AmountToWrite - Written;

            FsWrite(Filesystem, In + Written,
                    File.MultispanEntry[I].EntryLocation + WriteFrom, ToWrite);
            Written += ToWrite; FileOffset += ToWrite;
        }
        LogicalBase = SpanEnd;
    }
}

/* -------------------------------------------------------------------------
   Delete
   ------------------------------------------------------------------------- */

void WoofsDeleteFile(PWOOFS_HEAD Filesystem, WORD64 FileId) {
    WORD64 FileAddr = FindFileAddrById(Filesystem, FileId);
    if (!FileAddr) { Filesystem->LastError = WOOFS_ERROR_FILE_NOT_FOUND; return; }

    /* Heap-allocate to avoid a >4 KB stack frame at -O0 */
    PWOOFS_FILE File = (PWOOFS_FILE)HeapAlloc(sizeof(WOOFS_FILE));
    PWOOFS_FILE Tmp  = (PWOOFS_FILE)HeapAlloc(sizeof(WOOFS_FILE));
    FsRead(Filesystem, File, FileAddr, sizeof(WOOFS_FILE));

    if (!File->Flags.MultispanUsed) {
        if (File->RawData) AddGap(Filesystem, File->RawData, File->FileSize);
    } else {
        for (int I = 0; I < 16; I++)
            if (File->MultispanEntry[I].EntrySize)
                AddGap(Filesystem, File->MultispanEntry[I].EntryLocation,
                                   File->MultispanEntry[I].EntrySize);
    }

    /* Unlink from global file list */
    if (File->AbsolutePrevious) {
        FsRead(Filesystem, Tmp, File->AbsolutePrevious, sizeof(WOOFS_FILE));
        Tmp->AbsoluteNext = File->AbsoluteNext;
        FsWrite(Filesystem, Tmp, File->AbsolutePrevious, sizeof(WOOFS_FILE));
    } else {
        Filesystem->FirstFile = File->AbsoluteNext;
    }
    if (File->AbsoluteNext) {
        FsRead(Filesystem, Tmp, File->AbsoluteNext, sizeof(WOOFS_FILE));
        Tmp->AbsolutePrevious = File->AbsolutePrevious;
        FsWrite(Filesystem, Tmp, File->AbsoluteNext, sizeof(WOOFS_FILE));
    }

    /* Unlink from per-directory file list */
    if (File->DirPrevious) {
        FsRead(Filesystem, Tmp, File->DirPrevious, sizeof(WOOFS_FILE));
        Tmp->DirNext = File->DirNext;
        FsWrite(Filesystem, Tmp, File->DirPrevious, sizeof(WOOFS_FILE));
    } else {
        WORD64 ParentAddr = FindDirAddrById(Filesystem, File->ParentDirectory);
        if (ParentAddr) {
            PWOOFS_DIRECTORY Parent = (PWOOFS_DIRECTORY)HeapAlloc(sizeof(WOOFS_DIRECTORY));
            FsRead(Filesystem, Parent, ParentAddr, sizeof(WOOFS_DIRECTORY));
            Parent->FirstFile = File->DirNext;
            FsWrite(Filesystem, Parent, ParentAddr, sizeof(WOOFS_DIRECTORY));
            HeapFree(Parent);
        }
    }
    if (File->DirNext) {
        FsRead(Filesystem, Tmp, File->DirNext, sizeof(WOOFS_FILE));
        Tmp->DirPrevious = File->DirPrevious;
        FsWrite(Filesystem, Tmp, File->DirNext, sizeof(WOOFS_FILE));
    }

    HeapFree(Tmp);
    HeapFree(File);

    RemoveIndexByLocation(Filesystem, FileAddr);
    AddGap(Filesystem, FileAddr, sizeof(WOOFS_FILE));
    FlushHead(Filesystem);
    Filesystem->LastError = WOOFS_ERROR_NONE;
}

/* -------------------------------------------------------------------------
   Query
   ------------------------------------------------------------------------- */

WORD64 WoofsGetTotalFileCount(PWOOFS_HEAD Filesystem) {
    WORD64 Count = 0, Addr = Filesystem->FirstFile;
    WOOFS_FILE F;
    while (Addr) {
        FsRead(Filesystem, &F, Addr, sizeof(WOOFS_FILE));
        Count++; Addr = F.AbsoluteNext;
    }
    return Count;
}

WORD64 WoofsGetFileIdByName(PWOOFS_HEAD Filesystem, PSTR Path) {
    int IsDir;
    WORD64 Location = SearchIndex(Filesystem, WoofsHashString64(Path), &IsDir);
    if (!Location || IsDir) {
        Filesystem->LastError = WOOFS_ERROR_FILE_NOT_FOUND;
        return 0xFFFFFFFFFFFFFFFFULL;
    }
    WOOFS_FILE File;
    FsRead(Filesystem, &File, Location, sizeof(WOOFS_FILE));
    Filesystem->LastError = WOOFS_ERROR_NONE;
    return File.FileId;
}

/* Returns a heap-allocated local filename. Caller must HeapFree the result. */
PSTR WoofsGetFileNameById(PWOOFS_HEAD Filesystem, WORD64 FileId) {
    WORD64 FileAddr = FindFileAddrById(Filesystem, FileId);
    if (!FileAddr) { Filesystem->LastError = WOOFS_ERROR_FILE_NOT_FOUND; return (PSTR)0; }

    WOOFS_FILE File;
    FsRead(Filesystem, &File, FileAddr, sizeof(WOOFS_FILE));

    PSTR Out = (PSTR)HeapAlloc(64 * sizeof(STR));
    KsStrCpy(Out, (PCSTR)File.FileName);
    Filesystem->LastError = WOOFS_ERROR_NONE;
    return Out;
}

/* Returns a heap-allocated full path. Caller must HeapFree the result. */
PSTR WoofsGetFilePathById(PWOOFS_HEAD Filesystem, WORD64 FileId) {
    WORD64 FileAddr = FindFileAddrById(Filesystem, FileId);
    if (!FileAddr) { Filesystem->LastError = WOOFS_ERROR_FILE_NOT_FOUND; return (PSTR)0; }

    WOOFS_FILE File;
    FsRead(Filesystem, &File, FileAddr, sizeof(WOOFS_FILE));

    /* Walk directory chain from leaf to root, collecting names */
    STR  Parts[16][64];
    int  PartCount = 0;
    WORD64 DirId = File.ParentDirectory;

    while (PartCount < 16) {
        WORD64 DirAddr = FindDirAddrById(Filesystem, DirId);
        if (!DirAddr) break;
        WOOFS_DIRECTORY Dir;
        FsRead(Filesystem, &Dir, DirAddr, sizeof(WOOFS_DIRECTORY));
        if (Dir.ParentDirectory == 0xFFFFFFFFFFFFFFFFULL) break; /* reached root */
        KsStrCpy((PSTR)Parts[PartCount++], (PCSTR)Dir.DirectoryName);
        DirId = Dir.ParentDirectory;
    }

    /* 16 dirs × 65 chars + filename 64 + separators + null */
    PSTR Path = (PSTR)HeapAlloc(1088 * sizeof(STR));
    if (!Path) { Filesystem->LastError = WOOFS_ERROR_OUT_OF_MEMORY; return (PSTR)0; }

    int Pos = 0;
    Path[Pos++] = (STR)'/';
    for (int I = PartCount - 1; I >= 0; I--) {
        int Len = KsStrLen((PCSTR)Parts[I]);
        for (int J = 0; J < Len; J++) Path[Pos++] = Parts[I][J];
        Path[Pos++] = (STR)'/';
    }
    int FnLen = KsStrLen((PCSTR)File.FileName);
    for (int J = 0; J < FnLen; J++) Path[Pos++] = File.FileName[J];
    Path[Pos] = 0;

    Filesystem->LastError = WOOFS_ERROR_NONE;
    return Path;
}

WORD64 WoofsGetFileSize(PWOOFS_HEAD Filesystem, WORD64 FileId) {
    WORD64 FileAddr = FindFileAddrById(Filesystem, FileId);
    if (!FileAddr) { Filesystem->LastError = WOOFS_ERROR_FILE_NOT_FOUND; return 0xFFFFFFFFFFFFFFFFULL; }
    WOOFS_FILE File;
    FsRead(Filesystem, &File, FileAddr, sizeof(WOOFS_FILE));
    Filesystem->LastError = WOOFS_ERROR_NONE;
    return File.FileSize;
}

/* -------------------------------------------------------------------------
   Refactor / defragment
   Consolidates multispan files that have at least one span < 1 MiB into a
   single contiguous block, but only when a large enough gap already exists.
   Gratuitous moves (e.g. shifting contiguous files to close gaps) are
   intentionally avoided to minimize write amplification and disk wear.
   ------------------------------------------------------------------------- */

#define REFACTOR_SPAN_THRESHOLD (1024ULL * 1024ULL)

void WoofsRefactorFilesystem(PWOOFS_HEAD Filesystem) {
    WORD64 Addr = Filesystem->FirstFile;
    WOOFS_FILE File;

    while (Addr) {
        FsRead(Filesystem, &File, Addr, sizeof(WOOFS_FILE));
        WORD64 Next = File.AbsoluteNext;

        if (File.Flags.MultispanUsed) {
            int SmallSpans = 0, TotalSpans = 0;
            for (int I = 0; I < 16; I++) {
                if (!File.MultispanEntry[I].EntrySize) continue;
                    TotalSpans++;
                if (File.MultispanEntry[I].EntrySize < REFACTOR_SPAN_THRESHOLD) SmallSpans++;
            }

            if (SmallSpans > 0 && TotalSpans > 1) {
                WORD64 NewDataAddr = FindGap(Filesystem, File.FileSize);
                if (NewDataAddr) {
                    void *Buf = HeapAlloc(65536);
                    if (Buf) {
                        WORD64 WriteOff = 0;
                        for (int I = 0; I < 16; I++) {
                            if (!File.MultispanEntry[I].EntrySize) continue;
                            WORD64 Rem = File.MultispanEntry[I].EntrySize, Src = 0;
                            while (Rem) {
                                WORD64 Chunk = Rem > 65536 ? 65536 : Rem;
                                FsRead(Filesystem, Buf,
                                       File.MultispanEntry[I].EntryLocation + Src, Chunk);
                                FsWrite(Filesystem, Buf, NewDataAddr + WriteOff, Chunk);
                                Src += Chunk; WriteOff += Chunk; Rem -= Chunk;
                            }
                            AddGap(Filesystem, File.MultispanEntry[I].EntryLocation,
                                              File.MultispanEntry[I].EntrySize);
                        }
                        HeapFree(Buf);
                        KsMemSet(File.MultispanEntry, 0, sizeof(File.MultispanEntry));
                        File.RawData             = NewDataAddr;
                        File.Flags.MultispanUsed = 0;
                        FsWrite(Filesystem, &File, Addr, sizeof(WOOFS_FILE));
                    }
                }
            }
        }

        Addr = Next;
    }

    FlushHead(Filesystem);
}
