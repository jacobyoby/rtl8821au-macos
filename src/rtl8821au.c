/*
 * rtl8821au — chip ops for the RTL8821AU / RTL8811AU (1x1 802.11ac, USB 2.0).
 *
 * Reference: Linux rtw88 (GPL-2.0 OR BSD-3-Clause, (c) Realtek):
 *   drivers/net/wireless/realtek/rtw88/rtw8821a.c        (chip spec, ops)
 *   drivers/net/wireless/realtek/rtw88/rtw8821a_table.c  (power sequences, tables)
 *   drivers/net/wireless/realtek/rtw88/rtw88xxa.c        (code shared with 8812A)
 *
 * PORT STATUS
 *   [x] USB ID match                 rtl_ids.c
 *   [x] hw reset (lightweight deinit) rtw88xxau_hw_reset
 *   [x] power-on sequence            card_enable_flow_8821a, USB steps only
 *   [x] CR enable + LDO tweak        rtw88xxau_init_power_on
 *   [x] firmware download            rtw8821a_fw.bin (fw_8821a_nic.c), same
 *                                    8051 FWDL procedure as the 8812A
 *                                    (verified on hardware: checksum OK,
 *                                    WINTINI_RDY, TP-Link Archer T2U Plus)
 *   [x] endpoint map                 usbprobe on 2357:0120 (USB 2.0 high speed):
 *                                    bulk IN 0x84, bulk OUT 0x05 0x06 0x08 0x09,
 *                                    interrupt IN 0x87
 *   [x] LLT + TX DMA drop            rtw88xxa_llt_init (in power_on, as rtw88)
 *   [x] MAC init                     rtw8821a_mac_tbl + rtw88xxa_power_on tail
 *                                    (queue pages, boundaries, priorities,
 *                                    WMAC, EDCA, beacon, USB burst, ARFB,
 *                                    MACTXEN|MACRXEN)
 *   [x] monitor RCR                  rtl_mac_set_monitor (same registers)
 *   [x] BB init                      rtl8821au_phy.c: bb + agc tables with the
 *                                    rtw88 condition parser, crystal cap
 *   [x] RF init                      rf path A table, switch_band(2G), post-RF
 *                                    MAC bits
 *   [x] channel / bandwidth          rtw88xxa_set_channel, 20 MHz verified path
 *   [x] IQK                          rtl8821au_cal.c (rtw8821a_do_iqk, 20 MHz)
 *   [x] LCK                          none on 8821A in rtw88 (no-op)
 *   [x] initial gain                 rtl8821au_dig_set, env RTL8821AU_IGI to override
 *   [x] TX power from efuse          rtl8821au_txpwr.c: 2.4 GHz path A, CCK/OFDM/
 *                                    HT1S/VHT1S rows, capped by the rtw88 limit
 *                                    table (FCC/MKK/ETSI by efuse country, else min)
 *   [ ] TX descriptor                verify 88xxA TX desc for the 1x1 part
 *   [ ] tools use ops->tx_ep_mgmt    associate/alfa_netd/inject still pass the
 *                                    8812AU macro RTL_TX_EP_MGMT (0x02) directly
 *
 * Unported steps return RTL_ENOSYS so rtl_hal stops with a clear message.
 */
#include "rtl_chip.h"
#include "rtl_usb.h"
#include "rtl_tx.h"
#include "rtl_mac.h"
#include "rtl8821au.h"
#include <stdio.h>
#include <unistd.h>

#define BIT(n) (1u << (n))
#define RTL_ENOSYS (-38)   /* ENOSYS: step not ported yet */

/* Registers (rtw88 reg.h). The ones in rtl_usb.h are reused. */
#define REG_APS_FSMCO          0x0004   /* +1 = 0x0005 */
#define REG_RF_CTRL            0x001F
#define REG_LDO_SWR_CTRL       0x007C
#define REG_SYS_CFG1           0x00F0   /* +3 = 0x00F3 */
#define REG_CR                 0x0100
#define REG_FPGA0_XCD_RF_PARA  0x08B4

/* LLT / TX DMA */
#define REG_LLT_INIT           0x01E0
#define LLT_WRITE_ACCESS       BIT(30)
#define REG_TXDMA_OFFSET_CHK   0x020C
#define DROP_DATA_EN           BIT(9)

