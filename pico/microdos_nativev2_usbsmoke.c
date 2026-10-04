/*
 * microDOS Native v2 USB-stdio smoke test.
 *
 * This deliberately contains NO decoder, NO native compiler, NO generated
 * code execution and NO 300 MHz clock change. It isolates the Pico target's
 * USB stdio path from Native v2 itself.
 */
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#include <stdio.h>

int main(void)
{
    unsigned tick = 0u;

    stdio_init_all();

    for (;;) {
        if (stdio_usb_connected()) {
            printf("[NV2-USB-SMOKE] alive tick=%u\n", tick++);
            stdio_flush();
        }
        sleep_ms(500);
    }
}
