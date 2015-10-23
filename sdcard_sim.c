#include <stdint.h>
#include <stdio.h>

#include "sdcard_sim.h"
#include "sdcard_standard.h"

static FILE *simFile;

typedef enum sdcardState_e {
    SDCARD_STATE_NOT_PRESENT    = 0,
    SDCARD_STATE_INITIALIZATION = 1,
    SDCARD_STATE_READY          = 2,
    SDCARD_STATE_READING        = 3,
} sdcardState_e;

static struct {
    sdcard_operationCompleteCallback_c callback;
    uint32_t callbackData;
    uint8_t *buffer;
    uint32_t blockIndex;
} currentOperation;

static sdcardState_e sdcardState = SDCARD_STATE_NOT_PRESENT;
static sdcardOperationStatus_e sdcardLastOperationStatus = SDCARD_NO_OPERATION;

void sdcard_sim_init(const char *filename)
{
    simFile = fopen(filename, "wb");
}

void sdcard_sim_destroy()
{
    fclose(simFile);
}

bool sdcard_init()
{
    sdcardState = SDCARD_STATE_READY;
    sdcardLastOperationStatus = SDCARD_NO_OPERATION;

    return true;
}

static void sdcard_completeReadBlock()
{
    fseek(simFile, currentOperation.blockIndex * SDCARD_BLOCK_SIZE, SEEK_SET);

    if (fread(currentOperation.buffer, sizeof(uint8_t), SDCARD_BLOCK_SIZE, simFile) == SDCARD_BLOCK_SIZE) {
        sdcardLastOperationStatus = SDCARD_OPERATION_SUCCESS;

        if (currentOperation.callback) {
            currentOperation.callback(SDCARD_BLOCK_OPERATION_READ, currentOperation.blockIndex, currentOperation.buffer, currentOperation.callbackData);
        }
    } else {
        // Attempt to read beyond EOF
        sdcardLastOperationStatus = SDCARD_OPERATION_ERROR;
    }

    sdcardState = SDCARD_STATE_READY;
}

bool sdcard_readBlock(uint32_t blockIndex, uint8_t *buffer, sdcard_operationCompleteCallback_c callback, uint32_t callbackData)
{
    if (sdcardState != SDCARD_STATE_READY)
        return false;

    /* Just like the real SD card will, we will defer this read till later, so the operation won't be done yet when
     * this routine returns.
     */
    sdcardState = SDCARD_STATE_READING;
    sdcardLastOperationStatus = SDCARD_OPERATION_IN_PROGRESS;

    currentOperation.buffer = buffer;
    currentOperation.blockIndex = blockIndex;
    currentOperation.callback = callback;
    currentOperation.callbackData = callbackData;

    return true;
}

void sdcard_poll()
{
    switch (sdcardState) {
        case SDCARD_STATE_READING:
            sdcard_completeReadBlock();
        break;
        default:
            ;
    }
}

bool sdcard_isReady()
{
    return false;
}

sdcardOperationStatus_e sdcard_getLastOperationStatus()
{
    return sdcardLastOperationStatus;
}
