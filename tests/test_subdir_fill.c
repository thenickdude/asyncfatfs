#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sdcard_sim.h"
#include "sdcard.h"
#include "fat_standard.h"
#include "asyncfatfs.h"

#include "common.h"

// FAT tops out at 64k files/subdir, but this seems enough for us to test:
#define MAX_TEST_FILES 10000

typedef enum {
    TEST_STAGE_INIT = 0,
    TEST_STAGE_CREATE_LOG_DIRECTORY = 0,
    TEST_STAGE_CREATE_LOG_FILES,
    TEST_STAGE_OPEN_LOG_DIRECTORY,
    TEST_STAGE_VALIDATE_DIRECTORY_CONTENTS,
    TEST_STAGE_IDLE,
    TEST_STAGE_COMPLETE,
    TEST_STAGE_FAILURE
} testStage_e;

static testStage_e testStage = TEST_STAGE_INIT;

static int testLogFileNumber = 0;
static int validateLogFileNumber = 0;
static afatfsFilePtr_t logDirectory;
static afatfsFinder_t finder;

static void logDirCreated(afatfsFilePtr_t dir)
{
    if (!dir) {
        fprintf(stderr, "Creating 'logs' directory failed\n");
        exit(EXIT_FAILURE);
    }

    afatfs_chdir(dir);
    testAssert(afatfs_fclose(dir, NULL), "Expected to be able to queue close on directory");

    testStage = TEST_STAGE_CREATE_LOG_FILES;
}

static void logFileCreated(afatfsFilePtr_t file)
{
    if (file) {
        afatfs_fclose(file, NULL);

        testLogFileNumber++;
        testStage = TEST_STAGE_CREATE_LOG_FILES;
    } else {
        testStage = TEST_STAGE_OPEN_LOG_DIRECTORY;
    }
}

static void logDirectoryOpened(afatfsFilePtr_t file) {
    logDirectory = file;

    if (logDirectory) {
        afatfs_findFirst(logDirectory, &finder);
        testStage = TEST_STAGE_VALIDATE_DIRECTORY_CONTENTS;
    } else {
        fprintf(stderr, "Opening subdirectory failed\n");
        testStage = TEST_STAGE_FAILURE;
    }
}

static bool continueTesting() {
    char filenameBuffer[13];
    afatfsOperationStatus_e status;
    fatDirectoryEntry_t *dirEntry;

    switch (testStage) {
        case TEST_STAGE_IDLE:
            // Waiting for file operations...
        break;
        case TEST_STAGE_CREATE_LOG_DIRECTORY:
            /*
             * The callback can be called before mkdir() returns, so set the testStage now to avoid stomping on state
             * set by the callback:
             */
            testStage = TEST_STAGE_IDLE;

            afatfs_mkdir("logs", logDirCreated);
        break;
        case TEST_STAGE_CREATE_LOG_FILES:
            if (testLogFileNumber == MAX_TEST_FILES) {
                testStage = TEST_STAGE_OPEN_LOG_DIRECTORY;
            } else {
                testStage = TEST_STAGE_IDLE;

                sprintf(filenameBuffer, "LOG%05d.TXT", testLogFileNumber);

                afatfs_fopen(filenameBuffer, "a", logFileCreated);
            }
        break;
        case TEST_STAGE_OPEN_LOG_DIRECTORY:
            afatfs_fopen(".", "r", logDirectoryOpened);
        break;
        case TEST_STAGE_VALIDATE_DIRECTORY_CONTENTS:
            status = afatfs_findNext(logDirectory, &finder, &dirEntry);

            if (status == AFATFS_OPERATION_SUCCESS) {
                if (dirEntry == NULL) {
                    if (validateLogFileNumber < testLogFileNumber) {
                        /*
                         * The number of files we were told were written correctly is less than the entries we were
                         * able to read back from the directory!
                         */
                        testStage = TEST_STAGE_FAILURE;
                    } else {
                        testStage = TEST_STAGE_COMPLETE;
                    }
                } else {
                    sprintf(filenameBuffer, "LOG%05dTXT", validateLogFileNumber);

                    if (memcmp(filenameBuffer, dirEntry->filename, FAT_FILENAME_LENGTH)== 0) {
                        validateLogFileNumber++;
                    }
                }
            }
        break;
        case TEST_STAGE_FAILURE:
            fprintf(stderr, "[Fail]     Subdirectory only retained %d/%d files\n", validateLogFileNumber, testLogFileNumber);
            exit(-1);

        case TEST_STAGE_COMPLETE:
            fprintf(stderr, "[Success]  Subdirectory holds %d files\n", validateLogFileNumber);
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

    afatfs_init();

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
