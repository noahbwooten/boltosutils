//
//  filesystem.c
//  boltosutils
//
//  Created by Noah Wooten on 5/5/26.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "woofs/woofs.h"

/*
 Disk I/O callbacks for woofs — backed by a host FILE*.
 The FILE* is stored in a static global since the woofs callbacks
 have a fixed signature (no user-data pointer).
 */
static FILE* DiskFile = NULL;

static void DiskReadCb(void* Buffer, WORD64 Address, WORD64 Size) {
    fseek(DiskFile, (long)Address, SEEK_SET);
    fread(Buffer, 1, (size_t)Size, DiskFile);
}

static void DiskWriteCb(void* Buffer, WORD64 Address, WORD64 Size) {
    fseek(DiskFile, (long)Address, SEEK_SET);
    fwrite(Buffer, 1, (size_t)Size, DiskFile);
    fflush(DiskFile);
}

/*
 STR helpers — woofs uses STR (signed short, 2 bytes) as its character type.
 On macOS wchar_t is 4 bytes, so we cannot use wcslen/wcscpy directly.
 These helpers convert between char* (CLI) and STR* (woofs).
 */
static int StrLen16(const STR* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void AsciiToStr(STR* dst, const char* src) {
    while (*src) {
        *dst++ = (STR)(unsigned char)*src++;
    }
    *dst = 0;
}

/*
 Ensure the image file is at least `MinSize` bytes by zero-extending.
 woofs writes at arbitrary offsets; the file must be large enough.
 */
static void EnsureFileSize(WORD64 MinSize) {
    fseek(DiskFile, 0, SEEK_END);
    long Current = ftell(DiskFile);
    if ((WORD64)Current < MinSize) {
        char Zero = 0;
        fseek(DiskFile, (long)(MinSize - 1), SEEK_SET);
        fwrite(&Zero, 1, 1, DiskFile);
        fflush(DiskFile);
    }
}

/*
 Recursively create all directories along a path.
 For "/test/cfg.bmp", this ensures "/" and "/test" exist as directories.
 `FullPath` is the complete fs-internal path (e.g., "/test/cfg.bmp").
 We create every intermediate directory component that doesn't exist yet.
 */
static int EnsureDirectories(PWOOFS_HEAD Fs, const char* Path) {
    /* Walk the path and create each directory component */
    int Len = (int)strlen(Path);
    char Buf[512];
    STR WideBuf[256];

    for (int i = 1; i < Len; i++) {
        if (Path[i] == '/') {
            memcpy(Buf, Path, i);
            Buf[i] = '\0';

            AsciiToStr(WideBuf, Buf);
            WORD64 Result = WoofsCreateDirectory(Fs, WideBuf);
            WORD64 Err = WoofsGetLastError(Fs);

            if (Result == 0xFFFFFFFFFFFFFFFFULL && Err != WOOFS_ERROR_ALREADY_EXISTS) {
                printf("[ERR]: Failed to create directory '%s' (error 0x%lx)\n", Buf, Err);
                return -1;
            }
        }
    }
    return 0;
}

static void PrintUsage(void) {
    printf("boltosutils filesystem help                              : Display this help\n");
    printf("boltosutils filesystem format <image> <size_mb>          : Create a new filesystem image\n");
    printf("boltosutils filesystem add <image> <localfile> <fspath>  : Add a local file into the image\n");
    printf("boltosutils filesystem list <image>                      : List all files in the image\n");
    printf("boltosutils filesystem extract <image> <fspath> <outfile>: Extract a file from the image\n");
}

static void FsFormat(int argc, char** argv) {
    if (argc < 5) {
        printf("[ERR]: Usage: boltosutils filesystem format <image> <size_mb>\n");
        return;
    }

    const char* ImagePath = argv[3];
    WORD64 SizeMB = (WORD64)atol(argv[4]);
    if (SizeMB == 0) {
        printf("[ERR]: Invalid size.\n");
        return;
    }
    WORD64 PartitionSize = SizeMB * 1024 * 1024;

    DiskFile = fopen(ImagePath, "wb+");
    if (!DiskFile) {
        printf("[ERR]: Cannot create image file '%s'.\n", ImagePath);
        return;
    }

    EnsureFileSize(PartitionSize);
    WoofsFormat(DiskReadCb, DiskWriteCb, 0, PartitionSize);

    printf("Formatted '%s' (%llu MiB)\n", ImagePath, SizeMB);
    fclose(DiskFile);
    DiskFile = NULL;
}

static void FsAdd(int argc, char** argv) {
    if (argc < 6) {
        printf("[ERR]: Usage: boltosutils filesystem add <image> <localfile> <fspath>\n");
        return;
    }

    const char* ImagePath = argv[3];
    const char* LocalFile = argv[4];
    const char* FsPath    = argv[5];

    if (FsPath[0] != '/') {
        printf("[ERR]: Filesystem path must be absolute (start with '/').\n");
        return;
    }

    /* Read the local file into memory */
    FILE* LocalFp = fopen(LocalFile, "rb");
    if (!LocalFp) {
        printf("[ERR]: Cannot open local file '%s'.\n", LocalFile);
        return;
    }
    fseek(LocalFp, 0, SEEK_END);
    long LocalSize = ftell(LocalFp);
    fseek(LocalFp, 0, SEEK_SET);

    void* LocalData = malloc((size_t)LocalSize);
    if (!LocalData) {
        printf("[ERR]: Out of memory.\n");
        fclose(LocalFp);
        return;
    }
    fread(LocalData, 1, (size_t)LocalSize, LocalFp);
    fclose(LocalFp);

    /* Open or auto-format the image */
    int NeedFormat = 0;
    DiskFile = fopen(ImagePath, "rb+");
    if (!DiskFile) {
        NeedFormat = 1;
        DiskFile = fopen(ImagePath, "wb+");
        if (!DiskFile) {
            printf("[ERR]: Cannot open/create image file '%s'.\n", ImagePath);
            free(LocalData);
            return;
        }
    }

    PWOOFS_HEAD Fs = NULL;

    if (NeedFormat) {
        /* Auto-format: size = data + 4 MiB overhead, minimum 8 MiB */
        WORD64 ImgSize = (WORD64)LocalSize + 4 * 1024 * 1024;
        if (ImgSize < 8 * 1024 * 1024) ImgSize = 8 * 1024 * 1024;
        EnsureFileSize(ImgSize);
        WoofsFormat(DiskReadCb, DiskWriteCb, 0, ImgSize);
        printf("Auto-formatted new image '%s' (%llu MiB)\n", ImagePath, ImgSize / (1024 * 1024));
    }

    WoofsMount(DiskReadCb, DiskWriteCb, 0, &Fs);
    if (WoofsGetLastError(Fs) == WOOFS_ERROR_BAD_MAGIC) {
        printf("[ERR]: '%s' is not a valid woofs image. Use 'filesystem format' first.\n", ImagePath);
        WoofsShutdown(Fs);
        fclose(DiskFile);
        DiskFile = NULL;
        free(LocalData);
        return;
    }

    /* Ensure the image file can accommodate growth */
    EnsureFileSize(Fs->PartitionSize);

    /* Create intermediate directories */
    if (EnsureDirectories(Fs, FsPath) < 0) {
        WoofsShutdown(Fs);
        fclose(DiskFile);
        DiskFile = NULL;
        free(LocalData);
        return;
    }

    /* Create the file */
    STR WidePath[256];
    AsciiToStr(WidePath, FsPath);

    WORD64 FileId = WoofsCreateFile(Fs, WidePath);
    if (FileId == 0xFFFFFFFFFFFFFFFFULL) {
        WORD64 Err = WoofsGetLastError(Fs);
        if (Err == WOOFS_ERROR_ALREADY_EXISTS) {
            /* File exists — get its ID and overwrite */
            FileId = WoofsGetFileIdByName(Fs, WidePath);
            if (FileId == 0xFFFFFFFFFFFFFFFFULL) {
                printf("[ERR]: File exists but could not resolve ID.\n");
                WoofsShutdown(Fs);
                fclose(DiskFile);
                DiskFile = NULL;
                free(LocalData);
                return;
            }
            printf("Overwriting existing file '%s'\n", FsPath);
        } else {
            printf("[ERR]: Failed to create '%s' (error 0x%llx)\n", FsPath, Err);
            WoofsShutdown(Fs);
            fclose(DiskFile);
            DiskFile = NULL;
            free(LocalData);
            return;
        }
    }

    /* Write file data */
    if (LocalSize > 0) {
        /* Ensure the backing file is large enough for the new data */
        EnsureFileSize(Fs->HighestAddress + (WORD64)LocalSize + 4096);
        WoofsWriteFile(Fs, FileId, LocalData, 0, (WORD64)LocalSize);
    }

    printf("Added '%s' -> '%s' (%ld bytes)\n", LocalFile, FsPath, LocalSize);

    WoofsShutdown(Fs);
    fclose(DiskFile);
    DiskFile = NULL;
    free(LocalData);
}

static void FsList(int argc, char** argv) {
    if (argc < 4) {
        printf("[ERR]: Usage: boltosutils filesystem list <image>\n");
        return;
    }

    const char* ImagePath = argv[3];
    DiskFile = fopen(ImagePath, "rb+");
    if (!DiskFile) {
        printf("[ERR]: Cannot open image '%s'.\n", ImagePath);
        return;
    }

    PWOOFS_HEAD Fs = NULL;
    WoofsMount(DiskReadCb, DiskWriteCb, 0, &Fs);
    if (WoofsGetLastError(Fs) == WOOFS_ERROR_BAD_MAGIC) {
        printf("[ERR]: '%s' is not a valid woofs image.\n", ImagePath);
        WoofsShutdown(Fs);
        fclose(DiskFile);
        DiskFile = NULL;
        return;
    }

    WORD64 Count = WoofsGetTotalFileCount(Fs);
    printf("Files in '%s': %llu\n", ImagePath, Count);

    WORD64 Addr = Fs->FirstFile;
    WOOFS_FILE File;
    while (Addr) {
        Fs->ReadFn(&File, Fs->RelativeBase + Addr, sizeof(WOOFS_FILE));

        PSTR WPath = WoofsGetFilePathById(Fs, File.FileId);
        if (WPath) {
            /* Convert STR path back to ASCII for printing */
            int Len = StrLen16(WPath);
            char AsciiPath[1024];
            for (int i = 0; i < Len && i < 1023; i++)
                AsciiPath[i] = (char)(WPath[i] & 0x7F);
            AsciiPath[Len < 1023 ? Len : 1023] = '\0';
            free(WPath);

            printf("  %s  (%llu bytes)\n", AsciiPath, File.FileSize);
        } else {
            /* Convert local filename */
            int Len = StrLen16(File.FileName);
            char AsciiFn[128];
            for (int i = 0; i < Len && i < 127; i++)
                AsciiFn[i] = (char)(File.FileName[i] & 0x7F);
            AsciiFn[Len < 127 ? Len : 127] = '\0';

            printf("  ??? /%s  (%llu bytes)\n", AsciiFn, File.FileSize);
        }

        Addr = File.AbsoluteNext;
    }

    WoofsShutdown(Fs);
    fclose(DiskFile);
    DiskFile = NULL;
}

static void FsExtract(int argc, char** argv) {
    if (argc < 6) {
        printf("[ERR]: Usage: boltosutils filesystem extract <image> <fspath> <outfile>\n");
        return;
    }

    const char* ImagePath = argv[3];
    const char* FsPath    = argv[4];
    const char* OutFile   = argv[5];

    DiskFile = fopen(ImagePath, "rb+");
    if (!DiskFile) {
        printf("[ERR]: Cannot open image '%s'.\n", ImagePath);
        return;
    }

    PWOOFS_HEAD Fs = NULL;
    WoofsMount(DiskReadCb, DiskWriteCb, 0, &Fs);
    if (WoofsGetLastError(Fs) == WOOFS_ERROR_BAD_MAGIC) {
        printf("[ERR]: '%s' is not a valid woofs image.\n", ImagePath);
        WoofsShutdown(Fs);
        fclose(DiskFile);
        DiskFile = NULL;
        return;
    }

    STR WidePath[256];
    AsciiToStr(WidePath, FsPath);

    WORD64 FileId = WoofsGetFileIdByName(Fs, WidePath);
    if (FileId == 0xFFFFFFFFFFFFFFFFULL) {
        printf("[ERR]: File '%s' not found in image.\n", FsPath);
        WoofsShutdown(Fs);
        fclose(DiskFile);
        DiskFile = NULL;
        return;
    }

    WORD64 FileSize = WoofsGetFileSize(Fs, FileId);
    void* Buf = malloc((size_t)FileSize);
    if (!Buf) {
        printf("[ERR]: Out of memory.\n");
        WoofsShutdown(Fs);
        fclose(DiskFile);
        DiskFile = NULL;
        return;
    }

    WoofsReadFile(Fs, FileId, Buf, 0, FileSize);

    FILE* Out = fopen(OutFile, "wb");
    if (!Out) {
        printf("[ERR]: Cannot create output file '%s'.\n", OutFile);
        free(Buf);
        WoofsShutdown(Fs);
        fclose(DiskFile);
        DiskFile = NULL;
        return;
    }

    fwrite(Buf, 1, (size_t)FileSize, Out);
    fclose(Out);
    free(Buf);

    printf("Extracted '%s' -> '%s' (%llu bytes)\n", FsPath, OutFile, FileSize);

    WoofsShutdown(Fs);
    fclose(DiskFile);
    DiskFile = NULL;
}

void Filesystem(int argc, char** argv) {
    if (argc < 3) {
        PrintUsage();
        return;
    }

    const char* Cmd = argv[2];

    if (!strcmp(Cmd, "help")) {
        PrintUsage();
    } else if (!strcmp(Cmd, "format")) {
        FsFormat(argc, argv);
    } else if (!strcmp(Cmd, "add")) {
        FsAdd(argc, argv);
    } else if (!strcmp(Cmd, "list")) {
        FsList(argc, argv);
    } else if (!strcmp(Cmd, "extract")) {
        FsExtract(argc, argv);
    } else {
        printf("[ERR]: Unknown filesystem command '%s'.\n", Cmd);
        PrintUsage();
    }
}
