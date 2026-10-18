/*
 * rtl8821au_phy — baseband, RF and channel control for the RTL8821AU.
 *
 * Ported from Linux rtw88 (GPL-2.0 OR BSD-3-Clause, (c) Realtek):
 *   phy.c      rtw_parse_tbl_phy_cond, check_positive, rtw_phy_setup_phy_cond,
 *              rtw_phy_cfg_bb/agc/rf, rtw_phy_write_rf_reg_sipi
 *   rtw88xxa.c rtw88xxa_phy_bb_config, rtw88xxa_phy_rf_config,
 *              rtw88xxa_phy_read_rf, rtw88xxa_switch_band (8821A branch),
 *              rtw8821a_set_ext_band_switch, rtw8821a_phy_set_rfe_reg_24g/5g,
 *              rtw88xxa_set_channel_bb_swing, rtw88xxa_switch_channel,
 *              rtw88xxa_post_set_bw_mode, rtw88xxa_set_channel_rf,
 *              rtw88xxa_read_efuse (field offsets), the post-RF MAC writes
 *              at the end of rtw88xxa_power_on
 * Scope: 1T1R, 20 MHz. 40/80 MHz paths are carried where cheap but untested.
 */
#include "rtl8821au.h"
#include "rtl_usb.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BIT(n) (1u << (n))
#define GENMASK(h, l) ((0xFFFFFFFFu >> (31 - (h))) & (0xFFFFFFFFu << (l)))

/* registers (rtw88 reg.h) */
#define REG_AFE_CTRL3        0x002C
#define REG_LED_CFG          0x004C
#define REG_WL_BT_PWR_CTRL   0x0068
#define REG_RF_B_CTRL        0x0076
#define REG_SYS_CFG1         0x00F0
#define REG_TXPKT_EMPTY      0x041A
#define REG_HWSEQ_CTRL       0x0423
#define REG_RRSR             0x0440
#define REG_CCK_CHECK        0x0454
#define REG_DATA_SC          0x0483
#define REG_QUEUE_CTRL       0x04C6
#define REG_BAR_MODE_CTRL    0x04CC
#define REG_TX_RPT_TIME      0x04F0
#define REG_NAV_CTRL         0x0650
#define REG_WMAC_TRXPTCL_CTL 0x0668
#define REG_GPIO_MUXCFG      0x0040
#define REG_SYS_SDIO_CTRL    0x0070
#define REG_ACLK_MON         0x003E
#define REG_EARLY_MODE_CTRL  0x02BC
#define REG_FWHW_TXQ_CTRL    0x0420
#define REG_USB_HRPWM        0xFE58
#define RTW_SEC_CMD_REG      0x0670
#define REG_RXPSEL           0x0808
#define REG_TXPSEL           0x080C
#define REG_BWINDICATION     0x0834
#define REG_CCA2ND           0x0838
#define REG_L1PKTH           0x0848
#define REG_CLKTRK           0x0860
#define REG_ADCCLK           0x08AC
#define REG_HSSI_READ        0x08B0
#define REG_ADC160           0x08C4
#define REG_RXSB             0x0A00
#define REG_CCK_RX           0x0A04
#define REG_3WIRE_SWA        0x0C00
#define REG_TXSCALE_A        0x0C1C
#define REG_LSSI_WRITE_A     0x0C90
#define REG_RFE_PINMUX_A     0x0CB0
#define REG_RFE_INV_A        0x0CB4
#define REG_PI_READ_A        0x0D04
#define REG_SI_READ_A        0x0D08
#define REG_TXSCALE_B        0x0E1C

#define REG_RF_CTRL          0x001F
#define BIT_RF_EN            BIT(0)
#define BIT_RF_RSTB          BIT(1)
#define BIT_RF_SDM_RSTB      BIT(2)
#define BIT_FEN_BB_RSTB      BIT(0)
#define BIT_FEN_BB_GLB_RST   BIT(1)
#define BIT_FEN_USBA         BIT(2)
#define BIT_RX_PSEL_RST      (BIT(28) | BIT(29))
#define BIT_CHECK_CCK_EN     BIT(7)
#define BB_SWING_MASK        GENMASK(31, 21)
#define BIT_DPDT_SEL_EN      BIT(23)
#define BIT_DPDT_WL_SEL      BIT(24)
#define BIT_RFMOD            (BIT(7) | BIT(8))
#define BIT_RFMOD_80M        BIT(8)
#define BIT_RFMOD_40M        BIT(7)

