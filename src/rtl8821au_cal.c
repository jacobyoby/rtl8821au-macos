/*
 * rtl8821au_cal — IQ calibration and initial gain for the RTL8821AU.
 *
 * Ported from Linux rtw88 (GPL-2.0 OR BSD-3-Clause, (c) Realtek):
 *   rtw8821a.c  rtw8821a_do_iqk, rtw8821a_iqk, rtw8821a_iqk_tx_vdf_false,
 *               rtw8821a_iqk_rx, rtw8821a_iqk_tx_fill / rx_fill,
 *               rtw8821a_iqk_backup_rf / restore_rf / restore_afe
 *   rtw88xxa.c  rtw88xxa_iqk_backup_mac_bb / backup_afe / configure_mac /
 *               restore_mac_bb / iqk_finish
 *   phy.c       rtw_phy_dig_write (chip->dig = REG_RXIGI_A[6:0])
 *   rtw88xxa.c  rtw88xxa_phy_cck_pd_set (REG_CCK_PD_TH levels)
 *
 * Scope: path A, 20 MHz (the 80 MHz "VDF" TX variant is not ported), USB
 * (one RX IQK loop). rtw88 performs no LC calibration on the 8821A, so
 * rtl8821au_lck is a documented no-op.
 */
#include "rtl8821au.h"
#include "rtl_usb.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define BIT(n) (1u << (n))
#define GENMASK(h, l) ((0xFFFFFFFFu >> (31 - (h))) & (0xFFFFFFFFu << (l)))
#define RFREG_MASK 0xFFFFF

#define REG_TXPAUSE          0x0522
#define REG_BCN_CTRL         0x0550
#define REG_RXPSEL           0x0808
#define REG_CCASEL           0x082C
#define REG_CCA2ND           0x0838
#define REG_DAC_RSTB         0x090C
#define REG_IQK_COM00        0x0978
#define REG_IQK_COM32        0x097C
#define REG_IQK_COM64        0x0980
#define REG_IQK_COM96        0x0984
#define REG_CCK_PD_TH        0x0A0A
#define REG_CCK_RX           0x0A04
#define REG_3WIRE_SWA        0x0C00
#define REG_RX_IQC_AB_A      0x0C10
#define REG_RXIGI_A          0x0C50
#define REG_CK_MONHA         0x0C5C
#define REG_AFE_PWR1_A       0x0C60
#define REG_AFE_PWR2_A       0x0C64
#define REG_RX_WAIT_CCA_A    0x0C68
#define REG_OFDM0_XA_TX_IQ   0x0C80
#define REG_OFDM0_A_TX_AFE   0x0C84
#define REG_OFDM0_XB_TX_IQ   0x0C88
#define REG_TSSI_TRK_SW      0x0C8C
#define REG_LSSI_WRITE_A     0x0C90
#define REG_TXAGCIDX         0x0C94
#define REG_RFECTL_A         0x0CB8
#define REG_IQK_DPD_CFG      0x0CC4
#define REG_CFG_PMPD         0x0CC8
#define REG_IQC_Y            0x0CCC
#define REG_IQC_X            0x0CD4
#define REG_IQKA_END         0x0D00

#define RF_DTXLOK            0x08
#define RF_CFGCH             0x18
#define RF_MODE_TABLE_ADDR   0x30
#define RF_MODE_TABLE_DATA0  0x31
#define RF_MODE_TABLE_DATA1  0x32
#define RF_TXMOD             0x58
#define RF_TXA_PREPAD        0x65
#define RF_RXBB2             0x8F
#define RF_LUTWE             0xEF

#define CAL_NUM_8821A        3
#define MACBB_NUM            8
#define AFE_NUM              4
#define RF_NUM               3

static uint32_t ffs32(uint32_t m) { uint32_t s = 0; while (s < 32 && !(m & (1u << s))) s++; return s; }
static uint32_t rd32(libusb_device_handle *h, uint16_t r) { return rtl_read32(h, r, NULL); }
static void     wr32(libusb_device_handle *h, uint16_t r, uint32_t v) { rtl_write32(h, r, v); }
static uint32_t rd_mask(libusb_device_handle *h, uint16_t r, uint32_t mask) { return (rd32(h, r) & mask) >> ffs32(mask); }
static void wr_mask(libusb_device_handle *h, uint16_t r, uint32_t mask, uint32_t val) {
    uint32_t v = rd32(h, r);
    wr32(h, r, (v & ~mask) | ((val << ffs32(mask)) & mask));
}
static void page_c (libusb_device_handle *h) { wr_mask(h, REG_CCASEL, BIT(31), 0); }
static void page_c1(libusb_device_handle *h) { wr_mask(h, REG_CCASEL, BIT(31), 1); }
static void rf(libusb_device_handle *h, uint8_t a, uint32_t mask, uint32_t d) { rtl8821a_rf_write(h, a, mask, d); }
static void mdelay(unsigned ms) { usleep(ms * 1000u); }
static int  abs_diff(int a, int b) { return a > b ? a - b : b - a; }