/* MAC init (rtw88xxa_power_on tail) */
#define REG_HMETFR             0x01CC
#define REG_TXDMA_PQ_MAP       0x010C
#define REG_TRXFF_BNDY         0x0114
#define REG_RQPN               0x0200
#define REG_DWBCN0_CTRL        0x0208
#define REG_RQPN_NPQ           0x0214
#define REG_DWBCN1_CTRL        0x0228
#define REG_RXDMA_AGG_PG_TH    0x0280
#define REG_RXDMA_STATUS       0x0288
#define REG_RXDMA_MODE         0x0290
#define REG_FWHW_TXQ_CTRL      0x0420
#define REG_BCNQ_BDNY          0x0424
#define REG_MGQ_BDNY           0x0425
#define REG_SPEC_SIFS          0x0428
#define REG_RETRY_LIMIT        0x042A
#define REG_RRSR               0x0440
#define REG_ARFR0              0x0444
#define REG_ARFRH0             0x0448
#define REG_ARFR1_V1           0x044C
#define REG_ARFRH1_V1          0x0450
#define REG_AMPDU_MAX_TIME     0x0456
#define REG_AMPDU_MAX_LENGTH   0x0458
#define REG_WMAC_LBK_BF_HD     0x045D
#define REG_FAST_EDCA_CTRL     0x0460
#define REG_ARFR2_V1           0x048C
#define REG_ARFRH2_V1          0x0490
#define REG_ARFR3_V1           0x0494
#define REG_ARFRH3_V1          0x0498
#define REG_SINGLE_AMPDU_CTRL  0x04C7
#define REG_MAX_AGGR_NUM       0x04CA
#define REG_EDCA_VO_PARAM      0x0500
#define REG_EDCA_VI_PARAM      0x0504
#define REG_EDCA_BE_PARAM      0x0508
#define REG_EDCA_BK_PARAM      0x050C
#define REG_BCNTCFG            0x0510
#define REG_PIFS               0x0512
#define REG_SIFS               0x0514
#define REG_TBTT_PROHIBIT      0x0540
#define REG_BCN_CTRL           0x0550
#define REG_DRVERLYINT         0x0558
#define REG_BCNDMATIM          0x0559
#define REG_USTIME_TSF         0x055C
#define REG_BCN_MAX_ERR        0x055D
#define REG_HIQ_NO_LMT_EN      0x05A7
#define REG_RX_PKT_LIMIT       0x060C
#define REG_RX_DRVINFO_SZ      0x060F
#define REG_MAR                0x0620
#define REG_USTIME_EDCA        0x0638
#define REG_MAC_SPEC_SIFS      0x063A
#define REG_ACKTO              0x0640
#define REG_RXFLTMAP0          0x06A0
#define REG_RXFLTMAP1          0x06A2
#define REG_RXFLTMAP2          0x06A4
#define REG_HIMR0              0x00B0
#define REG_HIMR1              0x00B8
#define REG_WL_BT_PWR_CTRL     0x0068
#define REG_USB_MOD            0xF008
#define REG_USB3_RXITV         0xF050

/* rtw8821a_hw_spec FIFO geometry (rtw_set_trx_fifo_info, 8051 wcpu) */
#define TXFF_SIZE_8821A        65536
#define RXFF_SIZE_8821A        16128
#define PAGE_SIZE_8821A        256
#define RSVD_DRV_PG_NUM_8821A  8
#define TXFF_PG_NUM_8821A      (TXFF_SIZE_8821A / PAGE_SIZE_8821A)             /* 256 */
#define RSVD_BOUNDARY_8821A    (TXFF_PG_NUM_8821A - RSVD_DRV_PG_NUM_8821A)     /* 248 = 0xF8 */
#define ACQ_PG_NUM_8821A       RSVD_BOUNDARY_8821A
#define REPORT_BUF             128
#define PHY_STATUS_SIZE        4
#define USB_TX_AGG_DESC_NUM    6

/* page_table_8821a[4] (four bulk-OUT endpoints): hq, nq, lq, exq, gapq */
#define PG_HQ   8
#define PG_NQ   0
#define PG_LQ   8
#define PG_EXQ  4
#define PG_GAPQ 1

/* rqpn_table_8821a[4]: dma mapping per queue (rtw88 enum rtw_dma_mapping) */
#define DMA_EXTRA  0
#define DMA_LOW    1
#define DMA_NORMAL 2
#define DMA_HIGH   3
#define MAP_VO DMA_NORMAL
#define MAP_VI DMA_NORMAL
#define MAP_BE DMA_LOW
#define MAP_BK DMA_LOW
#define MAP_MG DMA_EXTRA
#define MAP_HI DMA_HIGH

#define MACTXEN BIT(6)
#define MACRXEN BIT(7)

#define APS_FSMCO_MAC_ENABLE_HI BIT(0)  /* APS_FSMCO_MAC_ENABLE (BIT8) >> 8 */
#define APS_FSMCO_MAC_OFF_HI    BIT(1)  /* APS_FSMCO_MAC_OFF    (BIT9) >> 8 */

