#include "rtl_usb.h"
#include "rtl_ids.h"
#include <string.h>

int rtl_open_first_ex(libusb_context *ctx, libusb_device_handle **out_handle,
                      uint16_t *out_vid, uint16_t *out_pid, int *claimed) {
    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    if (n < 0) return (int)n;

    int rc = LIBUSB_ERROR_NO_DEVICE;
    *out_handle = NULL;
    if (claimed) *claimed = 0;

    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) != 0) continue;
        if (!rtl_usb_id_lookup(d.idVendor, d.idProduct)) continue; /* only known adapters */

        libusb_device_handle *h = NULL;
        rc = libusb_open(list[i], &h);
        if (rc != 0) continue;

        /* If macOS holds a class driver, try to release it. */
        libusb_set_auto_detach_kernel_driver(h, 1);

        int cl = libusb_claim_interface(h, 0);
        if (claimed) *claimed = (cl == 0);

        if (out_vid) *out_vid = d.idVendor;
        if (out_pid) *out_pid = d.idProduct;
        *out_handle = h;
        rc = 0;
        break;
    }

    libusb_free_device_list(list, 1);
    return rc;
}

int rtl_open_first(libusb_context *ctx, libusb_device_handle **out_handle,
                   uint16_t *out_pid, int *claimed) {
    return rtl_open_first_ex(ctx, out_handle, NULL, out_pid, claimed);
}

int rtl_reg_read(libusb_device_handle *h, uint16_t addr, uint8_t *buf, uint16_t len) {
    int r = libusb_control_transfer(
        h, RTL_VENDOR_READ, RTL_VENDOR_REQ, addr, RTL_VENDOR_IDX,
        buf, len, RTL_CTRL_TIMEOUT);
    return (r == len) ? 0 : (r < 0 ? r : LIBUSB_ERROR_IO);
}

int rtl_reg_write(libusb_device_handle *h, uint16_t addr, const uint8_t *buf, uint16_t len) {
    /* libusb expects a non-const buffer for the OUT transfer. */
    uint8_t tmp[64];
    if (len > sizeof(tmp)) return LIBUSB_ERROR_INVALID_PARAM;
    memcpy(tmp, buf, len);
    int r = libusb_control_transfer(
        h, RTL_VENDOR_WRITE, RTL_VENDOR_REQ, addr, RTL_VENDOR_IDX,
        tmp, len, RTL_CTRL_TIMEOUT);
    return (r == len) ? 0 : (r < 0 ? r : LIBUSB_ERROR_IO);
}

int rtl_reg_write_block(libusb_device_handle *h, uint16_t addr, const uint8_t *buf, uint16_t len) {
    uint8_t tmp[256];
    if (len > sizeof(tmp)) return LIBUSB_ERROR_INVALID_PARAM;
    memcpy(tmp, buf, len);
    int r = libusb_control_transfer(
        h, RTL_VENDOR_WRITE, RTL_VENDOR_REQ, addr, RTL_VENDOR_IDX,
        tmp, len, RTL_CTRL_TIMEOUT);
    return (r == len) ? 0 : (r < 0 ? r : LIBUSB_ERROR_IO);
}

uint8_t rtl_read8(libusb_device_handle *h, uint16_t addr, int *rc) {
    uint8_t v = 0; int r = rtl_reg_read(h, addr, &v, 1);
    if (rc) *rc = r; return v;
}
uint16_t rtl_read16(libusb_device_handle *h, uint16_t addr, int *rc) {
    uint8_t b[2] = {0,0}; int r = rtl_reg_read(h, addr, b, 2);
    if (rc) *rc = r; return (uint16_t)(b[0] | (b[1] << 8));
}
uint32_t rtl_read32(libusb_device_handle *h, uint16_t addr, int *rc) {
    uint8_t b[4] = {0,0,0,0}; int r = rtl_reg_read(h, addr, b, 4);
    if (rc) *rc = r;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
int rtl_write8(libusb_device_handle *h, uint16_t addr, uint8_t val) {
    return rtl_reg_write(h, addr, &val, 1);
}
int rtl_write16(libusb_device_handle *h, uint16_t addr, uint16_t val) {
    uint8_t b[2] = { (uint8_t)val, (uint8_t)(val >> 8) };
    return rtl_reg_write(h, addr, b, 2);
}
int rtl_write32(libusb_device_handle *h, uint16_t addr, uint32_t val) {
    uint8_t b[4] = { (uint8_t)val, (uint8_t)(val >> 8),
                     (uint8_t)(val >> 16), (uint8_t)(val >> 24) };
    return rtl_reg_write(h, addr, b, 4);
}
