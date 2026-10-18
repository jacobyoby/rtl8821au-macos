/*
 * ledtest — visible proof of hardware control: blink the status LED.
 *
 * Flow: open -> power-on -> firmware -> toggle LED0 on/off N times.
 * You should physically see the LED on the adapter blink.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "rtl_usb.h"

int main(int argc, char **argv) {
    int blinks = (argc > 1) ? atoi(argv[1]) : 10;

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }

    libusb_device_handle *h = NULL;
    uint16_t vid = 0, pid = 0; int claimed = 0;
    int rc = rtl_open_first_ex(ctx, &h, &vid, &pid, &claimed);
    if (rc == LIBUSB_ERROR_NO_DEVICE) { printf("No supported adapter found. Plug in the adapter.\n"); libusb_exit(ctx); return 2; }
    if (rc != 0 || !h) { fprintf(stderr, "Open failed: %s\n", libusb_error_name(rc)); libusb_exit(ctx); return 1; }
    printf("Device %04x:%04x, Claim: %s\n", vid, pid, claimed ? "OK" : "NOT");

    printf("Power-On: %s\n", rtl_power_on(h, 0) == 0 ? "OK" : "ERROR");
    printf("Firmware: %s\n", rtl_fw_download(h, 0) == 0 ? "OK (WINTINI_RDY)" : "ERROR");

    printf("\nLED blinks %dx — look at the adapter:\n", blinks);
    for (int i = 0; i < blinks; i++) {
        rtl_led_on(h);  printf("  [%2d] ON\n",  i + 1); fflush(stdout); usleep(300000);
        rtl_led_off(h); printf("  [%2d] OFF\n", i + 1); fflush(stdout); usleep(300000);
    }
    rtl_led_on(h); /* leave it on at the end */
    printf("\nDone. LED stays on.\n");

    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
