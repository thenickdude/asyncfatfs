#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sdcard_sim.h"
#include "sdcard.h"
#include "asyncfatfs.h"

#include "common.h"

#define LOG_ENTRY_COUNT 1000

typedef enum {
    TEST_STAGE_INITIAL = 0,
    TEST_STAGE_SOLID_APPEND_OPEN = 0,
    TEST_STAGE_SOLID_APPEND,
    TEST_STAGE_SOLID_APPEND_CLOSE,
    TEST_STAGE_READ_OPEN,
    TEST_STAGE_READ_VALIDATE,
    TEST_STAGE_READ_CLOSE,
    TEST_STAGE_APPEND_OPEN,
    TEST_STAGE_APPEND,
    TEST_STAGE_APPEND_CLOSE,
    TEST_STAGE_READ_WRITE_OPEN,
    TEST_STAGE_READ_WRITE_VALIDATE,
    TEST_STAGE_READ_WRITE_CLOSE,
    TEST_STAGE_WRITE_OPEN,
    TEST_STAGE_WRITE_CLOSE,
    TEST_STAGE_IDLE,
    TEST_STAGE_COMPLETE
} testStage_e;

static testStage_e testStage = TEST_STAGE_INITIAL;
static afatfsFilePtr_t testFile;

static uint32_t logEntryIndex = 0;
static uint32_t logFileSize;

void logFileCreatedForSolidAppend(afatfsFilePtr_t file)
{
    uint32_t position;

    testAssert(file, "Creating testfile failed");
    testAssert(afatfs_ftell(file, &position), "ftell should work after file opens");
    testAssert(position == 0, "File opened for solid append didn't start at offset 0");

    testFile = file;
    testStage = TEST_STAGE_SOLID_APPEND;
}

void logFileOpenedForAppend(afatfsFilePtr_t file)
{
    uint32_t position;

    testAssert(file, "Opening testfile for append failed");
    testAssert(afatfs_ftell(file, &position), "ftell should work after file opens");
    testAssert(position == logFileSize, "Cursor in incorrect initial position when opened for append");

    testFile = file;
    testStage = TEST_STAGE_APPEND;
}

void logFileOpenedForRead(afatfsFilePtr_t file)
{
    uint32_t position;

    testAssert(file, "Opening log for read failed");

    testFile = file;

    testAssert(afatfs_ftell(file, &position), "ftell should work after file opens");
    testAssert(position == 0, "File opened for read didn't start at offset 0");

    testStage = TEST_STAGE_READ_VALIDATE;
}

void logFileOpenedForReadWrite(afatfsFilePtr_t file)
{
    uint32_t position;

    testAssert(file, "Opening log for read/write failed");

    testFile = file;

    testAssert(afatfs_ftell(file, &position), "ftell should work after file opens");
    testAssert(position == 0, "File opened for read/write didn't start at offset 0");

    testStage = TEST_STAGE_READ_WRITE_VALIDATE;
}

void logFileOpenedForWrite(afatfsFilePtr_t file)
{
    uint32_t position;

    testAssert(file, "Opening log for write failed");

    testFile = file;

    testAssert(afatfs_ftell(file, &position), "ftell should work after file opens");
    testAssert(position == 0, "File opened for write didn't start at offset 0");

    testAssert(afatfs_fseek(file, 0, AFATFS_SEEK_END) == AFATFS_OPERATION_SUCCESS, "fseek to end of empty file should complete instantly");
    testAssert(afatfs_ftell(file, &position), "ftell() should run without queueing");
    testAssert(position == 0, "Opening file for write did not truncate the file");

    testStage = TEST_STAGE_WRITE_CLOSE;
}

bool continueTesting() {
    switch (testStage) {
        case TEST_STAGE_SOLID_APPEND_OPEN:
            testStage = TEST_STAGE_IDLE;

            logEntryIndex = 0;

            afatfs_fopen("test.txt", "as", logFileCreatedForSolidAppend);
        break;
        case TEST_STAGE_SOLID_APPEND:
            if (writeLogTestEntries(testFile, &logEntryIndex, LOG_ENTRY_COUNT)) {
                testAssert(afatfs_ftell(testFile, &logFileSize), "ftell() expected to work when no file operation queued");
                testAssert(logFileSize > 0, "Log file still empty after solid appends");

                testStage = TEST_STAGE_SOLID_APPEND_CLOSE;
            }
        break;
        case TEST_STAGE_SOLID_APPEND_CLOSE:
            if (afatfs_fclose(testFile, NULL)) {
                testStage = TEST_STAGE_READ_OPEN;
            }
        break;
        case TEST_STAGE_READ_OPEN:
            testStage = TEST_STAGE_IDLE;
            logEntryIndex = 0;
            afatfs_fopen("test.txt", "r", logFileOpenedForRead);
        break;
        case TEST_STAGE_READ_VALIDATE:
            if (validateLogTestEntries(testFile, &logEntryIndex, LOG_ENTRY_COUNT)) {
                testStage = TEST_STAGE_READ_CLOSE;
            }
        break;
        case TEST_STAGE_READ_CLOSE:
            if (afatfs_fclose(testFile, NULL)) {
                testStage = TEST_STAGE_APPEND_OPEN;
            }
        break;
        case TEST_STAGE_APPEND_OPEN:
            testStage = TEST_STAGE_IDLE;
            afatfs_fopen("test.txt", "a", logFileOpenedForAppend);
        break;
        case TEST_STAGE_APPEND:
            if (writeLogTestEntries(testFile, &logEntryIndex, LOG_ENTRY_COUNT + 10)) {
                uint32_t newLogFileSize;

                testAssert(afatfs_ftell(testFile, &newLogFileSize), "ftell() expected to work when no file operation queued");
                testAssert(newLogFileSize == logFileSize + 10 * TEST_LOG_ENTRY_SIZE, "Cursor didn't move properly after append");

                testStage = TEST_STAGE_APPEND_CLOSE;
            }
        break;
        case TEST_STAGE_APPEND_CLOSE:
            if (afatfs_fclose(testFile, NULL)) {
                testStage = TEST_STAGE_READ_WRITE_OPEN;
            }
        break;
        case TEST_STAGE_READ_WRITE_OPEN:
            testStage = TEST_STAGE_IDLE;
            logEntryIndex = 0;
            afatfs_fopen("test.txt", "r+", logFileOpenedForReadWrite);
        break;
        case TEST_STAGE_READ_WRITE_VALIDATE:
            if (validateLogTestEntries(testFile, &logEntryIndex, LOG_ENTRY_COUNT + 10)) {
                testStage = TEST_STAGE_READ_WRITE_CLOSE;
            }
        break;
        case TEST_STAGE_READ_WRITE_CLOSE:
            if (afatfs_fclose(testFile, NULL)) {
                testStage = TEST_STAGE_WRITE_OPEN;
            }
        break;
        case TEST_STAGE_WRITE_OPEN:
            testStage = TEST_STAGE_IDLE;
            logEntryIndex = 0;
            afatfs_fopen("test.txt", "w", logFileOpenedForWrite);
        break;
        case TEST_STAGE_WRITE_CLOSE:
            if (afatfs_fclose(testFile, NULL)) {
                testStage = TEST_STAGE_COMPLETE;
            }
        break;
        case TEST_STAGE_IDLE:
            // Waiting for file operations...
        break;
        case TEST_STAGE_COMPLETE:
            fprintf(stderr, "[Success]  File modes a, as, r, r+ and w work\n");
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
