/*
 * wpatest — run the rtl_wpa_connect() handshake with a password taken from
 * the WPA_TEST_PSK environment variable (TEST USE ONLY: this is for checking
 * the frame/crypto pipeline against an offline reference with a throwaway
 * passphrase; never put a real password in an environment variable).
 *
 * Usage: WPA_TEST_PSK=dummy ./wpatest <channel> <bssid> <ssid>
 * No root needed (no utun). Leaves the AP cleanly afterwards.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rtl_usb.h"
#include "rtl_hal.h"
#include "rtl_rx.h"
#include "rtl_wpa.h"

static int parse_mac(const char *s, uint8_t *m) {
    return sscanf(s, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &m[0],&m[1],&m[2],&m[3],&m[4],&m[5]) == 6;
}

int main(int argc, char **argv) {
    if (argc < 4) { printf("Usage: WPA_TEST_PSK=... %s <channel> <bssid> <ssid>\n", argv[0]); return 1; }
    const char *psk = getenv("WPA_TEST_PSK");
    if (!psk || !*psk) { printf("WPA_TEST_PSK not set\n"); return 1; }
    int channel = atoi(argv[1]); uint8_t bssid[6];
    if (!parse_mac(argv[2], bssid)) { printf("bad bssid\n"); return 1; }

    libusb_context *ctx = NULL; if (libusb_init(&ctx) != 0) return 1;
    libusb_device_handle *h = NULL; uint16_t vid = 0, pid = 0; int claimed = 0;
    if (rtl_open_first_ex(ctx, &h, &vid, &pid, &claimed) || !h) { printf("no device\n"); return 2; }
    rtl_rx_bind(ctx, rtl_chip_probe(vid, pid));

    wpa_keys_t k; memset(&k, 0, sizeof(k));
    int rc = rtl_wpa_connect(h, channel, bssid, argv[3], psk, &k, 1);
    printf("rtl_wpa_connect -> %d\n", rc);
    rtl_wpa_leave(h);
    rtl_rx_unbind();
    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h); libusb_exit(ctx);
    return rc == 0 ? 0 : 3;
}
