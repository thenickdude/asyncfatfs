#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "fat_standard.h"

typedef struct afatfs_file_t* afatfsFilePtr_t;

typedef enum {
    AFATFS_FILESYSTEM_STATE_UNKNOWN,
    AFATFS_FILESYSTEM_STATE_FATAL,
    AFATFS_FILESYSTEM_STATE_INITIALIZATION,
    AFATFS_FILESYSTEM_STATE_READY,
} afatfsFilesystemState_e;

typedef enum {
    AFATFS_NO_OPERATION = 0,
    AFATFS_OPERATION_IN_PROGRESS = 1,
    AFATFS_OPERATION_SUCCESS = 2,
    AFATFS_OPERATION_FAILURE = 3,
} afatfsOperationStatus_e;

typedef struct afatfsDirEntryPointer_t {
    uint32_t clusterNumber;
    uint32_t sectorNumber;
    uint8_t entryIndex;

    bool finished;
} afatfsDirEntryPointer_t;

typedef void (*afatfsOperationCallback_t)(bool success);

afatfsFilePtr_t afatfs_fopen(const char *filename, const char *mode);
void afatfs_fclose(afatfsFilePtr_t file);
int afatfs_fwrite(uint8_t *buffer, int len);
int afatfs_fread(uint8_t *buffer, int len);
void afatfs_fseek(int offset, int whence);
void afatfs_mkdir(const char *name, afatfsOperationCallback_t complete);
void afatfs_chdir(const char *name);

bool afatfs_flush();
void afatfs_init();
void afatfs_destroy();
void afatfs_poll();
uint32_t afatfs_getContiguousFreeSpace();

afatfsFilesystemState_e afatfs_getFilesystemState();


/*

mkdir("blackbox");
chdir("blackbox");
findFirst();

while (entry = findNext()) {
    // Mark down newest sequence number
}

fopen("log00001", "wb")

 */