#define RF_CFGCH             0x18
#define RF18_BAND_MASK       (BIT(16) | BIT(9) | BIT(8))
#define RF18_CHANNEL_MASK    0xFF
#define RF18_RFSI_MASK       (BIT(18) | BIT(17))
#define RF18_BW_MASK         (BIT(11) | BIT(10))
#define RFREG_MASK           0xFFFFF

/* rtw_phy_cond.intf */
#define INTF_PCIE BIT(0)
#define INTF_USB  BIT(1)
#define INTF_SDIO BIT(2)

/* efuse offsets, struct rtw88xxa_efuse */
#define EF_XTAL_K        0xB9
#define EF_THERMAL       0xBA
#define EF_PA_TYPE       0xBC
#define EF_LNA_TYPE_2G   0xBD
#define EF_LNA_TYPE_5G   0xBF
#define EF_RF_BOARD_OPT  0xC1
#define EF_RF_BT_SETTING 0xC3
#define EF_TX_BB_SWING2G 0xC6
#define EF_TX_BB_SWING5G 0xC7
#define EF_RF_ANT_OPT    0xC9
#define EF_RFE_OPTION    0xCA
#define EF_MAC_8821AU    0x107

/* ---- small register helpers --------------------------------------------- */
static uint32_t ffs32(uint32_t m) { uint32_t s = 0; while (s < 32 && !(m & (1u << s))) s++; return s; }
static uint32_t rd_mask(libusb_device_handle *h, uint16_t r, uint32_t mask) {
    return (rtl_read32(h, r, NULL) & mask) >> ffs32(mask);
}
static int wr_mask(libusb_device_handle *h, uint16_t r, uint32_t mask, uint32_t val) {
    /* rtw_write32_mask */
    uint32_t v = rtl_read32(h, r, NULL);
    return rtl_write32(h, r, (v & ~mask) | ((val << ffs32(mask)) & mask));
}
static void set8 (libusb_device_handle *h, uint16_t r, uint8_t b)  { rtl_write8(h, r, rtl_read8(h, r, NULL) | b); }
static void clr8 (libusb_device_handle *h, uint16_t r, uint8_t b)  { rtl_write8(h, r, rtl_read8(h, r, NULL) & (uint8_t)~b); }
static void set32(libusb_device_handle *h, uint16_t r, uint32_t b) { rtl_write32(h, r, rtl_read32(h, r, NULL) | b); }
static void clr32(libusb_device_handle *h, uint16_t r, uint32_t b) { rtl_write32(h, r, rtl_read32(h, r, NULL) & ~b); }

/* ---- board info from efuse ---------------------------------------------- */
static rtl8821a_board g_board;

