/**
 * Check that while a file is being written to, the file's filesize in the directory entry is set to the physical size
 * of the file. This allows the contents of the end of the file to be read even if power is lost halfway through
 * writing (with some trailing garbage appended since the physical size is never smaller than the logical size).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sdcard_sim.h"
#include "sdcard.h"
#include "asyncfatfs.h"

#include "common.h"

#define SDCARD_SECTOR_SIZE 512

#define TEST_LOG_ENTRIES_PER_SECTOR (SDCARD_SECTOR_SIZE / TEST_LOG_ENTRY_SIZE)

// Import these normally-internal methods for testing
extern uint32_t afatfs_clusterSize();
extern uint32_t afatfs_superClusterSize();

typedef enum {
    TEST_STAGE_INITIAL = 0,
    TEST_STAGE_SOLID_APPEND_BEGIN = 0,
    TEST_STAGE_SOLID_APPEND_CONTINUE,
    TEST_STAGE_APPEND_BEGIN,
    TEST_STAGE_APPEND_CONTINUE,
    TEST_STAGE_SOLID_APPEND_LARGE_BEGIN,
    TEST_STAGE_SOLID_APPEND_LARGE_CONTINUE,
    TEST_STAGE_APPEND_LARGE_BEGIN,
    TEST_STAGE_APPEND_LARGE_CONTINUE,
    TEST_STAGE_IDLE,
    TEST_STAGE_COMPLETE
} testStage_e;

typedef enum {
    POWERLOSS_TEST_STAGE_OPEN,
    POWERLOSS_TEST_STAGE_APPEND,
    POWERLOSS_TEST_STAGE_FLUSH,
    POWERLOSS_TEST_STAGE_READ_OPEN,
    POWERLOSS_TEST_STAGE_READ_SEEK_TO_END,
    POWERLOSS_TEST_STAGE_READ_MEASURE_FILE_LENGTH,
    POWERLOSS_TEST_STAGE_READ_VALIDATE,
    POWERLOSS_TEST_STAGE_READ_CLOSE,
    POWERLOSS_TEST_STAGE_IDLE
} powerlossTestStage_e;

static testStage_e testStage = TEST_STAGE_INITIAL;
static powerlossTestStage_e powerlossStage;

static afatfsFilePtr_t powerlossFile;

static void initFilesystem()
{
    afatfs_init();

    while (afatfs_getFilesystemState() != AFATFS_FILESYSTEM_STATE_READY) {
        testPoll();

        if (afatfs_getFilesystemState() == AFATFS_FILESYSTEM_STATE_FATAL) {
            fprintf(stderr, "[Fail]     Fatal filesystem error during init\n");
            exit(-1);
        }
    }
}

static void powerlossTestFileCreatedForAppend(afatfsFilePtr_t file)
{
    testAssert(file, "Creating testfile failed");
    testAssert(afatfs_feof(file), "feof() should be true after creating file");

    powerlossFile = file;
    powerlossStage = POWERLOSS_TEST_STAGE_APPEND;
}

static void powerlossTestFileOpenedForRead(afatfsFilePtr_t file)
{
    testAssert(file, "Opening log for read failed");

    powerlossFile = file;
    powerlossStage = POWERLOSS_TEST_STAGE_READ_SEEK_TO_END;
}

/**
 * Continue testing for data retention over powerloss.
 *
 * start - For the first call of the test, set start to true
 *
 * Returns true if the test is still continuing, or false if the test was completed successfully.
 */
