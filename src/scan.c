/*
 * scan — WLAN scanner (first milestone toward an internet client).
 *
 * Brings the chip up, hops across 2.4- and 5-GHz channels, collects beacons
 * and probe responses, and lists the networks found (SSID, BSSID, channel,
 * signal, encryption), strongest first. Receive only, no TX.
 *
 * Frames with a CRC error are dropped: on a busy band they are the source of
 * garbled SSIDs and impossible BSSIDs. Signal is the median of the samples
 * taken on the AP's own channel, Peak the strongest one.
 *
 * Usage: ./scan [ms_per_channel]   (default 500 ms)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rtl_usb.h"
#include "rtl_hal.h"
#include "rtl_rf.h"
#include "rtl_rx.h"

#define MAX_NETS     512
#define MAX_SAMPLES  64     /* ring buffer per BSSID for the median */

typedef struct {
    uint8_t bssid[6]; char ssid[33]; int channel; int privacy;
    int     peak_dbm;                    /* RTL_RSSI_NONE until a sample arrives */
    int     adj_peak_dbm;                /* strongest sample from a neighbouring channel */
    int8_t  samples[MAX_SAMPLES];
    int     nsamples;                    /* total seen, may exceed MAX_SAMPLES */
} net_t;
typedef struct { net_t nets[MAX_NETS]; int count; int cur_ch; long crc_dropped; } scan_ctx;

static void add_sample(net_t *n, int dbm) {
    if (dbm == RTL_RSSI_NONE) return;
    if (n->peak_dbm == RTL_RSSI_NONE || dbm > n->peak_dbm) n->peak_dbm = dbm;
    n->samples[n->nsamples % MAX_SAMPLES] = (int8_t)dbm;
    n->nsamples++;
}

static int cmp_i8(const void *a, const void *b) {
    return *(const int8_t *)a - *(const int8_t *)b;
}

static int median_dbm(const net_t *n) {
    int k = n->nsamples < MAX_SAMPLES ? n->nsamples : MAX_SAMPLES;
    if (k == 0) return RTL_RSSI_NONE;
    int8_t v[MAX_SAMPLES];
    memcpy(v, n->samples, (size_t)k);
    qsort(v, (size_t)k, 1, cmp_i8);
    return (k & 1) ? v[k / 2] : (v[k / 2 - 1] + v[k / 2]) / 2;
}

static void scan_cb(const uint8_t *f, uint32_t len, const rtl_rx_sub *rx, void *v) {
    scan_ctx *s = (scan_ctx *)v;
    if (len < 38) return;
    uint8_t fc = f[0];
    if (fc != 0x80 && fc != 0x50) return;          /* Beacon (0x80) / Probe-Resp (0x50) */
    if (rx->crc_err) { s->crc_dropped++; return; }
    const uint8_t *bssid = f + 16;                  /* addr3 */
    if (bssid[0] & 0x01) return;                    /* group bit: never a real BSSID */
    uint16_t caps = (uint16_t)(f[34] | (f[35] << 8)); /* Capability (24+8+2) */
    int privacy = (caps & 0x0010) ? 1 : 0;

    char ssid[33] = ""; int ds_ch = 0;
    const uint8_t *ie = f + 36; int rem = (int)len - 36;
    while (rem >= 2) {
        int id = ie[0], l = ie[1];
        if (2 + l > rem) break;
        if (id == 0 && !ssid[0]) {
            int n = l > 32 ? 32 : l;
            for (int i = 0; i < n; i++)             /* keep the terminal safe */
                ssid[i] = (ie[2 + i] >= 0x20 && ie[2 + i] < 0x7f) ? (char)ie[2 + i] : '?';
            ssid[n] = 0;
            /* hidden networks send an empty or all-zero SSID */
            int zero = 1;
            for (int i = 0; i < l; i++) if (ie[2 + i]) { zero = 0; break; }
            if (zero) ssid[0] = 0;
        } else if (id == 3 && l == 1) {
            ds_ch = ie[2];                          /* DS Parameter Set: AP's own channel */
        }
        ie += 2 + l; rem -= 2 + l;
    }

    net_t *n = NULL;
    for (int i = 0; i < s->count; i++)
        if (memcmp(s->nets[i].bssid, bssid, 6) == 0) { n = &s->nets[i]; break; }
    if (!n) {
        if (s->count >= MAX_NETS) return;
        n = &s->nets[s->count++];
        memcpy(n->bssid, bssid, 6);
        strncpy(n->ssid, ssid[0] ? ssid : "<hidden>", 32);
        n->channel = ds_ch ? ds_ch : s->cur_ch; n->privacy = privacy;
        n->peak_dbm = n->adj_peak_dbm = RTL_RSSI_NONE;
    }
    /* 2.4 GHz leaks into adjacent channels at a lower level: only count
     * samples taken on the AP's own channel, keep the rest as a fallback. */
    if (ds_ch == 0 || ds_ch == s->cur_ch) add_sample(n, rx->rssi_dbm);
    else if (rx->rssi_dbm != RTL_RSSI_NONE &&
             (n->adj_peak_dbm == RTL_RSSI_NONE || rx->rssi_dbm > n->adj_peak_dbm))
        n->adj_peak_dbm = rx->rssi_dbm;
}