const rtl8821a_board *rtl8821a_board_info(libusb_device_handle *h, int verbose) {
    if (g_board.valid) return &g_board;

    uint8_t map[EFUSE_MAP_LEN];
    int rc = rtl_efuse_read_map(h, map, EFUSE_MAP_LEN);
    if (rc) { printf("[8821au] efuse read failed (%d), using defaults\n", rc); memset(map, 0xff, sizeof(map)); }

    rtl8821a_board *b = &g_board;
    memset(b, 0, sizeof(*b));
    b->cut              = (uint8_t)((rtl_read32(h, REG_SYS_CFG1, NULL) >> 12) & 0xF);
    b->xtal_k           = map[EF_XTAL_K] == 0xff ? 0x20 : map[EF_XTAL_K];
    b->thermal          = map[EF_THERMAL];
    b->pa_type          = map[EF_PA_TYPE]     == 0xff ? 0 : map[EF_PA_TYPE];
    b->lna_type_2g      = map[EF_LNA_TYPE_2G] == 0xff ? 0 : map[EF_LNA_TYPE_2G];
    b->lna_type_5g      = map[EF_LNA_TYPE_5G] == 0xff ? 0 : map[EF_LNA_TYPE_5G];
    b->rf_board_option  = map[EF_RF_BOARD_OPT];
    b->rf_bt_setting    = map[EF_RF_BT_SETTING];
    b->tx_bb_swing_2g   = map[EF_TX_BB_SWING2G];
    b->tx_bb_swing_5g   = map[EF_TX_BB_SWING5G];
    b->rf_antenna_option= map[EF_RF_ANT_OPT];
    b->rfe_option       = map[EF_RFE_OPTION];
    memcpy(b->mac, &map[EF_MAC_8821AU], 6);

    /* rtw_chip_efuse_info_setup (generic, main.c) */
    b->ext_pa_2g  = (b->pa_type     & BIT(4)) ? 1 : 0;
    b->ext_lna_2g = (b->lna_type_2g & BIT(3)) ? 1 : 0;
    b->ext_pa_5g  = (b->pa_type     & BIT(0)) ? 1 : 0;
    b->ext_lna_5g = (b->lna_type_5g & BIT(3)) ? 1 : 0;
    b->btcoex     = (int)((rtl_read32(h, REG_WL_BT_PWR_CTRL, NULL) >> 18) & 1);
    b->ant_div_cfg = 0;
    b->phy_rfe = (uint8_t)(b->ext_lna_2g | (b->ext_pa_2g << 1) | (b->ext_lna_5g << 2) |
                           (b->ext_pa_5g << 3) | (b->btcoex << 4));
    b->valid = 1;

    if (verbose)
        printf("[8821au] board: cut=%u xtal=0x%02x thermal=0x%02x pa=0x%02x lna2g=0x%02x lna5g=0x%02x "
               "board_opt=0x%02x rfe_opt=0x%02x bbswing2g=0x%02x btcoex=%d ext(pa2g=%u lna2g=%u pa5g=%u lna5g=%u) "
               "phy_rfe=0x%02x mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
               b->cut, b->xtal_k, b->thermal, b->pa_type, b->lna_type_2g, b->lna_type_5g,
               b->rf_board_option, b->rfe_option, b->tx_bb_swing_2g, b->btcoex,
               b->ext_pa_2g, b->ext_lna_2g, b->ext_pa_5g, b->ext_lna_5g, b->phy_rfe,
               b->mac[0], b->mac[1], b->mac[2], b->mac[3], b->mac[4], b->mac[5]);
    return b;
}

/* ---- RF path A access ---------------------------------------------------- */
uint32_t rtl8821a_rf_read(libusb_device_handle *h, uint8_t addr, uint32_t mask) {
    /* rtw88xxa_phy_read_rf: no CCA toggle on 8821A, 20 us settle */
    int pi_mode = (int)rd_mask(h, REG_3WIRE_SWA, 0x4);
    wr_mask(h, REG_HSSI_READ, 0xFF, addr);
    usleep(20);
    return rd_mask(h, pi_mode ? REG_PI_READ_A : REG_SI_READ_A, mask);
}

int rtl8821a_rf_write(libusb_device_handle *h, uint8_t addr, uint32_t mask, uint32_t data) {
    /* rtw_phy_write_rf_reg_sipi: note data is NOT re-masked, as in rtw88 */
    mask &= RFREG_MASK;
    if (mask != RFREG_MASK) {
        uint32_t old = rtl8821a_rf_read(h, addr, RFREG_MASK);
        data = (old & ~mask) | (data << ffs32(mask));
    }
    uint32_t v = (((uint32_t)addr << 20) | (data & 0x000FFFFF)) & 0x0FFFFFFF;
    int rc = rtl_write32(h, REG_LSSI_WRITE_A, v);
    usleep(13);
    return rc;
}

/* ---- conditional table parser (rtw_parse_tbl_phy_cond) ------------------ */
typedef void (*cfg_fn)(libusb_device_handle *h, uint32_t addr, uint32_t data);

