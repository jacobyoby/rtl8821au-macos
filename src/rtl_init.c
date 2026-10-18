/*
 * rtl_init — power-on sequences (card enable), USB.
 *
 * Generic interpreter for the Realtek "power sequence" tables, ported from:
 *   include/Hal8812PwrSeq.h   (RTL8812_TRANS_CARDEMU_TO_ACT)
 *   hal/HalPwrSeqCmd.c        (interpreter semantics)
 * and matching rtw88's rtw_sub_pwr_seq_parser() (mac.c) for the 8821A tables.
 *
 * WRITE   : v = read8(off); v = (v & ~mask) | (value & mask); write8(off, v)
 * POLLING : repeat read8(off) until (v & mask) == (value & mask)
 * DELAY_US: microseconds from the offset field
 * DELAY_MS: milliseconds from the offset field
 *
 * Only the USB-relevant steps are kept in the tables (PWR_INTF_ALL includes USB).
 */
#include "rtl_usb.h"
#include <stdio.h>
#include <unistd.h>

#define BIT(n) (1u << (n))

/* RTL8812_TRANS_CARDEMU_TO_ACT (USB), BITn expanded to numbers. */
static const rtl_pwr_step card_enable_8812[] = {
    {0x0005, RTL_PWR_WRITE,   BIT(2), 0,      "disable SW LPS 0x04[10]=0"},
    {0x0006, RTL_PWR_POLLING, BIT(1), BIT(1), "wait power ready 0x04[17]=1"},
    {0x0005, RTL_PWR_WRITE,   BIT(3), 0,      "disable WL suspend"},
    {0x0005, RTL_PWR_WRITE,   BIT(0), BIT(0), "0x04[8]=1 (APFM_ONMAC)"},
    {0x0005, RTL_PWR_POLLING, BIT(0), 0,      "wait 0x04[8]=0 (MAC on)"},
    {0x0024, RTL_PWR_WRITE,   BIT(1), 0,      "0x24[1]=0 xosc buffer"},
    {0x0028, RTL_PWR_WRITE,   BIT(3), 0,      "0x28[3]=0 xosc buffer"},
    {0x0000, RTL_PWR_END,     0,      0,      NULL},
};

#define POLL_TRIES   1000
#define POLL_DELAY_US 10

int rtl_pwr_seq_run(libusb_device_handle *h, const rtl_pwr_step *seq, int verbose) {
    for (int i = 0; seq[i].cmd != RTL_PWR_END; i++) {
        const rtl_pwr_step *s = &seq[i];
        int rc = 0;

        if (s->cmd == RTL_PWR_WRITE) {
            uint8_t v = rtl_read8(h, s->offset, &rc);
            if (rc != 0) { if (verbose) printf("  [%d] READ 0x%04x FEHLER\n", i, s->offset); return rc; }
            v = (uint8_t)((v & ~s->mask) | (s->value & s->mask));
            rc = rtl_write8(h, s->offset, v);
            if (rc != 0) { if (verbose) printf("  [%d] WRITE 0x%04x FEHLER\n", i, s->offset); return rc; }
            if (verbose) printf("  [%d] WRITE   0x%04x mask=0x%02x val=0x%02x  (%s)\n",
                                i, s->offset, s->mask, s->value, s->note ? s->note : "");

        } else if (s->cmd == RTL_PWR_POLLING) {
            int ok = 0;
            uint8_t v = 0;
            for (int t = 0; t < POLL_TRIES; t++) {
                v = rtl_read8(h, s->offset, &rc);
                if (rc != 0) { if (verbose) printf("  [%d] POLL READ FEHLER\n", i); return rc; }
                if ((v & s->mask) == (s->value & s->mask)) { ok = 1; break; }
                usleep(POLL_DELAY_US);
            }
            if (!ok) {
                if (verbose) printf("  [%d] POLLING 0x%04x mask=0x%02x erwartet=0x%02x "
                                    "letzter=0x%02x  TIMEOUT  (%s)\n",
                                    i, s->offset, s->mask, s->value, v & s->mask,
                                    s->note ? s->note : "");
                return i + 1; /* >0: number of the failed step */
            }
            if (verbose) printf("  [%d] POLLING 0x%04x mask=0x%02x == 0x%02x  OK  (%s)\n",
                                i, s->offset, s->mask, s->value & s->mask, s->note ? s->note : "");

        } else if (s->cmd == RTL_PWR_DELAY_US) {
            usleep(s->offset);
            if (verbose) printf("  [%d] DELAY   %u us\n", i, s->offset);
        } else if (s->cmd == RTL_PWR_DELAY_MS) {
            usleep((useconds_t)s->offset * 1000u);
            if (verbose) printf("  [%d] DELAY   %u ms\n", i, s->offset);
        }
    }
    return 0;
}

int rtl_power_on(libusb_device_handle *h, int verbose) {
    return rtl_pwr_seq_run(h, card_enable_8812, verbose);
}
