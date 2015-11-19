#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sdcard_sim.h"
#include "sdcard.h"
#include "asyncfatfs.h"

#include "common.h"

#define TEST_LOGS_TO_WRITE 50

// Attempt to write about 1MB of log per file
#define LOG_ENTRY_COUNT 35000

typedef enum {
    TEST_STAGE_INIT = 0,
    TEST_STAGE_CREATE_LOG_DIRECTORY = 0,
    TEST_STAGE_CREATE_LOG_FILE,
    TEST_STAGE_WRITE_LOG,
    TEST_STAGE_CLOSE_LOG,
    TEST_STAGE_OPEN_LOG_FOR_READ,
    TEST_STAGE_READ_LOG,
    TEST_STAGE_READ_LOG_CLOSE,
    TEST_STAGE_IDLE,
    TEST_STAGE_COMPLETE
} testStage_e;

static testStage_e testStage = TEST_STAGE_INIT;
static afatfsFilePtr_t testFile;

static uint32_t writeLogFileNumber, readLogFileNumber;
static uint32_t writeLogEntryCount, readLogEntryCount;

static bool shouldKeepLog(uint32_t logNumber)
{
    // Use something like a LCG with the logNumber as the seed to decide to keep 1/2 logs (and delete the remainder)
    return ((uint32_t) (logNumber * 1103515245 + 12345) & 0x02) != 0;
}

void logFileCreatedForSolidAppend(afatfsFilePtr_t file)
{
    if (file) {
        testFile = file;

        testStage = TEST_STAGE_WRITE_LOG;
    } else {
        // Retry
        testStage = TEST_STAGE_CREATE_LOG_FILE;
    }
}

void logDirCreated(afatfsFilePtr_t dir)
{
    if (!dir) {
        fprintf(stderr, "Creating 'logs' directory failed\n");
        exit(EXIT_FAILURE);
    }

    afatfs_chdir(dir);

    testAssert(afatfs_fclose(dir, NULL), "Expected to be able to close idle directory immediately");

    testStage = TEST_STAGE_CREATE_LOG_FILE;
}

void logFileOpenedForRead(afatfsFilePtr_t file)
{
    if (shouldKeepLog(readLogFileNumber)) {
        if (file) {
            testFile = file;

            testStage = TEST_STAGE_READ_LOG;
        } else {
            fprintf(stderr, "Opening log for read failed\n");
            exit(-1);
        }
    } else {
        if (file) {
            fprintf(stderr, "Log that ought to have been deleted was openable!\n");
            exit(-1);
        } else {
            testStage = TEST_STAGE_OPEN_LOG_FOR_READ;
        }
    }
}


bool continueTesting() {
    char filename[13];

    doMore:
    switch (testStage) {
        case TEST_STAGE_CREATE_LOG_DIRECTORY:
            // Create a subdirectory for logging

            /*
             * The callback can be called before mkdir() returns, so set the testStage now to avoid stomping on state
             * set by the callback:
             */
            testStage = TEST_STAGE_IDLE;
            writeLogFileNumber = 0;

            afatfs_mkdir("logs", logDirCreated);
        break;
        case TEST_STAGE_CREATE_LOG_FILE:
            writeLogFileNumber++;

            if (writeLogFileNumber >= TEST_LOGS_TO_WRITE) {
                testStage = TEST_STAGE_OPEN_LOG_FOR_READ;
                readLogFileNumber = 0;
            } else {
                testStage = TEST_STAGE_IDLE;

                writeLogEntryCount = 0;

                // Write a file in contigous-append mode
                sprintf(filename, "LOG%05d.TXT", writeLogFileNumber);

                afatfs_fopen(filename, "as", logFileCreatedForSolidAppend);
            }
        break;
        case TEST_STAGE_WRITE_LOG:
            if (writeLogTestEntries(testFile, &writeLogEntryCount, LOG_ENTRY_COUNT)) {
                testStage = TEST_STAGE_CLOSE_LOG;
            } else if (afatfs_isFull()) {
                testStage = TEST_STAGE_CLOSE_LOG;
            }
        break;
        case TEST_STAGE_CLOSE_LOG:
            /* Wait for the file close operation to queue on the file, but just like Blackbox, don't wait for the close
             * operation to complete before continuing on to open more files.
             */
            if (shouldKeepLog(writeLogFileNumber)) {
                if (!afatfs_fclose(testFile, NULL)) {
                    break;
                }
            } else {
                if (!afatfs_funlink(testFile, NULL)) {
                    break;
                }
            }

            testFile = NULL;

            testStage = TEST_STAGE_CREATE_LOG_FILE;
            goto doMore;
        break;
        case TEST_STAGE_OPEN_LOG_FOR_READ:
            readLogFileNumber++;

            if (readLogFileNumber == writeLogFileNumber) {
                testStage = TEST_STAGE_COMPLETE;
            } else {
                testStage = TEST_STAGE_IDLE;

                readLogEntryCount = 0;

                sprintf(filename, "LOG%05d.TXT", readLogFileNumber);

                afatfs_fopen(filename, "r", logFileOpenedForRead);
            }
        break;
        case TEST_STAGE_READ_LOG:
            if (validateLogTestEntries(testFile, &readLogEntryCount, LOG_ENTRY_COUNT)) {
                testStage = TEST_STAGE_READ_LOG_CLOSE;
                goto doMore;
            }
        break;
        case TEST_STAGE_READ_LOG_CLOSE:
            if (afatfs_fclose(testFile, NULL)) {
                sprintf(filename, "LOG%05d.TXT", readLogFileNumber);
                testFile = NULL;
                testStage = TEST_STAGE_OPEN_LOG_FOR_READ;
                goto doMore;
            }
        break;
        case TEST_STAGE_IDLE:
            // Waiting for file operations...
        break;
        case TEST_STAGE_COMPLETE:
            fprintf(stderr, "[Success]  Logged %u bytes in %u files in simulated logging workload\n",
                TEST_LOG_ENTRY_SIZE * LOG_ENTRY_COUNT * writeLogFileNumber, writeLogFileNumber);

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

    if (!sdcard_init()) {
        fprintf(stderr, "sdcard_init() failed\n");
        return EXIT_FAILURE;
    }

    afatfs_init();

    bool keepGoing = true;

    while (keepGoing) {
        afatfs_poll();

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

    while (!afatfs_destroy()) {
    }

    sdcard_sim_destroy();

    return EXIT_SUCCESS;
}