static int check_positive(const rtl8821a_board *b, uint32_t cond, uint32_t cond2) {
    uint32_t rfe  = cond & 0xFF;
    uint32_t intf = (cond >> 8)  & 0xF;
    uint32_t pkg  = (cond >> 12) & 0xF;
    uint32_t cut  = (cond >> 24) & 0xF;
    uint32_t drv_cut = b->cut ? b->cut : 15;
    uint32_t drv_pkg = 15;                 /* pkg = 0 -> 15 in rtw_phy_setup_phy_cond */

    if (cut && cut != drv_cut) return 0;
    if (pkg && pkg != drv_pkg) return 0;
    if (intf && intf != INTF_USB) return 0;

    /* 8812A/8821A branch */
    if (!(rfe & 0x0F)) return 1;
    if ((rfe & b->phy_rfe) != rfe) return 0;
    /* cond2 types: our efuse path leaves glna/gpa/alna/apa = 0 (no
     * rtw8812a_read_amplifier_type on 8821A), so compare against 0 */
    if ((rfe & BIT(0)) && ((cond2 >> 0)  & 0xFF) != 0) return 0;
    if ((rfe & BIT(1)) && ((cond2 >> 8)  & 0xFF) != 0) return 0;
    if ((rfe & BIT(2)) && ((cond2 >> 16) & 0xFF) != 0) return 0;
    if ((rfe & BIT(3)) && ((cond2 >> 24) & 0xFF) != 0) return 0;
    return 1;
}

static int load_table(libusb_device_handle *h, const rtl8821a_board *b,
                      const uint32_t *tbl, unsigned len, cfg_fn cfg, const char *name, int verbose) {
    uint32_t pos_cond = 0, pos_cond2 = 0;
    int is_matched = 1, is_skipped = 0, writes = 0;

    for (unsigned i = 0; i + 1 < len; i += 2) {
        uint32_t a = tbl[i], d = tbl[i + 1];
        if (a & BIT(31)) {                              /* pos */
            switch ((a >> 28) & 3) {                    /* branch */
                case 3: is_matched = 1; is_skipped = 0; break;              /* ENDIF */
                case 2: is_matched = is_skipped ? 0 : 1; break;             /* ELSE */
                default: pos_cond = a; pos_cond2 = d; break;                /* IF / ELIF */
            }
        } else if (a & BIT(30)) {                       /* neg */
            if (!is_skipped) {
                if (check_positive(b, pos_cond, pos_cond2)) { is_matched = 1; is_skipped = 1; }
                else                                        { is_matched = 0; is_skipped = 0; }
            } else {
                is_matched = 0;
            }
        } else if (is_matched) {
            cfg(h, a, d);
            writes++;
        }
    }
    if (verbose) printf("[8821au] %s: %d writes of %u entries\n", name, writes, len / 2);
    return 0;
}

static void cfg_bb(libusb_device_handle *h, uint32_t addr, uint32_t data) {
    if      (addr == 0xfe) usleep(50000);
    else if (addr == 0xfd) usleep(5000);
    else if (addr == 0xfc) usleep(1000);
    else if (addr == 0xfb) usleep(50);
    else if (addr == 0xfa) usleep(5);
    else if (addr == 0xf9) usleep(1);
    else rtl_write32(h, (uint16_t)addr, data);
}
static void cfg_agc(libusb_device_handle *h, uint32_t addr, uint32_t data) {
    rtl_write32(h, (uint16_t)addr, data);
}
static void cfg_rf_a(libusb_device_handle *h, uint32_t addr, uint32_t data) {
    if      (addr == 0xffe) usleep(50000);
    else if (addr == 0xfe)  usleep(100);
    else { rtl8821a_rf_write(h, (uint8_t)addr, RFREG_MASK, data); usleep(1); }
}