/* REG_CR bits enabled after power-on (rtw88xxau_init_power_on). */
#define CR_HCI_TXDMA_EN   BIT(0)
#define CR_HCI_RXDMA_EN   BIT(1)
#define CR_TXDMA_EN       BIT(2)
#define CR_RXDMA_EN       BIT(3)
#define CR_PROTOCOL_EN    BIT(4)
#define CR_SCHEDULE_EN    BIT(5)
#define CR_MAC_SEC_EN     BIT(9)
#define CR_32K_CAL_TMR_EN BIT(10)

/* Embedded firmware (src/fw_8821a_nic.c). */
extern unsigned char array_rtw8821a_fw[];
extern const unsigned int array_rtw8821a_fw_len;

static void set8(libusb_device_handle *h, uint16_t r, uint8_t b) { rtl_write8(h, r, rtl_read8(h, r, NULL) | b); }
static void clr8(libusb_device_handle *h, uint16_t r, uint8_t b) { rtl_write8(h, r, rtl_read8(h, r, NULL) & (uint8_t)~b); }
static void set16(libusb_device_handle *h, uint16_t r, uint16_t b) { rtl_write16(h, r, rtl_read16(h, r, NULL) | b); }
static void set32(libusb_device_handle *h, uint16_t r, uint32_t b) { rtl_write32(h, r, rtl_read32(h, r, NULL) | b); }
static void mask32(libusb_device_handle *h, uint16_t r, uint32_t mask, uint32_t val) {
    /* rtw_write32_mask: val is placed at the mask's lowest set bit */
    uint32_t shift = 0; while (shift < 32 && !(mask & (1u << shift))) shift++;
    uint32_t v = rtl_read32(h, r, NULL);
    rtl_write32(h, r, (v & ~mask) | ((val << shift) & mask));
}

/* ---- LLT (rtw88xxa_llt_write / rtw88xxa_llt_init) ---------------------- */
static int llt_write(libusb_device_handle *h, uint32_t address, uint32_t data) {
    uint32_t value = LLT_WRITE_ACCESS | (address << 8) | data;
    int rc = rtl_write32(h, REG_LLT_INIT, value);
    if (rc) return rc;
    for (int count = 0; ; count++) {
        uint32_t v = rtl_read32(h, REG_LLT_INIT, &rc);
        if (rc) return rc;
        if (!(v & (BIT(31) | BIT(30)))) return 0;
        if (count > 20) { printf("  [llt] poll timeout at %u\n", address); return 1; }
    }
}

static int llt_init(libusb_device_handle *h, uint32_t boundary, int verbose) {
    const uint32_t last_entry = 255;
    int rc;
    for (uint32_t i = 0; i < boundary - 1; i++) { rc = llt_write(h, i, i + 1); if (rc) return rc; }
    rc = llt_write(h, boundary - 1, 0xFF);                                       if (rc) return rc;
    for (uint32_t i = boundary; i < last_entry; i++) { rc = llt_write(h, i, i + 1); if (rc) return rc; }
    rc = llt_write(h, last_entry, boundary);                                      if (rc) return rc;
    if (verbose) printf("  [llt] table initialised, boundary 0x%02x\n", boundary);
    return 0;
}

/* rtw88xxa_reset_8051, non-8812A branch (RSV_CTRL+1 bit 0 instead of bit 3). */
static void reset_8051_8821a(libusb_device_handle *h) {
    clr8(h, REG_RSV_CTRL, BIT(1));          /* Reset MCU IO Wrapper */
    clr8(h, REG_RSV_CTRL + 1, BIT(0));
    uint8_t v = rtl_read8(h, REG_SYS_FUNC_EN + 1, NULL);
    rtl_write8(h, REG_SYS_FUNC_EN + 1, v & (uint8_t)~BIT(2));   /* hold 8051 */
    clr8(h, REG_RSV_CTRL, BIT(1));          /* Enable MCU IO Wrapper */
    set8(h, REG_RSV_CTRL + 1, BIT(0));
    rtl_write8(h, REG_SYS_FUNC_EN + 1, v | BIT(2));             /* release 8051 */
}

/* rtw88xxau_hw_reset: lightweight deinit if firmware RAM code is running
 * (i.e. a previous run left the chip initialised). */
