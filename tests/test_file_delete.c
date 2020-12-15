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
#define RECLAIM_LOG_ENTRY_COUNT (1024 * 100) / TEST_LOG_ENTRY_SIZE
// 2000 of those files should exceed the capacity of a 100MB test volume if deletion is broken:
#define RECLAIM_LOG_FILE_COUNT 2000

// We want the files to have at least a couple of clusters so we can ensure their FAT chains are intact
#define RETAIN_LOG_ENTRY_COUNT (afatfs_clusterSize() * 2 + 128) / TEST_LOG_ENTRY_SIZE

// Import these normally-internal methods for testing
extern uint32_t afatfs_clusterSize();

typedef enum {
    TEST_STAGE_INITIAL = 0,
    TEST_STAGE_SPACE_RECLAIM_BEGIN = 0,
    TEST_STAGE_SPACE_RECLAIM_CONTINUE,
    TEST_STAGE_SPACE_RETAIN_APPEND_BEGIN,
    TEST_STAGE_SPACE_RETAIN_APPEND_CONTINUE,
    TEST_STAGE_SPACE_RETAIN_SOLID_BEGIN,
    TEST_STAGE_SPACE_RETAIN_SOLID_CONTINUE,
    TEST_STAGE_COMPLETE
} testStage_e;

typedef enum {
    SPACE_RECLAIM_TEST_STAGE_EMPTY_INIT,
    SPACE_RECLAIM_TEST_STAGE_EMPTY_OPEN,
    SPACE_RECLAIM_TEST_STAGE_EMPTY_DELETE,
    SPACE_RECLAIM_TEST_STAGE_SOLID_APPEND_INIT,
    SPACE_RECLAIM_TEST_STAGE_SOLID_APPEND_OPEN,
    SPACE_RECLAIM_TEST_STAGE_SOLID_APPEND,
    SPACE_RECLAIM_TEST_STAGE_SOLID_APPEND_DELETE,
    SPACE_RECLAIM_TEST_STAGE_APPEND_INIT,
    SPACE_RECLAIM_TEST_STAGE_APPEND_OPEN,
    SPACE_RECLAIM_TEST_STAGE_APPEND,
    SPACE_RECLAIM_TEST_STAGE_APPEND_DELETE,
    SPACE_RECLAIM_TEST_STAGE_IDLE,
    SPACE_RECLAIM_TEST_STAGE_COMPLETE
} spaceReclaimTestStage_e;

typedef enum {
    SPACE_RETAIN_TEST_STAGE_CREATE_A,
    SPACE_RETAIN_TEST_STAGE_FILL_A,
    SPACE_RETAIN_TEST_STAGE_CLOSE_A,
    SPACE_RETAIN_TEST_STAGE_CREATE_B,
    SPACE_RETAIN_TEST_STAGE_FILL_B,
    SPACE_RETAIN_TEST_STAGE_CLOSE_B,
    SPACE_RETAIN_TEST_STAGE_CREATE_C,
    SPACE_RETAIN_TEST_STAGE_FILL_C,
    SPACE_RETAIN_TEST_STAGE_CLOSE_C,
    SPACE_RETAIN_TEST_STAGE_OPEN_B_FOR_UNLINK,
    SPACE_RETAIN_TEST_STAGE_UNLINK_B,
    SPACE_RETAIN_TEST_STAGE_VERIFY_A_OPEN,
    SPACE_RETAIN_TEST_STAGE_VERIFY_A_READ,
    SPACE_RETAIN_TEST_STAGE_VERIFY_A_UNLINK,
    SPACE_RETAIN_TEST_STAGE_VERIFY_B_OPEN,
    SPACE_RETAIN_TEST_STAGE_VERIFY_C_OPEN,
    SPACE_RETAIN_TEST_STAGE_VERIFY_C_READ,
    SPACE_RETAIN_TEST_STAGE_VERIFY_C_UNLINK,
    SPACE_RETAIN_TEST_STAGE_IDLE,
    SPACE_RETAIN_TEST_STAGE_COMPLETE,
} spaceRetainTestStage_e;

static testStage_e testStage = TEST_STAGE_INITIAL;
static spaceReclaimTestStage_e reclaimTestStage;
static spaceRetainTestStage_e retainTestStage;

static afatfsFilePtr_t testFile;
static afatfsFilePtr_t retainTestFileA, retainTestFileB, retainTestFileC;

