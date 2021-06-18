#define _FILE_OFFSET_BITS 64

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <time.h>

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
    SDCARD_STATE_WRITING,
    SDCARD_STATE_WRITING_MULTIPLE_BLOCKS,
} sdcardState_e;

static struct {
    struct {
        sdcard_operationCompleteCallback_c callback;
        uint32_t callbackData;
        uint8_t *buffer;
        uint32_t blockIndex;
        uint32_t startTime;
        int countdownTimer;
    } currentOperation;

    sdcard_profilerCallback_c profiler;

    uint64_t capacity;
    sdcardState_e state;

    uint32_t multiWriteNextBlock;
    uint32_t multiWriteBlocksRemain;
} sdcard;

/**
 * Get the current time in microseconds
 */
static uint32_t getCurrentTime()
{
    return ((uint64_t) clock() * 1000000) / CLOCKS_PER_SEC;
}

bool sdcard_sim_init(const char *filename)
{
    simFile = fopen(filename, "r+b");

    if (!simFile) {
        return false;
    }

    fseek(simFile, 0, SEEK_END);
    sdcard.capacity = ftello(simFile);

    fseek(simFile, 0, SEEK_SET);

    sdcard.state = SDCARD_STATE_READY;

    return true;
}

void sdcard_sim_destroy()
{
    fclose(simFile);
    sdcard.state = SDCARD_STATE_NOT_PRESENT;
}

static void sdcard_continueReadBlock()
{
    if (--sdcard.currentOperation.countdownTimer <= 0) {
        uint64_t byteIndex = (uint64_t) sdcard.currentOperation.blockIndex * SDCARD_SIM_BLOCK_SIZE;

        sdcard.state = SDCARD_STATE_READY;

        fseeko(simFile, byteIndex, SEEK_SET);

        if (fread(sdcard.currentOperation.buffer, sizeof(uint8_t), SDCARD_SIM_BLOCK_SIZE, simFile) == SDCARD_SIM_BLOCK_SIZE) {
            if (sdcard.currentOperation.callback) {
                sdcard.currentOperation.callback(SDCARD_BLOCK_OPERATION_READ, sdcard.currentOperation.blockIndex, sdcard.currentOperation.buffer, sdcard.currentOperation.callbackData);
            }
            if (sdcard.profiler) {
                sdcard.profiler(SDCARD_BLOCK_OPERATION_READ, sdcard.currentOperation.blockIndex, getCurrentTime() - sdcard.currentOperation.startTime);
            }
        } else {
            fprintf(stderr, "SDCardSim: fread failed on underlying file\n");
            exit(-1);
        }
    }
}

static void sdcard_continueWriteBlock()
{
    if (--sdcard.currentOperation.countdownTimer <= 0) {
        uint64_t byteIndex = (uint64_t) sdcard.currentOperation.blockIndex * SDCARD_SIM_BLOCK_SIZE;

        fseeko(simFile, byteIndex, SEEK_SET);

        if (fwrite(sdcard.currentOperation.buffer, sizeof(uint8_t), SDCARD_SIM_BLOCK_SIZE, simFile) == SDCARD_SIM_BLOCK_SIZE) {
            if (sdcard.multiWriteBlocksRemain > 1) {
                sdcard.multiWriteBlocksRemain--;
                sdcard.multiWriteNextBlock++;
                sdcard.state = SDCARD_STATE_WRITING_MULTIPLE_BLOCKS;
            } else {
                if (sdcard.multiWriteBlocksRemain == 1) {
                    sdcard.multiWriteBlocksRemain = 0;
#ifdef AFATFS_DEBUG_VERBOSE
                    fprintf(stderr, "SD card - Finished multiple block write\n");
#endif
                }
                sdcard.state = SDCARD_STATE_READY;
            }

            if (sdcard.currentOperation.callback) {
                sdcard.currentOperation.callback(SDCARD_BLOCK_OPERATION_WRITE, sdcard.currentOperation.blockIndex, sdcard.currentOperation.buffer, sdcard.currentOperation.callbackData);
            }
            if (sdcard.profiler) {
                sdcard.profiler(SDCARD_BLOCK_OPERATION_WRITE, sdcard.currentOperation.blockIndex, getCurrentTime() - sdcard.currentOperation.startTime);
            }
        } else {
            fprintf(stderr, "SDCardSim: fwrite failed on underlying file\n");
            exit(-1);
        }

    }
}

sdcardOperationStatus_e sdcard_endWriteBlocks()
{
    switch (sdcard.state) {
        case SDCARD_STATE_WRITING_MULTIPLE_BLOCKS:
#ifdef AFATFS_DEBUG_VERBOSE
            if (sdcard.multiWriteBlocksRemain > 0) {
                fprintf(stderr, "SD card - Terminated multiple block write with %u still remaining\n", sdcard.multiWriteBlocksRemain);
            } else {
                fprintf(stderr, "SD card - Finished multiple block write\n");
            }
#endif

            sdcard.state = SDCARD_STATE_READY;
            sdcard.multiWriteBlocksRemain = 0;

            // Fall through

       case SDCARD_STATE_READY:
            return SDCARD_OPERATION_SUCCESS;

       default:
            return SDCARD_OPERATION_BUSY;
    }
}

