#include "rtl_tx.h"
#include <string.h>

/* SET_BITS_TO_LE_4BYTE equivalent: nbits starting at bitoff in the LE dword at byteoff. */
static void set_bits(uint8_t *d, int byteoff, int bitoff, int nbits, uint32_t val) {
    uint32_t cur = (uint32_t)d[byteoff] | ((uint32_t)d[byteoff+1] << 8) |
                   ((uint32_t)d[byteoff+2] << 16) | ((uint32_t)d[byteoff+3] << 24);
    uint32_t mask = ((nbits >= 32) ? 0xFFFFFFFFu : ((1u << nbits) - 1)) << bitoff;
    cur = (cur & ~mask) | ((val << bitoff) & mask);
    d[byteoff]   = cur & 0xff;
    d[byteoff+1] = (cur >> 8) & 0xff;
    d[byteoff+2] = (cur >> 16) & 0xff;
    d[byteoff+3] = (cur >> 24) & 0xff;
}

/* rtl8812a_cal_txdesc_chksum: XOR of the first 32 bytes (16 u16), field zeroed first. */
static void tx_checksum(uint8_t *desc) {
    set_bits(desc, 28, 0, 16, 0);
    uint16_t cs = 0;
    for (int i = 0; i < 16; i++)
        cs ^= (uint16_t)(desc[2*i] | (desc[2*i+1] << 8));
    set_bits(desc, 28, 0, 16, cs);
}

int rtl_tx_inject(libusb_device_handle *h, const uint8_t *frame, int len,
                  uint8_t rate, uint8_t qsel, uint8_t ep) {
    if (len <= 0 || len > 4096) return LIBUSB_ERROR_INVALID_PARAM;

    uint8_t pkt[RTL_TXDESC_SIZE + 4096];
    uint8_t *desc = pkt;
    memset(desc, 0, RTL_TXDESC_SIZE);

    /* dword0: pkt_size, offset(=desc len), first/last seg, own */
    set_bits(desc, 0, 0, 16, (uint32_t)len);        /* PKT_SIZE */
    set_bits(desc, 0, 16, 8, RTL_TXDESC_SIZE);      /* OFFSET */
    set_bits(desc, 0, 26, 1, 1);                    /* LAST_SEG */
    set_bits(desc, 0, 27, 1, 1);                    /* FIRST_SEG */
    set_bits(desc, 0, 31, 1, 1);                    /* OWN */
    /* dword1(+4): macid, queue_sel, rate_id */
    set_bits(desc, 4, 8, 5, qsel);                  /* QUEUE_SEL */
    set_bits(desc, 4, 16, 5, (rate <= 0x03) ? 1u : 2u); /* RATE_ID: CCK(B)=1, OFDM(G)=2 */
    /* dword3(+12): use_rate, disable_fb */
    set_bits(desc, 12, 8, 1, 1);                    /* USE_RATE */
    set_bits(desc, 12, 10, 1, 1);                   /* DISABLE_FB */
    /* dword4(+16): tx_rate */
    set_bits(desc, 16, 0, 7, rate);                 /* TX_RATE */
    /* Sequence number: software-assigned, as rtw88/mac80211 do (header seq
     * field, EN_HWSEQ = 0). With hardware sequencing the AP saw our
     * retransmissions (Retry=1) carrying a stale/zero sequence number and
     * dropped them as duplicates; software numbering keeps every frame
     * unique and lets hardware retries keep the right number. Callers that
     * pre-fill a non-zero seq field keep it. */
    memcpy(pkt + RTL_TXDESC_SIZE, frame, len);
    {
        static uint16_t sw_seq = 0;
        uint8_t *hdr = pkt + RTL_TXDESC_SIZE;
        if (len >= 24 && hdr[22] == 0 && hdr[23] == 0) {
            sw_seq = (uint16_t)((sw_seq + 1) & 0x0fff);
            hdr[22] = (uint8_t)(sw_seq << 4);          /* fragment 0, seq low nibble */
            hdr[23] = (uint8_t)(sw_seq >> 4);
        }
    }
    set_bits(desc, 32, 15, 1, 0);                   /* HWSEQ_EN off */

    tx_checksum(desc);

    int total = RTL_TXDESC_SIZE + len;
    int sent = 0;
    int rc = libusb_bulk_transfer(h, ep, pkt, total, &sent, 500);
    if (rc != 0) return rc;
    return (sent == total) ? 0 : LIBUSB_ERROR_IO;
}