static int hw_reset_8821a(libusb_device_handle *h, int verbose) {
    int rc = 0;
    uint8_t v = rtl_read8(h, REG_MCUFWDL, &rc);
    if (rc) return rc;
    if (!(v & RAM_DL_SEL)) {
        if (verbose) printf("  [hw-reset] 8051 not running RAM code, skip\n");
        return 0;
    }
    if (verbose) printf("  [hw-reset] firmware running -> reset 8051/BB/RF/MAC\n");

    reset_8051_8821a(h);
    rtl_write8(h, REG_MCUFWDL, 0x00);

    /* before BB reset should do clock gated */
    rtl_write32(h, REG_FPGA0_XCD_RF_PARA, rtl_read32(h, REG_FPGA0_XCD_RF_PARA, NULL) | BIT(6));
    clr8(h, REG_SYS_FUNC_EN, BIT(0) | BIT(1));   /* reset BB */
    rtl_write8(h, REG_RF_CTRL, 0);               /* reset RF */
    rtl_write16(h, REG_CR, 0);                   /* reset TRX path */

    /* reset MAC, reg0x5[1], auto FSM off; wait for it to self-clear */
    set8(h, REG_APS_FSMCO + 1, APS_FSMCO_MAC_OFF_HI);
    int cleared = 0;
    for (int i = 0; i < 5000; i++) {
        v = rtl_read8(h, REG_APS_FSMCO + 1, NULL);
        if (!(v & APS_FSMCO_MAC_OFF_HI)) { cleared = 1; break; }
        usleep(1);
    }
    if (!cleared) printf("  [hw-reset] timed out waiting for 0x5[1]\n");

    /* reg0x5[0], auto FSM on */
    rtl_write8(h, REG_APS_FSMCO + 1, v | APS_FSMCO_MAC_ENABLE_HI);

    clr8(h, REG_SYS_FUNC_EN + 1, BIT(4) | BIT(7));
    set8(h, REG_SYS_FUNC_EN + 1, BIT(4) | BIT(7));
    return 0;
}

/* trans_carddis_to_cardemu_8821a, USB-relevant steps (INTF_ALL | INTF_USB). */
static const rtl_pwr_step carddis_to_cardemu_8821a[] = {
    {0x0005, RTL_PWR_WRITE, BIT(3) | BIT(7), 0, "clear 0x05[3],[7]"},
    {0x004A, RTL_PWR_WRITE, BIT(0),          0, "USB: 0x4A[0]=0"},
    {0x0005, RTL_PWR_WRITE, BIT(3) | BIT(4), 0, "clear 0x05[3],[4] (WL suspend)"},
    {0x0000, RTL_PWR_END, 0, 0, NULL},
};

/* trans_cardemu_to_act_8821a, USB-relevant steps (INTF_ALL | INTF_USB). */
static const rtl_pwr_step cardemu_to_act_8821a[] = {
    {0x0020, RTL_PWR_WRITE,    BIT(0),                   BIT(0),          "0x20[0]=1 LDO"},
    {0x0067, RTL_PWR_WRITE,    BIT(4),                   0,               "0x67[4]=0"},
    {1,      RTL_PWR_DELAY_MS, 0,                        0,               "1 ms"},
    {0x0000, RTL_PWR_WRITE,    BIT(5),                   0,               "0x00[5]=0"},
    {0x0005, RTL_PWR_WRITE,    BIT(4) | BIT(3) | BIT(2), 0,               "clear 0x05[4:2]"},
    {0x0006, RTL_PWR_POLLING,  BIT(1),                   BIT(1),          "wait power ready 0x06[1]=1"},
    {0x0006, RTL_PWR_WRITE,    BIT(0),                   BIT(0),          "0x06[0]=1"},
    {0x0005, RTL_PWR_WRITE,    BIT(7),                   0,               "0x05[7]=0"},
    {0x0005, RTL_PWR_WRITE,    BIT(4) | BIT(3),          0,               "clear 0x05[4:3]"},
    {0x0005, RTL_PWR_WRITE,    BIT(0),                   BIT(0),          "0x05[0]=1 (APFM_ONMAC)"},
    {0x0005, RTL_PWR_POLLING,  BIT(0),                   0,               "wait 0x05[0]=0 (MAC on)"},
    {0x004F, RTL_PWR_WRITE,    BIT(0),                   BIT(0),          "0x4F[0]=1"},
    {0x0067, RTL_PWR_WRITE,    BIT(5) | BIT(4),          BIT(5) | BIT(4), "0x67[5:4]=11"},
    {0x0025, RTL_PWR_WRITE,    BIT(6),                   0,               "0x25[6]=0"},
    {0x0049, RTL_PWR_WRITE,    BIT(1),                   BIT(1),          "0x49[1]=1"},
    {0x0063, RTL_PWR_WRITE,    BIT(1),                   BIT(1),          "0x63[1]=1"},
    {0x0062, RTL_PWR_WRITE,    BIT(1),                   0,               "0x62[1]=0"},
    {0x0058, RTL_PWR_WRITE,    BIT(0),                   BIT(0),          "0x58[0]=1"},
    {0x005A, RTL_PWR_WRITE,    BIT(1),                   BIT(1),          "0x5A[1]=1"},
    {0x002E, RTL_PWR_WRITE,    0xFF,                     0x82,            "0x2E=0x82"},
    {0x0010, RTL_PWR_WRITE,    BIT(6),                   BIT(6),          "0x10[6]=1"},
    {0x0000, RTL_PWR_END, 0, 0, NULL},
};

