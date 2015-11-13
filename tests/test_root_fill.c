#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sdcard_sim.h"
#include "sdcard.h"
#include "sdcard_standard.h"
#include "fat_standard.h"
#include "asyncfatfs.h"

#define MAX_TEST_FILES 2000

typedef enum {
    TEST_STAGE_INIT = 0,
    TEST_STAGE_CREATE_LOG_FILES = 0,
    TEST_STAGE_OPEN_ROOT,
    TEST_STAGE_VALIDATE_DIRECTORY_CONTENTS,
    TEST_STAGE_IDLE,
    TEST_STAGE_COMPLETE,
    TEST_STAGE_FAILURE
} testStage_e;

static testStage_e testStage = TEST_STAGE_INIT;

static int testLogFileNumber = 0;
static int validateLogFileNumber = 0;
static afatfsFilePtr_t rootDirectory;
static afatfsFinder_t finder;

void logFileCreated(afatfsFilePtr_t file)
{
    if (file) {
        afatfs_fclose(file);

        testLogFileNumber++;
        testStage = TEST_STAGE_CREATE_LOG_FILES;
    } else {
        testStage = TEST_STAGE_OPEN_ROOT;
    }
}

void logDirectoryOpened(afatfsFilePtr_t file) {
    rootDirectory = file;

    if (rootDirectory) {
        afatfs_findFirst(rootDirectory, &finder);
        testStage = TEST_STAGE_VALIDATE_DIRECTORY_CONTENTS;
    } else {
        fprintf(stderr, "Opening root directory failed\n");
        testStage = TEST_STAGE_FAILURE;
    }
}

bool continueTesting() {
    char filenameBuffer[13];
    afatfsOperationStatus_e status;
    fatDirectoryEntry_t *dirEntry;

    switch (testStage) {
        case TEST_STAGE_CREATE_LOG_FILES:
            if (testLogFileNumber == MAX_TEST_FILES) {
                testStage = TEST_STAGE_OPEN_ROOT;
            } else {
                testStage = TEST_STAGE_IDLE;

                sprintf(filenameBuffer, "LOG%05d.TXT", testLogFileNumber);

                afatfs_fopen(filenameBuffer, "a", logFileCreated);
            }
        break;
        case TEST_STAGE_IDLE:
            // Waiting for file operations...
        break;
        case TEST_STAGE_OPEN_ROOT:
            if (testLogFileNumber == 0) {
                fprintf(stderr, "[Fail]     Failed to create any files in the root directory\n");
                exit(-1);
            } else {
                afatfs_fopen(".", "r", logDirectoryOpened);
            }
        break;
        case TEST_STAGE_VALIDATE_DIRECTORY_CONTENTS:
            status = afatfs_findNext(rootDirectory, &finder, &dirEntry);

            if (status == AFATFS_OPERATION_SUCCESS) {
                if (dirEntry == NULL) {
                    if (validateLogFileNumber < testLogFileNumber || testLogFileNumber == 0) {
                        /*
                         * The number of files we were told were written correctly is less than the entries we were
                         * able to read back from the root directory!
                         */
                        testStage = TEST_STAGE_FAILURE;
                    } else {
                        testStage = TEST_STAGE_COMPLETE;
                    }
                } else {
                    sprintf(filenameBuffer, "LOG%05dTXT", validateLogFileNumber);

                    if (memcmp(filenameBuffer, dirEntry->filename, FAT_FILENAME_LENGTH) == 0) {
                        validateLogFileNumber++;
                    }
                }
            }
        break;
        case TEST_STAGE_FAILURE:
            fprintf(stderr, "[Fail]     Root directory only retained %d/%d files\n", validateLogFileNumber, testLogFileNumber);
            exit(-1);

        case TEST_STAGE_COMPLETE:
            fprintf(stderr, "[Success]  Root directory holds %d files\n", validateLogFileNumber);
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