/* ---- BB init (rtw88xxa_phy_bb_config) ----------------------------------- */
int rtl8821au_bb_init(libusb_device_handle *h, int verbose) {
    const rtl8821a_board *b = rtl8821a_board_info(h, verbose);

    uint8_t v = rtl_read8(h, REG_SYS_FUNC_EN, NULL);
    v |= BIT_FEN_USBA;
    rtl_write8(h, REG_SYS_FUNC_EN, v);                 /* power on BB/RF domain */
    v |= BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST;
    rtl_write8(h, REG_SYS_FUNC_EN, v);                 /* toggle BB reset */

    rtl_write8(h, REG_RF_CTRL,   BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB);
    rtl_write8(h, REG_RF_B_CTRL, BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB);

    load_table(h, b, rtw8821a_bb,  rtw8821a_bb_len,  cfg_bb,  "bb table",  verbose);
    load_table(h, b, rtw8821a_agc, rtw8821a_agc_len, cfg_agc, "agc table", verbose);

    uint32_t cap = b->xtal_k & 0x3F;
    wr_mask(h, REG_AFE_CTRL3, 0x00FFF000, cap | (cap << 6));   /* 8821A mask */

    if (verbose) printf("[8821au] bb init done: SYS_FUNC_EN=0x%02x RF_CTRL=0x%02x 0x800=0x%08x\n",
                        rtl_read8(h, REG_SYS_FUNC_EN, NULL), rtl_read8(h, REG_RF_CTRL, NULL),
                        rtl_read32(h, 0x800, NULL));
    return 0;
}

/* ---- band / channel ------------------------------------------------------ */
static void set_channel_bb_swing(libusb_device_handle *h, const rtl8821a_board *b, int band5g) {
    static const uint32_t swing2setting[4] = {0x200, 0x16a, 0x101, 0x0b6};
    uint8_t s = band5g ? b->tx_bb_swing_5g : b->tx_bb_swing_2g;
    uint32_t a = swing2setting[s & 0x3];
    uint32_t bb = swing2setting[(s >> 2) & 0x3];
    wr_mask(h, REG_TXSCALE_A, BB_SWING_MASK, a);
    wr_mask(h, REG_TXSCALE_B, BB_SWING_MASK, bb);
}

static void set_ext_band_switch_8821a(libusb_device_handle *h, int band5g) {
    wr_mask(h, REG_LED_CFG, BIT_DPDT_SEL_EN, 0);
    wr_mask(h, REG_LED_CFG, BIT_DPDT_WL_SEL, 1);
    wr_mask(h, REG_RFE_INV_A, 0xf, 7);
    wr_mask(h, REG_RFE_INV_A, 0xf0, 7);
    wr_mask(h, REG_RFE_INV_A, BIT(29) | BIT(28), band5g ? 2 : 1);
}

static void set_rfe_reg_24g_8821a(libusb_device_handle *h, const rtl8821a_board *b) {
    wr_mask(h, REG_RFE_PINMUX_A, 0xF000, 0x7);     /* LNA_On */
    wr_mask(h, REG_RFE_PINMUX_A, 0xF0, 0x7);       /* PAPE_A */
    if (b->ext_lna_2g) {
        wr_mask(h, REG_RFE_INV_A, BIT(20), 1);
        wr_mask(h, REG_RFE_INV_A, BIT(22), 0);
        wr_mask(h, REG_RFE_PINMUX_A, GENMASK(2, 0), 0x2);
        wr_mask(h, REG_RFE_PINMUX_A, GENMASK(10, 8), 0x2);
    } else {
        wr_mask(h, REG_RFE_INV_A, BIT(20), 0);
        wr_mask(h, REG_RFE_INV_A, BIT(22), 0);
        wr_mask(h, REG_RFE_PINMUX_A, GENMASK(2, 0), 0x7);
        wr_mask(h, REG_RFE_PINMUX_A, GENMASK(10, 8), 0x7);
    }
}

