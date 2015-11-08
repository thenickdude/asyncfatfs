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
    TEST_STAGE_CREATE_DIRECTORY = 0,
    TEST_STAGE_CREATE_LOG_FILE,
    TEST_STAGE_WRITE_LOG,
    TEST_STAGE_IDLE,
    TEST_STAGE_COMPLETE
} testStage_e;

static testStage_e testStage = TEST_STAGE_INIT;
static afatfsFilePtr_t testFile;

static int testWriteMax = 100000;
static int testWriteCount = 0;

void printFSState(afatfsFilesystemState_e state)
{
    switch (state) {
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
       case AFATFS_FILESYSTEM_STATE_UNKNOWN:
       default:
           printf("Filesystem in unknown state %d!\n", (int) state);
       break;
   }
}

void testFileCreated(afatfsFilePtr_t file)
{
    if (!file) {
        fprintf(stderr, "Creating testfile failed\n");
        exit(EXIT_FAILURE);
    }

    testFile = file;

    testStage = TEST_STAGE_WRITE_LOG;
}

void logDirCreated(afatfsFilePtr_t dir)
{
    if (!dir) {
        fprintf(stderr, "Creating log directory failed\n");
        exit(EXIT_FAILURE);
    }

    afatfs_chdir(dir);

    afatfs_fclose(dir);

    testStage = TEST_STAGE_CREATE_LOG_FILE;
}


bool continueTesting() {
    char testBuffer[32];

    switch (testStage) {
        case TEST_STAGE_CREATE_DIRECTORY:
            // Create a subdirectory for logging

            /*
             * The callback can be called before mkdir() returns, so set the testStage now to avoid stomping on state
             * set by the callback:
             */
            testStage = TEST_STAGE_IDLE;

            afatfs_mkdir("logs", logDirCreated);
        break;
        case TEST_STAGE_CREATE_LOG_FILE:
            testStage = TEST_STAGE_IDLE;

            // Write a file in contigous-append mode
            afatfs_fopen("test.txt", "as", testFileCreated);
        break;
        case TEST_STAGE_WRITE_LOG:
            if (testWriteCount >= testWriteMax) {
                // We're okay to close without waiting for the write to complete
                afatfs_fclose(testFile);

                testStage = TEST_STAGE_COMPLETE;
            } else {
                sprintf(testBuffer, "This is print %6d/%6d\n", testWriteCount + 1, testWriteMax);

                if (afatfs_fwrite(testFile, (uint8_t*) testBuffer, strlen(testBuffer)) > 0) {
                    testWriteCount++;
                } else if (afatfs_isFull()) {
                    //Abort
                    testWriteCount = testWriteMax;
                }
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
        if (state != AFATFS_FILESYSTEM_STATE_READY) {
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
