/**
 * Verify that a file's size in the directory is set to the logical size of the file upon close.
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
    FILESIZE_TEST_STAGE_OPEN,
    FILESIZE_TEST_STAGE_APPEND,
    FILESIZE_TEST_STAGE_CLOSE,
    FILESIZE_TEST_STAGE_FLUSH,
    FILESIZE_TEST_STAGE_READ_OPEN,
    FILESIZE_TEST_STAGE_READ_SEEK_TO_END,
    FILESIZE_TEST_STAGE_READ_MEASURE_FILE_LENGTH,
    FILESIZE_TEST_STAGE_READ_VALIDATE,
    FILESIZE_TEST_STAGE_READ_CLOSE,
    FILESIZE_TEST_STAGE_IDLE
} filesizeTestStage_e;

static testStage_e testStage = TEST_STAGE_INITIAL;

static filesizeTestStage_e filesizeStage;
static afatfsFilePtr_t filesizeFile;

static void initFilesystem()
{
    afatfs_init();

    while (afatfs_getFilesystemState() != AFATFS_FILESYSTEM_STATE_READY) {
        afatfs_poll();

        if (afatfs_getFilesystemState() == AFATFS_FILESYSTEM_STATE_FATAL) {
            fprintf(stderr, "[Fail]     Fatal filesystem error during init\n");
            exit(-1);
        }
    }
}

static void filesizeTestFileCreatedForAppend(afatfsFilePtr_t file)
{
    testAssert(file, "Creating testfile failed");
    testAssert(afatfs_feof(file), "feof() should be true after creating file");

    filesizeFile = file;
    filesizeStage = FILESIZE_TEST_STAGE_APPEND;
}

static void filesizeTestFileOpenedForRead(afatfsFilePtr_t file)
{
    testAssert(file, "Opening log for read failed");

    filesizeFile = file;
    filesizeStage = FILESIZE_TEST_STAGE_READ_SEEK_TO_END;
}

/**
 * Continue testing for logical filesize recording in directory entries.
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
        filesizeStage = FILESIZE_TEST_STAGE_OPEN;
    }

    switch (filesizeStage) {
        case FILESIZE_TEST_STAGE_OPEN:
            filesizeStage = FILESIZE_TEST_STAGE_IDLE;

            logEntryIndex = 0;

            afatfs_fopen(filename, fileMode, filesizeTestFileCreatedForAppend);
        break;
        case FILESIZE_TEST_STAGE_APPEND:
            // Write just more than one sector of log entries
            if (writeLogTestEntries(filesizeFile, &logEntryIndex, logEntriesToWrite)) {
                testAssert(afatfs_feof(filesizeFile), "feof() should be true after extending file with write");
                testAssert(afatfs_ftell(filesizeFile, &logFileSize), "ftell() expected to work when no file operation queued");
                testAssert(logFileSize == bytesToWrite, "Log file correct after writes within a cluster");

                testAssert(afatfs_fseek(filesizeFile, 0, AFATFS_SEEK_END) == AFATFS_OPERATION_SUCCESS, "Seeks to end of file when we're already at end should be immediate");
                testAssert(afatfs_ftell(filesizeFile, &logFileSize), "ftell() should work after immediate seek");
                testAssert(logFileSize == bytesToWrite, "fseek() seeked to the wrong position for AFATFS_SEEK_END");

                filesizeStage = FILESIZE_TEST_STAGE_CLOSE;
            }
        break;
        case FILESIZE_TEST_STAGE_CLOSE:
            if (afatfs_fclose(filesizeFile, NULL)) {
                filesizeStage = FILESIZE_TEST_STAGE_FLUSH;
                return false; // Test is over now!
            }
        break;
        case FILESIZE_TEST_STAGE_FLUSH:
            // Wait for all the flushable data (completed sectors and updated directory entries) to make it to the disk
            if (afatfs_flush() && sdcard_sim_isReady()) {
                // Simulate a power interruption by restarting the filesystem
                afatfs_destroy(true);
                filesizeFile = NULL;

                initFilesystem();

                filesizeStage = FILESIZE_TEST_STAGE_READ_OPEN;
            }
        break;
        case FILESIZE_TEST_STAGE_READ_OPEN:
            filesizeStage = FILESIZE_TEST_STAGE_IDLE;
            logEntryIndex = 0;
            afatfs_fopen(filename, "r", filesizeTestFileOpenedForRead);
        break;
        case FILESIZE_TEST_STAGE_READ_SEEK_TO_END:
            testAssert(afatfs_fseek(filesizeFile, 0, AFATFS_SEEK_END) != AFATFS_OPERATION_FAILURE, "Seek to end should work");

            filesizeStage = FILESIZE_TEST_STAGE_READ_MEASURE_FILE_LENGTH;
        break;
        case FILESIZE_TEST_STAGE_READ_MEASURE_FILE_LENGTH:
            // We must wait for the the seek to complete before ftell() will succeed
            if (afatfs_ftell(filesizeFile, &position) == AFATFS_OPERATION_SUCCESS) {
                testAssert(position == bytesToWrite, "Logical filesize was not recorded correctly after close");

                testAssert(afatfs_fseek(filesizeFile, 0, AFATFS_SEEK_SET) == AFATFS_OPERATION_SUCCESS, "Should be able to seek to begininng of file instantly");
                filesizeStage = FILESIZE_TEST_STAGE_READ_VALIDATE;
            }
        break;
        case FILESIZE_TEST_STAGE_READ_VALIDATE:
            // All the data we wrote must be readable
            if (validateLogTestEntries(filesizeFile, &logEntryIndex, bytesToWrite / TEST_LOG_ENTRY_SIZE)) {
                filesizeStage = FILESIZE_TEST_STAGE_READ_CLOSE;
            }
        break;
        case FILESIZE_TEST_STAGE_READ_CLOSE:
            if (afatfs_fclose(filesizeFile, NULL)) {
                return false; // Test is over now!
            }
        break;
        case FILESIZE_TEST_STAGE_IDLE:
            // Waiting for callbacks
        break;
    }

    return true; // Still continuing test
}

bool continueTesting() {

    switch (testStage) {
        case TEST_STAGE_SOLID_APPEND_BEGIN:
        case TEST_STAGE_SOLID_APPEND_CONTINUE:
            // Write a sector and an incomplete sector to make sure the logical filesize is properly recorded
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
                fprintf(stderr, "[Success]  Logical filesize recorded accurately after file close (\"as\" and \"a\" filemodes, 1 sector written)\n");
                testStage = TEST_STAGE_SOLID_APPEND_LARGE_BEGIN;
            }
        break;
        case TEST_STAGE_SOLID_APPEND_LARGE_BEGIN:
        case TEST_STAGE_SOLID_APPEND_LARGE_CONTINUE:
            /* Write one supercluster + a little bit, so we can verify that the filesize in the directory gets increased
             * properly when superclusters are added to the file
             */
            if (continueFilesizeTest(testStage == TEST_STAGE_SOLID_APPEND_LARGE_BEGIN, "test3.txt", "as", afatfs_superClusterSize() / TEST_LOG_ENTRY_SIZE + 4)) {
                testStage = TEST_STAGE_SOLID_APPEND_LARGE_CONTINUE;
            } else {
                testStage = TEST_STAGE_APPEND_LARGE_BEGIN;
            }
        break;
        case TEST_STAGE_APPEND_LARGE_BEGIN:
        case TEST_STAGE_APPEND_LARGE_CONTINUE:
            /* Write one cluster + a little bit, so we can verify that the filesize in the directory gets increased
             * properly when superclusters are added to the file
             */
            if (continueFilesizeTest(testStage == TEST_STAGE_APPEND_LARGE_BEGIN, "test4.txt", "a", afatfs_clusterSize() / TEST_LOG_ENTRY_SIZE + 4)) {
                testStage = TEST_STAGE_APPEND_LARGE_CONTINUE;
            } else {
                fprintf(stderr, "[Success]  Logical filesize recorded accurately after file close (\"as\" and \"a\" filemodes, 1 cluster written)\n");
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