/* ---- initial gain / CCK PD ------------------------------------------------ */
int rtl8821au_dig_set(libusb_device_handle *h, uint8_t igi) {
    wr_mask(h, REG_RXIGI_A, 0x7f, igi);
    return 0;
}
uint8_t rtl8821au_dig_get(libusb_device_handle *h) { return (uint8_t)rd_mask(h, REG_RXIGI_A, 0x7f); }

int rtl8821au_cck_pd_set(libusb_device_handle *h, int lvl) {
    static const uint8_t pd[5] = {0x40, 0x83, 0xcd, 0xdd, 0xed};
    if (lvl < 0 || lvl > 4) return 1;
    return rtl_write8(h, REG_CCK_PD_TH, pd[lvl]);
}

/* ---- IQK ------------------------------------------------------------------- */
static const uint16_t backup_macbb_reg[MACBB_NUM] = { 0x520, 0x550, 0x808, 0xa04, 0x90c, 0xc00, 0x838, 0x82c };
static const uint16_t backup_afe_reg[AFE_NUM]     = { 0xc5c, 0xc60, 0xc64, 0xc68 };
static const uint8_t  backup_rf_reg[RF_NUM]       = { 0x65, 0x8f, 0x0 };

static void iqk_configure_mac(libusb_device_handle *h) {
    page_c(h);
    rtl_write8(h, REG_TXPAUSE, 0x3f);
    wr_mask(h, REG_BCN_CTRL, (BIT(3) << 8) | BIT(3), 0);     /* EN_BCN_FUNCTION both ports off */
    rtl_write8(h, REG_RXPSEL, 0x00);                          /* RX ante off */
    wr_mask(h, REG_CCA2ND, 0xf, 0xc);                         /* CCA off */
    rtl_write8(h, REG_CCK_RX + 3, 0xf);                       /* CCK RX path off */
}

static void iqk_tx_fill(libusb_device_handle *h, unsigned tx_x, unsigned tx_y) {
    page_c1(h);
    wr32(h, REG_LSSI_WRITE_A, 0x00000080);
    wr32(h, REG_IQK_DPD_CFG, 0x20040000);
    wr32(h, REG_CFG_PMPD, 0x20000000);
    wr_mask(h, REG_IQC_Y, 0x000007ff, tx_y);
    wr_mask(h, REG_IQC_X, 0x000007ff, tx_x);
}

static void iqk_rx_fill(libusb_device_handle *h, unsigned rx_x, unsigned rx_y) {
    page_c(h);
    wr_mask(h, REG_RX_IQC_AB_A, 0x000003ff, rx_x >> 1);
    wr_mask(h, REG_RX_IQC_AB_A, 0x03ff0000, (rx_y >> 1) & 0x3ff);
}

static void iqk_restore_afe(libusb_device_handle *h, const uint32_t *afe_backup) {
    page_c(h);
    for (int i = 0; i < AFE_NUM; i++) wr32(h, backup_afe_reg[i], afe_backup[i]);
    page_c1(h);
    wr32(h, REG_OFDM0_XA_TX_IQ, 0x0);
    wr32(h, REG_OFDM0_A_TX_AFE, 0x0);
    wr32(h, REG_OFDM0_XB_TX_IQ, 0x0);
    wr32(h, REG_TSSI_TRK_SW, 0x3c000000);
    wr32(h, REG_LSSI_WRITE_A, 0x00000080);
    wr32(h, REG_TXAGCIDX, 0x00000000);
    wr32(h, REG_IQK_DPD_CFG, 0x20040000);
    wr32(h, REG_CFG_PMPD, 0x20000000);
    wr32(h, REG_RFECTL_A, 0x0);
}