/* Median of the on-channel samples; if the AP was only heard from a
 * neighbouring channel, that peak as a lower bound (*adj set). */
static int signal_dbm(const net_t *n, int *adj) {
    int m = median_dbm(n);
    if (adj) *adj = (m == RTL_RSSI_NONE && n->adj_peak_dbm != RTL_RSSI_NONE);
    return m != RTL_RSSI_NONE ? m : n->adj_peak_dbm;
}

static int cmp_signal(const void *a, const void *b) {
    const net_t *x = (const net_t *)a, *y = (const net_t *)b;
    int mx = signal_dbm(x, NULL), my = signal_dbm(y, NULL);
    if (mx != my) return my - mx;                   /* strongest first, unknown last */
    return y->peak_dbm - x->peak_dbm;
}

static void fmt_dbm(char *buf, size_t sz, int dbm, int adj) {
    if (dbm == RTL_RSSI_NONE) snprintf(buf, sz, "?");
    else                      snprintf(buf, sz, adj ? ">%d" : "%d", dbm);
}

static void usage(const char *argv0) {
    printf("Usage: %s [ms_per_channel]\n"
           "  Scan 2.4- and 5-GHz channels for beacons and probe responses and\n"
           "  list the networks, strongest first. Default dwell is 500 ms per\n"
           "  channel. Signal is the median dBm, Peak the strongest sample.\n",
           argv0);
}

int main(int argc, char **argv) {
    int dwell = 500;
    if (argc > 2) { usage(argv[0]); return 2; }
    if (argc == 2) {
        if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) { usage(argv[0]); return 0; }
        char *end = NULL;
        long ms = strtol(argv[1], &end, 10);
        if (!end || *end || ms <= 0 || ms > 60000) { usage(argv[0]); return 2; }
        dwell = (int)ms;
    }
    static const int chans[] = {1,2,3,4,5,6,7,8,9,10,11,12,13,
        36,40,44,48,52,56,60,64,100,104,108,112,116,132,136,140,149,153,157,161,165};

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) return 1;
    libusb_device_handle *h = NULL; uint16_t vid = 0, pid = 0; int claimed = 0;
    int rc = rtl_open_first_ex(ctx, &h, &vid, &pid, &claimed);
    if (rc || !h) { printf("No supported adapter found.\n"); libusb_exit(ctx); return 2; }
    printf("Device %04x:%04x. Bringing up ...\n", vid, pid);

    rtl_rx_bind(ctx, rtl_chip_probe(vid, pid));   /* enable the asynchronous RX path */
    if (rtl_hal_full_init(h, 1, 0, 0) != 0) { printf("Init failed.\n"); goto done; }

    static scan_ctx s; memset(&s, 0, sizeof(s));
    printf("Scanning %zu channels (%d ms/channel) ...\n", sizeof(chans)/sizeof(int), dwell);
    for (size_t i = 0; i < sizeof(chans)/sizeof(int); i++) {
        s.cur_ch = chans[i];
        /* rtl_rf_set_channel() directly instead of rtl_hal_set_channel(): scan
         * is pure RX operation (no TX), so the channel-dependent TX power
         * tracking of rtl_hal_set_channel() would have no effect here.
         * rtl_hal_set_channel() is meant for later TX-capable tools with
         * channel hopping. */
        if (rtl_rx_bound_ops()->set_channel(h, chans[i], 0) != 0) continue;   /* chip ops, not the 8812AU routine */
        rtl_rx_poll_ex(h, dwell, scan_cb, &s);
        printf("\r  Channel %3d  found: %d networks ", chans[i], s.count); fflush(stdout);
    }

    qsort(s.nets, (size_t)s.count, sizeof(net_t), cmp_signal);
    printf("\n\n%-32s %-17s %5s %6s %5s  %s\n",
           "SSID", "BSSID", "Chan", "Signal", "Peak", "Security");
    printf("--------------------------------------------------------------------------------\n");
    int any_adj = 0;
    for (int i = 0; i < s.count; i++) {
        net_t *n = &s.nets[i];
        char sig[8], peak[8]; int adj;
        int dbm = signal_dbm(n, &adj);
        fmt_dbm(sig, sizeof(sig), dbm, adj);
        fmt_dbm(peak, sizeof(peak), adj ? dbm : n->peak_dbm, adj);
        any_adj |= adj;
        printf("%-32s %02x:%02x:%02x:%02x:%02x:%02x %5d %6s %5s  %s\n",
               n->ssid, n->bssid[0],n->bssid[1],n->bssid[2],n->bssid[3],n->bssid[4],n->bssid[5],
               n->channel, sig, peak, n->privacy ? "WPA/WEP" : "open");
    }
    printf("\n%d networks found (Signal/Peak in dBm; %ld frames with CRC error dropped).\n",
           s.count, s.crc_dropped);
    if (any_adj)
        printf(">dBm: only heard from a neighbouring channel, real level is higher.\n");

done:
    rtl_rx_unbind();
    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h); libusb_exit(ctx);
    return 0;
}
