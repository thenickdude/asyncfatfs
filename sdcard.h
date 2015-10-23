#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct sdcard_metadata_t {
    uint8_t manufacturerID;
    uint16_t oemID;

    char productName[5];

    uint8_t productRevisionMajor;
    uint8_t productRevisionMinor;
    uint32_t productSerial;

    uint16_t productionYear;
    uint8_t productionMonth;

    uint32_t numBlocks; /* Card capacity in 512-byte blocks*/
} sdcard_metadata_t;

typedef enum {
    SDCARD_NO_OPERATION,
    SDCARD_OPERATION_IN_PROGRESS,
    SDCARD_OPERATION_SUCCESS,
    SDCARD_OPERATION_ERROR,
} sdcardOperationStatus_e;

typedef enum {
    SDCARD_BLOCK_OPERATION_READ,
    SDCARD_BLOCK_OPERATION_WRITE,
    SDCARD_BLOCK_OPERATION_ERASE,
} sdcardBlockOperation_e;

typedef void(*sdcard_operationCompleteCallback_c)(sdcardBlockOperation_e operation, uint32_t blockIndex, uint8_t *buffer, uint32_t callbackData);

bool sdcard_init();
void sdcard_write();
bool sdcard_readBlock(uint32_t blockIndex, uint8_t *buffer, sdcard_operationCompleteCallback_c callback, uint32_t callbackData);
sdcardOperationStatus_e sdcard_getLastOperationStatus();
void sdcard_poll();

bool sdcard_isReady();