bool continueFilesizeTest(bool start, const char *filename, const char *fileMode, uint32_t logEntriesToWrite)
{
    static uint32_t logEntryIndex = 0;

    uint32_t bytesToWrite = logEntriesToWrite * TEST_LOG_ENTRY_SIZE;
    uint32_t logFileSize;
    uint32_t position;

    if (start) {
        powerlossStage = POWERLOSS_TEST_STAGE_OPEN;
    }

    switch (powerlossStage) {
        case POWERLOSS_TEST_STAGE_OPEN:
            powerlossStage = POWERLOSS_TEST_STAGE_IDLE;

            logEntryIndex = 0;

            afatfs_fopen(filename, fileMode, powerlossTestFileCreatedForAppend);
        break;
        case POWERLOSS_TEST_STAGE_APPEND:
            // Write just more than one sector of log entries
            if (writeLogTestEntries(powerlossFile, &logEntryIndex, logEntriesToWrite)) {
                testAssert(afatfs_feof(powerlossFile), "feof() should be true after extending file with write");
                testAssert(afatfs_ftell(powerlossFile, &logFileSize), "ftell() expected to work when no file operation queued");
                testAssert(logFileSize == bytesToWrite, "Log file correct after writes within a cluster");

                testAssert(afatfs_fseek(powerlossFile, 0, AFATFS_SEEK_END) == AFATFS_OPERATION_SUCCESS, "Seeks to end of file when we're already at end should be immediate");
                testAssert(afatfs_ftell(powerlossFile, &logFileSize), "ftell() should work after immediate seek");
                testAssert(logFileSize == bytesToWrite, "fseek() seeked to the wrong position for AFATFS_SEEK_END");

                powerlossStage = POWERLOSS_TEST_STAGE_FLUSH;
            }
        break;
        case POWERLOSS_TEST_STAGE_FLUSH:
            // Wait for all the flushable data (i.e. completed sectors) to make it to the disk
            if (afatfs_flush() && sdcard_sim_isReady()) {
                // Simulate a power interruption by restarting the filesystem
                afatfs_destroy(true);
                powerlossFile = NULL;

                initFilesystem();

                powerlossStage = POWERLOSS_TEST_STAGE_READ_OPEN;
            }
        break;
        case POWERLOSS_TEST_STAGE_READ_OPEN:
            powerlossStage = POWERLOSS_TEST_STAGE_IDLE;
            logEntryIndex = 0;
            afatfs_fopen(filename, "r", powerlossTestFileOpenedForRead);
        break;
        case POWERLOSS_TEST_STAGE_READ_SEEK_TO_END:
            testAssert(afatfs_fseek(powerlossFile, 0, AFATFS_SEEK_END) != AFATFS_OPERATION_FAILURE, "Seek to end should work");

            powerlossStage = POWERLOSS_TEST_STAGE_READ_MEASURE_FILE_LENGTH;
        break;
        case POWERLOSS_TEST_STAGE_READ_MEASURE_FILE_LENGTH:
            // We must wait for the the seek to complete before ftell() will succeed
            if (afatfs_ftell(powerlossFile, &position) == AFATFS_OPERATION_SUCCESS) {
                // We expect all of the sectors we completely wrote to have made it to the card
                testAssert(position >= (bytesToWrite / SDCARD_SECTOR_SIZE) * SDCARD_SECTOR_SIZE, "Filesize after power interruption was smaller than expected");

                testAssert(afatfs_fseek(powerlossFile, 0, AFATFS_SEEK_SET) == AFATFS_OPERATION_SUCCESS, "Should be able to seek to begininng of file instantly");
                powerlossStage = POWERLOSS_TEST_STAGE_READ_VALIDATE;
            }
        break;
        case POWERLOSS_TEST_STAGE_READ_VALIDATE:
            // The sectors we completely filled should be readable
            if (validateLogTestEntries(powerlossFile, &logEntryIndex, (bytesToWrite / SDCARD_SECTOR_SIZE) * TEST_LOG_ENTRIES_PER_SECTOR)) {
                powerlossStage = POWERLOSS_TEST_STAGE_READ_CLOSE;
            }
        break;
        case POWERLOSS_TEST_STAGE_READ_CLOSE:
            if (afatfs_fclose(powerlossFile, NULL)) {
                return false; // Test is over now!
            }
        break;
        case POWERLOSS_TEST_STAGE_IDLE:
            // Waiting for callbacks
        break;
    }

    return true; // Still continuing test
}

