/*
 * rtl_rx — receive path using asynchronous libusb transfers.
 *
 * RTL_RX_URBS transfers are permanently queued at the host controller, so the
 * chip ALWAYS finds a free buffer -- even while we parse or write to the pcap
 * file. The old synchronous path had no buffer open in exactly that window
 * and lost frames on busy channels.
 *
 * All four public readers now share one parser and one engine; descriptor
 * parsing comes from rtl_chip_ops.
 */
#include "rtl_rx.h"
#include "rtl_usb.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Binding to context + chip                                          */
/* ------------------------------------------------------------------ */

static libusb_context      *g_ctx = NULL;
static const rtl_chip_ops  *g_ops = NULL;

void rtl_rx_bind(libusb_context *ctx, const rtl_chip_ops *ops) {
    g_ctx = ctx;
    g_ops = ops ? ops : rtl_chip_default();
}

static const rtl_chip_ops *ops_or_default(void) {
    return g_ops ? g_ops : rtl_chip_default();
}
const rtl_chip_ops *rtl_rx_bound_ops(void) { return ops_or_default(); }

/* ------------------------------------------------------------------ */
/* LED (main thread only, NEVER from a libusb callback)              */
/* ------------------------------------------------------------------ */

static void led_activity(libusb_device_handle *h) {
    static struct timespec last; static int inited = 0, state = 0;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    if (!inited) { last = now; inited = 1; }
    long ms = (now.tv_sec - last.tv_sec) * 1000
            + (now.tv_nsec - last.tv_nsec) / 1000000;
    if (ms >= 120) {
        state = !state;
        if (state) rtl_led_on(h); else rtl_led_off(h);
        last = now;
    }
}

/* ------------------------------------------------------------------ */
/* pcap output                                                         */
/* ------------------------------------------------------------------ */

void rtl_rx_write_pcap_header(FILE *f) {
    uint32_t magic = 0xa1b2c3d4;
    uint16_t vmaj = 2, vmin = 4;
    uint32_t zone = 0, sig = 0, snap = 65535, net = 127;
    fwrite(&magic, 4, 1, f);
    fwrite(&vmaj, 2, 1, f); fwrite(&vmin, 2, 1, f);
    fwrite(&zone, 4, 1, f); fwrite(&sig, 4, 1, f);
    fwrite(&snap, 4, 1, f); fwrite(&net, 4, 1, f);
    fflush(f);
}

typedef struct { FILE *f; int broken; int live; } pcap_sink;