static int iqk_finish(int average, int threshold, const int *x_temp, const int *y_temp, int *x, int *y) {
    int finish = 0;
    for (int i = 0; i < average && !finish; i++) {
        for (int ii = i + 1; ii < average; ii++) {
            int dx = abs_diff(x_temp[i] >> 21, x_temp[ii] >> 21);
            int dy = abs_diff(y_temp[i] >> 21, y_temp[ii] >> 21);
            if (dx < threshold && dy < threshold) {
                *x = ((x_temp[i] >> 21) + (x_temp[ii] >> 21)) / 2;
                *y = ((y_temp[i] >> 21) + (y_temp[ii] >> 21)) / 2;
                finish = 1;
                break;
            }
        }
    }
    return finish;
}

static void one_shot(libusb_device_handle *h) {
    wr32(h, REG_IQK_COM64, 0xfa000000);
    wr32(h, REG_IQK_COM64, 0xf8000000);
    mdelay(10);
    wr32(h, REG_RFECTL_A, 0x00000000);
}

/* rtw8821a_iqk_tx_vdf_false: rtw88 forces iqk_ready=true and tx_fail=false
 * (the vendor code's "~x" checks were typos), so the first shot is taken. */
static void iqk_tx(libusb_device_handle *h, int *tx_x0, int *tx_y0, int *ok) {
    wr32(h, REG_OFDM0_XA_TX_IQ, 0x18008c10);   /* TX tone 16 */
    wr32(h, REG_OFDM0_A_TX_AFE, 0x38008c10);   /* RX tone    */
    wr32(h, REG_RFECTL_A, 0x00100000);
    *ok = 0;
    for (int retry = 0; retry < 10; retry++) {
        one_shot(h);
        wr32(h, REG_RFECTL_A, 0x02000000);
        *tx_x0 = (int)(rd_mask(h, REG_IQKA_END, 0x07ff0000) << 21);
        wr32(h, REG_RFECTL_A, 0x04000000);
        *tx_y0 = (int)(rd_mask(h, REG_IQKA_END, 0x07ff0000) << 21);
        *ok = 1;
        break;
    }
}

static void iqk_rx(libusb_device_handle *h, int *rx_x0, int *rx_y0, int *ok) {
    wr32(h, REG_RFECTL_A, 0x00100000);
    *ok = 0;
    for (int retry = 0; retry < 10; retry++) {
        one_shot(h);
        uint32_t rx_fail = rd_mask(h, REG_IQKA_END, BIT(11));
        if (!rx_fail) {
            wr32(h, REG_RFECTL_A, 0x06000000);
            *rx_x0 = (int)(rd_mask(h, REG_IQKA_END, 0x07ff0000) << 21);
            wr32(h, REG_RFECTL_A, 0x08000000);
            *rx_y0 = (int)(rd_mask(h, REG_IQKA_END, 0x07ff0000) << 21);
            *ok = 1;
            break;
        }
        wr_mask(h, REG_RX_IQC_AB_A, 0x000003ff, 0x200 >> 1);
        wr_mask(h, REG_RX_IQC_AB_A, 0x03ff0000, 0x0);
    }
}

