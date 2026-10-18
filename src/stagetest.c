/*
 * stagetest — staged, controlled bring-up for safe testing.
 *
 * Usage: ./stagetest <stage>
 *   1 = power-on
 *   2 = + firmware
 *   3 = + MAC init + LED on        (LED must come on here; still NO RF/TX)
 *   4 = + BB init
 *   5 = + RF init                  (RF path on, but no TX)
 *   6 = + LCK + IQK                (calibration; internal TX — highest stage)
 *   7 = + channel + monitor RCR
 *
 * Each stage prints its return code. Aborts on error.
 */
#include <stdio.h>
#include <stdlib.h>
#include "rtl_usb.h"
#include "rtl_chip.h"

#define RUN(cond, label, call) do { \
    if (stage >= (cond)) { \
        printf("[Stufe %d] %s ...\n", (cond), label); \
        int _rc = (call); \
        if (_rc != 0) { printf("   -> FEHLER rc=%d, Abbruch.\n", _rc); goto done; } \
        printf("   -> OK\n"); \
    } \
} while (0)

int main(int argc, char **argv) {
    int stage = (argc > 1) ? atoi(argv[1]) : 3;
    int channel = (argc > 2) ? atoi(argv[2]) : 6;
    int rc = 0;

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init fehlgeschlagen\n"); return 1; }
    libusb_device_handle *h = NULL;
    uint16_t vid = 0, pid = 0; int claimed = 0;
    rc = rtl_open_first_ex(ctx, &h, &vid, &pid, &claimed);
    if (rc == LIBUSB_ERROR_NO_DEVICE) { printf("Kein bekannter Adapter gefunden.\n"); libusb_exit(ctx); return 2; }
    if (rc != 0 || !h) { fprintf(stderr, "Oeffnen: %s\n", libusb_error_name(rc)); libusb_exit(ctx); return 1; }
    const rtl_chip_ops *o = rtl_chip_probe(vid, pid);
    printf("Geraet %04x:%04x (%s), Claim: %s, Ziel-Stufe %d\n\n", vid, pid, o->name, claimed ? "OK" : "NEIN", stage);

    RUN(1, "Power-On",          o->power_on(h, 1));
    RUN(2, "Firmware",          o->fw_download(h, 1));
    RUN(3, "MAC-Init",          o->mac_init(h, 1));
    if (stage >= 3 && o->led_on) { o->led_on(h); printf("[Stufe 3] LED an -> schau auf den Adapter\n"); }
    RUN(4, "BB-Init",           o->bb_init(h, 1));
    RUN(5, "RF-Init",           o->rf_init(h, 1));
    RUN(6, "LC-Kalibrierung",   o->lck(h, 1));
    RUN(6, "IQ-Kalibrierung",   o->iqk(h, 1));
    RUN(7, "Kanal setzen",      o->set_channel(h, channel, 0));
    RUN(7, "Monitor-RCR",       o->set_monitor(h, 1));

    printf("\nStufe %d erreicht, alles OK.\n", stage);
done:
    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return rc;
}
