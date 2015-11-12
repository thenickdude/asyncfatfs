#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "sdcard_sim.h"
#include "sdcard_standard.h"

static FILE *simFile;

typedef enum sdcardState_e {
    SDCARD_STATE_NOT_PRESENT,
    SDCARD_STATE_INITIALIZATION,
    SDCARD_STATE_READY,
    SDCARD_STATE_READING,
    SDCARD_STATE_WRITING
} sdcardState_e;

static struct {
    sdcard_operationCompleteCallback_c callback;
    uint32_t callbackData;
    uint8_t *buffer;
    uint32_t blockIndex;
} currentOperation;

static uint32_t sdcardCapacity;
static sdcardState_e sdcardState = SDCARD_STATE_NOT_PRESENT;

bool sdcard_sim_init(const char *filename)
{
    simFile = fopen(filename, "r+b");

    if (!simFile) {
        return false;
    }

    fseek(simFile, 0, SEEK_END);
    sdcardCapacity = ftell(simFile);

    fseek(simFile, 0, SEEK_SET);

    return true;
}

void sdcard_sim_destroy()
{
    fclose(simFile);
    sdcardState = SDCARD_STATE_NOT_PRESENT;
}

bool sdcard_init()
{
    sdcardState = SDCARD_STATE_READY;

    return true;
}

static void sdcard_completeReadBlock()
{
    fseek(simFile, currentOperation.blockIndex * SDCARD_BLOCK_SIZE, SEEK_SET);

    if (fread(currentOperation.buffer, sizeof(uint8_t), SDCARD_BLOCK_SIZE, simFile) == SDCARD_BLOCK_SIZE) {
        if (currentOperation.callback) {
            currentOperation.callback(SDCARD_BLOCK_OPERATION_READ, currentOperation.blockIndex, currentOperation.buffer, currentOperation.callbackData);
        }
    } else {
        fprintf(stderr, "SDCardSim: fread failed on underlying file\n");
        exit(-1);
    }

    sdcardState = SDCARD_STATE_READY;
}

static void sdcard_completeWriteBlock()
{
    fseek(simFile, currentOperation.blockIndex * SDCARD_BLOCK_SIZE, SEEK_SET);

    if (fwrite(currentOperation.buffer, sizeof(uint8_t), SDCARD_BLOCK_SIZE, simFile) == SDCARD_BLOCK_SIZE) {
        if (currentOperation.callback) {
            currentOperation.callback(SDCARD_BLOCK_OPERATION_WRITE, currentOperation.blockIndex, currentOperation.buffer, currentOperation.callbackData);
        }
    } else {
        fprintf(stderr, "SDCardSim: fwrite failed on underlying file\n");
        exit(-1);
    }

    sdcardState = SDCARD_STATE_READY;
}

bool sdcard_readBlock(uint32_t blockIndex, uint8_t *buffer, sdcard_operationCompleteCallback_c callback, uint32_t callbackData)
{
    if (sdcardState != SDCARD_STATE_READY)
        return false;

    if (blockIndex * SDCARD_BLOCK_SIZE >= sdcardCapacity) {
        fprintf(stderr, "SDCardSim: Attempted to read from %u but capacity is %u\n", blockIndex * SDCARD_BLOCK_SIZE, sdcardCapacity);
        exit(-1);
    }

    /*
     * Just like the real SD card will, we will defer this read till later, so the operation won't be done yet when
     * this routine returns.
     */
    sdcardState = SDCARD_STATE_READING;

    currentOperation.buffer = buffer;
    currentOperation.blockIndex = blockIndex;
    currentOperation.callback = callback;
    currentOperation.callbackData = callbackData;

    return true;
}

bool sdcard_writeBlock(uint32_t blockIndex, uint8_t *buffer, sdcard_operationCompleteCallback_c callback, uint32_t callbackData)
{
    if (sdcardState != SDCARD_STATE_READY)
        return false;

    if (blockIndex * SDCARD_BLOCK_SIZE >= sdcardCapacity) {
        fprintf(stderr, "SDCardSim: Attempted to write to block at %u but capacity is %u\n", blockIndex * SDCARD_BLOCK_SIZE, sdcardCapacity);
        exit(-1);
    }

    /*
     * Just like the real SD card will, we will defer this write till later, so the operation won't be done yet when
     * this routine returns.
     */
    sdcardState = SDCARD_STATE_WRITING;

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
        case SDCARD_STATE_WRITING:
            sdcard_completeWriteBlock();
        break;
        default:
            ;
    }
}

bool sdcard_isReady()
{
    return false;
}
