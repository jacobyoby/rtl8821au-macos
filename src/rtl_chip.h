/*
 * rtl_chip — chip abstraction (ops table).
 *
 * Bundles all chip-specific entry points behind one table, so that
 * rtl_hal / rtl_rx / rtl_tx no longer contain 8812AU-specific code.
 * Each chip is an instance of rtl_chip_ops (rtl_chip.c for the 8812AU,
 * rtl8821au.c for the 8821AU). The USB ID -> chip mapping lives in
 * rtl_ids.c, so small tools can link it without the whole core.
 */
#ifndef RTL_CHIP_H
#define RTL_CHIP_H

#include <stdint.h>
#include <libusb.h>
#include "rtl_ids.h"

/* Result of parsing ONE aggregated USB RX subframe. */
typedef struct {
    uint32_t hdr_len;    /* bytes before the 802.11 frame (desc + drvinfo + shift) */
    uint32_t pkt_len;    /* length of the 802.11 frame                          */
    uint32_t advance;    /* bytes to the next subframe (already 8-aligned)      */
    int      is_report;  /* !=0 -> C2H/report, not an 802.11 frame              */

    /* Per-frame RX metadata. desc/phy point into the USB buffer and are only
     * valid for the duration of the frame callback. */
    const uint8_t *desc;     /* RX descriptor (rxdesc_size bytes)                */
    const uint8_t *phy;      /* PHY status (drvinfo), NULL if not present         */
    uint32_t phy_len;        /* bytes at phy                                      */
    int      crc_err;        /* !=0 -> FCS check failed, frame is corrupt         */
    uint8_t  rate;           /* DESC_RATE index: 0-3 CCK, 4-11 OFDM, 12+ HT/VHT   */
    int      rssi_dbm;       /* path A signal level, RTL_RSSI_NONE if unknown     */
} rtl_rx_sub;

#define RTL_RSSI_NONE (-128)

typedef struct rtl_chip_ops {
    const char     *name;
    uint8_t         chip;        /* enum rtl_chip_id (rtl_ids.h) */

    /* --- Bring-up (order is set by rtl_hal, not by the chip) --- */
    int  (*power_on)      (libusb_device_handle *h, int verbose);
    int  (*fw_download)   (libusb_device_handle *h, int verbose);
    int  (*fw_active_mode)(libusb_device_handle *h, int verbose);
    int  (*mac_init)      (libusb_device_handle *h, int verbose);
    int  (*bb_init)       (libusb_device_handle *h, int verbose);
    int  (*rf_init)       (libusb_device_handle *h, int verbose);
    int  (*set_channel)   (libusb_device_handle *h, int ch, int bw);
    int  (*set_monitor)   (libusb_device_handle *h, int verbose);
    int  (*lck)           (libusb_device_handle *h, int verbose);
    int  (*iqk)           (libusb_device_handle *h, int verbose);
    int  (*txpwr_apply)   (libusb_device_handle *h, int ch, int verbose);
    void (*txpwr_readback)(libusb_device_handle *h, const char *label);
    void (*led_on)        (libusb_device_handle *h);
    void (*led_off)       (libusb_device_handle *h);
    void (*dbg_dump)      (libusb_device_handle *h);   /* optional, may be NULL */

    /* --- Data path geometry --- */
    uint8_t  rx_ep;
    uint8_t  tx_ep_mgmt;
    uint16_t rxdesc_size;
    uint16_t txdesc_size;

    /* Parses the descriptor at 'p'. 0 = ok, <0 = unusable/abort. */
    int (*rx_parse_desc)(const uint8_t *p, int avail, rtl_rx_sub *out);

    int (*tx_inject)(libusb_device_handle *h, const uint8_t *frame, int len,
                     uint8_t rate, uint8_t qsel, uint8_t ep);
} rtl_chip_ops;

/* (VID, PID) -> ops. Unknown ID returns rtl_chip_default() (same behavior as before). */
const rtl_chip_ops *rtl_chip_probe(uint16_t vid, uint16_t pid);
/* enum rtl_chip_id -> ops, NULL if unknown. */
const rtl_chip_ops *rtl_chip_for(uint8_t chip);
const rtl_chip_ops *rtl_chip_default(void);

/* Common 88xxA RX descriptor (8812A and 8821A share the format). */
int rtl88xxa_rx_parse_desc(const uint8_t *p, int avail, rtl_rx_sub *out);

extern const rtl_chip_ops rtl8812au_ops;
extern const rtl_chip_ops rtl8821au_ops;

#endif /* RTL_CHIP_H */