static uint32_t logEntryIndex, logFileIndex;

void spaceReclaimTestFileCreatedForEmpty(afatfsFilePtr_t file)
{
    uint32_t position;

    testAssert(file, "Creating testfile failed");
    testAssert(afatfs_ftell(file, &position), "ftell should work after file opens");
    testAssert(position == 0, "File opened for solid append didn't start at offset 0");

    testFile = file;
    reclaimTestStage = SPACE_RECLAIM_TEST_STAGE_EMPTY_DELETE;
}

void spaceReclaimTestFileEmptyDeleted()
{
    testFile = NULL;
    logFileIndex++;

    if (logFileIndex < RECLAIM_LOG_FILE_COUNT) {
        reclaimTestStage = SPACE_RECLAIM_TEST_STAGE_EMPTY_OPEN;
    } else {
        reclaimTestStage = SPACE_RECLAIM_TEST_STAGE_SOLID_APPEND_INIT;
    }
}

void spaceReclaimTestFileCreatedForSolidAppend(afatfsFilePtr_t file)
{
    uint32_t position;

    testAssert(file, "Creating testfile failed");
    testAssert(afatfs_ftell(file, &position), "ftell should work after file opens");
    testAssert(position == 0, "File opened for solid append didn't start at offset 0");

    testFile = file;
    reclaimTestStage = SPACE_RECLAIM_TEST_STAGE_SOLID_APPEND;
}

void spaceReclaimTestFileCreatedForAppend(afatfsFilePtr_t file)
{
    uint32_t position;

    testAssert(file, "Creating testfile failed");
    testAssert(afatfs_ftell(file, &position), "ftell should work after file opens");
    testAssert(position == 0, "File opened for append didn't start at offset 0");

    testFile = file;
    reclaimTestStage = SPACE_RECLAIM_TEST_STAGE_APPEND;
}

void spaceReclaimTestFileSolidAppendDeleted()
{
    testFile = NULL;
    logFileIndex++;

    if (logFileIndex < RECLAIM_LOG_FILE_COUNT) {
        reclaimTestStage = SPACE_RECLAIM_TEST_STAGE_SOLID_APPEND_OPEN;
    } else {
        reclaimTestStage = SPACE_RECLAIM_TEST_STAGE_APPEND_INIT;
    }
}

void spaceReclaimTestFileAppendDeleted()
{
    testFile = NULL;
    logFileIndex++;

    if (logFileIndex < RECLAIM_LOG_FILE_COUNT) {
        reclaimTestStage = SPACE_RECLAIM_TEST_STAGE_APPEND_OPEN;
    } else {
        reclaimTestStage = SPACE_RECLAIM_TEST_STAGE_COMPLETE;
    }
}

/*
 * Test that creating and deleting files definitely releases free space back to the filesystem,
 * by allocating and deleting more files than the disk could hold if they were being retained.
 *
 * start - For the first call of the test, set start to true
 *
 * Returns true if the test is still continuing, or false if the test was completed successfully.
 */
