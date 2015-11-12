#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sdcard_sim.h"
#include "sdcard.h"
#include "sdcard_standard.h"
#include "asyncfatfs.h"

typedef enum {
    TEST_STAGE_INIT = 0,
    TEST_STAGE_CREATE_TEST_DIRECTORY = 0,
    TEST_STAGE_CREATE_LOG_DIRECTORY,
    TEST_STAGE_CREATE_LOG_FILE,
    TEST_STAGE_WRITE_LOG,
    TEST_STAGE_CLOSE_LOG,
    TEST_STAGE_OPEN_LOG_FOR_READ,
    TEST_STAGE_READ_LOG,
    TEST_STAGE_IDLE,
    TEST_STAGE_COMPLETE
} testStage_e;

static testStage_e testStage = TEST_STAGE_INIT;
static afatfsFilePtr_t testFile;

static int testWriteMax = 10000;
static int testWriteCount = 0;
static int testLogFileNumber = 0;

void printFSState(afatfsFilesystemState_e state)
{
    switch (state) {
       case AFATFS_FILESYSTEM_STATE_UNKNOWN:
           printf("Filesystem in unknown state\n");
       break;
       case AFATFS_FILESYSTEM_STATE_READY:
           printf("Filesystem online!\n");
       break;
       case AFATFS_FILESYSTEM_STATE_FATAL:
           printf("Fatal error\n");
           exit(-1);
       break;
       case AFATFS_FILESYSTEM_STATE_INITIALIZATION:
           printf(".");
       break;
       default:
           printf("Filesystem in unknown state %d!\n", (int) state);
       break;
   }
}

void logFileCreatedForWrite(afatfsFilePtr_t file)
{
    if (file) {
        testFile = file;

        testStage = TEST_STAGE_WRITE_LOG;
        fprintf(stderr, "Log file LOG%05d.TXT created\n", testLogFileNumber);
    } else {
        fprintf(stderr, "Creating testfile failed\n");
        testStage = TEST_STAGE_COMPLETE;
    }
}

void logDirCreated(afatfsFilePtr_t dir)
{
    if (!dir) {
        fprintf(stderr, "Creating 'logs' directory failed\n");
        exit(EXIT_FAILURE);
    }

    afatfs_chdir(dir);

    afatfs_fclose(dir);

    testStage = TEST_STAGE_CREATE_LOG_FILE;
}

void testDirCreated(afatfsFilePtr_t dir)
{
    if (!dir) {
        fprintf(stderr, "Creating 'test' directory failed\n");
        exit(EXIT_FAILURE);
    }

    afatfs_fclose(dir);

    testStage = TEST_STAGE_CREATE_LOG_DIRECTORY;
}

void logFileOpenedForRead(afatfsFilePtr_t file)
{
    if (file) {
        testFile = file;

        testStage = TEST_STAGE_READ_LOG;
        fprintf(stderr, "Log file LOG%05d.TXT opened for read\n", testLogFileNumber);
    } else {
        fprintf(stderr, "Opening log for read failed\n");
        testStage = TEST_STAGE_COMPLETE;
    }
}


bool continueTesting() {
    char testBuffer[64];
    char filename[13];
    uint32_t readCount;

    switch (testStage) {
        case TEST_STAGE_CREATE_TEST_DIRECTORY:
            testStage = TEST_STAGE_IDLE;

            afatfs_mkdir("test", testDirCreated);
        break;
        case TEST_STAGE_CREATE_LOG_DIRECTORY:
            // Create a subdirectory for logging

            /*
             * The callback can be called before mkdir() returns, so set the testStage now to avoid stomping on state
             * set by the callback:
             */
            testStage = TEST_STAGE_IDLE;

            afatfs_mkdir("logs", logDirCreated);
        break;
        case TEST_STAGE_CREATE_LOG_FILE:
            testLogFileNumber++;

            if (testLogFileNumber >= 1000) {
                testStage = TEST_STAGE_COMPLETE;
            } else {
                testStage = TEST_STAGE_IDLE;

                testWriteCount = 0;

                // Write a file in contigous-append mode
                sprintf(filename, "LOG%05d.TXT", testLogFileNumber);

                afatfs_fopen(filename, "as", logFileCreatedForWrite);
            }
        break;
        case TEST_STAGE_WRITE_LOG:
            if (testWriteCount >= testWriteMax) {
                testStage = TEST_STAGE_CLOSE_LOG;
            } else {
                sprintf(testBuffer, "Log %05d entry %5d/%5d\n", testLogFileNumber, testWriteCount + 1, testWriteMax);

                uint32_t writtenBytes;

                writtenBytes = afatfs_fwrite(testFile, (uint8_t*) testBuffer, strlen(testBuffer));

                if (writtenBytes > 0) {
                    testWriteCount++;
                    if (writtenBytes < strlen(testBuffer)) {
                        // fprintf(stderr, "Write of %u bytes truncated to %u.\n", (unsigned) strlen(testBuffer), (unsigned) writtenBytes);
                    }
                } else if (afatfs_isFull()) {
                    testStage = TEST_STAGE_CLOSE_LOG;
                }
            }
        break;
        case TEST_STAGE_CLOSE_LOG:
            // We're okay to close without waiting for the write to complete
            afatfs_fclose(testFile);
            if (afatfs_isFull()) {
                testStage = TEST_STAGE_OPEN_LOG_FOR_READ;
            } else {
                testStage = TEST_STAGE_CREATE_LOG_FILE;
            }
        break;
        case TEST_STAGE_OPEN_LOG_FOR_READ:
            testStage = TEST_STAGE_IDLE;

            testLogFileNumber = 1;
            sprintf(filename, "LOG%05d.TXT", testLogFileNumber);

            afatfs_fopen(filename, "r", logFileOpenedForRead);
        break;
        case TEST_STAGE_READ_LOG:
            readCount = afatfs_fread(testFile, (uint8_t*)testBuffer, sizeof(testBuffer));

            if (readCount == 0 && afatfs_feof(testFile)) {
                afatfs_fclose(testFile);
                testStage = TEST_STAGE_COMPLETE;
            } else {
                printf("%.*s", readCount, testBuffer);
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

int main(void)
{

    if (!sdcard_sim_init("simcard.dmg")) {
        fprintf(stderr, "sdcard_sim_init() failed\n");
        return EXIT_FAILURE;
    }

    if (!sdcard_init()) {
        fprintf(stderr, "sdcard_init() failed\n");
        return EXIT_FAILURE;
    }

    afatfs_init();
    printf("Filesystem is initting");

    afatfsFilesystemState_e state = AFATFS_FILESYSTEM_STATE_UNKNOWN;

    while (1) {
        afatfs_poll();

        // Report a change in FS state if needed
        if (afatfs_getFilesystemState() != state) {
            state = afatfs_getFilesystemState();
            printFSState(state);
        }

        if (state == AFATFS_FILESYSTEM_STATE_READY) {
            if (!continueTesting()) {
                break;
            }
        }
    }

    printf("Flushing and shutting down...\n");

    while (!afatfs_destroy()) {
    }

    sdcard_sim_destroy();

    return EXIT_SUCCESS;
}
