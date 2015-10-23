#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct afatfs_file_t* afatfs_fileptr_t;

typedef enum {
    AFATFS_FILESYSTEM_STATE_UNKNOWN,
    AFATFS_FILESYSTEM_STATE_FATAL,
    AFATFS_FILESYSTEM_STATE_INITIALIZATION,
    AFATFS_FILESYSTEM_STATE_READY,
} AFATFS_Filesystem_State;

typedef enum {
    AFATFS_NO_OPERATION = 0,
    AFATFS_OPERATION_IN_PROGRESS = 1,
    AFATFS_OPERATION_SUCCESS = 2,
    AFATFS_OPERATION_ERROR = 3,
} AFATFSOperationStatus;

afatfs_fileptr_t afatfs_fopen(const char *filename, const char *mode);
void afatfs_fclose(afatfs_fileptr_t file);
int afatfs_fwrite(uint8_t *buffer, int len);
int afatfs_fread(uint8_t *buffer, int len);
void afatfs_fseek(int offset, int whence);

void afatfs_init();
void afatfs_destroy();

AFATFS_Filesystem_State afatfs_getFilesystemState();
