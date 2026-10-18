/*
 * rtl_wpa — WPA2-PSK connection (auth + assoc + 4-way) as a reusable module.
 * Returns the negotiated keys. Used by connect/alfa-netd.
 */
#ifndef RTL_WPA_H
#define RTL_WPA_H

#include <stdint.h>
#include <libusb.h>

typedef struct {
    uint8_t sa[6];      /* own MAC */
    uint8_t bssid[6];
    uint8_t tk[16];     /* Pairwise Temporal Key (unicast CCMP) */
    uint8_t gtk[16];    /* Group Temporal Key (broadcast CCMP) */
    uint8_t kck[16];
    uint8_t kek[16];
    int     have_gtk;
    int     aid;        /* association ID from the Assoc Response (for the beacon TIM) */
} wpa_keys_t;

/* Complete WPA2-PSK connection on 'channel' with 'bssid'/'ssid'/'psk'.
 * Does NOT require prior chip init — calls rtl_hal_full_init itself. On
 * success the chip is associated, REG_MACID is set, keys are in *k.
 * Returns 0 ok, <0 error (verbose prints progress). */
int rtl_wpa_connect(libusb_device_handle *h, int channel,
                    const uint8_t bssid[6], const char *ssid, const char *psk,
                    wpa_keys_t *k, int verbose);

/* Leave the AP cleanly: deauthenticate our own association (reason 3,
 * "station is leaving"). Without this the AP keeps our MAC in state 3 and
 * answers the NEXT connect's auth/assoc with deauth reason 1/6 (observed on
 * a consumer AP: every second connect failed). Call on every exit path
 * after rtl_wpa_connect() succeeded (or at least authenticated). */
void rtl_wpa_leave(libusb_device_handle *h);

#endif /* RTL_WPA_H */
