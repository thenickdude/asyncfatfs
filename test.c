#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sdcard_sim.h"
#include "sdcard.h"
#include "sdcard_standard.h"
#include "asyncfatfs.h"

static int stage = 0;
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

    stage = 2;
}

int main(void)
{
    char testBuffer[32];

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

    while (stage < 3) {
        afatfs_poll();

        // Report a change in FS state if needed
        if (state != AFATFS_FILESYSTEM_STATE_READY) {
            state = afatfs_getFilesystemState();

            printFSState(state);
        }

        if (state == AFATFS_FILESYSTEM_STATE_READY) {
            switch (stage) {
                case 0:
                    stage = 1;
                    afatfs_fopen("test.txt", "sb", testFileCreated);
                break;
                case 1:
                    // Waiting for file to open...
                break;
                case 2:
                    if (testWriteCount >= testWriteMax) {
                        // We're okay to close without waiting for the write to complete
                        afatfs_fclose(testFile);

                        stage = 3;
                    } else {
                        sprintf(testBuffer, "This is print %d/%d\n", testWriteCount + 1, testWriteMax);

                        if (afatfs_fwrite(testFile, (uint8_t*) testBuffer, strlen(testBuffer)) > 0) {
                            testWriteCount++;
                        }
                    }
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
