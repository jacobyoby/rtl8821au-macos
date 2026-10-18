/*
 * associate — IE2: authentication (Open) + association with an AP.
 *
 * Verifiable WITHOUT a password: Open auth and assoc run before the WPA handshake.
 * The AP answers with an Auth-Response and an Assoc-Response (status codes).
 *
 * Usage: ./associate <channel> <bssid aa:bb:cc:dd:ee:ff> <ssid>
 * Example: ./associate 6 04:b4:fe:81:91:cf "FRITZ!Box 6360 Cable"
 *
 * Sets REG_MACID = our MAC so the hardware automatically ACKs the AP's
 * unicast responses, otherwise the AP aborts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rtl_usb.h"
#include "rtl_hal.h"
#include "rtl_rf.h"
#include "rtl_rx.h"
#include "rtl_tx.h"

static uint8_t g_sa[6] = { 0x00, 0xc0, 0xca, 0xbc, 0x4e, 0xfa };
static uint8_t g_bssid[6];

typedef struct { int auth_status; int assoc_status; int aid; } assoc_ctx;

static int parse_mac(const char *s, uint8_t *m) {
    return sscanf(s, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &m[0],&m[1],&m[2],&m[3],&m[4],&m[5]) == 6;
}

static int g_acks = 0;
static void cb(const uint8_t *f, uint32_t len, void *v) {
    if (len >= 10 && f[0] == 0xd4 && !memcmp(f + 4, g_sa, 6)) { g_acks++; return; }   /* ACK to us */
    if (len >= 24 && !memcmp(f + 10, g_bssid, 6) && !memcmp(f + 4, g_sa, 6)) {
        static int logged = 0;
        if (logged < 12) { logged++; printf("    <- from AP to us: fc=0x%02x%02x len=%u%s", f[1], f[0], len,
            (f[0]==0xb0)?" (auth)":(f[0]==0x10)?" (assoc-resp)":(f[0]==0xc0)?" (deauth)":(f[0]==0xa0)?" (disassoc)":(f[0]==0x30)?" (reassoc-resp)":"");
            if ((f[0]==0xc0 || f[0]==0xa0) && len >= 26) printf(" reason=%u", f[24] | (f[25] << 8));
            printf("%s\n", (f[1] & 0x08) ? " [retry]" : ""); }
    }
    assoc_ctx *c = (assoc_ctx *)v;
    if (len < 24) return;
    uint8_t fc = f[0];
    if (memcmp(f + 10, g_bssid, 6) != 0) return;   /* SA = target AP? */
    if (memcmp(f + 4, g_sa, 6) != 0) return;       /* DA = us? */
    if (fc == 0xB0 && len >= 30) {                 /* Authentication */
        c->auth_status = f[28] | (f[29] << 8);     /* status at body+4 */
    } else if (fc == 0x10 && len >= 30) {          /* Assoc Response */
        c->assoc_status = f[26] | (f[27] << 8);    /* status at body+2 */
        c->aid = (f[28] | (f[29] << 8)) & 0x3fff;
    }
}

