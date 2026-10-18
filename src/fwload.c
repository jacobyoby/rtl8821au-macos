/*
 * fwload — Milestone 2c: load firmware into the chip.
 *
 * Success criterion: after the download the chip reports WINTINI_RDY (firmware
 * running). This is the native macOS equivalent of FirmwareDownload8812 /
 * rtw_download_firmware. Chip-aware via the ops table (8812AU or 8821AU).
 */
#include <stdio.h>
#include "rtl_usb.h"
#include "rtl_chip.h"

int main(void) {
    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }

    libusb_device_handle *h = NULL;
    uint16_t vid = 0, pid = 0; int claimed = 0;
    int rc = rtl_open_first_ex(ctx, &h, &vid, &pid, &claimed);
    if (rc == LIBUSB_ERROR_NO_DEVICE) { printf("No supported adapter found. Plug in the adapter.\n"); libusb_exit(ctx); return 2; }
    if (rc != 0 || !h) { fprintf(stderr, "Open failed: %s\n", libusb_error_name(rc)); libusb_exit(ctx); return 1; }

    const rtl_chip_ops *o = rtl_chip_probe(vid, pid);
    printf("Device %04x:%04x (%s), Claim: %s\n", vid, pid, o->name, claimed ? "OK" : "NOT");

    int pr = o->power_on(h, 0);
    printf("Power-On: %s\n", pr == 0 ? "OK" : "failed");
    if (pr != 0) { printf("Aborting: no firmware download without power-on.\n"); goto done; }

    printf("\nFirmware download:\n");
    rc = o->fw_download(h, 1);
    if (rc == 0)      printf("\n==> FIRMWARE RUNNING. Chip fully initialized (except PHY/RF).\n");
    else if (rc == 1) printf("\n==> Checksum failed (transfer).\n");
    else if (rc == 2) printf("\n==> Firmware not ready (WINTINI_RDY).\n");
    else              printf("\n==> USB error during download: %s\n", libusb_error_name(rc));

done:
    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return (pr == 0 && rc == 0) ? 0 : 1;
}
