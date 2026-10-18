/*
 * rtl8821au_txpwr — efuse TX power for the RTL8821AU, 2.4 GHz, RF path A,
 * capped by the rtw88 regulatory limit table.
 *
 * Reference (Linux rtw88, GPL-2.0 OR BSD-3-Clause, (c) Realtek):
 *   phy.c  rtw_phy_get_2g_tx_power_index: base = cck_base[group] for CCK,
 *          bw40_base[group] otherwise; OFDM adds ht_1s_diff.ofdm; HT/VHT 1SS
 *          at 20 MHz add ht_1s_diff.bw20 (txgi_factor = 1 on this chip).
 *   phy.c  rtw_phy_get_tx_power_limit: min with rtw8821a_txpwr_lmt for the
 *          regulatory domain; worldwide = min(FCC, ETSI). We also fold in
 *          MKK so an unknown domain never exceeds any of the three.
 *   rtw88xxa.c rtw88xxa_set_tx_power_index_by_rate: TX AGC registers
 *          0xC20.. hold one index byte per rate.
 * Not ported: tx-power-by-rate offsets (rtw8821a_bb_pg), power training,
 * thermal tracking, 40/80 MHz, 5 GHz, path B.
 *
 * Limit rows generated from rtw8821a_table.c (band 2G, bw 20 MHz), index by
 * channel-1; 63 = no limit in the table.
 */
#include "rtl8821au.h"
#include "rtl_usb.h"
#include <stdio.h>

#define TXGI_MAX 0x3f

static const uint8_t lmt_cck_fcc[14] = { 32, 32, 36, 36, 36, 36, 36, 36, 32, 32, 32, 28, 26, 63 };
static const uint8_t lmt_cck_mkk[14] = { 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32 };
static const uint8_t lmt_cck_etsi[14] = { 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 63 };
static const uint8_t lmt_ofdm_fcc[14] = { 30, 30, 32, 32, 32, 32, 32, 32, 30, 30, 30, 26, 24, 63 };
static const uint8_t lmt_ofdm_mkk[14] = { 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 63 };
static const uint8_t lmt_ofdm_etsi[14] = { 30, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 30, 63 };
static const uint8_t lmt_ht_1s_fcc[14] = { 26, 26, 32, 32, 32, 32, 32, 32, 26, 26, 26, 26, 24, 63 };
static const uint8_t lmt_ht_1s_mkk[14] = { 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 63 };
static const uint8_t lmt_ht_1s_etsi[14] = { 26, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 26, 63 };

#define R_TXAGC_A_CCK11_CCK1     0x0C20
#define R_TXAGC_A_OFDM18_OFDM6   0x0C24
#define R_TXAGC_A_OFDM54_OFDM24  0x0C28
#define R_TXAGC_A_MCS3_MCS0      0x0C2C
#define R_TXAGC_A_MCS7_MCS4      0x0C30
#define R_TXAGC_A_NSS1_IDX3_0    0x0C3C
#define R_TXAGC_A_NSS1_IDX7_4    0x0C40

static int group_2g(int ch) {
    if (ch <= 2) return 0; if (ch <= 5) return 1; if (ch <= 8) return 2;
    if (ch <= 11) return 3; if (ch <= 13) return 4; return 5;
}
static int nib_s8(uint8_t n) { n &= 0xF; return (n & 0x8) ? (int)n - 16 : (int)n; }
static int min3(int a, int b, int c) { int m = a < b ? a : b; return m < c ? m : c; }
static uint8_t clamp(int v) { return (uint8_t)(v < 0 ? 0 : v > TXGI_MAX ? TXGI_MAX : v); }
static uint32_t rep4(uint8_t v) { return (uint32_t)v | ((uint32_t)v << 8) | ((uint32_t)v << 16) | ((uint32_t)v << 24); }

/* regd: 0 FCC, 1 MKK, 2 ETSI, anything else = worldwide (min of the three) */
static int limit_for(int regd, int ch, const uint8_t *fcc, const uint8_t *mkk, const uint8_t *etsi) {
    int i = ch - 1;
    switch (regd) {
        case 0: return fcc[i];
        case 1: return mkk[i];
        case 2: return etsi[i];
        default: return min3(fcc[i], mkk[i], etsi[i]);
    }
}

