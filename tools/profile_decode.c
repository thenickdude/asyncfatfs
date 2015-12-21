/**
 * Decode a afatfs.log profiling log created by its introspective profiling feature to CSV.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../lib/sdcard.h"

void printLogEntry(uint8_t *buffer)
{
    sdcardBlockOperation_e operation = (sdcardBlockOperation_e) buffer[0];
    const char *operationText;

    uint32_t blockIndex = buffer[4] | (buffer[5] << 8) | (buffer[6] << 16) | (buffer[7] << 24);
    uint32_t duration = buffer[8] | (buffer[9] << 8) | (buffer[10] << 16) | (buffer[11] << 24);

    switch (operation) {
        case SDCARD_BLOCK_OPERATION_ERASE:
            operationText = "erase";
        break;
        case SDCARD_BLOCK_OPERATION_READ:
            operationText = "read";
        break;
        case SDCARD_BLOCK_OPERATION_WRITE:
            operationText = "write";
        break;
        default:
            operationText = "unknown";
    }

    printf("%s,%u,%u\n", operationText, blockIndex, duration);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Missing filename argument\n");

        return EXIT_FAILURE;
    }

    FILE *logFile = fopen(argv[1], "rb");
    if (!logFile) {
        fprintf(stderr, "Failed to open log file '%s'\n", argv[1]);

        return EXIT_FAILURE;
    }

    printf("operation,block,duration\n");

    while (1) {
        enum {
            LOG_ENTRY_SIZE = 16
        };

        uint8_t buffer[LOG_ENTRY_SIZE];

        int bytesRead = fread(buffer, 1, LOG_ENTRY_SIZE, logFile);

        if (bytesRead < LOG_ENTRY_SIZE) {
            break;
        }

        printLogEntry(buffer);
    }

    return EXIT_SUCCESS;
}
