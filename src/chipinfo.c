/*
 * chipinfo — Milestone 2 (part 1): prove that the chip responds to register
 * accesses. Reads REG_SYS_CFG (chip version) and the MAC address.
 *
 * This is the native macOS equivalent of the Linux "read_chip_version_8812a":
 * if the chip answers plausibly here, the USB register layer is working.
 * Matches any adapter listed in rtl_ids.c (8812AU and 8821AU families).
 */
#include <stdio.h>
#include "rtl_usb.h"
#include "rtl_ids.h"

int main(void) {
    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }

    libusb_device_handle *h = NULL;
    uint16_t vid = 0, pid = 0; int claimed = 0;
    int rc = rtl_open_first_ex(ctx, &h, &vid, &pid, &claimed);
    if (rc == LIBUSB_ERROR_NO_DEVICE) {
        printf("No supported adapter (rtl_ids.c) found. Plug in the adapter.\n");
        libusb_exit(ctx); return 2;
    }
    if (rc != 0 || !h) {
        fprintf(stderr, "Open failed: %s\n", libusb_error_name(rc));
        libusb_exit(ctx); return 1;
    }

    const rtl_usb_id *id = rtl_usb_id_lookup(vid, pid);
    printf("Device opened: %04x:%04x (%s, %s)   Interface claim: %s\n",
           vid, pid, id ? rtl_chip_id_name(id->chip) : "?", id ? id->note : "?",
           claimed ? "OK" : "NOT claimed (reads will be attempted anyway)");

    /* --- Chip version from SYS_CFG --- */
    int r = 0;
    uint32_t sys = rtl_read32(h, REG_SYS_CFG, &r);
    if (r != 0) {
        printf("\nSYS_CFG read ERROR: %s\n", libusb_error_name(r));
        printf("Indicates that the interface claim/seizing is still missing\n");
        printf("(macOS class driver) or that the chip is not powered.\n");
    } else {
        printf("\nSYS_CFG (0x00F0) = 0x%08x\n", sys);
        printf("  Chip type    : %s\n", (sys & SYS_CFG_RTL_ID) ? "TEST chip" : "MP (normal)");
        printf("  Vendor       : %s\n", (sys & SYS_CFG_VENDOR_ID) ? "UMC" : "TSMC");
        printf("  Cut version  : %u (raw, +1 for 8812)\n",
               (sys & SYS_CFG_CHIP_VER_MASK) >> SYS_CFG_CHIP_VER_SHIFT);
        if (sys == 0xffffffff || sys == 0x00000000)
            printf("  ! Implausible value — probably no real register access yet.\n");
    }

    /* --- MAC address --- */
    uint8_t mac[6] = {0};
    r = rtl_reg_read(h, REG_MACID, mac, 6);
    if (r == 0) {
        printf("\nMAC (0x0610) = %02x:%02x:%02x:%02x:%02x:%02x\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        printf("\nMAC read ERROR: %s\n", libusb_error_name(r));
    }

    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    return (r == 0) ? 0 : 1;
}
