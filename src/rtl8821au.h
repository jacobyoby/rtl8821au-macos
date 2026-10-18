/*
 * rtl8821au — internal interface between rtl8821au.c (ops, MAC) and
 * rtl8821au_phy.c (BB/RF/channel). Reference: Linux rtw88 rtw88xxa.c.
 */
#ifndef RTL8821AU_H
#define RTL8821AU_H

#include <stdint.h>
#include <libusb.h>

/* Board info decoded from the logical efuse map (struct rtw88xxa_efuse). */
typedef struct {
    int     valid;
    uint8_t cut;                 /* SYS_CFG[15:12]: 0=A 1=B 2=C ... */
    uint8_t xtal_k;              /* 0xb9, crystal cap (0xff -> 0x20) */
    uint8_t thermal;             /* 0xba */
    uint8_t pa_type;             /* 0xbc */
    uint8_t lna_type_2g;         /* 0xbd */
    uint8_t lna_type_5g;         /* 0xbf */
    uint8_t rf_board_option;     /* 0xc1 */
    uint8_t rf_bt_setting;       /* 0xc3 */
    uint8_t tx_bb_swing_2g;      /* 0xc6 */
    uint8_t tx_bb_swing_5g;      /* 0xc7 */
    uint8_t rf_antenna_option;   /* 0xc9 */
    uint8_t rfe_option;          /* 0xca */
    uint8_t mac[6];              /* 0x107 (8821AU layout) */
    uint8_t ext_pa_2g, ext_lna_2g, ext_pa_5g, ext_lna_5g;
    int     btcoex;              /* REG_WL_BT_PWR_CTRL[18] */
    uint8_t ant_div_cfg;         /* 0 (disabled in vendor driver) */
    uint8_t phy_rfe;             /* rtw_phy_cond.rfe as built by rtw_phy_setup_phy_cond */
    uint8_t pg2g[12];            /* 0x10..0x1b: cck_base[6], bw40_base[5], ht_1s_diff */
    uint8_t country[2];          /* 0xcb, 0xcc */
    int     regd;                /* 0 FCC, 1 MKK, 2 ETSI, -1 worldwide */
} rtl8821a_board;

/* Reads (once, cached) the efuse map and chip version. Needs power-on. */
const rtl8821a_board *rtl8821a_board_info(libusb_device_handle *h, int verbose);

/* RF path A register access (rtw88xxa_phy_read_rf / rtw_phy_write_rf_reg_sipi). */
uint32_t rtl8821a_rf_read (libusb_device_handle *h, uint8_t addr, uint32_t mask);
int      rtl8821a_rf_write(libusb_device_handle *h, uint8_t addr, uint32_t mask, uint32_t data);

/* rtl8821au_txpwr.c */
int  rtl8821au_txpwr_apply(libusb_device_handle *h, int ch, int verbose);
void rtl8821au_txpwr_readback(libusb_device_handle *h, const char *label);

/* rtl8821au_cal.c */
int rtl8821au_iqk(libusb_device_handle *h, int verbose);
int rtl8821au_lck(libusb_device_handle *h, int verbose);   /* no-op on 8821A */
int rtl8821au_dig_set(libusb_device_handle *h, uint8_t igi);
uint8_t rtl8821au_dig_get(libusb_device_handle *h);
int rtl8821au_cck_pd_set(libusb_device_handle *h, int lvl);  /* 0..4 */

/* ops entries implemented in rtl8821au_phy.c */
int rtl8821au_bb_init    (libusb_device_handle *h, int verbose);
int rtl8821au_rf_init    (libusb_device_handle *h, int verbose);
int rtl8821au_set_channel(libusb_device_handle *h, int ch, int bw);

/* generated tables (rtl8821au_tables.c) */
extern const uint32_t rtw8821a_agc[];  extern const unsigned rtw8821a_agc_len;
extern const uint32_t rtw8821a_bb[];   extern const unsigned rtw8821a_bb_len;
extern const uint32_t rtw8821a_rf_a[]; extern const unsigned rtw8821a_rf_a_len;

#endif /* RTL8821AU_H */