bool continueSpaceReclaimTest(bool start)
{
    if (start) {
        reclaimTestStage = SPACE_RECLAIM_TEST_STAGE_EMPTY_INIT;
    }

    switch (reclaimTestStage) {
        case SPACE_RECLAIM_TEST_STAGE_EMPTY_INIT:
            reclaimTestStage = SPACE_RECLAIM_TEST_STAGE_EMPTY_OPEN;
            logFileIndex = 0;
        break;
        case SPACE_RECLAIM_TEST_STAGE_EMPTY_OPEN:
            reclaimTestStage = SPACE_RECLAIM_TEST_STAGE_IDLE;
            logEntryIndex = 0;
            afatfs_fopen("test.txt", "w+", spaceReclaimTestFileCreatedForEmpty);
        break;
        case SPACE_RECLAIM_TEST_STAGE_EMPTY_DELETE:
            if (afatfs_funlink(testFile, spaceReclaimTestFileEmptyDeleted)) {
                // Wait for the unlink to complete
                reclaimTestStage = SPACE_RECLAIM_TEST_STAGE_IDLE;
            }
        break;
        case SPACE_RECLAIM_TEST_STAGE_SOLID_APPEND_INIT:
            reclaimTestStage = SPACE_RECLAIM_TEST_STAGE_SOLID_APPEND_OPEN;
            logFileIndex = 0;
        break;
        case SPACE_RECLAIM_TEST_STAGE_SOLID_APPEND_OPEN:
            reclaimTestStage = SPACE_RECLAIM_TEST_STAGE_IDLE;

            logEntryIndex = 0;

            afatfs_fopen("test.txt", "as", spaceReclaimTestFileCreatedForSolidAppend);
        break;
        case SPACE_RECLAIM_TEST_STAGE_SOLID_APPEND:
            if (writeLogTestEntries(testFile, &logEntryIndex, RECLAIM_LOG_ENTRY_COUNT)) {
                reclaimTestStage = SPACE_RECLAIM_TEST_STAGE_SOLID_APPEND_DELETE;
            }
        break;
        case SPACE_RECLAIM_TEST_STAGE_SOLID_APPEND_DELETE:
            if (afatfs_funlink(testFile, spaceReclaimTestFileSolidAppendDeleted)) {
                // Wait for the unlink to complete
                reclaimTestStage = SPACE_RECLAIM_TEST_STAGE_IDLE;
            }
        break;
        case SPACE_RECLAIM_TEST_STAGE_APPEND_INIT:
            reclaimTestStage = SPACE_RECLAIM_TEST_STAGE_APPEND_OPEN;
            logFileIndex = 0;
        break;
        case SPACE_RECLAIM_TEST_STAGE_APPEND_OPEN:
            reclaimTestStage = SPACE_RECLAIM_TEST_STAGE_IDLE;

            logEntryIndex = 0;

            afatfs_fopen("test.txt", "a", spaceReclaimTestFileCreatedForAppend);
        break;
        case SPACE_RECLAIM_TEST_STAGE_APPEND:
            if (writeLogTestEntries(testFile, &logEntryIndex, RECLAIM_LOG_ENTRY_COUNT)) {
                reclaimTestStage = SPACE_RECLAIM_TEST_STAGE_APPEND_DELETE;
            }
        break;
        case SPACE_RECLAIM_TEST_STAGE_APPEND_DELETE:
            if (afatfs_funlink(testFile, spaceReclaimTestFileAppendDeleted)) {
                // Wait for the unlink to complete
                reclaimTestStage = SPACE_RECLAIM_TEST_STAGE_IDLE;
            }
        break;
        case SPACE_RECLAIM_TEST_STAGE_IDLE:
            // Waiting for file operations...
        break;
        case SPACE_RECLAIM_TEST_STAGE_COMPLETE:
            fprintf(stderr, "[Success]  Free space is reclaimed when files are deleted (empty, solid and standard append)\n");
            return false;
    }

    return true; // Still work to be done
}

void spaceRetainTestFileACreated(afatfsFilePtr_t file)
{
    uint32_t position;

    testAssert(file, "Creating testfile failed");

    testAssert(afatfs_ftell(file, &position), "ftell should work after file opens");
    testAssert(position == 0, "Created file didn't start at offset 0");

    retainTestFileA = file;

    logEntryIndex = 0;
    retainTestStage = SPACE_RETAIN_TEST_STAGE_FILL_A;
}

void spaceRetainTestFileBCreated(afatfsFilePtr_t file)
{
    uint32_t position;

    testAssert(file, "Creating testfile failed");

    testAssert(afatfs_ftell(file, &position), "ftell should work after file opens");
    testAssert(position == 0, "Created file didn't start at offset 0");

    retainTestFileB = file;

    logEntryIndex = 0;
    retainTestStage = SPACE_RETAIN_TEST_STAGE_FILL_B;
}

void spaceRetainTestFileCCreated(afatfsFilePtr_t file)
{
    uint32_t position;

    testAssert(file, "Creating testfile failed");

    testAssert(afatfs_ftell(file, &position), "ftell should work after file opens");
    testAssert(position == 0, "Created file didn't start at offset 0");

    retainTestFileC = file;

    logEntryIndex = 0;
    retainTestStage = SPACE_RETAIN_TEST_STAGE_FILL_C;
}

void retainTestFileBDeleted(afatfsFilePtr_t file)
{
    (void) file;

    retainTestStage = SPACE_RETAIN_TEST_STAGE_VERIFY_A_OPEN;
}

static void retainTestFileAOpenedForRead(afatfsFilePtr_t file)
{
    testAssert(file, "Opening log for read failed");

    retainTestFileA = file;
    retainTestStage = SPACE_RETAIN_TEST_STAGE_VERIFY_A_READ;
}