bool continueTesting() {

    switch (testStage) {
        case TEST_STAGE_SOLID_APPEND_BEGIN:
        case TEST_STAGE_SOLID_APPEND_CONTINUE:
            // Write at least a sector (so it can be flushed to disk) but less than a cluster
            if (continueFilesizeTest(testStage == TEST_STAGE_SOLID_APPEND_BEGIN, "test.txt", "as", TEST_LOG_ENTRIES_PER_SECTOR + 4)) {
                testStage = TEST_STAGE_SOLID_APPEND_CONTINUE;
            } else {
                testStage = TEST_STAGE_APPEND_BEGIN;
            }
        break;
        case TEST_STAGE_APPEND_BEGIN:
        case TEST_STAGE_APPEND_CONTINUE:
            if (continueFilesizeTest(testStage == TEST_STAGE_APPEND_BEGIN, "test2.txt", "a", TEST_LOG_ENTRIES_PER_SECTOR + 4)) {
                testStage = TEST_STAGE_APPEND_CONTINUE;
            } else {
                fprintf(stderr, "[Success]  File size updated optimistically to allow data recovery after powerloss (\"as\" and \"a\" filemodes, 1 sector written)\n");
                testStage = TEST_STAGE_SOLID_APPEND_LARGE_BEGIN;
            }
        break;
        case TEST_STAGE_SOLID_APPEND_LARGE_BEGIN:
        case TEST_STAGE_SOLID_APPEND_LARGE_CONTINUE:
            /* Write one supercluster + one sector, so we can verify that the filesize in the directory gets increased
             * when superclusters are added to the file
             */
            if (continueFilesizeTest(testStage == TEST_STAGE_SOLID_APPEND_LARGE_BEGIN, "test3.txt", "as", (afatfs_superClusterSize() + SDCARD_SECTOR_SIZE) / TEST_LOG_ENTRY_SIZE)) {
                testStage = TEST_STAGE_SOLID_APPEND_LARGE_CONTINUE;
            } else {
                testStage = TEST_STAGE_APPEND_LARGE_BEGIN;
            }
        break;
        case TEST_STAGE_APPEND_LARGE_BEGIN:
        case TEST_STAGE_APPEND_LARGE_CONTINUE:
            /* Write one cluster + one sector, so we can verify that the filesize in the directory gets increased
             * when clusters are added to the file
             */
            if (continueFilesizeTest(testStage == TEST_STAGE_APPEND_LARGE_BEGIN, "test4.txt", "a", (afatfs_clusterSize() + SDCARD_SECTOR_SIZE) / TEST_LOG_ENTRY_SIZE)) {
                testStage = TEST_STAGE_APPEND_LARGE_CONTINUE;
            } else {
                fprintf(stderr, "[Success]  File size updated optimistically to allow data recovery after powerloss (\"as\" and \"a\" filemodes, 1 cluster written)\n");
                testStage = TEST_STAGE_COMPLETE;
            }
        break;
        case TEST_STAGE_IDLE:
            // Waiting for file operations...
        break;
        case TEST_STAGE_COMPLETE:
            return false;
    }

    // Continue test...
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Missing argument for sdcard image filename\n");
        return EXIT_FAILURE;
    }

    if (!sdcard_sim_init(argv[1])) {
        fprintf(stderr, "sdcard_sim_init() failed\n");
        return EXIT_FAILURE;
    }

    initFilesystem();

    bool keepGoing = true;

    while (keepGoing) {
        testPoll();

        switch (afatfs_getFilesystemState()) {
            case AFATFS_FILESYSTEM_STATE_READY:
                if (!continueTesting()) {
                    keepGoing = false;
                    break;
                }
           break;
           case AFATFS_FILESYSTEM_STATE_FATAL:
                fprintf(stderr, "[Fail]     Fatal filesystem error\n");
                exit(-1);
           default:
               ;
        }
    }

    while (!afatfs_destroy(false)) {
        testPoll();
    }

    sdcard_sim_destroy();

    return EXIT_SUCCESS;
}
