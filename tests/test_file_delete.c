/**
 * This test repeatedly creates and deletes a file in two modes, freefile allocation mode and regular allocation mode.
 *
 * If deletion isn't implemented properly, this will cause the volume to fill up.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sdcard_sim.h"
#include "sdcard.h"
#include "asyncfatfs.h"

#include "common.h"

// Make the test file about 100kB (we don't a large amount of regular free space to play with when using a freefile)
#define LOG_ENTRY_COUNT (1024 * 100) / TEST_LOG_ENTRY_SIZE
// 2000 of those files should exceed the capacity of a 100MB test volume if deletion is broken:
#define LOG_FILE_COUNT 2000

typedef enum {
    TEST_STAGE_INITIAL = 0,
    TEST_STAGE_SOLID_APPEND_INIT = 0,
    TEST_STAGE_SOLID_APPEND_OPEN,
    TEST_STAGE_SOLID_APPEND,
    TEST_STAGE_SOLID_APPEND_DELETE,
    TEST_STAGE_APPEND_INIT,
    TEST_STAGE_APPEND_OPEN,
    TEST_STAGE_APPEND,
    TEST_STAGE_APPEND_DELETE,
    TEST_STAGE_IDLE,
    TEST_STAGE_COMPLETE
} testStage_e;

static testStage_e testStage = TEST_STAGE_INITIAL;
static afatfsFilePtr_t testFile;

static uint32_t logEntryIndex, logFileIndex;

void logFileCreatedForSolidAppend(afatfsFilePtr_t file)
{
    uint32_t position;

    testAssert(file, "Creating testfile failed");
    testAssert(afatfs_ftell(file, &position), "ftell should work after file opens");
    testAssert(position == 0, "File opened for solid append didn't start at offset 0");

    testFile = file;
    testStage = TEST_STAGE_SOLID_APPEND;
}

void logFileCreatedForAppend(afatfsFilePtr_t file)
{
    uint32_t position;

    testAssert(file, "Creating testfile failed");
    testAssert(afatfs_ftell(file, &position), "ftell should work after file opens");
    testAssert(position == 0, "File opened for append didn't start at offset 0");

    testFile = file;
    testStage = TEST_STAGE_APPEND;
}

void testFileSolidAppendDeleted()
{
    testFile = NULL;
    logFileIndex++;

    if (logFileIndex < LOG_FILE_COUNT) {
        testStage = TEST_STAGE_SOLID_APPEND_OPEN;
    } else {
        fprintf(stderr, "[Success]  Created and deleted %d files for solid append\n", logFileIndex);
        testStage = TEST_STAGE_APPEND_INIT;
    }
}

void testFileAppendDeleted()
{
    testFile = NULL;
    logFileIndex++;

    if (logFileIndex < LOG_FILE_COUNT) {
        testStage = TEST_STAGE_APPEND_OPEN;
    } else {
        fprintf(stderr, "[Success]  Created and deleted %d files for append\n", logFileIndex);
        testStage = TEST_STAGE_COMPLETE;
    }
}

bool continueTesting()
{
    switch (testStage) {
        case TEST_STAGE_SOLID_APPEND_INIT:
            testStage = TEST_STAGE_SOLID_APPEND_OPEN;
            logFileIndex = 0;
        break;
        case TEST_STAGE_SOLID_APPEND_OPEN:
            testStage = TEST_STAGE_IDLE;

            logEntryIndex = 0;

            afatfs_fopen("test.txt", "as", logFileCreatedForSolidAppend);
        break;
        case TEST_STAGE_SOLID_APPEND:
            if (writeLogTestEntries(testFile, &logEntryIndex, LOG_ENTRY_COUNT)) {
                testStage = TEST_STAGE_SOLID_APPEND_DELETE;
            }
        break;
        case TEST_STAGE_SOLID_APPEND_DELETE:
            if (afatfs_funlink(testFile, testFileSolidAppendDeleted)) {
                // Wait for the unlink to complete
                testStage = TEST_STAGE_IDLE;
            }
        break;
        case TEST_STAGE_APPEND_INIT:
            testStage = TEST_STAGE_APPEND_OPEN;
            logFileIndex = 0;
        break;
        case TEST_STAGE_APPEND_OPEN:
            testStage = TEST_STAGE_IDLE;

            logEntryIndex = 0;

            afatfs_fopen("test.txt", "a", logFileCreatedForAppend);
        break;
        case TEST_STAGE_APPEND:
            if (writeLogTestEntries(testFile, &logEntryIndex, LOG_ENTRY_COUNT)) {
                testStage = TEST_STAGE_APPEND_DELETE;
            }
        break;
        case TEST_STAGE_APPEND_DELETE:
            if (afatfs_funlink(testFile, testFileAppendDeleted)) {
                // Wait for the unlink to complete
                testStage = TEST_STAGE_IDLE;
            }
        break;
        case TEST_STAGE_IDLE:
            // Waiting for file operations...
        break;
        case TEST_STAGE_COMPLETE:
            fprintf(stderr, "[Success]  File deletion works\n");
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

    while (!afatfs_destroy(false)) {
    }

    sdcard_sim_destroy();

    return EXIT_SUCCESS;
}
