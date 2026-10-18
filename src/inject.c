/*
 * inject — injection test (M5): sends broadcast probe requests and records
 * the responses. Probe responses prove that TX actually gets on the air.
 *
 * Usage: ./inject [channel] [count] [calibration 0/1] [output.pcap]
 * Default: channel 6, 30 probe requests, without calibration, inject.pcap
 *
 * SAFETY: moderate default TX power (index 0x12), management frames.
 * Only use on networks you are authorized for.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "rtl_usb.h"
#include "rtl_hal.h"
#include "rtl_rx.h"
#include "rtl_tx.h"

int main(int argc, char **argv) {
    int channel  = (argc > 1) ? atoi(argv[1]) : 6;
    int count    = (argc > 2) ? atoi(argv[2]) : 30;
    int with_cal = (argc > 3) ? atoi(argv[3]) : 0;
    const char *out = (argc > 4) ? argv[4] : "inject.pcap";

    /* Broadcast probe request, SA = our efuse MAC (Alfa OUI). */
    uint8_t sa[6] = { 0x00, 0xc0, 0xca, 0xbc, 0x4e, 0xfa };
    uint8_t probe[] = {
        0x40, 0x00,                         /* FC: Mgmt, Probe Request */
        0x00, 0x00,                         /* Duration */
        0xff,0xff,0xff,0xff,0xff,0xff,      /* DA: Broadcast */
        sa[0],sa[1],sa[2],sa[3],sa[4],sa[5],/* SA */
        0xff,0xff,0xff,0xff,0xff,0xff,      /* BSSID: Broadcast */
        0x00, 0x00,                         /* Seq (HW sets) */
        0x00, 0x00,                         /* SSID-Element, len 0 (Wildcard) */
        0x01, 0x08, 0x02,0x04,0x0b,0x16,0x0c,0x12,0x18,0x24, /* Supported Rates */
        0x32, 0x04, 0x30,0x48,0x60,0x6c     /* Extended Rates */
    };

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
    libusb_device_handle *h = NULL;
    uint16_t vid = 0, pid = 0; int claimed = 0;
    int rc = rtl_open_first_ex(ctx, &h, &vid, &pid, &claimed);
    if (rc == LIBUSB_ERROR_NO_DEVICE) { printf("No supported adapter found.\n"); libusb_exit(ctx); return 2; }
    if (rc != 0 || !h) { fprintf(stderr, "Open: %s\n", libusb_error_name(rc)); libusb_exit(ctx); return 1; }
    printf("Device %04x:%04x, Claim: %s\n\n", vid, pid, claimed ? "OK" : "NO");

    rtl_rx_bind(ctx, rtl_chip_probe(vid, pid));   /* enable the asynchronous RX path */

    rc = rtl_hal_full_init(h, channel, with_cal, 1);
    if (rc != 0) { printf("Init failed (rc=%d).\n", rc); goto done; }

    FILE *f = fopen(out, "wb");
    if (!f) { perror("fopen"); rc = 1; goto done; }
    rtl_rx_write_pcap_header(f);

    printf("\nInjecting %d broadcast probe requests on channel %d (cal=%d)%s\n",
           count, channel, with_cal, with_cal ? "" : " [RX-only power, no IQK]");

    long tx_ok = 0, tx_err = 0, resp = 0;
    for (int i = 0; i < count; i++) {
        int r = rtl_tx_inject(h, probe, (int)sizeof(probe), RTL_RATE_6M, RTL_QSLT_MGNT, rtl_rx_bound_ops()->tx_ep_mgmt);
        if (r == 0) tx_ok++; else { tx_err++; if (tx_err <= 3) printf("  TX error: %s\n", libusb_error_name(r)); }
        resp += rtl_rx_pump(h, f, 150);   /* listen 150ms after each probe */
    }
    fclose(f);
    printf("\nResult: TX ok=%ld  TX errors=%ld  frames received during test=%ld\n",
           tx_ok, tx_err, resp);
    printf("Count probe responses: tshark -r %s -Y 'wlan.fc.type_subtype==0x05' 2>/dev/null | wc -l\n", out);
    printf("                   or: tcpdump -r %s 2>/dev/null | grep -i 'Probe Response' | wc -l\n", out);

done:
    rtl_rx_unbind();
    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return rc;
}
