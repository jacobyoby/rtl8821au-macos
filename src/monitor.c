/*
 * monitor — final goal M4: monitor-mode capture into a pcap file.
 *
 * Usage:    ./monitor [channel] [seconds] [output.pcap]
 * Default:  channel 6, 15 s, capture.pcap
 *
 * Flow: open device -> full bring-up (rtl_hal_full_init) ->
 * read 802.11 frames from 0x81 -> write as pcap with radiotap.
 * Result can be opened in Wireshark/tshark.
 */
#include <stdio.h>
#include <stdlib.h>
#include "rtl_usb.h"
#include "rtl_hal.h"
#include "rtl_rx.h"

int main(int argc, char **argv) {
    int channel   = (argc > 1) ? atoi(argv[1]) : 6;
    int seconds   = (argc > 2) ? atoi(argv[2]) : 15;
    const char *out = (argc > 3) ? argv[3] : "capture.pcap";
    int with_cal  = (argc > 4) ? atoi(argv[4]) : 0;  /* 0 = RX-only (safe) */

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }

    libusb_device_handle *h = NULL;
    uint16_t vid = 0, pid = 0; int claimed = 0;
    int rc = rtl_open_first_ex(ctx, &h, &vid, &pid, &claimed);
    if (rc == LIBUSB_ERROR_NO_DEVICE) { printf("No supported adapter found. Plug in the adapter.\n"); libusb_exit(ctx); return 2; }
    if (rc != 0 || !h) { fprintf(stderr, "Open failed: %s\n", libusb_error_name(rc)); libusb_exit(ctx); return 1; }
    printf("Device %04x:%04x, Claim: %s\n\n", vid, pid, claimed ? "OK" : "NOT");

    rtl_rx_bind(ctx, rtl_chip_probe(vid, pid));   /* enable the asynchronous RX path */

    rc = rtl_hal_full_init(h, channel, with_cal, 1);
    if (rc != 0) { printf("\nBring-up failed (rc=%d). Aborting.\n", rc); goto done; }

    FILE *f = fopen(out, "wb");
    if (!f) { perror("fopen"); rc = 1; goto done; }
    rtl_rx_write_pcap_header(f);

    printf("\nMonitor on channel %d, %d s -> %s\n", channel, seconds, out);
    long n = rtl_rx_capture(h, f, seconds, 1);
    fclose(f);
    printf("Done: %ld frames written to %s\n", n, out);
    printf("Open with:  wireshark %s   or   tshark -r %s\n", out, out);

done:
    rtl_rx_unbind();
    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return rc;
}