static int rtl8821au_power_on(libusb_device_handle *h, int verbose) {
    int rc = hw_reset_8821a(h, verbose);
    if (rc) return rc;

    if (verbose) printf("  [pwr] carddis -> cardemu\n");
    rc = rtl_pwr_seq_run(h, carddis_to_cardemu_8821a, verbose);
    if (rc) return rc;

    if (verbose) printf("  [pwr] cardemu -> act\n");
    rc = rtl_pwr_seq_run(h, cardemu_to_act_8821a, verbose);
    if (rc) return rc;

    /* rtw88xxau_init_power_on tail */
    rtl_write16(h, REG_CR, 0);
    uint16_t cr = CR_HCI_TXDMA_EN | CR_HCI_RXDMA_EN | CR_TXDMA_EN | CR_RXDMA_EN |
                  CR_PROTOCOL_EN | CR_SCHEDULE_EN | CR_MAC_SEC_EN | CR_32K_CAL_TMR_EN;
    rtl_write16(h, REG_CR, rtl_read16(h, REG_CR, NULL) | cr);

    /* 8821A only: LDO tweak when SYS_CFG1[24] is set */
    if (rtl_read8(h, REG_SYS_CFG1 + 3, NULL) & BIT(0))
        set8(h, REG_LDO_SWR_CTRL, BIT(6));

    if (verbose) printf("  [pwr] CR=0x%04x\n", rtl_read16(h, REG_CR, NULL));

    /* rtw88xxa_power_on: trx fifo info -> LLT -> drop-data, before firmware */
    rc = llt_init(h, RSVD_BOUNDARY_8821A, verbose);
    if (rc) return rc;
    set32(h, REG_TXDMA_OFFSET_CHK, DROP_DATA_EN);
    return 0;
}

static int rtl8821au_fw_download(libusb_device_handle *h, int verbose) {
    return rtl_fw_download_blob(h, array_rtw8821a_fw, array_rtw8821a_fw_len, verbose);
}

/* MAC register table, generated from rtw88 rtw8821a_table.c (rtw8821a_mac[],
 * applied with rtw_phy_cfg_mac = 8-bit writes). 98 entries. */
typedef struct { uint16_t addr; uint8_t val; } mac_reg;
static const mac_reg rtw8821a_mac_tbl[] = {
    {0x0421, 0x0F},
    {0x0428, 0x0A},
    {0x0429, 0x10},
    {0x0430, 0x00},
    {0x0431, 0x00},
    {0x0432, 0x00},
    {0x0433, 0x01},
    {0x0434, 0x04},
    {0x0435, 0x05},
    {0x0436, 0x07},
    {0x0437, 0x08},
    {0x043C, 0x04},
    {0x043D, 0x05},
    {0x043E, 0x07},
    {0x043F, 0x08},
    {0x0440, 0x5D},
    {0x0441, 0x01},
    {0x0442, 0x00},
    {0x0444, 0x10},
    {0x0445, 0x00},
    {0x0446, 0x00},
    {0x0447, 0x00},
    {0x0448, 0x00},
    {0x0449, 0xF0},
    {0x044A, 0x0F},
    {0x044B, 0x3E},
    {0x044C, 0x10},
    {0x044D, 0x00},
    {0x044E, 0x00},
    {0x044F, 0x00},
    {0x0450, 0x00},
    {0x0451, 0xF0},
    {0x0452, 0x0F},
    {0x0453, 0x00},
    {0x0456, 0x5E},
    {0x0460, 0x66},
    {0x0461, 0x66},
    {0x04C8, 0x3F},
    {0x04C9, 0xFF},
    {0x04CC, 0xFF},
    {0x04CD, 0xFF},
    {0x04CE, 0x01},
    {0x0500, 0x26},
    {0x0501, 0xA2},
    {0x0502, 0x2F},
    {0x0503, 0x00},
    {0x0504, 0x28},
    {0x0505, 0xA3},
    {0x0506, 0x5E},
    {0x0507, 0x00},
    {0x0508, 0x2B},
    {0x0509, 0xA4},
    {0x050A, 0x5E},
    {0x050B, 0x00},
    {0x050C, 0x4F},
    {0x050D, 0xA4},
    {0x050E, 0x00},
    {0x050F, 0x00},
    {0x0512, 0x1C},
    {0x0514, 0x0A},
    {0x0516, 0x0A},
    {0x0525, 0x4F},
    {0x0550, 0x10},
    {0x0551, 0x10},
    {0x0559, 0x02},
    {0x055C, 0x50},
    {0x055D, 0xFF},
    {0x0605, 0x30},
    {0x0607, 0x07},
    {0x0608, 0x0E},
    {0x0609, 0x2A},
    {0x0620, 0xFF},
    {0x0621, 0xFF},
    {0x0622, 0xFF},
    {0x0623, 0xFF},
    {0x0624, 0xFF},
    {0x0625, 0xFF},
    {0x0626, 0xFF},
    {0x0627, 0xFF},
    {0x0638, 0x50},
    {0x063C, 0x0A},
    {0x063D, 0x0A},
    {0x063E, 0x0E},
    {0x063F, 0x0E},
    {0x0640, 0x40},
    {0x0642, 0x40},
    {0x0643, 0x00},
    {0x0652, 0xC8},
    {0x066E, 0x05},
    {0x0700, 0x21},
    {0x0701, 0x43},
    {0x0702, 0x65},
    {0x0703, 0x87},
    {0x0708, 0x21},
    {0x0709, 0x43},
    {0x070A, 0x65},
    {0x070B, 0x87},
    {0x0718, 0x40},
};