bool sdcard_readBlock(uint32_t blockIndex, uint8_t *buffer, sdcard_operationCompleteCallback_c callback, uint32_t callbackData)
{
    uint64_t byteIndex = (uint64_t) blockIndex * SDCARD_SIM_BLOCK_SIZE;

    if (sdcard.state != SDCARD_STATE_READY) {
        if (sdcard.state == SDCARD_STATE_WRITING_MULTIPLE_BLOCKS) {
            sdcard_endWriteBlocks();
        } else {
            return false;
        }
    }

#ifdef AFATFS_DEBUG_VERBOSE
    fprintf(stderr, "SD card - Read block %u\n", blockIndex);
#endif

    if (byteIndex >= sdcard.capacity) {
        fprintf(stderr, "SDCardSim: Attempted to read from %" PRIu64 " but capacity is %" PRIu64 "\n", byteIndex, sdcard.capacity);
        exit(-1);
    }

    /*
     * Just like the real SD card will, we will defer this read till later, so the operation won't be done yet when
     * this routine returns.
     */
    sdcard.state = SDCARD_STATE_READING;

    sdcard.currentOperation.buffer = buffer;
    sdcard.currentOperation.blockIndex = blockIndex;
    sdcard.currentOperation.callback = callback;
    sdcard.currentOperation.callbackData = callbackData;
    sdcard.currentOperation.countdownTimer = SDCARD_SIM_READ_DELAY;
    sdcard.currentOperation.startTime = getCurrentTime();

    return true;
}

sdcardOperationStatus_e sdcard_writeBlock(uint32_t blockIndex, uint8_t *buffer, sdcard_operationCompleteCallback_c callback, uint32_t callbackData)
{
    uint64_t byteIndex = (uint64_t) blockIndex * SDCARD_SIM_BLOCK_SIZE;

    if (sdcard.state != SDCARD_STATE_READY) {
        if (sdcard.state == SDCARD_STATE_WRITING_MULTIPLE_BLOCKS) {
            if (blockIndex != sdcard.multiWriteNextBlock) {
                sdcard_endWriteBlocks();
            }
        } else {
            return SDCARD_OPERATION_BUSY;
        }
    }

#ifdef AFATFS_DEBUG_VERBOSE
    fprintf(stderr, "SD card - Write block %u\n", blockIndex);
#endif

    if (byteIndex >= sdcard.capacity) {
        fprintf(stderr, "SDCardSim: Attempted to write to block at %" PRIu64 " but capacity is %" PRIu64 "\n", byteIndex, sdcard.capacity);
        exit(-1);
    }

    /*
     * Just like the real SD card will, we will defer this write till later, so the operation won't be done yet when
     * this routine returns.
     */
    sdcard.state = SDCARD_STATE_WRITING;

    sdcard.currentOperation.buffer = buffer;
    sdcard.currentOperation.blockIndex = blockIndex;
    sdcard.currentOperation.callback = callback;
    sdcard.currentOperation.callbackData = callbackData;
    sdcard.currentOperation.countdownTimer = SDCARD_SIM_WRITE_DELAY;
    sdcard.currentOperation.startTime = getCurrentTime();

    return SDCARD_OPERATION_IN_PROGRESS;
}

sdcardOperationStatus_e sdcard_beginWriteBlocks(uint32_t blockIndex, uint32_t blockCount)
{
    uint64_t byteIndex = (uint64_t) blockIndex * SDCARD_SIM_BLOCK_SIZE;

    if (sdcard.state != SDCARD_STATE_READY) {
        if (sdcard.state == SDCARD_STATE_WRITING_MULTIPLE_BLOCKS) {
            if (blockIndex != sdcard.multiWriteNextBlock) {
                sdcard_endWriteBlocks();
            } else {
                // Assume that the caller wants to continue the multi-block write they already have in progress!
                return SDCARD_OPERATION_SUCCESS;
            }
        } else {
            return SDCARD_OPERATION_BUSY;
        }
    }

    if (byteIndex + blockCount * SDCARD_SIM_BLOCK_SIZE > sdcard.capacity) {
        fprintf(stderr, "SDCardSim: Attempted to write to multi-block write at %" PRIu64 " but capacity is %" PRIu64 "\n", byteIndex + blockCount * SDCARD_SIM_BLOCK_SIZE, sdcard.capacity);
        exit(-1);
    }

#ifdef AFATFS_DEBUG_VERBOSE
    fprintf(stderr, "SD card - Begin multi-block write of %u blocks beginning with block %u\n", blockCount, blockIndex);
#endif

    sdcard.state = SDCARD_STATE_WRITING_MULTIPLE_BLOCKS;
    sdcard.multiWriteBlocksRemain = blockCount;
    sdcard.multiWriteNextBlock = blockIndex;

    /*
     * The SD card doesn't guarantee the contents of sectors that we asked it to erase, but didn't end up overwriting
     * during our multi-block write.
     *
     * So fill those with some non-zero garbage to make sure we're not depending on them being erased to sensible values.
     */
    uint8_t garbageBuffer[SDCARD_SIM_BLOCK_SIZE];

    for (uint32_t i = 0 ; i < SDCARD_SIM_BLOCK_SIZE; i++) {
        garbageBuffer[i] = i | 1;
    }

    fseeko(simFile, byteIndex, SEEK_SET);

    for (uint32_t i = 0; i < blockCount; i++) {
        fwrite((char*) garbageBuffer, sizeof(uint8_t), SDCARD_SIM_BLOCK_SIZE, simFile);
    }

    return SDCARD_OPERATION_SUCCESS;
}

bool sdcard_sim_isReady()
{
    return sdcard.state == SDCARD_STATE_READY || sdcard.state == SDCARD_STATE_WRITING_MULTIPLE_BLOCKS;
}

bool sdcard_poll()
{
    switch (sdcard.state) {
        case SDCARD_STATE_READING:
            sdcard_continueReadBlock();
        break;
        case SDCARD_STATE_WRITING:
            sdcard_continueWriteBlock();
        break;
        default:
            ;
    }

    return sdcard_sim_isReady();
}

void sdcard_setProfilerCallback(sdcard_profilerCallback_c callback)
{
    sdcard.profiler = callback;
}