static void iqk_8821a(libusb_device_handle *h, const rtl8821a_board *b, int band5g, int verbose) {
    int tx_x0[CAL_NUM_8821A] = {0}, tx_y0[CAL_NUM_8821A] = {0};
    int rx_x0[CAL_NUM_8821A] = {0}, rx_y0[CAL_NUM_8821A] = {0};
    int tx_average = 0, rx_average = 0;
    int tx_x = 0, tx_y = 0, rx_x = 0, rx_y = 0;

    for (int cal = 0; cal < CAL_NUM_8821A; cal++) {
        int tx_ok = 0, rx_ok = 0;

        /* ---- path-A LOK ---- */
        page_c(h);
        wr32(h, REG_AFE_PWR1_A, 0x77777777);       /* DAC/ADC on */
        wr32(h, REG_AFE_PWR2_A, 0x77777777);
        wr32(h, REG_RX_WAIT_CCA_A, 0x19791979);
        wr_mask(h, REG_3WIRE_SWA, 0xf, 0x4);       /* hardware 3-wire off */
        wr_mask(h, REG_CK_MONHA, GENMASK(26, 24), 0x7);   /* 160 MHz sampling */

        rf(h, RF_LUTWE, RFREG_MASK, 0x80002);
        rf(h, RF_CFGCH, 0x00c00, 0x3);             /* LOK at BW 20 */
        rf(h, RF_MODE_TABLE_ADDR,  RFREG_MASK, 0x20000);
        rf(h, RF_MODE_TABLE_DATA0, RFREG_MASK, 0x0003f);
        rf(h, RF_MODE_TABLE_DATA1, RFREG_MASK, 0xf3fc3);
        rf(h, RF_TXA_PREPAD, RFREG_MASK, 0x931d5);
        rf(h, RF_RXBB2, RFREG_MASK, 0x8a001);
        wr32(h, REG_DAC_RSTB, 0x00008000);
        wr_mask(h, REG_TXAGCIDX, BIT(0), 0x1);
        wr32(h, REG_IQK_COM00, 0x29002000);        /* TX (X,Y) */
        wr32(h, REG_IQK_COM32, 0xa9002000);        /* RX (X,Y) */
        wr32(h, REG_IQK_COM96, 0x00462910);        /* [0] AGC_en, [15] idac_K_Mask */

        page_c1(h);
        wr32(h, REG_OFDM0_XB_TX_IQ, b->ext_pa_5g ? 0x821403f7 : 0x821403f4);
        wr32(h, REG_TSSI_TRK_SW, band5g ? 0x68163e96 : 0x28163e96);
        wr32(h, REG_OFDM0_XA_TX_IQ, 0x18008c10);
        wr32(h, REG_OFDM0_A_TX_AFE, 0x38008c10);
        wr32(h, REG_RFECTL_A, 0x00100000);
        wr32(h, REG_IQK_COM64, 0xfa000000);
        wr32(h, REG_IQK_COM64, 0xf8000000);
        mdelay(10);
        wr32(h, REG_RFECTL_A, 0x00000000);

        page_c(h);
        rf(h, RF_TXMOD, 0x7fe00, rtl8821a_rf_read(h, RF_DTXLOK, 0xffc00));
        /* bandwidth 20: RF18 BW bits already 3 */
        page_c1(h);

        /* ---- TX RF setting ---- */
        page_c(h);
        rf(h, RF_LUTWE, RFREG_MASK, 0x80000);
        rf(h, RF_MODE_TABLE_ADDR,  RFREG_MASK, 0x20000);
        rf(h, RF_MODE_TABLE_DATA0, RFREG_MASK, 0x0003f);
        rf(h, RF_MODE_TABLE_DATA1, RFREG_MASK, 0xf3fc3);
        rf(h, RF_TXA_PREPAD, RFREG_MASK, 0x931d5);
        rf(h, RF_RXBB2, RFREG_MASK, 0x8a001);
        rf(h, RF_LUTWE, RFREG_MASK, 0x00000);
        wr32(h, REG_DAC_RSTB, 0x00008000);
        wr_mask(h, REG_TXAGCIDX, BIT(0), 0x1);
        wr32(h, REG_IQK_COM00, 0x29002000);
        wr32(h, REG_IQK_COM32, 0xa9002000);
        wr32(h, REG_IQK_COM96, 0x0046a910);

        page_c1(h);
        wr32(h, REG_OFDM0_XB_TX_IQ, b->ext_pa_5g ? 0x821403f7 : 0x821403e3);
        wr32(h, REG_TSSI_TRK_SW, band5g ? 0x40163e96 : 0x00163e96);

        iqk_tx(h, &tx_x0[cal], &tx_y0[cal], &tx_ok);
        if (!tx_ok) break;                         /* TXK fail, don't do RXK */

        /* ---- RX IQK ---- */
        page_c(h);
        rf(h, RF_LUTWE, RFREG_MASK, 0x80000);
        rf(h, RF_MODE_TABLE_ADDR,  RFREG_MASK, 0x30000);
        rf(h, RF_MODE_TABLE_DATA0, RFREG_MASK, 0x0002f);
        rf(h, RF_MODE_TABLE_DATA1, RFREG_MASK, 0xfffbb);
        rf(h, RF_RXBB2, RFREG_MASK, 0x88001);
        rf(h, RF_TXA_PREPAD, RFREG_MASK, 0x931d8);
        rf(h, RF_LUTWE, RFREG_MASK, 0x00000);

        wr_mask(h, REG_IQK_COM00, 0x03FF8000, ((uint32_t)tx_x0[cal] >> 21) & 0x7ff);
        wr_mask(h, REG_IQK_COM00, 0x000007FF, ((uint32_t)tx_y0[cal] >> 21) & 0x7ff);
        wr_mask(h, REG_IQK_COM00, BIT(31), 0x1);
        wr_mask(h, REG_IQK_COM00, BIT(31), 0x0);
        wr32(h, REG_DAC_RSTB, 0x00008000);
        wr32(h, REG_IQK_COM96, 0x0046a911);

        page_c1(h);
        wr32(h, REG_OFDM0_XA_TX_IQ, 0x38008c10);
        wr32(h, REG_OFDM0_A_TX_AFE, 0x18008c10);
        wr32(h, REG_OFDM0_XB_TX_IQ, 0x02140119);

        wr32(h, REG_TSSI_TRK_SW, 0x28160d00);      /* USB: one RX loop */
        iqk_rx(h, &rx_x0[cal], &rx_y0[cal], &rx_ok);

        if (tx_ok) tx_average++;
        if (rx_ok) rx_average++;
        if (verbose)
            printf("[8821au] iqk[%d]: tx (x=0x%03x y=0x%03x ok=%d)  rx (x=0x%03x y=0x%03x ok=%d)\n", cal,
                   ((uint32_t)tx_x0[cal] >> 21) & 0x7ff, ((uint32_t)tx_y0[cal] >> 21) & 0x7ff, tx_ok,
                   ((uint32_t)rx_x0[cal] >> 21) & 0x7ff, ((uint32_t)rx_y0[cal] >> 21) & 0x7ff, rx_ok);
    }

    /* ---- fill results ---- */
    if (tx_average == 0) { printf("[8821au] iqk: no TX result\n"); return; }
    if (iqk_finish(tx_average, 3, tx_x0, tx_y0, &tx_x, &tx_y)) {
        iqk_tx_fill(h, (unsigned)tx_x, (unsigned)tx_y);
        if (verbose) printf("[8821au] iqk: TX IQC x=0x%03x y=0x%03x\n", tx_x & 0x7ff, tx_y & 0x7ff);
    } else {
        iqk_tx_fill(h, 0x200, 0x0);
        printf("[8821au] iqk: TX results inconsistent, default 0x200/0\n");
    }
    if (rx_average == 0) { printf("[8821au] iqk: no RX result\n"); return; }
    if (iqk_finish(rx_average, 4, rx_x0, rx_y0, &rx_x, &rx_y)) {
        iqk_rx_fill(h, (unsigned)rx_x, (unsigned)rx_y);
        if (verbose) printf("[8821au] iqk: RX IQC x=0x%03x y=0x%03x\n", rx_x & 0x7ff, rx_y & 0x7ff);
    } else {
        iqk_rx_fill(h, 0x200, 0x0);
        printf("[8821au] iqk: RX results inconsistent, default 0x200/0\n");
    }
}

