/*
 * txtest — TX path diagnostics for a chip-ops port.
 *
 * Usage: ./txtest [channel] [ep_hex] [qsel_hex] [count] [cal 0/1]
 * Default: 6 (ops->tx_ep_mgmt) 0x12 10 1
 *
 * Brings the chip up, programs REG_MACID with the efuse MAC, sends 'count'
 * broadcast probe requests with that MAC as SA on the given endpoint/queue,
 * dumps TX queue status registers before/after, then listens 2 s and counts
 * FCS-clean probe responses addressed to us.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rtl_usb.h"
#include "rtl_hal.h"
#include "rtl_rx.h"
#include "rtl_tx.h"
#include "rtl_ids.h"
#include "rtl8821au.h"

static uint8_t g_mac[6];
static int g_resp = 0, g_resp_bad = 0, g_frames = 0;

static uint32_t crc32_tab(const uint8_t *p, int n) {
    uint32_t c = 0xffffffff;
    for (int i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c;
}
static void cb(const uint8_t *f, uint32_t len, void *ctx) {
    (void)ctx; g_frames++;
    if (len < 28) return;
    int fcs_ok = (len >= 8) && (crc32_tab(f, (int)len - 4) == ((uint32_t)f[len-4] | ((uint32_t)f[len-3] << 8) | ((uint32_t)f[len-2] << 16) | ((uint32_t)f[len-1] << 24)));
    if (((f[0] >> 2) & 3) == 0 && ((f[0] >> 4) & 0xF) == 5 && memcmp(f + 4, g_mac, 6) == 0) {
        if (fcs_ok) g_resp++; else g_resp_bad++;
    }
}

static void dump_tx_regs(libusb_device_handle *h, const char *label) {
    printf("  [%s] TXPKT_EMPTY(0x41a)=0x%04x TXDMA_STATUS(0x210)=0x%08x TXPAUSE(0x522)=0x%02x CR=0x%04x "
           "RQPN(0x200)=0x%08x FIFOPAGE_CTRL_2(0x204)=0x%08x TXDMA_PQ_MAP(0x10c)=0x%04x\n", label,
           rtl_read16(h, 0x041a, NULL), rtl_read32(h, 0x0210, NULL), rtl_read8(h, 0x0522, NULL),
           rtl_read16(h, 0x0100, NULL), rtl_read32(h, 0x0200, NULL), rtl_read32(h, 0x0204, NULL),
           rtl_read16(h, 0x010c, NULL));
}

int main(int argc, char **argv) {
    int channel = (argc > 1) ? atoi(argv[1]) : 6;
    int ep      = (argc > 2) ? (int)strtoul(argv[2], NULL, 16) : -1;
    int qsel    = (argc > 3) ? (int)strtoul(argv[3], NULL, 16) : RTL_QSLT_MGNT;
    int count   = (argc > 4) ? atoi(argv[4]) : 10;
    int cal     = (argc > 5) ? atoi(argv[5]) : 1;

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) return 1;
    libusb_device_handle *h = NULL; uint16_t vid = 0, pid = 0; int claimed = 0;
    if (rtl_open_first_ex(ctx, &h, &vid, &pid, &claimed) || !h) { printf("no device\n"); return 2; }
    const rtl_chip_ops *ops = rtl_chip_probe(vid, pid);
    rtl_rx_bind(ctx, ops);
    if (ep < 0) ep = ops->tx_ep_mgmt;
    printf("Device %04x:%04x %s, ep 0x%02x qsel 0x%02x count %d cal %d\n", vid, pid, ops->name, ep, qsel, count, cal);

    if (rtl_hal_full_init(h, channel, cal, 0) != 0) { printf("init failed\n"); goto done; }

    /* program our MAC (efuse MAC when known, else a locally administered one) */
    uint8_t mac[6] = {0x02, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f};
    if (ops->chip == RTL_CHIP_8821AU) memcpy(mac, rtl8821a_board_info(h, 0)->mac, 6);
    rtl_reg_write(h, 0x0610, mac, 6);
    memcpy(g_mac, mac, 6);
    printf("MACID = %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    uint8_t probe[64]; int p = 0;
    probe[p++] = 0x40; probe[p++] = 0x00;                 /* probe request */
    probe[p++] = 0x00; probe[p++] = 0x00;                 /* duration */
    memset(probe + p, 0xff, 6); p += 6;                   /* DA broadcast */
    memcpy(probe + p, mac, 6); p += 6;                    /* SA */
    memset(probe + p, 0xff, 6); p += 6;                   /* BSSID broadcast */
    probe[p++] = 0x00; probe[p++] = 0x00;                 /* seq (HWSEQ) */
    probe[p++] = 0x00; probe[p++] = 0x00;                 /* SSID wildcard */
    const uint8_t rates[] = {0x01, 0x08, 0x02, 0x04, 0x0b, 0x16, 0x0c, 0x12, 0x18, 0x24};
    memcpy(probe + p, rates, sizeof(rates)); p += sizeof(rates);
    const uint8_t xrates[] = {0x32, 0x04, 0x30, 0x48, 0x60, 0x6c};
    memcpy(probe + p, xrates, sizeof(xrates)); p += sizeof(xrates);

    dump_tx_regs(h, "before");
    int ok = 0, err = 0;
    for (int i = 0; i < count; i++) {
        int r = rtl_tx_inject(h, probe, p, RTL_RATE_6M, (uint8_t)qsel, (uint8_t)ep);
        if (r == 0) ok++; else { err++; printf("  tx %d: %s\n", i, libusb_error_name(r)); }
        rtl_rx_poll(h, 60, cb, NULL);
    }
    dump_tx_regs(h, "after ");
    rtl_rx_poll(h, 2000, cb, NULL);
    dump_tx_regs(h, "+2s   ");
    printf("TX ok=%d err=%d; RX frames=%d; probe responses to us: %d clean, %d with bad FCS\n",
           ok, err, g_frames, g_resp, g_resp_bad);
done:
    rtl_rx_unbind();
    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h); libusb_exit(ctx);
    return 0;
}