static void set_rfe_reg_5g_8821a(libusb_device_handle *h) {
    wr_mask(h, REG_RFE_PINMUX_A, 0xF000, 0x5);
    wr_mask(h, REG_RFE_PINMUX_A, 0xF0, 0x4);
    wr_mask(h, REG_RFE_INV_A, BIT(20), 0);
    wr_mask(h, REG_RFE_INV_A, BIT(22), 0);
    wr_mask(h, REG_RFE_PINMUX_A, GENMASK(2, 0), 0x7);
    wr_mask(h, REG_RFE_PINMUX_A, GENMASK(10, 8), 0x7);
}

/* rtw88xxa_switch_band, 8821A branch. DESC_RATE: 1M=0 2M=1 5.5M=2 11M=3 6M=4 12M=6 24M=8 */
static void switch_band(libusb_device_handle *h, const rtl8821a_board *b, int band5g) {
    if (!b->btcoex && b->ant_div_cfg == 0)
        set_ext_band_switch_8821a(h, band5g);

    if (!band5g) {
        set32(h, REG_RXPSEL, BIT_RX_PSEL_RST);
        set_rfe_reg_24g_8821a(h, b);
        wr_mask(h, REG_TXSCALE_A, 0xf00, 0);
        wr_mask(h, REG_TXPSEL, 0xf0, 0x1);
        wr_mask(h, REG_CCK_RX, 0x0f000000, 0x1);
        wr_mask(h, REG_RRSR, 0xfffff, BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(4) | BIT(6) | BIT(8));
        clr8(h, REG_CCK_CHECK, BIT_CHECK_CCK_EN);
    } else {
        set_rfe_reg_5g_8821a(h);
        set8(h, REG_CCK_CHECK, BIT_CHECK_CCK_EN);
        for (int i = 0; i < 50; i++) {                       /* wait tx queues empty */
            if ((rtl_read16(h, REG_TXPKT_EMPTY, NULL) & 0x30) == 0x30) break;
            usleep(50);
        }
        set32(h, REG_RXPSEL, BIT_RX_PSEL_RST);
        wr_mask(h, REG_TXSCALE_A, 0xf00, 1);
        wr_mask(h, REG_TXPSEL, 0xf0, 0);
        wr_mask(h, REG_CCK_RX, 0x0f000000, 0xf);
        wr_mask(h, REG_RRSR, 0xfffff, BIT(4) | BIT(6) | BIT(8));
    }
    set_channel_bb_swing(h, b, band5g);
}

static void switch_channel(libusb_device_handle *h, int ch) {
    uint32_t fc_area, rf_mod_ag;
    if      (ch >= 36  && ch <= 48)  fc_area = 0x494;
    else if (ch >= 50  && ch <= 64)  fc_area = 0x453;
    else if (ch >= 100 && ch <= 116) fc_area = 0x452;
    else if (ch >= 118)              fc_area = 0x412;
    else                             fc_area = 0x96a;
    wr_mask(h, REG_CLKTRK, 0x1ffe0000, fc_area);

    if      (ch >= 36  && ch <= 64)  rf_mod_ag = 0x101;
    else if (ch >= 100 && ch <= 140) rf_mod_ag = 0x301;
    else if (ch > 140)               rf_mod_ag = 0x501;
    else                             rf_mod_ag = 0x000;
    rtl8821a_rf_write(h, RF_CFGCH, RF18_RFSI_MASK | RF18_BAND_MASK, rf_mod_ag);
    rtl8821a_rf_write(h, RF_CFGCH, RF18_CHANNEL_MASK, (uint32_t)ch);
}

