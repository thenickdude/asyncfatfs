#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "sdcard_sim.h"
#include "sdcard.h"
#include "sdcard_standard.h"
#include "asyncfatfs.h"

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

    while (1) {
        afatfs_poll();

        if (afatfs_getFilesystemState() == AFATFS_FILESYSTEM_STATE_READY) {
            printf("Filesystem online!\n");

            break;
        }
    }

    printf("Flushing...\n");

    while (!afatfs_flush()) {
        afatfs_poll();
    }

    for (int i = 0; i < 20; i++) {
        afatfs_flush();
        afatfs_poll();
    }

    printf("Flushed, shutting down...\n");

    afatfs_destroy();

    sdcard_sim_destroy();

    return EXIT_SUCCESS;
}