static void retainTestFileBOpenedForUnlink(afatfsFilePtr_t file)
{
    testAssert(file, "Opening log for unlink failed");

    retainTestFileB = file;
    retainTestStage = SPACE_RETAIN_TEST_STAGE_UNLINK_B;
}

static void retainTestFileBOpenedForRead(afatfsFilePtr_t file)
{
    testAssert(file == NULL, "Deleted file was still openable!");

    retainTestStage = SPACE_RETAIN_TEST_STAGE_VERIFY_C_OPEN;
}

static void retainTestFileCOpenedForRead(afatfsFilePtr_t file)
{
    testAssert(file, "Opening log for read failed");

    retainTestFileC = file;
    retainTestStage = SPACE_RETAIN_TEST_STAGE_VERIFY_C_READ;
}

/*
 * Test that deleting files does not damage other allocated files.
 *
 * start - For the first call of the test, set start to true
 *
 * Returns true if the test is still continuing, or false if the test was completed successfully.
 */
bool continueSpaceRetainTest(bool start, const char *fileMode)
{
    if (start) {
        retainTestStage = SPACE_RETAIN_TEST_STAGE_CREATE_A;
    }

    doMore:
    switch (retainTestStage) {
        case SPACE_RETAIN_TEST_STAGE_CREATE_A:
            retainTestStage = SPACE_RETAIN_TEST_STAGE_IDLE;
            afatfs_fopen("test-a.txt", fileMode, spaceRetainTestFileACreated);
        break;
        case SPACE_RETAIN_TEST_STAGE_FILL_A:
            if (writeLogTestEntries(retainTestFileA, &logEntryIndex, RETAIN_LOG_ENTRY_COUNT)) {
                retainTestStage = SPACE_RETAIN_TEST_STAGE_CLOSE_A;
            }
        break;
        case SPACE_RETAIN_TEST_STAGE_CLOSE_A:
            if (afatfs_fclose(retainTestFileA, NULL)) {
                retainTestFileA = NULL;
                retainTestStage = SPACE_RETAIN_TEST_STAGE_CREATE_B;
            }
        break;
        case SPACE_RETAIN_TEST_STAGE_CREATE_B:
            retainTestStage = SPACE_RETAIN_TEST_STAGE_IDLE;
            afatfs_fopen("test-b.txt", fileMode, spaceRetainTestFileBCreated);
        break;
        case SPACE_RETAIN_TEST_STAGE_FILL_B:
            if (writeLogTestEntries(retainTestFileB, &logEntryIndex, RETAIN_LOG_ENTRY_COUNT)) {
                retainTestStage = SPACE_RETAIN_TEST_STAGE_CLOSE_B;
            }
        break;
        case SPACE_RETAIN_TEST_STAGE_CLOSE_B:
            if (afatfs_fclose(retainTestFileB, NULL)) {
                retainTestFileB = NULL;
                retainTestStage = SPACE_RETAIN_TEST_STAGE_CREATE_C;
            }
        break;
        case SPACE_RETAIN_TEST_STAGE_CREATE_C:
            retainTestStage = SPACE_RETAIN_TEST_STAGE_IDLE;
            afatfs_fopen("test-c.txt", fileMode, spaceRetainTestFileCCreated);
        break;
        case SPACE_RETAIN_TEST_STAGE_FILL_C:
            if (writeLogTestEntries(retainTestFileC, &logEntryIndex, RETAIN_LOG_ENTRY_COUNT)) {
                retainTestStage = SPACE_RETAIN_TEST_STAGE_CLOSE_C;
            }
        break;
        case SPACE_RETAIN_TEST_STAGE_CLOSE_C:
            if (afatfs_fclose(retainTestFileC, NULL)) {
                retainTestFileC = NULL;
                retainTestStage = SPACE_RETAIN_TEST_STAGE_OPEN_B_FOR_UNLINK;
            }
        break;
        case SPACE_RETAIN_TEST_STAGE_OPEN_B_FOR_UNLINK:
            retainTestStage = SPACE_RETAIN_TEST_STAGE_IDLE;

            afatfs_fopen("test-b.txt", "r", retainTestFileBOpenedForUnlink);
        break;
        case SPACE_RETAIN_TEST_STAGE_UNLINK_B:
            if (afatfs_funlink(retainTestFileB, retainTestFileBDeleted)) {
                retainTestFileB = NULL;
                retainTestStage = SPACE_RETAIN_TEST_STAGE_IDLE;
            }
        break;
        case SPACE_RETAIN_TEST_STAGE_VERIFY_A_OPEN:
            retainTestStage = SPACE_RETAIN_TEST_STAGE_IDLE;

            logEntryIndex = 0;

            afatfs_fopen("test-a.txt", "r", retainTestFileAOpenedForRead);
        break;
        case SPACE_RETAIN_TEST_STAGE_VERIFY_A_READ:
            if (validateLogTestEntries(retainTestFileA, &logEntryIndex, RETAIN_LOG_ENTRY_COUNT)) {
                retainTestStage = SPACE_RETAIN_TEST_STAGE_VERIFY_A_UNLINK;
                goto doMore;
            }
        break;
        case SPACE_RETAIN_TEST_STAGE_VERIFY_A_UNLINK:
            if (afatfs_funlink(retainTestFileA, NULL)) {
                retainTestFileA = NULL;
                retainTestStage = SPACE_RETAIN_TEST_STAGE_VERIFY_B_OPEN;
                goto doMore;
            }
        break;
        case SPACE_RETAIN_TEST_STAGE_VERIFY_B_OPEN:
            retainTestStage = SPACE_RETAIN_TEST_STAGE_IDLE;

            // We expect this open to fail
            afatfs_fopen("test-b.txt", "r", retainTestFileBOpenedForRead);
        break;
        case SPACE_RETAIN_TEST_STAGE_VERIFY_C_OPEN:
            retainTestStage = SPACE_RETAIN_TEST_STAGE_IDLE;

            logEntryIndex = 0;

            afatfs_fopen("test-c.txt", "r", retainTestFileCOpenedForRead);
        break;
        case SPACE_RETAIN_TEST_STAGE_VERIFY_C_READ:
            if (validateLogTestEntries(retainTestFileC, &logEntryIndex, RETAIN_LOG_ENTRY_COUNT)) {
                retainTestStage = SPACE_RETAIN_TEST_STAGE_VERIFY_C_UNLINK;
                goto doMore;
            }
        break;
        case SPACE_RETAIN_TEST_STAGE_VERIFY_C_UNLINK:
            if (afatfs_funlink(retainTestFileC, NULL)) {
                retainTestFileC = NULL;
                retainTestStage = SPACE_RETAIN_TEST_STAGE_COMPLETE;
                goto doMore;
            }
        break;
        case SPACE_RETAIN_TEST_STAGE_IDLE:
            // Waiting for file operations...
        break;
        case SPACE_RETAIN_TEST_STAGE_COMPLETE:
            fprintf(stderr, "[Success]  Allocated files are retained when unrelated files are deleted (file mode: %s)\n", fileMode);
            return false;
    }

    return true; // Still work to be done
}