static int usb_speed_of(libusb_device_handle *h) {
    libusb_device *d = libusb_get_device(h);
    return d ? libusb_get_device_speed(d) : LIBUSB_SPEED_UNKNOWN;
}

/*
 * rtw88xxa_power_on, the part after rtw_download_firmware() and before
 * rtw88xxa_phy_bb_config(). Four bulk-OUT endpoints assumed (checked).
 */
static int rtl8821au_mac_init(libusb_device_handle *h, int verbose) {
    int rc;

    rtl_write8(h, REG_HMETFR, 0xf);

    /* rtw_load_table(chip->mac_tbl) */
    for (size_t i = 0; i < sizeof(rtw8821a_mac_tbl) / sizeof(rtw8821a_mac_tbl[0]); i++) {
        rc = rtl_write8(h, rtw8821a_mac_tbl[i].addr, rtw8821a_mac_tbl[i].val);
        if (rc) { printf("  [mac] table write 0x%04x failed (%d)\n", rtw8821a_mac_tbl[i].addr, rc); return rc; }
    }
    if (verbose) printf("  [mac] MAC table applied (%zu writes)\n", sizeof(rtw8821a_mac_tbl) / sizeof(rtw8821a_mac_tbl[0]));

    /* rtw88xxau_init_queue_reserved_page (page_table_8821a[4]) */
    uint16_t pubq = ACQ_PG_NUM_8821A - PG_HQ - PG_LQ - PG_NQ - PG_EXQ - PG_GAPQ;
    rtl_write32(h, REG_RQPN_NPQ, (uint32_t)PG_NQ | ((uint32_t)PG_EXQ << 16));
    rtl_write32(h, REG_RQPN, BIT(31) | (uint32_t)PG_HQ | ((uint32_t)PG_LQ << 8) | ((uint32_t)pubq << 16));
    if (verbose) printf("  [mac] pages: hq=%d nq=%d lq=%d exq=%d pub=%u, boundary 0x%02x\n",
                        PG_HQ, PG_NQ, PG_LQ, PG_EXQ, pubq, RSVD_BOUNDARY_8821A);

    /* rtw88xxau_init_tx_buffer_boundary */
    rtl_write8(h, REG_BCNQ_BDNY,       RSVD_BOUNDARY_8821A);
    rtl_write8(h, REG_MGQ_BDNY,        RSVD_BOUNDARY_8821A);
    rtl_write8(h, REG_WMAC_LBK_BF_HD,  RSVD_BOUNDARY_8821A);
    rtl_write8(h, REG_TRXFF_BNDY,      RSVD_BOUNDARY_8821A);
    rtl_write8(h, REG_DWBCN0_CTRL + 1, RSVD_BOUNDARY_8821A);

    /* rtw88xxau_init_queue_priority (rqpn_table_8821a[4]) */
    uint16_t pq = rtl_read16(h, REG_TXDMA_PQ_MAP, &rc) & 0x7;
    if (rc) return rc;
    pq |= (uint16_t)(MAP_HI << 14) | (uint16_t)(MAP_MG << 12) | (uint16_t)(MAP_BK << 10) |
          (uint16_t)(MAP_BE << 8)  | (uint16_t)(MAP_VI << 6)  | (uint16_t)(MAP_VO << 4);
    rtl_write16(h, REG_TXDMA_PQ_MAP, pq);
    rtl_write8(h, REG_HIQ_NO_LMT_EN, 0xff);   /* 4 bulk-out: hi queue tx immediately */
    if (verbose) printf("  [mac] TXDMA_PQ_MAP=0x%04x\n", pq);

    rtl_write16(h, REG_TRXFF_BNDY + 2, (uint16_t)(RXFF_SIZE_8821A - REPORT_BUF - 1));
    rtl_write8(h, REG_RX_DRVINFO_SZ, PHY_STATUS_SIZE);

    rtl_write32(h, REG_HIMR0, 0);
    rtl_write32(h, REG_HIMR1, 0);

    mask32(h, REG_CR, 0x30000, 0x2);           /* network type: link AP (as rtw88) */

    /* rtw88xxa_init_wmac_setting */
    rtl_write16(h, REG_RXFLTMAP0, 0xffff);
    rtl_write16(h, REG_RXFLTMAP1, 0x0400);
    rtl_write16(h, REG_RXFLTMAP2, 0xffff);
    rtl_write32(h, REG_MAR,     0xffffffff);
    rtl_write32(h, REG_MAR + 4, 0xffffffff);

    /* rtw88xxa_init_adaptive_ctrl */
    mask32(h, REG_RRSR, 0xfffff, 0xffff1);
    rtl_write16(h, REG_RETRY_LIMIT, 0x3030);

    /* rtw88xxa_init_edca */
    rtl_write16(h, REG_SPEC_SIFS,     0x100a);
    rtl_write16(h, REG_MAC_SPEC_SIFS, 0x100a);
    rtl_write16(h, REG_SIFS,          0x100a);
    rtl_write16(h, REG_SIFS + 2,      0x100a);
    rtl_write32(h, REG_EDCA_BE_PARAM, 0x005EA42B);
    rtl_write32(h, REG_EDCA_BK_PARAM, 0x0000A44F);
    rtl_write32(h, REG_EDCA_VI_PARAM, 0x005EA324);
    rtl_write32(h, REG_EDCA_VO_PARAM, 0x002FA226);
    rtl_write8(h, REG_USTIME_TSF,  0x50);
    rtl_write8(h, REG_USTIME_EDCA, 0x50);

    set8(h, REG_FWHW_TXQ_CTRL, BIT(7));
    rtl_write8(h, REG_ACKTO, 0x80);

    /* rtw88xxau_tx_aggregation */
    mask32(h, REG_DWBCN0_CTRL, 0xf0, USB_TX_AGG_DESC_NUM);
    rtl_write8(h, REG_DWBCN1_CTRL, USB_TX_AGG_DESC_NUM << 1);   /* 8821A only */

    /* rtw88xxa_init_beacon_parameters */
    {
        int btcoex = (rtl_read32(h, REG_WL_BT_PWR_CTRL, NULL) >> 18) & 1;   /* BIT_BT_FUNC_EN */
        uint16_t bcn = (uint16_t)((BIT(4) << 8) | BIT(4));                  /* DIS_TSF_UDT both ports */
        if (btcoex) bcn |= BIT(3);                                           /* EN_BCN_FUNCTION */
        rtl_write16(h, REG_BCN_CTRL, bcn);
        if (verbose) printf("  [mac] btcoex=%d BCN_CTRL=0x%04x\n", btcoex, bcn);
    }
    mask32(h, REG_TBTT_PROHIBIT, 0xfffff, 0x04 | (0x64 << 8));   /* WLAN_TBTT_TIME */
    rtl_write8(h, REG_DRVERLYINT, 0x05);
    rtl_write8(h, REG_BCNDMATIM,  0x02);                            /* WLAN_BCN_DMA_TIME */
    rtl_write16(h, REG_BCNTCFG, 0x4413);
    rtl_write8(h, REG_BCN_MAX_ERR, 0xff);

    /* rtw_hci_interface_cfg -> rtw_usb_init_burst_pkt_len */
    {
        int speed = usb_speed_of(h);
        uint8_t burst = (speed == LIBUSB_SPEED_SUPER) ? 0 : (speed == LIBUSB_SPEED_HIGH) ? 1 : 2;
        uint8_t rxdma = (uint8_t)(BIT(3) | BIT(2) | BIT(1) | (burst << 4));   /* BURST_CNT | DMA_MODE | size */
        rtl_write8(h, REG_RXDMA_MODE, rxdma);
        set16(h, REG_TXDMA_OFFSET_CHK, DROP_DATA_EN);
        if (verbose) printf("  [mac] usb speed=%d RXDMA_MODE=0x%02x\n", speed, rxdma);

        rtl_write8(h, REG_USB3_RXITV, 0x01);
        rtl_write16(h, REG_RXDMA_STATUS, 0x7400);      /* burst length = 4 */
        rtl_write8(h, REG_RXDMA_STATUS + 1, 0xf5);

        rtl_write8(h, REG_AMPDU_MAX_TIME, 0x5e);       /* 8821A */
        rtl_write32(h, REG_AMPDU_MAX_LENGTH, 0xffffffff);
        rtl_write8(h, REG_USTIME_TSF,  0x50);
        rtl_write8(h, REG_USTIME_EDCA, 0x50);

        if (speed == LIBUSB_SPEED_SUPER)
            clr8(h, REG_USB_MOD, BIT(4) | BIT(3));     /* no U1/U2: 2.5G spur */
    }

    set8(h, REG_SINGLE_AMPDU_CTRL, BIT(7));
    rtl_write8(h, REG_RX_PKT_LIMIT, 0x18);             /* VHT 11K */
    rtl_write8(h, REG_PIFS, 0x00);

    /* 8821A */
    rtl_write16(h, REG_MAX_AGGR_NUM, 0x1f1f);
    rtl_write8(h, REG_FWHW_TXQ_CTRL, 0x80);
    rtl_write32(h, REG_FAST_EDCA_CTRL, 0x03087777);

    set8(h, REG_RSV_CTRL, BIT(5) | BIT(6));            /* MAC not reset by bus */

    /* ARFB tables 9..12 */
    rtl_write32(h, REG_ARFR0,     0x00000010);
    rtl_write32(h, REG_ARFRH0,    0xfffff000);
    rtl_write32(h, REG_ARFR1_V1,  0x00000010);
    rtl_write32(h, REG_ARFRH1_V1, 0x003ff000);
    rtl_write32(h, REG_ARFR2_V1,  0x00000015);
    rtl_write32(h, REG_ARFRH2_V1, 0x003ff000);
    rtl_write32(h, REG_ARFR3_V1,  0x00000015);
    rtl_write32(h, REG_ARFRH3_V1, 0xffcff000);

    set8(h, REG_CR, MACTXEN | MACRXEN);

    if (verbose) printf("  [mac] done, CR=0x%04x\n", rtl_read16(h, REG_CR, NULL));
    return 0;
}


