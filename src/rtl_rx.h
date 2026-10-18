
#ifndef RTL_RX_H
#define RTL_RX_H

#include <stdio.h>
#include <libusb.h>
#include "rtl_chip.h"

#define RTL_RX_EP        0x81
#define RTL_RXDESC_SIZE  24

/* Number of URBs queued at the host controller simultaneously. */
#define RTL_RX_URBS      8

/*
 * MUST be called once after libusb_init()/rtl_open_first() to enable the
 * asynchronous RX path. ops may be NULL (-> rtl_chip_default).
 * Without this call rtl_rx transparently falls back to the old synchronous
 * path -- nothing breaks, it is just slower.
 */
void rtl_rx_bind(libusb_context *ctx, const rtl_chip_ops *ops);

/* The bound ops table (or rtl_chip_default()). Tools take e.g. tx_ep_mgmt
 * from it instead of the 8812AU macro RTL_TX_EP_MGMT. */
const rtl_chip_ops *rtl_rx_bound_ops(void);

/* MUST be called before libusb_release_interface()/libusb_close():
 * stops the persistent engine and waits until all URBs are back.
 * Without it, transfers are still running on the handle when it is closed. */
void rtl_rx_unbind(void);

void rtl_rx_write_pcap_header(FILE *f);

long rtl_rx_capture(libusb_device_handle *h, FILE *f, int seconds, int verbose);
long rtl_rx_pump   (libusb_device_handle *h, FILE *f, int ms);
long rtl_rx_stream (libusb_device_handle *h, FILE *f);

typedef void (*rtl_frame_cb)(const uint8_t *frame, uint32_t len, void *ctx);
long rtl_rx_poll(libusb_device_handle *h, int ms, rtl_frame_cb cb, void *ctx);

#endif /* RTL_RX_H */