bool continueTesting()
{
    switch (testStage) {
        case TEST_STAGE_SPACE_RECLAIM_BEGIN:
        case TEST_STAGE_SPACE_RECLAIM_CONTINUE:
            if (!continueSpaceReclaimTest(testStage == TEST_STAGE_SPACE_RECLAIM_BEGIN)) {
                testStage = TEST_STAGE_SPACE_RETAIN_APPEND_BEGIN;
            } else {
                testStage = TEST_STAGE_SPACE_RECLAIM_CONTINUE;
            }
        break;
        case TEST_STAGE_SPACE_RETAIN_APPEND_BEGIN:
        case TEST_STAGE_SPACE_RETAIN_APPEND_CONTINUE:
            if (!continueSpaceRetainTest(testStage == TEST_STAGE_SPACE_RETAIN_APPEND_BEGIN, "a")) {
                testStage = TEST_STAGE_SPACE_RETAIN_SOLID_BEGIN;
            } else {
                testStage = TEST_STAGE_SPACE_RETAIN_APPEND_CONTINUE;
            }
        break;
        case TEST_STAGE_SPACE_RETAIN_SOLID_BEGIN:
        case TEST_STAGE_SPACE_RETAIN_SOLID_CONTINUE:
            if (!continueSpaceRetainTest(testStage == TEST_STAGE_SPACE_RETAIN_SOLID_BEGIN, "as")) {
                testStage = TEST_STAGE_COMPLETE;
            } else {
                testStage = TEST_STAGE_SPACE_RETAIN_SOLID_CONTINUE;
            }
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