static void rtl8821au_dbg_dump(libusb_device_handle *h) {
    printf("[hal] Readback: SYS_CFG=0x%08x CR=0x%04x MCUFWDL=0x%08x RCR=0x%08x RF_A[0x18]=0x%05x IGI=0x%02x\n",
           rtl_read32(h, REG_SYS_CFG, NULL), rtl_read16(h, REG_CR, NULL),
           rtl_read32(h, REG_MCUFWDL, NULL), rtl_read32(h, 0x0608, NULL),
           rtl8821a_rf_read(h, 0x18, 0xFFFFF), rtl8821au_dig_get(h));
}

const rtl_chip_ops rtl8821au_ops = {
    .name           = "RTL8821AU",
    .chip           = RTL_CHIP_8821AU,

    .power_on       = rtl8821au_power_on,
    .fw_download    = rtl8821au_fw_download,
    .fw_active_mode = rtl_fw_set_active_mode,   /* same H2C mailbox as 8812A */
    .mac_init       = rtl8821au_mac_init,
    .bb_init        = rtl8821au_bb_init,
    .rf_init        = rtl8821au_rf_init,
    .set_channel    = rtl8821au_set_channel,
    .set_monitor    = rtl_mac_set_monitor,       /* same RCR/MSR/RXFLTMAP registers */
    .lck            = rtl8821au_lck,
    .iqk            = rtl8821au_iqk,
    .txpwr_apply    = rtl8821au_txpwr_apply,    /* efuse base capped by rtw88 limit table */
    .txpwr_readback = rtl8821au_txpwr_readback,
    .led_on         = NULL,                     /* optional: LED register differs, port later */
    .led_off        = NULL,
    .dbg_dump       = rtl8821au_dbg_dump,

    /* 88xxA descriptor geometry (rtw8821a_hw_spec: rx 24, tx 40).
     *
     * Endpoints (usbprobe, Archer T2U Plus): bulk IN 0x84; bulk OUT 0x05,
     * 0x06, 0x08, 0x09. rtw88 maps four OUT pipes via rqpn_table_8821a[4]
     * (vo/vi=NORMAL, be/bk=LOW, mgmt=EXTRA, hi=HIGH) and dma_mapping_to_ep
     * (HIGH=pipe0, NORMAL=pipe1, LOW=pipe2, EXTRA=pipe3), i.e.
     *   0x05 high/beacon/H2C, 0x06 VO/VI, 0x08 BE/BK, 0x09 management.
     * The hardware side of that mapping (REG_TXDMA_PQ_MAP) is programmed in
     * rtl8821au_mac_init. */
    .rx_ep          = 0x84,
    .tx_ep_mgmt     = 0x09,
    .rxdesc_size    = 24,
    .txdesc_size    = RTL_TXDESC_SIZE,

    .rx_parse_desc  = rtl88xxa_rx_parse_desc,
    .tx_inject      = rtl_tx_inject,            /* 88xxA TX desc, verify for 1x1 */
};
