//
//  resource.c
//  boltosutils
//
//  Created by Noah Wooten on 5/4/26.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#define __crt_malloc malloc
#define __crt_realloc realloc
#define __crt_strcmp strcmp
#define __crt_memcpy memcpy
#define __crt_strcpy strcpy
#define __crt_memset memset
#define __crt_free free

typedef struct _RESOURCE_ITEM {
    char ResName[256];

    uint32_t RawDataLocation;
    uint32_t RawDataSize;
    uint32_t Width, Height;
    unsigned char NoCompress;

    uint32_t NextResource;
}RESOURCE_ITEM, *PRESOURCE_ITEM;

typedef struct _RESOURCE_LIST {
    uint32_t FirstRes;
}RESOURCE_LIST, *PRESOURCE_LIST;

int   MultiBmpClient_GetResCount(void* ResList);
int   MultiBmpClient_GetResByName(void* ResList, char* Name);
void* MultiBmpClient_GetBmpDataFromRes(void* ResList, int i, unsigned short* Width, unsigned short* Height);
void* MultiBmpClient_AddResWithBmpData(void* ResList, uint32_t* ResListSize, char* Name, void* BmpFileData, unsigned char NoCompress, unsigned char NoFlip);
void* MultiBmpClient_Compress(void* RawIn, uint32_t RawSize, uint32_t* OutSize);
void* MultiBmpClient_Decompress(void* CompIn, uint32_t CompSize, uint32_t* UnCompSize);
char* MultiBmpClient_GetResName(void* _ResList, int i);
uint32_t MultiBmpClient_GetResSize(void* _ResList, int i, uint32_t* Width, uint32_t* Height);
unsigned char* MultiBmpClient_GetBytesFromBMP(unsigned char* fileData, int32_t* width, int32_t* height, int* bytesPerPixel, uint32_t* RawDataSize);

void __FlipBitmapVertically(void* Data, uint32_t Width, uint32_t Height) {
    if (!Data) return;

    uint32_t rowSize = Width * 3;  // Each row is Width * 3 bytes
    unsigned char* buffer = (unsigned char*)malloc(rowSize);
    if (!buffer) return;

    unsigned char* pixels = (unsigned char*)Data;
    for (uint32_t y = 0; y < Height / 2; y++) {
        unsigned char* rowTop = pixels + (y * rowSize);
        unsigned char* rowBottom = pixels + ((Height - 1 - y) * rowSize);

        // Swap the two rows
        memcpy(buffer, rowTop, rowSize);
        memcpy(rowTop, rowBottom, rowSize);
        memcpy(rowBottom, buffer, rowSize);
    }

    free(buffer);
}

void Resource(int argc, char** argv) {
    if (!strcmp(argv[2], "interactive")) {
        // interactive mode
    } else { // scripting mode
        /*
         boltosutils[0] resource[1] help[2] : Displays help msg
         boltosutils[0] resource[1] resourcefile[2] list[3]
         boltosutils[0] resource[1] resourcefile[2] add[3] file[4] name[5] (opt)noflip[x] (opt)nocompress[x]
         */

        char* ResFile = argv[2];
        if (argc >= 3 && !strcmp(ResFile, "help")) {
            printf("boltosutils resource help : Displays help msg\n");
            printf("boltosutils resource resourcefile list\n");
            printf("boltosutils resource resourcefile add file name (opt)noflip (opt)nocompress\n");
            return;
        }

        char* Function = argv[3];
        if (argc < 4) {
            printf("[ERR]: Missing function.\n");
            return;
        }

        if (!strcmp(Function, "list")) {
            // list
            FILE* ResFile_ = fopen(ResFile, "rb");
            if (!ResFile_) {
                printf("[ERR]: Failed to open resource file.\n");
                return;
            }

            uint32_t ResFileSz = 0;
            fseek(ResFile_, 0, SEEK_END);
            ResFileSz = (uint32_t)ftell(ResFile_);
            fseek(ResFile_, 0, SEEK_SET);

            void* BmpRes = malloc(ResFileSz);
            fread(BmpRes, ResFileSz, 1, ResFile_);
            fclose(ResFile_);

            int ResourceCnt = MultiBmpClient_GetResCount(BmpRes);

            printf("boltosutils Resource Manager\n");
            for (int i = 0; i < ResourceCnt; i++) {
                uint32_t Width, Height;
                uint32_t Compressed = MultiBmpClient_GetResSize(BmpRes, i, &Width, &Height) / 1024;
                uint32_t FullSize = (Width * Height * 3) / 1024;
                printf("%i.) '%s' (%" PRIu32 "x%" PRIu32 ", %" PRIu32 " KiB, %" PRIu32 " KiB Uncompressed)\n", i + 1, MultiBmpClient_GetResName(BmpRes, i),
                    Width, Height, Compressed, FullSize);
            }
            printf("Located %i resources in file.\n", ResourceCnt);

        } else {
            // add
            unsigned char FlipFlags = 0x00;
            for (int i = 6; i < argc; i++) {

                if (!strcmp(argv[i], "noflip"))
                    FlipFlags |= 0x01;
                if (!strcmp(argv[i], "nocompress"))
                    FlipFlags |= 0x02;
            }

            char* PicturePath = argv[4];
            char* PictureName = argv[5];

            if (!(argc >= 6)) {
                printf("[ERR]: An image and name must be passed!\n");
                return;
            }

            FILE* BitmapFile = fopen(PicturePath, "rb");
            if (!BitmapFile) {
                printf("[ERR]: Cannot open specified image.\n");
                return;
            }

            uint32_t BitmapSize = 0;
            fseek(BitmapFile, 0, SEEK_END);
            BitmapSize = (uint32_t)ftell(BitmapFile);

            void* BitmapData = malloc(BitmapSize);
            fseek(BitmapFile, 0, SEEK_SET);
            fread(BitmapData, BitmapSize, 1, BitmapFile);
            fclose(BitmapFile);

            FILE* File = fopen(argv[2], "rb");
            if (!File) {
                File = fopen(argv[2], "wb");
                RESOURCE_LIST ResList = { 0 };
                ResList.FirstRes = 0x4;
                fwrite(&ResList, sizeof(RESOURCE_LIST), 1, File);
                RESOURCE_ITEM FirstItem = { 0 };
                fwrite(&FirstItem, sizeof(RESOURCE_ITEM), 1, File);
            }

            fclose(File);
            File = fopen(argv[2], "rb+");

            uint32_t StoreSize;
            fseek(File, 0, SEEK_END);
            StoreSize = (uint32_t)ftell(File);
            fseek(File, 0, SEEK_SET);
            void* _ResFile = malloc(StoreSize);
            fread(_ResFile, StoreSize, 1, File);

            uint32_t OldStoreSize = StoreSize;
            _ResFile = MultiBmpClient_AddResWithBmpData(_ResFile, &StoreSize, PictureName, BitmapData, FlipFlags & 0x02, FlipFlags & 0x01);
            free(BitmapData);

            double CompressionPercentage = (1.0 - ((double)(StoreSize - OldStoreSize) / (BitmapSize))) * 100.0;
            printf("Added resource '%s', added %" PRIu32 " KiB (%0.2f%% compression)\n", PictureName, (StoreSize - OldStoreSize) / 1024, CompressionPercentage);

            fseek(File, 0, SEEK_SET);
            fwrite(_ResFile, StoreSize, 1, File);
            fflush(File);
            fclose(File);
            free(_ResFile);
        }
    }
    return;
}
