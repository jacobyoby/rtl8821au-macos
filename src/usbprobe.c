/*
 * usbprobe — Milestone 1 for native macOS access.
 *
 * Lists every USB device that is either a known adapter (rtl_ids.c: Realtek,
 * TP-Link, D-Link, Edimax, ... by VID:PID) or carries the Realtek vendor ID,
 * opens it, dumps descriptor + endpoints (so we know the bulk-IN/OUT and
 * control endpoints for RX/TX/register access) and tries to claim the
 * interface.
 *
 * Pure userspace, no kernel/kext/DriverKit dependency.
 *
 * Build: make        (see Makefile)
 * Use:   plug in the adapter, then ./usbprobe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libusb.h>
#include "rtl_ids.h"

#define REALTEK_VID 0x0bda

static const char *xfer_type(uint8_t attr) {
    switch (attr & LIBUSB_TRANSFER_TYPE_MASK) {
        case LIBUSB_TRANSFER_TYPE_CONTROL:     return "control";
        case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS: return "isochronous";
        case LIBUSB_TRANSFER_TYPE_BULK:        return "bulk";
        case LIBUSB_TRANSFER_TYPE_INTERRUPT:   return "interrupt";
        default:                               return "unknown";
    }
}

static const char *speed_name(int s) {
    switch (s) {
        case LIBUSB_SPEED_LOW:   return "low (1.5 Mbit/s)";
        case LIBUSB_SPEED_FULL:  return "full (12 Mbit/s)";
        case LIBUSB_SPEED_HIGH:  return "high (480 Mbit/s, USB 2.0)";
        case LIBUSB_SPEED_SUPER: return "super (5 Gbit/s, USB 3.0)";
        default:                 return "unknown";
    }
}

static void print_string(libusb_device_handle *h, uint8_t idx, const char *label) {
    if (!idx) return;
    unsigned char buf[256];
    int n = libusb_get_string_descriptor_ascii(h, idx, buf, sizeof(buf));
    if (n > 0) printf("  %-14s %s\n", label, buf);
}

static void dump_config(libusb_device *dev) {
    struct libusb_config_descriptor *cfg;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0) {
        printf("  (no active configuration readable)\n");
        return;
    }
    printf("  Interfaces:    %d\n", cfg->bNumInterfaces);
    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *itf = &cfg->interface[i];
        for (int a = 0; a < itf->num_altsetting; a++) {
            const struct libusb_interface_descriptor *id = &itf->altsetting[a];
            printf("  [if %d alt %d] class=0x%02x sub=0x%02x proto=0x%02x eps=%d\n",
                   id->bInterfaceNumber, id->bAlternateSetting,
                   id->bInterfaceClass, id->bInterfaceSubClass,
                   id->bInterfaceProtocol, id->bNumEndpoints);
            for (int e = 0; e < id->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
                int in = (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) != 0;
                printf("      ep 0x%02x  %-4s  %-6s  maxpkt=%d\n",
                       ep->bEndpointAddress, in ? "IN" : "OUT",
                       xfer_type(ep->bmAttributes),
                       ep->wMaxPacketSize);
            }
        }
    }
    libusb_free_config_descriptor(cfg);
}

int main(void) {
    libusb_context *ctx = NULL;
    int rc = libusb_init(&ctx);
    if (rc != 0) {
        fprintf(stderr, "libusb_init: %s\n", libusb_error_name(rc));
        return 1;
    }

    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    if (n < 0) {
        fprintf(stderr, "get_device_list: %s\n", libusb_error_name((int)n));
        libusb_exit(ctx);
        return 1;
    }

    int found = 0;
    for (ssize_t i = 0; i < n; i++) {
        libusb_device *dev = list[i];
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(dev, &d) != 0) continue;

        const rtl_usb_id *id = rtl_usb_id_lookup(d.idVendor, d.idProduct);
        if (!id && d.idVendor != REALTEK_VID) continue;

        found++;
        printf("\n=== Device %04x:%04x  (bus %d, addr %d)  ",
               d.idVendor, d.idProduct,
               libusb_get_bus_number(dev), libusb_get_device_address(dev));
        if (id) printf("[%s: %s] ===\n", rtl_chip_id_name(id->chip), id->note);
        else    printf("[Realtek VID, PID not in rtl_ids.c] ===\n");
        printf("  Speed:         %s\n", speed_name(libusb_get_device_speed(dev)));

        libusb_device_handle *h = NULL;
        rc = libusb_open(dev, &h);
        if (rc != 0) {
            printf("  open: %s (macOS may be holding the interface — permissions/claim)\n",
                   libusb_error_name(rc));
            dump_config(dev);
            continue;
        }

        print_string(h, d.iManufacturer, "Manufacturer:");
        print_string(h, d.iProduct,      "Product:");
        print_string(h, d.iSerialNumber, "Serial no.:");
        dump_config(dev);

        /* Claim attempt on interface 0 — reserves the device for userspace. */
        rc = libusb_claim_interface(h, 0);
        if (rc == 0) {
            printf("  claim if0:     OK — userspace owns the device.\n");
            libusb_release_interface(h, 0);
        } else {
            printf("  claim if0:     ERROR: %s\n", libusb_error_name(rc));
            printf("                 (macOS may be holding the interface via a\n");
            printf("                  composite class driver; next step:\n");
            printf("                  try IOKit detach / kernel auto-detach.)\n");
        }
        libusb_close(h);
    }

    if (!found) {
        printf("No supported adapter (rtl_ids.c) and no Realtek device (VID 0x0bda) found on USB.\n");
        printf("Plug in the adapter and run again. Cross-check:\n");
        printf("  ioreg -p IOUSB -w0 | grep -i wlan\n");
    }

    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return found ? 0 : 2;
}
