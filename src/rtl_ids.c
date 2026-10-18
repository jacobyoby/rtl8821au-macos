/*
 * rtl_ids — USB ID table.
 *
 * 8812AU entries: the list the original project matched on (Alfa AWUS036ACH
 * ships as 0bda:8812 or 0bda:881a).
 * 8821AU entries: copied from the Linux rtw88 driver's rtw_8821au_id_table
 * (drivers/net/wireless/realtek/rtw88/rtw8821au.c). Note that 0bda:0811,
 * 0bda:0820, 0bda:0823 and 0bda:a811 used to be listed as 8812AU in the old
 * rtl_usb.c known_pids[]; per rtw88 they are RTL8821AU parts.
 */
#include "rtl_ids.h"
#include <stddef.h>

#define REALTEK 0x0bda

const rtl_usb_id rtl_usb_ids[] = {
    /* ---- RTL8812AU ---- */
    { REALTEK, 0x8812, RTL_CHIP_8812AU, "Realtek RTL8812AU" },
    { REALTEK, 0x881a, RTL_CHIP_8812AU, "Realtek RTL8812AU (Alfa AWUS036ACH)" },
    { REALTEK, 0x881b, RTL_CHIP_8812AU, "Realtek RTL8812AU" },
    { REALTEK, 0x881c, RTL_CHIP_8812AU, "Realtek RTL8812AU" },
    { REALTEK, 0x8813, RTL_CHIP_8812AU, "Realtek (listed as 8812AU by the original project)" },

    /* ---- RTL8821AU / RTL8811AU (rtw88 rtw_8821au_id_table) ---- */
    { REALTEK, 0x0811, RTL_CHIP_8821AU, "Realtek RTL8821AU" },
    { REALTEK, 0x0820, RTL_CHIP_8821AU, "Realtek RTL8821AU" },
    { REALTEK, 0x0821, RTL_CHIP_8821AU, "Realtek RTL8821AU" },
    { REALTEK, 0x8822, RTL_CHIP_8821AU, "Realtek RTL8821AU" },
    { REALTEK, 0x0823, RTL_CHIP_8821AU, "Realtek RTL8821AU" },
    { REALTEK, 0xa811, RTL_CHIP_8821AU, "Realtek RTL8821AU" },
    { 0x0411,  0x0242, RTL_CHIP_8821AU, "Buffalo" },
    { 0x0411,  0x029b, RTL_CHIP_8821AU, "Buffalo" },
    { 0x04bb,  0x0953, RTL_CHIP_8821AU, "I-O DATA" },
    { 0x056e,  0x4007, RTL_CHIP_8821AU, "ELECOM" },
    { 0x056e,  0x400e, RTL_CHIP_8821AU, "ELECOM" },
    { 0x056e,  0x400f, RTL_CHIP_8821AU, "ELECOM" },
    { 0x0846,  0x9052, RTL_CHIP_8821AU, "Netgear A6100" },
    { 0x0e66,  0x0023, RTL_CHIP_8821AU, "Hawking" },
    { 0x2001,  0x3314, RTL_CHIP_8821AU, "D-Link DWA-171" },
    { 0x2001,  0x3318, RTL_CHIP_8821AU, "D-Link" },
    { 0x2019,  0xab32, RTL_CHIP_8821AU, "Planex" },
    { 0x20f4,  0x804b, RTL_CHIP_8821AU, "TRENDnet" },
    { 0x2357,  0x011e, RTL_CHIP_8821AU, "TP-Link Archer T2U Nano" },
    { 0x2357,  0x011f, RTL_CHIP_8821AU, "TP-Link Archer T2U v3" },
    { 0x2357,  0x0120, RTL_CHIP_8821AU, "TP-Link Archer T2U Plus" },
    { 0x3823,  0x6249, RTL_CHIP_8821AU, "Obihai" },
    { 0x7392,  0xa811, RTL_CHIP_8821AU, "Edimax" },
    { 0x7392,  0xa812, RTL_CHIP_8821AU, "Edimax" },
    { 0x7392,  0xa813, RTL_CHIP_8821AU, "Edimax" },
    { 0x7392,  0xb611, RTL_CHIP_8821AU, "Edimax" },

    { 0, 0, RTL_CHIP_UNKNOWN, NULL },
};

const rtl_usb_id *rtl_usb_id_lookup(uint16_t vid, uint16_t pid) {
    for (const rtl_usb_id *e = rtl_usb_ids; e->vid || e->pid; e++)
        if (e->vid == vid && e->pid == pid) return e;
    return NULL;
}

const char *rtl_chip_id_name(uint8_t chip) {
    switch (chip) {
        case RTL_CHIP_8812AU: return "RTL8812AU";
        case RTL_CHIP_8821AU: return "RTL8821AU";
        default:              return "unknown";
    }
}
