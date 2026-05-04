//
//  version.c
//  boltosutils
//
//  Created by Noah Wooten on 5/3/26.
//

#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>

typedef char CHAR, *PSTR, BOOL;
typedef unsigned short WCHAR, *PWSTR;
typedef unsigned long WORD32;

typedef struct _OSINFO {
    CHAR OsName[64];
    WORD32 MajorVersion, MinorVersion;
    WORD32 Build, Revision;
}OSINFO, *POSINFO;

#define FLAG_RELEASE 0x0001
#define FLAG_INCRMNT 0x0002
#define FLAG_UPDATE  0x0004

void Version(int argc, char** argv) {
    
    POSINFO OsInfo = malloc(sizeof(OSINFO));
    memset(OsInfo, 0, sizeof(OSINFO));
    WORD32 FlagWord = 0x00;
    
    // determine logic
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "release")) {
            FlagWord = FLAG_RELEASE;
        } else if (!strcmp(argv[i], "update")) {
            FlagWord = FLAG_UPDATE;
        } else {
            FlagWord = FLAG_INCRMNT;
        }
    }
    
    // open the osinfo file
    FILE* OsInfoFile = fopen("osinfo.dat", "rb+");
    if (!OsInfoFile) {
        OsInfoFile = fopen("osinfo.dat", "wb");
        printf("[WARN]: Could not open existing osinfo.dat file, creating new...\n");
        FlagWord = FLAG_UPDATE;
        if (!OsInfoFile) {
            printf("[ERR]: Failed to create OSInfo file.\n");
            free(OsInfo);
            return;
        }
    } else {
        fread(OsInfo, sizeof(OSINFO), 1, OsInfoFile);
    }
    
    if (FlagWord == FLAG_RELEASE) {
        OsInfo->Build += 1;
        OsInfo->Revision = 0;
    } else if (FlagWord == FLAG_UPDATE) {
        printf("OS Name (Current %s): ", OsInfo->OsName);
        scanf("%s", OsInfo->OsName);
        printf("OS Major (Current %lu): ", OsInfo->MajorVersion);
        scanf("%li", &OsInfo->MajorVersion);
        printf("OS Minor (Current %lu): ", OsInfo->MinorVersion);
        scanf("%li", &OsInfo->MinorVersion);
        printf("OS Build (Current %lu): ", OsInfo->Build);
        scanf("%li", &OsInfo->Build);
    } else if (FlagWord == FLAG_INCRMNT) {
        OsInfo->Revision += 1;
    }
    
    fclose(OsInfoFile);
    OsInfoFile = fopen("osinfo.dat", "wb+");
    if (!OsInfoFile) {
        printf("[ERR]: Failed to open osinfo.dat file.\n");
        free(OsInfo);
        return;
    }
    
    fwrite(OsInfo, sizeof(OSINFO), 1, OsInfoFile);
    fclose(OsInfoFile);
    
    // Get Branch Name
    char Buffer[128];
    char Branch[128] = { 0 };
    FILE* Pipe = popen("git branch --show-current", "r");
    while (fgets(Buffer, 128, Pipe)) {
        strcat(Branch, Buffer);
    }
    pclose(Pipe);
    Branch[strcspn(Branch, "\n")] = 0x00;
    
    // Get Time/Date
    unsigned long Time, Date;
    time_t Now = time(NULL);
    struct tm* Local = localtime(&Now);
        
    Date = ((Local->tm_year + 1900) - 2000) * 10000 +  // Calculate year
        (Local->tm_mon + 1) * 100 + // Calculate month
        (Local->tm_mday); // Calculate day
    Time = Local->tm_hour * 100 + Local->tm_min;
    
    char* HeaderOut = malloc(2048);
    
    sprintf(HeaderOut,
            "#ifndef _OSINFO\n" \
            "#define _OSINFO\n" \
            "/*\n" \
            " osinfo.h\n" \
            " (c) Noah Wooten 2023 - 2026, All Rights Reserved\n" \
            " */\n\n" \
            "#define OSINFO_NAME L\"%s\"\n" \
            "#define OSINFO_MAJOR %lu\n" \
            "#define OSINFO_MINOR %lu\n" \
            "#define OSINFO_BUILD %lu\n" \
            "#define OSINFO_REVSN %lu\n\n" \
            "#define OSINFO_BRANCH L\"%s\"\n" \
            "#define OSINFO_DATE %06lu\n" \
            "#define OSINFO_TIME %04lu\n" \
            "#endif", \
            OsInfo->OsName, OsInfo->MajorVersion,
            OsInfo->MinorVersion, OsInfo->Build,
            OsInfo->Revision, Branch, Date,
            Time);
    
    printf("%s Version %lu.%lu Build %lu.%lu.%s.%06lu-%04lu\n", OsInfo->OsName,
            OsInfo->MajorVersion, OsInfo->MinorVersion, OsInfo->Build,
            OsInfo->Revision, Branch, Date, Time);
    
    free(OsInfo);
    FILE* HeaderOutFile = fopen("osinfo.h", "w");
    if (!HeaderOutFile) {
        printf("[ERR]: Failed to open osinfo.h.\n");
        free(HeaderOut);
        return;
    }
    
    fwrite(HeaderOut, strlen(HeaderOut) + 1, 1, HeaderOutFile);
    return;
}