int rtl8821au_txpwr_apply(libusb_device_handle *h, int ch, int verbose) {
    const rtl8821a_board *b = rtl8821a_board_info(h, 0);
    if (ch < 1 || ch > 14) { if (verbose) printf("[txpwr] 5 GHz TX power not ported, leaving registers\n"); return 0; }

    int g = group_2g(ch);
    uint8_t cck_base = b->pg2g[g];
    if (cck_base > TXGI_MAX) {
        printf("[txpwr] efuse PG block unprogrammed (CCK base 0x%02x), leaving chip power untouched\n", cck_base);
        return 0;
    }
    uint8_t bw40_base = (g <= 4) ? b->pg2g[6 + g] : cck_base;
    if (bw40_base > TXGI_MAX) bw40_base = cck_base;
    int ofdm_diff = nib_s8(b->pg2g[11]);        /* ht_1s_diff.ofdm, low nibble  */
    int bw20_diff = nib_s8(b->pg2g[11] >> 4);   /* ht_1s_diff.bw20, high nibble */

    int regd = b->regd;
    int cck  = clamp(cck_base);
    int ofdm = clamp(bw40_base + ofdm_diff);
    int ht1s = clamp(bw40_base + bw20_diff);

    int l_cck  = limit_for(regd, ch, lmt_cck_fcc,   lmt_cck_mkk,   lmt_cck_etsi);
    int l_ofdm = limit_for(regd, ch, lmt_ofdm_fcc,  lmt_ofdm_mkk,  lmt_ofdm_etsi);
    int l_ht   = limit_for(regd, ch, lmt_ht_1s_fcc, lmt_ht_1s_mkk, lmt_ht_1s_etsi);
    int cck_f  = cck  < l_cck  ? cck  : l_cck;
    int ofdm_f = ofdm < l_ofdm ? ofdm : l_ofdm;
    int ht_f   = ht1s < l_ht   ? ht1s : l_ht;

    rtl_write32(h, R_TXAGC_A_CCK11_CCK1,    rep4((uint8_t)cck_f));
    rtl_write32(h, R_TXAGC_A_OFDM18_OFDM6,  rep4((uint8_t)ofdm_f));
    rtl_write32(h, R_TXAGC_A_OFDM54_OFDM24, rep4((uint8_t)ofdm_f));
    rtl_write32(h, R_TXAGC_A_MCS3_MCS0,     rep4((uint8_t)ht_f));
    rtl_write32(h, R_TXAGC_A_MCS7_MCS4,     rep4((uint8_t)ht_f));
    rtl_write32(h, R_TXAGC_A_NSS1_IDX3_0,   rep4((uint8_t)ht_f));
    rtl_write32(h, R_TXAGC_A_NSS1_IDX7_4,   rep4((uint8_t)ht_f));

    printf("[txpwr] ch %d group %d regd %s: efuse CCK=%d OFDM=%d(%d%+d) HT1S=%d(%d%+d); limits CCK=%d OFDM=%d HT=%d -> applied CCK=%d OFDM=%d HT/VHT1S=%d\n",
           ch, g, regd == 0 ? "FCC" : regd == 1 ? "MKK" : regd == 2 ? "ETSI" : "WW",
           cck, ofdm, bw40_base, ofdm_diff, ht1s, bw40_base, bw20_diff,
           l_cck, l_ofdm, l_ht, cck_f, ofdm_f, ht_f);
    return 0;
}

void rtl8821au_txpwr_readback(libusb_device_handle *h, const char *label) {
    printf("[txpwr] readback (%s): CCK=0x%08x OFDM6-18=0x%08x OFDM24-54=0x%08x MCS0-3=0x%08x\n", label,
           rtl_read32(h, R_TXAGC_A_CCK11_CCK1, NULL), rtl_read32(h, R_TXAGC_A_OFDM18_OFDM6, NULL),
           rtl_read32(h, R_TXAGC_A_OFDM54_OFDM24, NULL), rtl_read32(h, R_TXAGC_A_MCS3_MCS0, NULL));
}
