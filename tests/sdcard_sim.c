#define _FILE_OFFSET_BITS 64

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include "sdcard_sim.h"

static FILE *simFile;

#define SDCARD_SIM_WRITE_DELAY 4
#define SDCARD_SIM_READ_DELAY  1

#define SDCARD_SIM_BLOCK_SIZE 512

typedef enum {
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
    int countdownTimer;
} currentOperation;

static uint64_t sdcardCapacity;
static sdcardState_e sdcardState = SDCARD_STATE_NOT_PRESENT;

bool sdcard_sim_init(const char *filename)
{
    simFile = fopen(filename, "r+b");

    if (!simFile) {
        return false;
    }

    fseek(simFile, 0, SEEK_END);
    sdcardCapacity = ftello(simFile);

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

static void sdcard_continueReadBlock()
{
    if (--currentOperation.countdownTimer <= 0) {
        uint64_t byteIndex = (uint64_t) currentOperation.blockIndex * SDCARD_SIM_BLOCK_SIZE;

        fseeko(simFile, byteIndex, SEEK_SET);

        if (fread(currentOperation.buffer, sizeof(uint8_t), SDCARD_SIM_BLOCK_SIZE, simFile) == SDCARD_SIM_BLOCK_SIZE) {
            if (currentOperation.callback) {
                currentOperation.callback(SDCARD_BLOCK_OPERATION_READ, currentOperation.blockIndex, currentOperation.buffer, currentOperation.callbackData);
            }
        } else {
            fprintf(stderr, "SDCardSim: fread failed on underlying file\n");
            exit(-1);
        }

        sdcardState = SDCARD_STATE_READY;
    }
}

static void sdcard_continueWriteBlock()
{
    if (--currentOperation.countdownTimer <= 0) {
        uint64_t byteIndex = (uint64_t) currentOperation.blockIndex * SDCARD_SIM_BLOCK_SIZE;

        fseeko(simFile, byteIndex, SEEK_SET);

        if (fwrite(currentOperation.buffer, sizeof(uint8_t), SDCARD_SIM_BLOCK_SIZE, simFile) == SDCARD_SIM_BLOCK_SIZE) {
            if (currentOperation.callback) {
                currentOperation.callback(SDCARD_BLOCK_OPERATION_WRITE, currentOperation.blockIndex, currentOperation.buffer, currentOperation.callbackData);
            }
        } else {
            fprintf(stderr, "SDCardSim: fwrite failed on underlying file\n");
            exit(-1);
        }

        sdcardState = SDCARD_STATE_READY;
    }
}

bool sdcard_readBlock(uint32_t blockIndex, uint8_t *buffer, sdcard_operationCompleteCallback_c callback, uint32_t callbackData)
{
    uint64_t byteIndex = (uint64_t) blockIndex * SDCARD_SIM_BLOCK_SIZE;

    if (sdcardState != SDCARD_STATE_READY)
        return false;

#ifdef AFATFS_DEBUG_VERBOSE
    fprintf(stderr, "SD card - Read %u\n", blockIndex);
#endif

    if (byteIndex >= sdcardCapacity) {
        fprintf(stderr, "SDCardSim: Attempted to read from %" PRIu64 " but capacity is %" PRIu64 "\n", byteIndex, sdcardCapacity);
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
    currentOperation.countdownTimer = SDCARD_SIM_READ_DELAY;

    return true;
}

sdcardOperationStatus_e sdcard_writeBlock(uint32_t blockIndex, uint8_t *buffer, sdcard_operationCompleteCallback_c callback, uint32_t callbackData)
{
    uint64_t byteIndex = (uint64_t) blockIndex * SDCARD_SIM_BLOCK_SIZE;

    if (sdcardState != SDCARD_STATE_READY)
        return SDCARD_OPERATION_BUSY;

#ifdef AFATFS_DEBUG_VERBOSE
    fprintf(stderr, "SD card - Write %u\n", blockIndex);
#endif

    if (byteIndex >= sdcardCapacity) {
        fprintf(stderr, "SDCardSim: Attempted to write to block at %" PRIu64 " but capacity is %" PRIu64 "\n", byteIndex, sdcardCapacity);
        exit(-1);
    }

    /*
     * Just like the real SD card will, we will defer this write till later, so the operation won't be done yet when
     * this routine returns.
     */
    sdcardState = SDCARD_STATE_WRITING;

    currentOperation.countdownTimer = SDCARD_SIM_WRITE_DELAY;
    currentOperation.buffer = buffer;
    currentOperation.blockIndex = blockIndex;
    currentOperation.callback = callback;
    currentOperation.callbackData = callbackData;

    return SDCARD_OPERATION_IN_PROGRESS;
}

void sdcard_poll()
{
    switch (sdcardState) {
        case SDCARD_STATE_READING:
            sdcard_continueReadBlock();
        break;
        case SDCARD_STATE_WRITING:
            sdcard_continueWriteBlock();
        break;
        default:
            ;
    }
}

bool sdcard_isReady()
{
    return sdcardState == SDCARD_STATE_READY;
}
