//
//  multibmpres.c
//  boltosutils
//
//  Created by Noah Wooten on 5/3/26.
//

/*
 multibmpres implementation taken from
 https://github.com/noahbwooten/multibmpres
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

void FlipBitmapVertically(void* Data, uint32_t Width, uint32_t Height) {
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

int MultiBmpClient_GetResCount(void* _ResList) {
    PRESOURCE_LIST ResList = _ResList;
    uint32_t Count = 0;
    PRESOURCE_ITEM Item = (void*)((char*)_ResList + ResList->FirstRes);

    do {
        Count++;
        if (Item->NextResource &&    Item->NextResource != 0xFFFFFFFF)
            Item = (void*)((char*)_ResList + Item->NextResource);
        else
            break;
    } while (Item->NextResource);

    return Count;
}

int MultiBmpClient_GetResByName(void* _ResList, char* Name) {
    PRESOURCE_LIST ResList = _ResList;
    uint32_t Count = 0;
    PRESOURCE_ITEM Item = (void*)((char*)_ResList + ResList->FirstRes);

    do {
        if (!__crt_strcmp(Item->ResName, Name))
            return Count;
        Count++;
        Item = (void*)((char*)_ResList + Item->NextResource);
    } while (Item->NextResource);

    return -1;
}

void* MultiBmpClient_GetBmpDataFromRes(void* _ResList, int i, unsigned short* Width, unsigned short* Height) {
    PRESOURCE_LIST ResList = _ResList;
    uint32_t Count = 0;
    PRESOURCE_ITEM Item = (void*)((char*)_ResList + ResList->FirstRes);

    do {
        if (Count == i) {
            // get this data
            void* Return = __crt_malloc(Item->RawDataSize);
            __crt_memcpy(Return, (char*)_ResList + Item->RawDataLocation, Item->RawDataSize);
            uint32_t OutSize;
            void* _Return = MultiBmpClient_Decompress(Return, Item->RawDataSize, &OutSize);
            *Width = Item->Width;
            *Height = Item->Height;
            __crt_free(Return);
            return _Return;
        }

        Count++;
        if (Item->NextResource)
            Item = (void*)((char*)_ResList + Item->NextResource);
    } while (Item->NextResource);

    return NULL;
}

void* MultiBmpClient_AddResWithBmpData(void* _ResList, uint32_t* ResListSize, char* Name, void* BmpFileData, unsigned char NoCompress, unsigned char NoFlip) {
    PRESOURCE_LIST ResList = _ResList;
    PRESOURCE_ITEM Item = (void*)((char*)_ResList + ResList->FirstRes);

    uint32_t OldNext = ResList->FirstRes;

    do {
        if (!Item->RawDataLocation) {
            int32_t Width, Height;
            int BPP;
            uint32_t RawSize;
            void* BitmapData = MultiBmpClient_GetBytesFromBMP(BmpFileData, &Width, &Height, &BPP, &RawSize);
            if (!NoFlip)
                FlipBitmapVertically(BitmapData, Width, Height);

            uint32_t _RawSize;
            void* _BitmapData = NULL;
            if (!NoCompress)
                _BitmapData = MultiBmpClient_Compress(BitmapData, RawSize, &_RawSize);
            else {
                _BitmapData = malloc(RawSize);
                _RawSize = RawSize;
                memcpy(_BitmapData, BitmapData, RawSize);
            }

            _ResList = __crt_realloc(_ResList, *ResListSize + _RawSize);
            __crt_free(BitmapData);
            __crt_memcpy((char*)_ResList + *ResListSize, _BitmapData, _RawSize);
            PRESOURCE_ITEM NewItem = (void*)((char*)_ResList + OldNext);

            NewItem->Width = Width;
            NewItem->Height = Height;
            NewItem->RawDataLocation = *ResListSize;
            NewItem->RawDataSize = _RawSize;
            NewItem->NoCompress = NoCompress;
            __crt_strcpy(NewItem->ResName, Name);
            *ResListSize += _RawSize;
            NewItem->NextResource = *ResListSize;

            _ResList = __crt_realloc(_ResList, *ResListSize + sizeof(RESOURCE_ITEM));
            __crt_memset((char*)_ResList + *ResListSize, 0, sizeof(RESOURCE_ITEM));

            *ResListSize += sizeof(RESOURCE_ITEM);

            return _ResList;
        } else {
            OldNext = Item->NextResource;
            Item = (void*)((char*)_ResList + Item->NextResource);
        }
    } while (1);
}

char* MultiBmpClient_GetResName(void* _ResList, int i) {
    PRESOURCE_LIST ResList = _ResList;
    uint32_t Count = 0;
    PRESOURCE_ITEM Item = (void*)((char*)_ResList + ResList->FirstRes);

    do {
        if (Count == i) {
            return Item->ResName;
        }
        Count++;
        Item = (void*)((char*)_ResList + Item->NextResource);
    } while (Item->NextResource);

    return NULL;
}

uint32_t MultiBmpClient_GetResSize(void* _ResList, int i, uint32_t* Width, uint32_t* Height) {
    PRESOURCE_LIST ResList = _ResList;
    uint32_t Count = 0;
    PRESOURCE_ITEM Item = (void*)((char*)_ResList + ResList->FirstRes);

    do {
        if (Count == i) {
            *Width = Item->Width;
            *Height = Item->Height;
            return Item->RawDataSize;
        }
        Count++;
        Item = (void*)((char*)_ResList + Item->NextResource);
    } while (Item->NextResource);

    return 0;
}

void* MultiBmpClient_Compress(void* RawIn, uint32_t RawSize, uint32_t* OutSize) {
    if (!RawIn || RawSize == 0) {
        *OutSize = 0;
        return NULL;
    }

    unsigned char* Input = (unsigned char*)RawIn;
    // Allocate worst-case output buffer (each input byte becomes 2 bytes)
    uint32_t MaxOutputSize = RawSize * 2;
    unsigned char* Return = __crt_malloc(MaxOutputSize);
    if (!Return) {
        *OutSize = 0;
        return NULL;
    }

    uint32_t InIndex = 0;
    uint32_t OutIndex = 0;

    while (InIndex < RawSize) {
        unsigned char Current = Input[InIndex];
        unsigned int RunLength = 1;

        // Count how many consecutive bytes are equal to 'current'
        // Limit the run length to 255 (so it fits in one byte)
        while ((InIndex + RunLength < RawSize) &&
            (Input[InIndex + RunLength] == Current) &&
            (RunLength < 255)) {
            RunLength++;
        }

        // Write the run length and the byte value to the output
        Return[OutIndex++] = (unsigned char)RunLength;
        Return[OutIndex++] = Current;

        // Move past this run in the input data
        InIndex += RunLength;
    }

    *OutSize = OutIndex;

    // Optionally, shrink the allocated buffer to the actual output size
    unsigned char* ShrunkOutput = __crt_realloc(Return, OutIndex);
    return ShrunkOutput ? ShrunkOutput : Return;
}

void* MultiBmpClient_Decompress(void* CompIn, uint32_t CompSize, uint32_t* UnCompSize) {
    if (!CompIn || CompSize == 0) {
        *UnCompSize = 0;
        return NULL;
    }

    unsigned char* Comp = (unsigned char*)CompIn;
    uint32_t InIndex = 0;
    uint32_t TotalOutputSize = 0;

    // First pass: calculate the size of the uncompressed data.
    while (InIndex < CompSize) {
        // Ensure that we have a full pair available.
        if (InIndex + 1 >= CompSize) {
            // Corrupted/compressed data: not enough bytes for a complete pair.
            *UnCompSize = 0;
            return NULL;
        }

        unsigned char Count = Comp[InIndex];
        TotalOutputSize += Count;
        InIndex += 2;
    }

    // Allocate the output buffer
    unsigned char* Output = __crt_malloc(TotalOutputSize);

    if (!Output) {
        *UnCompSize = 0;
        return NULL;
    }

    InIndex = 0;
    uint32_t OutIndex = 0;

    // Second pass: decompress the data.
    while (InIndex < CompSize) {
        unsigned char count = Comp[InIndex++];
        unsigned char value = Comp[InIndex++];

        // Write 'count' copies of 'value'
        for (unsigned int i = 0; i < count; i++) {
            Output[OutIndex++] = value;
        }
    }

    *UnCompSize = TotalOutputSize;
    return Output;
}

#pragma pack(push, 1)
typedef struct {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
} BMPFileHeader;

typedef struct {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} BMPInfoHeader;
#pragma pack(pop)

unsigned char* MultiBmpClient_GetBytesFromBMP(
    unsigned char* fileData,
    int32_t* width,
    int32_t* height,
    int* bytesPerPixel,
    uint32_t* RawDataSize)
{
    const unsigned char* dataPtr = fileData;
    BMPFileHeader fileHeader;
    BMPInfoHeader infoHeader;

    memcpy(&fileHeader, dataPtr, sizeof(BMPFileHeader));
    dataPtr += sizeof(BMPFileHeader);

    if (fileHeader.bfType != 0x4D42) {
        return NULL;
    }

    memcpy(&infoHeader, dataPtr, sizeof(BMPInfoHeader));
    dataPtr += sizeof(BMPInfoHeader);

    *width        = infoHeader.biWidth;
    *height       = infoHeader.biHeight;
    *bytesPerPixel = infoHeader.biBitCount / 8;

    if (infoHeader.biCompression != 0 || *bytesPerPixel != 3) {
        return NULL;
    }

    unsigned char* pixelData = (unsigned char*)malloc(infoHeader.biSizeImage);
    if (!pixelData) {
        return NULL;
    }

    memcpy(pixelData, fileData + fileHeader.bfOffBits, infoHeader.biSizeImage);
    *RawDataSize = infoHeader.biSizeImage;
    return pixelData;
}
