/*
 * rtl_ids — USB vendor/product ID table -> chip family.
 *
 * Kept in its own tiny translation unit so that small tools (usbprobe,
 * chipinfo) can match devices without linking the whole chip core.
 * The chip-ops tables live in rtl_chip.c and are looked up by rtl_chip_id.
 */
#ifndef RTL_IDS_H
#define RTL_IDS_H

#include <stdint.h>

enum rtl_chip_id {
    RTL_CHIP_UNKNOWN = 0,
    RTL_CHIP_8812AU  = 1,   /* 2x2 802.11ac, USB 3.0 (Alfa AWUS036ACH)      */
    RTL_CHIP_8821AU  = 2,   /* 1x1 802.11ac, USB 2.0 (TP-Link Archer T2U+)  */
};

typedef struct {
    uint16_t    vid;
    uint16_t    pid;
    uint8_t     chip;   /* enum rtl_chip_id */
    const char *note;   /* vendor / product, for diagnostics only */
} rtl_usb_id;

/* Full table, terminated by an entry with vid == 0 && pid == 0. */
extern const rtl_usb_id rtl_usb_ids[];

/* NULL if the (vid, pid) pair is not a known adapter. */
const rtl_usb_id *rtl_usb_id_lookup(uint16_t vid, uint16_t pid);

/* Human-readable chip family name ("RTL8812AU", ...). */
const char *rtl_chip_id_name(uint8_t chip);

#endif /* RTL_IDS_H */
