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
    int     hidden;                      /* beacon carries an empty/zeroed SSID */
    int     hidden_len;                  /* length of that SSID element (0 = empty) */
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

/* Walk the IEs: SSID (printable, "" if empty or all-zero) and DS channel.
 * *ssid_len is the raw SSID element length, -1 if there is none. */
static void parse_ies(const uint8_t *ie, int rem, char ssid[33], int *ssid_len, int *ds_ch) {
    ssid[0] = 0; *ssid_len = -1; *ds_ch = 0;
    while (rem >= 2) {
        int id = ie[0], l = ie[1];
        if (2 + l > rem) break;
        if (id == 0 && *ssid_len < 0) {
            int n = l > 32 ? 32 : l, zero = 1;
            for (int i = 0; i < n; i++)             /* keep the terminal safe */
                ssid[i] = (ie[2 + i] >= 0x20 && ie[2 + i] < 0x7f) ? (char)ie[2 + i] : '?';
            ssid[n] = 0;
            for (int i = 0; i < l; i++) if (ie[2 + i]) { zero = 0; break; }
            if (zero) ssid[0] = 0;                  /* hidden: empty or all-zero SSID */
            *ssid_len = l;
        } else if (id == 3 && l == 1) {
            *ds_ch = ie[2];                         /* DS Parameter Set: AP's own channel */
        }
        ie += 2 + l; rem -= 2 + l;
    }
}

static net_t *find_net(scan_ctx *s, const uint8_t *bssid) {
    for (int i = 0; i < s->count; i++)
        if (memcmp(s->nets[i].bssid, bssid, 6) == 0) return &s->nets[i];
    return NULL;
}