int main(int argc, char **argv) {
    if (argc < 4) { printf("Usage: %s <channel> <bssid> <ssid>\n", argv[0]); return 1; }
    int channel = atoi(argv[1]);
    if (!parse_mac(argv[2], g_bssid)) { printf("Invalid BSSID\n"); return 1; }
    const char *ssid = argv[3];
    int slen = (int)strlen(ssid); if (slen > 32) slen = 32;

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) return 1;
    libusb_device_handle *h = NULL; uint16_t vid = 0, pid = 0; int claimed = 0;
    int rc = rtl_open_first_ex(ctx, &h, &vid, &pid, &claimed);
    if (rc || !h) { printf("No device.\n"); libusb_exit(ctx); return 2; }

    rtl_rx_bind(ctx, rtl_chip_probe(vid, pid));   /* enable the asynchronous RX path */

    int cal = (argc > 4) ? atoi(argv[4]) : 0;        /* 1 = run IQK like rtl_wpa_connect */
    int st  = (argc > 5) ? atoi(argv[5]) : 0;        /* bit0: station RCR, bit1: MSR+REG_BSSID (rtl_wpa setup) */
    if (rtl_hal_full_init(h, channel, cal, 0) != 0) { printf("Init failed.\n"); goto done; }

    /* Our own MAC into the MACID register -> hardware auto-ACK for responses to us. */
    rtl_reg_write(h, REG_MACID, g_sa, 6);
    if (st & 2) {
        rtl_reg_write(h, 0x0618, g_bssid, 6);                          /* REG_BSSID */
        uint8_t msr = rtl_read8(h, 0x0102, NULL);
        rtl_write8(h, 0x0102, (uint8_t)((msr & 0xFC) | 0x02));         /* MSR port0 = station */
    }
    if (st & 1) {
        uint32_t rcr = (1u<<26)|(1u<<13)|(1u<<1)|(1u<<2)|(1u<<3)|(1u<<6)|(1u<<7)|(1u<<14)|(1u<<28)|(1u<<29)|(1u<<30)|(1u<<31);
        rtl_write32(h, 0x0608, rcr);                                   /* rtl_wpa station RCR */
    }
    printf("Setup: cal=%d station-flags=%d RCR=0x%08x MSR=0x%02x\n", cal, st, rtl_read32(h, 0x0608, NULL), rtl_read8(h, 0x0102, NULL));

    assoc_ctx c = { -1, -1, 0 };

    /* --- Authentication (Open System) --- */
    uint8_t auth[30];
    uint8_t ah[] = { 0xB0,0x00, 0x00,0x00 };
    memcpy(auth, ah, 4);
    memcpy(auth+4, g_bssid, 6); memcpy(auth+10, g_sa, 6); memcpy(auth+16, g_bssid, 6);
    auth[22]=0; auth[23]=0;                 /* seq ctl */
    auth[24]=0x00; auth[25]=0x00;           /* algo = open */
    auth[26]=0x01; auth[27]=0x00;           /* transaction seq = 1 */
    auth[28]=0x00; auth[29]=0x00;           /* status */
    printf("Sending Authentication (Open) to %s ...\n", argv[2]);
    for (int i = 0; i < 8 && c.auth_status < 0; i++) {
        rtl_tx_inject(h, auth, sizeof(auth), RTL_RATE_6M, RTL_QSLT_MGNT, rtl_rx_bound_ops()->tx_ep_mgmt);
        rtl_rx_poll(h, 200, cb, &c);
    }
    if (c.auth_status < 0) { printf("No Auth Response received.\n"); goto done; }
    printf("Auth-Response: Status %d (%s)\n", c.auth_status, c.auth_status==0?"OK":"rejected");
    if (c.auth_status != 0) goto done;

    /* --- Association Request --- */
    uint8_t assoc[128]; int p = 0;
    assoc[p++]=0x00; assoc[p++]=0x00;       /* FC: assoc req */
    assoc[p++]=0x00; assoc[p++]=0x00;       /* dur */
    memcpy(assoc+p, g_bssid, 6); p+=6; memcpy(assoc+p, g_sa, 6); p+=6; memcpy(assoc+p, g_bssid, 6); p+=6;
    assoc[p++]=0x00; assoc[p++]=0x00;       /* seq ctl */
    assoc[p++]=0x11; assoc[p++]=0x00;       /* cap: ESS|Privacy */
    assoc[p++]=0x0a; assoc[p++]=0x00;       /* listen interval */
    assoc[p++]=0x00; assoc[p++]=(uint8_t)slen; memcpy(assoc+p, ssid, slen); p+=slen; /* SSID IE */
    { uint8_t r[]={0x01,0x08,0x02,0x04,0x0b,0x16,0x0c,0x12,0x18,0x24}; memcpy(assoc+p,r,sizeof(r)); p+=sizeof(r); }
    { uint8_t er[]={0x32,0x04,0x30,0x48,0x60,0x6c}; memcpy(assoc+p,er,sizeof(er)); p+=sizeof(er); } /* Ext Rates */
    /* RSN-IE: WPA2, Group=CCMP, Pairwise=CCMP, AKM=PSK */
    { uint8_t rsn[]={0x30,0x14, 0x01,0x00, 0x00,0x0f,0xac,0x04,
                     0x01,0x00, 0x00,0x0f,0xac,0x04, 0x01,0x00, 0x00,0x0f,0xac,0x02, 0x00,0x00};
      memcpy(assoc+p,rsn,sizeof(rsn)); p+=sizeof(rsn); }
    printf("Sending Association Request (SSID \"%s\") ...\n", ssid);
    for (int i = 0; i < 8 && c.assoc_status < 0; i++) {
        rtl_tx_inject(h, assoc, p, RTL_RATE_6M, RTL_QSLT_MGNT, rtl_rx_bound_ops()->tx_ep_mgmt);
        rtl_rx_poll(h, 200, cb, &c);
    }
    if (c.assoc_status < 0) { printf("No Assoc Response received.\n"); goto done; }
    printf("Assoc-Response: Status %d (%s), AID=%d\n",
           c.assoc_status, c.assoc_status==0?"ASSOCIATED":"rejected", c.aid);
    if (c.assoc_status == 0)
        printf("\n==> IE2 OK: associated with the AP. Next step: WPA2 handshake (IE3).\n");
    if ((st & 8) && c.assoc_status == 0) {
        int tep  = (argc > 6) ? (int)strtoul(argv[6], NULL, 16) : rtl_rx_bound_ops()->tx_ep_mgmt;
        int tqs  = (argc > 7) ? (int)strtoul(argv[7], NULL, 16) : RTL_QSLT_VO;
        uint8_t nd[24] = {0x48, 0x01, 0, 0};    /* null data, ToDS */
        memcpy(nd + 4, g_bssid, 6); memcpy(nd + 10, g_sa, 6); memcpy(nd + 16, g_bssid, 6);
        int ok = 0, err = 0; g_acks = 0;
        for (int i = 0; i < 20; i++) {
            int r = rtl_tx_inject(h, nd, 24, RTL_RATE_6M, (uint8_t)tqs, (uint8_t)tep);
            if (r == 0) ok++; else err++;
            rtl_rx_poll(h, 40, cb, &c);
        }
        printf("Null-data TX test: ep=0x%02x qsel=0x%02x  sent ok=%d err=%d  ACKs from AP to us=%d\n", tep, tqs, ok, err, g_acks);
    }
    if ((st & 4) && c.assoc_status == 0) {
        /* leave cleanly: deauthenticate our own association (reason 3 = station leaving) */
        uint8_t da[26] = {0xC0, 0, 0, 0}; memcpy(da + 4, g_bssid, 6); memcpy(da + 10, g_sa, 6); memcpy(da + 16, g_bssid, 6);
        da[22] = 0; da[23] = 0; da[24] = 3; da[25] = 0;
        int r = rtl_tx_inject(h, da, 26, RTL_RATE_6M, RTL_QSLT_MGNT, rtl_rx_bound_ops()->tx_ep_mgmt);
        rtl_rx_poll(h, 100, cb, &c);
        printf("Leave: deauth (reason 3) sent to AP, rc=%d\n", r);
    }

done:
    rtl_rx_unbind();
    if (claimed) libusb_release_interface(h, 0);
    libusb_close(h); libusb_exit(ctx);
    return 0;
}
