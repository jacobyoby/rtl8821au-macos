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
 *   [ ] MAC init                     rtw8821a_mac_tbl + rtw88xxa_power_on tail
 *                                    (LLT, queue pages, TRX FIFO, ...)
 *   [ ] BB init                      rtw8821a_bb_tbl / rtw8821a_agc_tbl
 *   [ ] RF init                      rtw8821a_rf_a_tbl (path A only, 1x1)
 *   [ ] channel / bandwidth          rtw88xxa_set_channel
 *   [ ] monitor RCR                  same registers as 8812A, verify
 *   [ ] LCK / IQK                    rtw8821a_phy_calibration
 *   [ ] TX power from efuse          rtw8821a_txpwr_lmt + efuse layout
 *   [ ] TX descriptor                verify 88xxA TX desc for the 1x1 part
 *   [ ] tools use ops->tx_ep_mgmt    associate/alfa_netd/inject still pass the
 *                                    8812AU macro RTL_TX_EP_MGMT (0x02) directly
 *
 * Unported steps return RTL_ENOSYS so rtl_hal stops with a clear message.
 */
#include "rtl_chip.h"
#include "rtl_usb.h"
#include "rtl_tx.h"
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
    return 0;
}

static int rtl8821au_fw_download(libusb_device_handle *h, int verbose) {
    return rtl_fw_download_blob(h, array_rtw8821a_fw, array_rtw8821a_fw_len, verbose);
}

/* ---- not ported yet ------------------------------------------------------ */
static int not_ported(const char *step) {
    printf("[8821au] %s: not ported yet (see rtw88 rtw8821a.c / rtw88xxa.c)\n", step);
    return RTL_ENOSYS;
}
static int np_mac_init   (libusb_device_handle *h, int v)        { (void)h; (void)v; return not_ported("mac_init"); }
static int np_bb_init    (libusb_device_handle *h, int v)        { (void)h; (void)v; return not_ported("bb_init"); }
static int np_rf_init    (libusb_device_handle *h, int v)        { (void)h; (void)v; return not_ported("rf_init"); }
static int np_set_channel(libusb_device_handle *h, int c, int b) { (void)h; (void)c; (void)b; return not_ported("set_channel"); }
static int np_set_monitor(libusb_device_handle *h, int v)        { (void)h; (void)v; return not_ported("set_monitor"); }
static int np_lck        (libusb_device_handle *h, int v)        { (void)h; (void)v; return not_ported("lck"); }
static int np_iqk        (libusb_device_handle *h, int v)        { (void)h; (void)v; return not_ported("iqk"); }
static int np_txpwr_apply(libusb_device_handle *h, int c, int v) { (void)h; (void)c; (void)v; return not_ported("txpwr_apply"); }

static void rtl8821au_dbg_dump(libusb_device_handle *h) {
    printf("[hal] Readback: SYS_CFG=0x%08x CR=0x%04x MCUFWDL=0x%08x\n",
           rtl_read32(h, REG_SYS_CFG, NULL), rtl_read16(h, REG_CR, NULL),
           rtl_read32(h, REG_MCUFWDL, NULL));
}

const rtl_chip_ops rtl8821au_ops = {
    .name           = "RTL8821AU",
    .chip           = RTL_CHIP_8821AU,

    .power_on       = rtl8821au_power_on,
    .fw_download    = rtl8821au_fw_download,
    .fw_active_mode = rtl_fw_set_active_mode,   /* same H2C mailbox as 8812A */
    .mac_init       = np_mac_init,
    .bb_init        = np_bb_init,
    .rf_init        = np_rf_init,
    .set_channel    = np_set_channel,
    .set_monitor    = np_set_monitor,
    .lck            = np_lck,
    .iqk            = np_iqk,
    .txpwr_apply    = np_txpwr_apply,
    .txpwr_readback = NULL,                     /* optional (TRY) */
    .led_on         = NULL,                     /* optional: LED register differs, port later */
    .led_off        = NULL,
    .dbg_dump       = rtl8821au_dbg_dump,

    /* 88xxA descriptor geometry (rtw8821a_hw_spec: rx 24, tx 40).
     *
     * Endpoints (usbprobe, Archer T2U Plus): bulk IN 0x84; bulk OUT 0x05,
     * 0x06, 0x08, 0x09. rtw88 maps four OUT pipes via rqpn_table_8821a[4]
     * (be/bk=NORMAL, vi/vo=LOW, mgmt=EXTRA, hi=HIGH) and dma_mapping_to_ep
     * (HIGH=pipe0, NORMAL=pipe1, LOW=pipe2, EXTRA=pipe3), i.e.
     *   0x05 high/beacon/H2C, 0x06 BE/BK, 0x08 VI/VO, 0x09 management.
     * The hardware side of that mapping (REG_TRXDMA_CTRL) is programmed in
     * mac_init (rtw88xxau_init_queue_priority), not ported yet. */
    .rx_ep          = 0x84,
    .tx_ep_mgmt     = 0x09,
    .rxdesc_size    = 24,
    .txdesc_size    = RTL_TXDESC_SIZE,

    .rx_parse_desc  = rtl88xxa_rx_parse_desc,
    .tx_inject      = rtl_tx_inject,            /* 88xxA TX desc, verify for 1x1 */
};