static void scan_cb(const uint8_t *f, uint32_t len, const rtl_rx_sub *rx, void *v) {
    scan_ctx *s = (scan_ctx *)v;
    if (len < 28) return;
    uint8_t fc = f[0];
    /* Beacon (0x80) / Probe-Resp (0x50) come from the AP. A hidden AP still
     * names itself in probe responses to a directed probe, and clients name
     * it in (Re)Assoc-Req (0x00/0x20): both reveal hidden SSIDs passively. */
    int body;
    switch (fc) {
    case 0x80: case 0x50: body = 12; break;         /* timestamp, interval, caps */
    case 0x00:            body = 4;  break;         /* caps, listen interval */
    case 0x20:            body = 10; break;         /* + current AP address */
    default: return;
    }
    if ((int)len < 24 + body + 2) return;
    if (rx->crc_err) { s->crc_dropped++; return; }
    const uint8_t *bssid = f + 16;                  /* addr3 */
    if (bssid[0] & 0x01) return;                    /* group bit: never a real BSSID */

    char ssid[33]; int ssid_len, ds_ch;
    parse_ies(f + 24 + body, (int)len - 24 - body, ssid, &ssid_len, &ds_ch);
    net_t *n = find_net(s, bssid);

    if (fc == 0x00 || fc == 0x20) {                 /* client frame: name only */
        if (n && !n->ssid[0] && ssid[0]) memcpy(n->ssid, ssid, sizeof(n->ssid));
        return;
    }

    if (!n) {
        if (s->count >= MAX_NETS) return;
        n = &s->nets[s->count++];
        memcpy(n->bssid, bssid, 6);
        n->channel = ds_ch ? ds_ch : s->cur_ch;
        n->peak_dbm = n->adj_peak_dbm = RTL_RSSI_NONE;
    }
    uint16_t caps = (uint16_t)(f[34] | (f[35] << 8)); /* Capability (24+8+2) */
    n->privacy = (caps & 0x0010) ? 1 : 0;
    if (ssid[0]) {
        if (!n->ssid[0]) memcpy(n->ssid, ssid, sizeof(n->ssid));
    } else if (fc == 0x80 && ssid_len >= 0) {
        n->hidden = 1; n->hidden_len = ssid_len;
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

/*
 * How likely two BSSIDs belong to the same router; -1 = unrelated, lower =
 * closer. Routers derive their per-SSID / per-band BSSIDs from one base MAC:
 * they vary the last byte (extra SSIDs), byte 3 (2.4 vs 5 GHz radio) or set
 * the locally-administered bits in byte 0. Bytes 1, 2 and 4 stay the same.
 */
static int sibling_score(const uint8_t *a, const uint8_t *b) {
    if (a[1] != b[1] || a[2] != b[2] || a[4] != b[4]) return -1;
    int d3 = abs(a[3] - b[3]), d5 = abs(a[5] - b[5]);
    if (d3 >= 16 || d5 >= 16) return -1;
    return (a[0] != b[0]) * 4096 + d3 * 256 + d5;
}

/* Closest named BSSID on the same router as nets[i], or -1. SSIDs that show
 * up on many BSSIDs (ISP hotspots like "xfinitywifi") say little about whose
 * router it is, so the owner's own SSID wins over them. */
static int named_sibling(const scan_ctx *s, int i) {
    int best = -1, best_score = 0;
    for (int j = 0; j < s->count; j++) {
        if (j == i || !s->nets[j].ssid[0]) continue;
        int sc = sibling_score(s->nets[i].bssid, s->nets[j].bssid);
        if (sc < 0) continue;
        int uses = 0;
        for (int k = 0; k < s->count; k++) uses += !strcmp(s->nets[k].ssid, s->nets[j].ssid);
        if (uses > 3) sc += 8192;
        if (best < 0 || sc < best_score) { best = j; best_score = sc; }
    }
    return best;
}

static void fmt_dbm(char *buf, size_t sz, int dbm, int adj) {
    if (dbm == RTL_RSSI_NONE) snprintf(buf, sz, "?");
    else                      snprintf(buf, sz, adj ? ">%d" : "%d", dbm);
}

static void usage(const char *argv0) {
    printf("Usage: %s [ms_per_channel]\n"
           "  Scan 2.4- and 5-GHz channels for beacons and probe responses and\n"
           "  list the networks, strongest first. Default dwell is 500 ms per\n"
           "  channel. Signal is the median dBm, Peak the strongest sample.\n"
           "  Hidden SSIDs are filled in passively when a client probes for or\n"
           "  joins the network during the scan; a longer dwell catches more.\n"
           "  Otherwise 'Same router as' names the visible network whose BSSID\n"
           "  shows it comes from the same router.\n",
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
    printf("\n\n%-32s %-17s %5s %6s %5s  %-16s %s\n",
           "SSID", "BSSID", "Chan", "Signal", "Peak", "Security", "Same router as");
    printf("------------------------------------------------------------------------------------------------------------\n");
    int any_adj = 0, n_hidden = 0, n_revealed = 0, n_sibling = 0;
    for (int i = 0; i < s.count; i++) {
        net_t *n = &s.nets[i];
        char sig[8], peak[8], name[33]; int adj;
        int dbm = signal_dbm(n, &adj);
        fmt_dbm(sig, sizeof(sig), dbm, adj);
        fmt_dbm(peak, sizeof(peak), adj ? dbm : n->peak_dbm, adj);
        any_adj |= adj;
        if (n->ssid[0])          snprintf(name, sizeof(name), "%s", n->ssid);
        else if (n->hidden_len)  snprintf(name, sizeof(name), "<hidden, %d chars>", n->hidden_len);
        else                     snprintf(name, sizeof(name), "<hidden>");
        n_hidden   += n->hidden;
        n_revealed += n->hidden && n->ssid[0];
        char sec[17];
        snprintf(sec, sizeof(sec), "%s%s", n->privacy ? "WPA/WEP" : "open",
                 (n->hidden && n->ssid[0]) ? ", hidden" : "");
        int sib = n->ssid[0] ? -1 : named_sibling(&s, i);
        n_sibling += sib >= 0;
        printf("%-32s %02x:%02x:%02x:%02x:%02x:%02x %5d %6s %5s  %-16s %s\n",
               name, n->bssid[0],n->bssid[1],n->bssid[2],n->bssid[3],n->bssid[4],n->bssid[5],
               n->channel, sig, peak, sec, sib >= 0 ? s.nets[sib].ssid : "");
    }
    printf("\n%d networks found (Signal/Peak in dBm; %ld frames with CRC error dropped).\n",
           s.count, s.crc_dropped);
    if (n_hidden)
        printf("%d hidden networks, %d named from probe responses or association requests\n"
               "(\", hidden\"); the rest had no client asking for them by name during the scan.\n",
               n_hidden, n_revealed);
    if (n_sibling)
        printf("Same router as: %d unnamed hidden networks share a base MAC address with a\n"
               "named one, i.e. they are extra SSIDs of that router (guessed from the BSSID).\n",
               n_sibling);
    if (any_adj)
        printf(">dBm: only heard from a neighbouring channel, real level is higher.\n");

done:
    rtl_rx_unbind();
    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h); libusb_exit(ctx);
    return 0;
}
