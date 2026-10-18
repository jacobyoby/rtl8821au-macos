/*
 * rtl_wpa — WPA2-PSK-Verbindung (Auth + Assoc + 4-Way) als wiederverwendbares
 * Modul. Liefert die ausgehandelten Schluessel. Von connect/alfa-netd genutzt.
 */
#ifndef RTL_WPA_H
#define RTL_WPA_H

#include <stdint.h>
#include <libusb.h>

typedef struct {
    uint8_t sa[6];      /* eigene MAC */
    uint8_t bssid[6];
    uint8_t tk[16];     /* Pairwise Temporal Key (Unicast-CCMP) */
    uint8_t gtk[16];    /* Group Temporal Key (Broadcast-CCMP) */
    uint8_t kck[16];
    uint8_t kek[16];
    int     have_gtk;
    int     aid;        /* association ID from the Assoc Response (for the beacon TIM) */
} wpa_keys_t;

/* Vollstaendige WPA2-PSK-Verbindung auf 'channel' mit 'bssid'/'ssid'/'psk'.
 * Setzt Chip-Init voraus NICHT — macht rtl_hal_full_init selbst. Nach Erfolg
 * ist der Chip assoziiert, REG_MACID gesetzt, Schluessel in *k.
 * Rueckgabe 0 ok, <0 Fehler (verbose druckt Fortschritt). */
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