static void pcap_cb(const uint8_t *frame, uint32_t len, const rtl_rx_sub *rx,
                    void *v) {
    (void)rx;
    pcap_sink *s = (pcap_sink *)v;
    if (s->broken) return;

    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    uint8_t rtap[8] = { 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint32_t caplen = len + (uint32_t)sizeof(rtap);
    uint32_t sec = (uint32_t)ts.tv_sec, usec = (uint32_t)(ts.tv_nsec / 1000);

    fwrite(&sec, 4, 1, s->f); fwrite(&usec, 4, 1, s->f);
    fwrite(&caplen, 4, 1, s->f); fwrite(&caplen, 4, 1, s->f);
    fwrite(rtap, sizeof(rtap), 1, s->f);
    fwrite(frame, 1, len, s->f);

    /* Live sink (extcap FIFO): flush immediately, otherwise Wireshark sees
     * nothing and we never notice the FIFO being closed as an error. */
    if (s->live) fflush(s->f);
    if (ferror(s->f)) s->broken = 1;
}

/* ------------------------------------------------------------------ */
/* One parser for all                                                  */
/* ------------------------------------------------------------------ */

static long parse_bulk(const rtl_chip_ops *ops, const uint8_t *buf, int total,
                       rtl_frame_ex_cb cb, void *ctx) {
    long n = 0;
    const uint8_t *p = buf;
    int left = total;

    while (left > (int)ops->rxdesc_size) {
        rtl_rx_sub sub;
        if (ops->rx_parse_desc(p, left, &sub) != 0) break;
        if (!sub.is_report) {
            cb(p + sub.hdr_len, sub.pkt_len, &sub, ctx);
            n++;
        }
        if (sub.advance == 0 || (int)sub.advance > left) break;
        p    += sub.advance;
        left -= (int)sub.advance;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Asynchronous engine                                                 */
/* ------------------------------------------------------------------ */

#define RX_BUF_SIZE 32768

typedef struct { long frames, xfers, bytes, timeouts; } rx_stats;

typedef struct {
    libusb_device_handle   *h;
    const rtl_chip_ops     *ops;
    struct libusb_transfer *xfer[RTL_RX_URBS];
    uint8_t                *buf [RTL_RX_URBS];
    int          inflight;
    volatile int stop;
    volatile int fatal;
    volatile int activity;
    rtl_frame_ex_cb cb;
    void        *cbctx;
    rx_stats    *st;
} rx_engine;

static void engine_free(rx_engine *e) {
    for (int i = 0; i < RTL_RX_URBS; i++) {
        if (e->xfer[i]) { libusb_free_transfer(e->xfer[i]); e->xfer[i] = NULL; }
        if (e->buf[i])  { free(e->buf[i]);                  e->buf[i]  = NULL; }
    }
}

static void LIBUSB_CALL xfer_done(struct libusb_transfer *t) {
    rx_engine *e = (rx_engine *)t->user_data;

    switch (t->status) {
    case LIBUSB_TRANSFER_COMPLETED:
        /* cb==NULL: the engine keeps running between two poll calls (URBs
         * stay open), but the last caller's ctx is dead -- discard the data
         * instead of writing through a stack pointer. */
        if (e->cb && e->st && t->actual_length > (int)e->ops->rxdesc_size) {
            e->st->xfers++;
            e->st->bytes  += t->actual_length;
            e->st->frames += parse_bulk(e->ops, t->buffer, t->actual_length,
                                        e->cb, e->cbctx);
            e->activity = 1;   /* the main thread drives the LED -- no sync I/O here */
        }
        break;

    case LIBUSB_TRANSFER_TIMED_OUT:
        if (e->st) e->st->timeouts++;
        break;

    case LIBUSB_TRANSFER_CANCELLED:
        e->inflight--;
        return;

    default:                    /* ERROR / STALL / NO_DEVICE / OVERFLOW */
        e->fatal = 1;
        e->inflight--;
        return;
    }

    if (e->stop) { e->inflight--; return; }
    if (libusb_submit_transfer(t) != 0) { e->fatal = 1; e->inflight--; }
}

static int engine_start(rx_engine *e, libusb_device_handle *h,
                        const rtl_chip_ops *ops, rtl_frame_ex_cb cb, void *ctx,
                        rx_stats *st) {
    memset(e, 0, sizeof(*e));
    e->h = h; e->ops = ops; e->cb = cb; e->cbctx = ctx; e->st = st;

    for (int i = 0; i < RTL_RX_URBS; i++) {
        e->buf[i]  = malloc(RX_BUF_SIZE);
        e->xfer[i] = libusb_alloc_transfer(0);
        if (!e->buf[i] || !e->xfer[i]) { engine_free(e); return -1; }
        /* timeout 0 = unlimited: no timeout churn on quiet channels. */
        libusb_fill_bulk_transfer(e->xfer[i], h, ops->rx_ep,
                                  e->buf[i], RX_BUF_SIZE, xfer_done, e, 0);
    }
    for (int i = 0; i < RTL_RX_URBS; i++)
        if (libusb_submit_transfer(e->xfer[i]) == 0) e->inflight++;

    if (e->inflight == 0) { engine_free(e); return -1; }
    return 0;
}

static void engine_stop(rx_engine *e) {
    e->stop = 1;
    for (int i = 0; i < RTL_RX_URBS; i++)
        if (e->xfer[i]) libusb_cancel_transfer(e->xfer[i]);

    while (e->inflight > 0) {
        struct timeval tv = { 0, 20000 };
        if (libusb_handle_events_timeout_completed(g_ctx, &tv, NULL) < 0) break;
    }
    engine_free(e);
}

/* ms < 0 = forever. abort may be NULL. */
static void engine_run(rx_engine *e, int ms, const volatile int *abort) {
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        /* Never wait longer than the remaining time: otherwise an
         * rtl_rx_poll(h,20,..) always takes >=50ms (alfa-netd bridge latency). */
        long slice_us = 50000;
        if (ms >= 0) {
            struct timespec tn; clock_gettime(CLOCK_MONOTONIC, &tn);
            long el = (tn.tv_sec - t0.tv_sec) * 1000
                    + (tn.tv_nsec - t0.tv_nsec) / 1000000;
            if (el >= ms) break;
            long rest_us = (long)(ms - el) * 1000;
            if (rest_us < slice_us) slice_us = rest_us;
        }
        struct timeval tv = { 0, slice_us };
        if (libusb_handle_events_timeout_completed(g_ctx, &tv, NULL) < 0) break;

        if (e->activity) { e->activity = 0; led_activity(e->h); }
        if (e->fatal) break;
        if (abort && *abort) break;
    }
}

/* ------------------------------------------------------------------ */
/* Synchronous fallback (if rtl_rx_bind was never called)              */
/* ------------------------------------------------------------------ */

static void sync_run(libusb_device_handle *h, const rtl_chip_ops *ops, int ms,
                     rtl_frame_ex_cb cb, void *ctx, const volatile int *abort,
                     rx_stats *st) {
    uint8_t *buf = malloc(RX_BUF_SIZE);
    if (!buf) return;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);

    for (;;) {
        if (abort && *abort) break;
        if (ms >= 0) {
            struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
            long el = (t1.tv_sec - t0.tv_sec) * 1000
                    + (t1.tv_nsec - t0.tv_nsec) / 1000000;
            if (el >= ms) break;
        }
        int got = 0;
        int rc = libusb_bulk_transfer(h, ops->rx_ep, buf, RX_BUF_SIZE, &got, 100);
        if (rc == LIBUSB_ERROR_TIMEOUT) { st->timeouts++; continue; }
        if (rc != 0) break;
        if (got > (int)ops->rxdesc_size) {
            st->xfers++; st->bytes += got;
            st->frames += parse_bulk(ops, buf, got, cb, ctx);
            led_activity(h);
        }
    }
    free(buf);
}

/* ------------------------------------------------------------------ */
/* Persistent engine: start once, keep open across all calls          */
/* ------------------------------------------------------------------ */

static rx_engine g_eng;
static int       g_eng_live = 0;

/* Common entry point: picks async if bound, otherwise sync. */
static void rx_run(libusb_device_handle *h, int ms, rtl_frame_ex_cb cb, void *ctx,
                   const volatile int *abort, rx_stats *st) {
    const rtl_chip_ops *ops = ops_or_default();
    if (!g_ctx) { sync_run(h, ops, ms, cb, ctx, abort, st); return; }

    if (!g_eng_live) {
        if (engine_start(&g_eng, h, ops, cb, ctx, st) != 0) {
            sync_run(h, ops, ms, cb, ctx, abort, st);
            return;
        }
        g_eng_live = 1;
    } else {
        /* URBs stay in flight -- only swap the sink and counters. */
        g_eng.h = h; g_eng.ops = ops;
        g_eng.cb = cb; g_eng.cbctx = ctx; g_eng.st = st;
    }

    engine_run(&g_eng, ms, abort);

    g_eng.cb = NULL; g_eng.cbctx = NULL; g_eng.st = NULL;
    if (g_eng.fatal) { engine_stop(&g_eng); g_eng_live = 0; }
}

void rtl_rx_unbind(void) {
    if (g_eng_live) { engine_stop(&g_eng); g_eng_live = 0; }
    g_ctx = NULL; g_ops = NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

long rtl_rx_poll_ex(libusb_device_handle *h, int ms, rtl_frame_ex_cb cb, void *ctx) {
    rx_stats st = {0};
    rx_run(h, ms, cb, ctx, NULL, &st);
    return st.frames;
}

/* rtl_frame_cb adapter: the engine drops cb/ctx after each run, so a
 * stack-allocated wrapper is safe. */
typedef struct { rtl_frame_cb cb; void *ctx; } plain_cb;

static void plain_adapter(const uint8_t *frame, uint32_t len,
                          const rtl_rx_sub *rx, void *v) {
    (void)rx;
    const plain_cb *p = (const plain_cb *)v;
    p->cb(frame, len, p->ctx);
}

long rtl_rx_poll(libusb_device_handle *h, int ms, rtl_frame_cb cb, void *ctx) {
    plain_cb p = { cb, ctx };
    return rtl_rx_poll_ex(h, ms, plain_adapter, &p);
}

long rtl_rx_pump(libusb_device_handle *h, FILE *f, int ms) {
    pcap_sink s = { f, 0, 0 };
    rx_stats st = {0};
    rx_run(h, ms, pcap_cb, &s, &s.broken, &st);
    return st.frames;
}

long rtl_rx_stream(libusb_device_handle *h, FILE *f) {
    pcap_sink s = { f, 0, 1 };      /* live=1: flush the FIFO immediately */
    rx_stats st = {0};
    rx_run(h, -1, pcap_cb, &s, &s.broken, &st);
    return st.frames;
}

long rtl_rx_capture(libusb_device_handle *h, FILE *f, int seconds, int verbose) {
    pcap_sink s = { f, 0, 0 };
    rx_stats st = {0};

    rx_run(h, seconds * 1000, pcap_cb, &s, &s.broken, &st);
    fflush(f);

    if (verbose) {
        printf("\n  DIAGNOSTICS: Transfers=%ld  Bytes=%ld  Frames=%ld  Timeouts=%ld\n",
               st.xfers, st.bytes, st.frames, st.timeouts);
        printf("  Path: %s (%d URBs in flight)\n",
               g_ctx ? "asynchronous" : "synchronous (rtl_rx_bind missing)",
               g_ctx ? RTL_RX_URBS : 1);
        if (st.xfers == 0)
            printf("  NO raw bytes received from the chip (%#04x returns nothing).\n",
                   ops_or_default()->rx_ep);
    }
    return st.frames;
}