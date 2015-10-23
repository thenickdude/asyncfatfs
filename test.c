#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "sdcard_sim.h"
#include "sdcard.h"
#include "sdcard_standard.h"

int main(void)
{
    sdcard_sim_init("simcard");

    if (!sdcard_init()) {
        fprintf(stderr, "sdcard_init() failed\n");
        return EXIT_FAILURE;
    }

    afatfs_init();

    while (1) {
        afatfs_poll();


    }

    finish:
    afatfs_destroy();

    sdcard_sim_destroy();

    return EXIT_SUCCESS;
}
