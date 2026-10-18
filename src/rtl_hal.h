/*
 * rtl_hal — orchestrator: complete bring-up via rtl_chip_ops.
 *
 * The order (power-on -> firmware -> MAC -> BB -> RF -> channel ->
 * calibration -> monitor RCR) is chip-independent; WHAT each step does is
 * supplied by the ops table from rtl_chip.h.
 */
#ifndef RTL_HAL_H
#define RTL_HAL_H

#include <stdint.h>
#include <libusb.h>
#include "rtl_chip.h"

typedef struct {
    libusb_device_handle *usb;
    const rtl_chip_ops   *ops;
    uint16_t              vid;
    uint16_t              pid;
    int                   channel;   /* last channel set successfully */
} rtl_dev;

/* Binds the ops table by USB ID. dev is usable afterwards.
 * Unknown ID -> rtl_chip_default() (same behavior as before). */
void rtl_dev_bind(rtl_dev *dev, libusb_device_handle *h, uint16_t vid, uint16_t pid);

/* Like rtl_dev_bind, but reads VID/PID from the USB descriptor itself. */
void rtl_dev_bind_auto(rtl_dev *dev, libusb_device_handle *h);

/* Bring-up via the ops table. with_cal=1 adds LCK+IQK.
 * Return: 0 ok, otherwise the rc of the failed step. */
int rtl_hal_init_dev(rtl_dev *dev, int channel, int with_cal, int verbose);

/* Channel change after bring-up (updates dev->channel and updates the TX
 * power for the new channel -- previously missing when scanning). */
int rtl_hal_set_channel(rtl_dev *dev, int channel, int verbose);

/* Compatibility wrapper: binds the ops itself. Existing tools need no
 * changes. */
int rtl_hal_full_init(libusb_device_handle *h, int channel, int with_cal, int verbose);

#endif /* RTL_HAL_H */
