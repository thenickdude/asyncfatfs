#include "common.h"
#include "sdcard.h"

#include <stdlib.h>
#include <stdio.h>

/**
 * Write test log entries to the given file, starting from *entryIndex. Increments *entryIndex to keep track of the
 * progress so far until it reaches targetEntries.
 *
 * Keep calling until it returns true.
 */
bool writeLogTestEntries(afatfsFilePtr_t file, uint32_t *entryIndex, uint32_t targetEntries) {
    uint8_t testBuffer[TEST_LOG_ENTRY_SIZE];

    while (*entryIndex < targetEntries) {
        for (int i = 0; i < TEST_LOG_ENTRY_SIZE; i++) {
            testBuffer[i] = *entryIndex;
        }

        uint32_t writtenBytes = afatfs_fwrite(file, (uint8_t*) testBuffer, TEST_LOG_ENTRY_SIZE);

        if (writtenBytes == 0) {
            testAssert(!afatfs_isFull(), "Device filled up unexpectedly");
            return false;
        } else {
            testAssert(writtenBytes == TEST_LOG_ENTRY_SIZE, "Power-of-two sized fwrites not expected to be truncated during writing");
            (*entryIndex)++;
        }
    }

    return true;
}

/**
 * Validate log entries written to the file by writeLogTestEntries.
 *
 * Keep calling until it returns true.
 */
bool validateLogTestEntries(afatfsFilePtr_t file, uint32_t *entryIndex, uint32_t targetEntries) {
    uint8_t testBuffer[TEST_LOG_ENTRY_SIZE];

    while (*entryIndex < targetEntries) {
        uint32_t readBytes = afatfs_fread(file, (uint8_t*) testBuffer, TEST_LOG_ENTRY_SIZE);

        if (readBytes == 0) {
            return false;
        } else {
            testAssert(readBytes == TEST_LOG_ENTRY_SIZE, "Power-of-two sized freads not expected to be truncated during reading");

            for (int i = 0; i < TEST_LOG_ENTRY_SIZE; i++) {
                testAssert(testBuffer[i] == (uint8_t)(*entryIndex), "Log file content validation failed");
            }

            (*entryIndex)++;
        }
    }

    return true;
}

void testAssert(bool condition, const char *errorMessage)
{
    if (!condition) {
        fprintf(stderr, "%s\n", errorMessage);
        exit(-1);
    }
}

void testPoll()
{
#ifdef AFATFS_ASYNC_IO
    // Let's reuse the sdcard_poll() function for asynchronous I/O mode, even if it's not strictly necessary.
    // It could as well be implemented as a separate worker thread.
    sdcard_poll();
#else
    afatfs_poll();
#endif
}