int rtl8821au_iqk(libusb_device_handle *h, int verbose) {
    const rtl8821a_board *b = rtl8821a_board_info(h, 0);
    uint32_t macbb_backup[MACBB_NUM], afe_backup[AFE_NUM], rfa_backup[RF_NUM];
    int band5g = (rtl_read8(h, 0x0454, NULL) & BIT(7)) ? 1 : 0;   /* REG_CCK_CHECK */

    page_c(h);
    for (int i = 0; i < MACBB_NUM; i++) macbb_backup[i] = rd32(h, backup_macbb_reg[i]);
    page_c(h);
    for (int i = 0; i < AFE_NUM; i++)   afe_backup[i]   = rd32(h, backup_afe_reg[i]);
    page_c(h);
    for (int i = 0; i < RF_NUM; i++)    rfa_backup[i]   = rtl8821a_rf_read(h, backup_rf_reg[i], RFREG_MASK);

    iqk_configure_mac(h);
    iqk_8821a(h, b, band5g, verbose);

    page_c(h);
    for (int i = 0; i < RF_NUM; i++) rf(h, backup_rf_reg[i], RFREG_MASK, rfa_backup[i]);
    iqk_restore_afe(h, afe_backup);
    page_c(h);
    for (int i = 0; i < MACBB_NUM; i++) wr32(h, backup_macbb_reg[i], macbb_backup[i]);

    if (verbose) printf("[8821au] iqk done (band %s), registers restored\n", band5g ? "5G" : "2G");
    return 0;
}

int rtl8821au_lck(libusb_device_handle *h, int verbose) {
    (void)h;
    if (verbose) printf("[8821au] lck: rtw88 performs no LC calibration on the 8821A, nothing to do\n");
    return 0;
}