static void post_set_bw_mode(libusb_device_handle *h, int bw) {
    uint16_t v16 = rtl_read16(h, REG_WMAC_TRXPTCL_CTL, NULL) & (uint16_t)~BIT_RFMOD;
    if (bw == 2) v16 |= BIT_RFMOD_80M; else if (bw == 1) v16 |= BIT_RFMOD_40M;
    rtl_write16(h, REG_WMAC_TRXPTCL_CTL, v16);

    uint8_t txsc = 0;                                   /* primary channel idx 0 */
    rtl_write8(h, REG_DATA_SC, txsc);

    switch (bw) {
    default:
    case 0:   /* 20 MHz, 1T1R */
        wr_mask(h, REG_ADCCLK, 0x003003C3, 0x00300200);
        wr_mask(h, REG_ADC160, BIT(30), 0);
        wr_mask(h, REG_L1PKTH, 0x03C00000, 8);
        break;
    case 1:   /* 40 MHz: untested */
        wr_mask(h, REG_ADCCLK, 0x003003C3, 0x00300201);
        wr_mask(h, REG_ADC160, BIT(30), 0);
        wr_mask(h, REG_ADCCLK, 0x3C, txsc);
        wr_mask(h, REG_CCA2ND, 0xf0000000, txsc);
        wr_mask(h, REG_L1PKTH, 0x03C00000, (rtl_read8(h, REG_BWINDICATION + 3, NULL) & BIT(2)) ? 6 : 8);
        clr32(h, REG_RXSB, BIT(4));
        break;
    }
}

static void set_channel_rf(libusb_device_handle *h, int bw) {
    uint32_t v = (bw == 2) ? 0 : (bw == 1) ? 1 : 3;
    rtl8821a_rf_write(h, RF_CFGCH, RF18_BW_MASK, v);
}

/* ops: set_channel (rtw88xxa_set_channel). bw: 0=20 1=40 2=80 */
int rtl8821au_set_channel(libusb_device_handle *h, int ch, int bw) {
    if (ch < 1 || ch > 177) return 1;
    const rtl8821a_board *b = rtl8821a_board_info(h, 0);

    int old_5g = (rtl_read8(h, REG_CCK_CHECK, NULL) & BIT_CHECK_CCK_EN) ? 1 : 0;
    int new_5g = (ch > 14) ? 1 : 0;
    if (new_5g != old_5g) switch_band(h, b, new_5g);

    switch_channel(h, ch);
    post_set_bw_mode(h, bw);
    set_channel_rf(h, bw);
    return 0;
}

/* ---- RF init (rtw88xxa_phy_rf_config + the rest of rtw88xxa_power_on) --- */
int rtl8821au_rf_init(libusb_device_handle *h, int verbose) {
    const rtl8821a_board *b = rtl8821a_board_info(h, verbose);

    load_table(h, b, rtw8821a_rf_a, rtw8821a_rf_a_len, cfg_rf_a, "rf path A table", verbose);

    if (verbose) printf("[8821au] RF_A[0x00]=0x%05x RF_A[0x18]=0x%05x\n",
                        rtl8821a_rf_read(h, 0x00, RFREG_MASK), rtl8821a_rf_read(h, 0x18, RFREG_MASK));

    /* rtw88 switches to 2G/20MHz right after RF config, unconditionally */
    switch_band(h, b, 0);

    /* post-RF MAC bits from the end of rtw88xxa_power_on */
    rtl_write32(h, RTW_SEC_CMD_REG, BIT(31) | BIT(30));
    rtl_write8(h, REG_HWSEQ_CTRL, 0xff);
    rtl_write32(h, REG_BAR_MODE_CTRL, 0x0201ffff);
    rtl_write8(h, REG_NAV_CTRL + 2, 0);
    clr8(h, REG_GPIO_MUXCFG, BIT(5));
    /* rtw_phy_init / pwrtrack_init: driver-side state only */
    clr8(h, REG_QUEUE_CTRL, BIT(3));                   /* RTS BW follows CCA */
    rtl_write8(h, REG_FWHW_TXQ_CTRL + 1, 0x0f);        /* enable Tx report */
    rtl_write8(h, REG_EARLY_MODE_CTRL + 3, 0x01);      /* Pretx_en */
    rtl_write16(h, REG_TX_RPT_TIME, 0x3df0);
    rtl_write8(h, REG_SYS_SDIO_CTRL, 0x0);             /* reset USB mode switch */
    rtl_write8(h, REG_ACLK_MON, 0x0);
    rtl_write8(h, REG_USB_HRPWM, 0);
    set32(h, REG_FWHW_TXQ_CTRL, BIT(12));              /* ack for xmit mgmt frames */

    if (verbose) printf("[8821au] rf init done, band 2G\n");
    return 0;
}
