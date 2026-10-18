/*
 * rtl_hal — bring-up orchestrator on top of rtl_chip_ops.
 *
 * Deliberately contains NO 8812AU-specific code any more: register addresses,
 * tables and debug dumps live in the ops table (rtl_chip.c).
 */
#include "rtl_hal.h"
#include <stdio.h>

/* Mandatory step: missing function pointer = broken ops table. */
#define STEP(fn, call, name) do {                                              \
    if (!(fn)) { printf("[hal] %s: not implemented\n", name); return -1; }    \
    if (verbose) printf("[hal] %s ...\n", name);                               \
    int _rc = (call);                                                          \
    if (_rc != 0) {                                                            \
        printf("[hal] %s FAILED (rc=%d)\n", name, _rc);                        \
        return _rc;                                                            \
    }                                                                          \
} while (0)

/* Best-effort: may be missing and may fail. */
#define TRY(fn, call) do { if (fn) { (call); } } while (0)

void rtl_dev_bind(rtl_dev *dev, libusb_device_handle *h, uint16_t vid, uint16_t pid) {
    dev->usb     = h;
    dev->vid     = vid;
    dev->pid     = pid;
    dev->ops     = rtl_chip_probe(vid, pid);
    dev->channel = 0;
}

void rtl_dev_bind_auto(rtl_dev *dev, libusb_device_handle *h) {
    uint16_t vid = 0, pid = 0;
    struct libusb_device_descriptor d;
    libusb_device *dv = libusb_get_device(h);
    if (dv && libusb_get_device_descriptor(dv, &d) == 0) {
        vid = d.idVendor;
        pid = d.idProduct;
    }
    rtl_dev_bind(dev, h, vid, pid);
}

int rtl_hal_init_dev(rtl_dev *dev, int channel, int with_cal, int verbose) {
    if (!dev || !dev->ops) { printf("[hal] no ops bound\n"); return -1; }

    const rtl_chip_ops   *o = dev->ops;
    libusb_device_handle *h = dev->usb;

    if (verbose) printf("[hal] Chip: %s (%04x:%04x)\n", o->name, dev->vid, dev->pid);

    STEP(o->power_on,    o->power_on(h, verbose),       "Power-On");
    STEP(o->fw_download, o->fw_download(h, verbose),    "Firmware download");

    /* Explicitly keep the firmware in PS_MODE_ACTIVE: otherwise its autonomous
     * power save can, after a TX pause, silently make the AP buffer unicast --
     * without a deauth we would notice. May fail. */
    TRY(o->fw_active_mode, o->fw_active_mode(h, verbose));

    STEP(o->mac_init,    o->mac_init(h, verbose),       "MAC init");
    STEP(o->bb_init,     o->bb_init(h, verbose),        "BB init");
    STEP(o->rf_init,     o->rf_init(h, verbose),        "RF init");
    STEP(o->set_channel, o->set_channel(h, channel, 0), "Set channel");

    /* Efuse-calibrated TX power: without it the chip transmits with its raw
     * post-reset gain, far below rated power. An efuse read error must not
     * abort the bring-up. */
    TRY(o->txpwr_apply, o->txpwr_apply(h, channel, verbose));

    if (with_cal) {
        STEP(o->lck, o->lck(h, verbose), "LC calibration");
        STEP(o->iqk, o->iqk(h, verbose), "IQ calibration");
    }

    STEP(o->set_monitor, o->set_monitor(h, verbose), "Monitor RCR");

    /* Check whether calibration or monitor setup reset the TX AGC registers
     * in the meantime. */
    TRY(o->txpwr_readback, o->txpwr_readback(h, "end of rtl_hal_init_dev"));
    TRY(o->led_on, o->led_on(h));

    dev->channel = channel;

    if (verbose) {
        TRY(o->dbg_dump, o->dbg_dump(h));
        printf("[hal] Init complete, channel %d, monitor active%s.\n",
               channel, with_cal ? " (calibrated)" : " (RX-only, no calibration)");
    }
    return 0;
}

int rtl_hal_set_channel(rtl_dev *dev, int channel, int verbose) {
    if (!dev || !dev->ops || !dev->ops->set_channel) return -1;

    int rc = dev->ops->set_channel(dev->usb, channel, 0);
    if (rc != 0) return rc;

    /* TX power calibration is channel-dependent -- it has to follow along when
     * hopping, otherwise the init channel's value stays in effect. */
    TRY(dev->ops->txpwr_apply, dev->ops->txpwr_apply(dev->usb, channel, verbose));

    dev->channel = channel;
    return 0;
}

int rtl_hal_full_init(libusb_device_handle *h, int channel, int with_cal, int verbose) {
    rtl_dev dev;
    rtl_dev_bind_auto(&dev, h);
    return rtl_hal_init_dev(&dev, channel, with_cal, verbose);
}