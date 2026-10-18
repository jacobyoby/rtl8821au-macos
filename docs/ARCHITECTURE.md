# Architecture

## Why userspace

On Apple Silicon there is no workable kernel-driver path for a USB Wi-Fi chip:

- Third-party kexts need Reduced Security and, unsigned, SIP disabled, and the
  `IO80211Family` interfaces they would plug into are private.
- DriverKit offers `USBDriverKit` and an Ethernet-only `NetworkingDriverKit`,
  but no 802.11 family, and the networking entitlement is Apple-approved.

None of that is needed to drive the chip itself. Monitor mode, injection and
even a WPA2 station are register writes, a firmware upload and raw 802.11
frames on USB bulk endpoints. macOS ships no driver for these chips, so the
USB interface is unclaimed and any userspace program can open it with libusb.
The result runs on a fully locked-down Mac: no SIP change, no reduced
security, no signing or notarization.

The one thing userspace cannot do without privileges is create a network
interface. `alfa-netd` uses a `utun` tunnel (root-only) and routes IP packets
between it and the encrypted 802.11 link.

## Layers

```
  USB adapter (RTL8812AU or RTL8821AU)
        │  control: registers, firmware   bulk IN: RX   bulk OUT: TX
        ▼
  ┌──────────────────────────────────────────────────────────────┐
  │ rtl_usb      vendor-request register I/O, device open        │
  │ rtl_ids      USB VID:PID -> chip family                      │
  │ rtl_chip     chip ops table: one instance per chip family    │
  │   ├─ RTL8812AU: rtl_init, rtl_fw, rtl_mac, rtl_bb, rtl_rf,   │
  │   │             rtl_cal, rtl_txpwr                           │
  │   └─ RTL8821AU: rtl8821au (power, MAC), rtl8821au_phy (BB,   │
  │                 RF, channel), rtl8821au_cal (IQK, gain),     │
  │                 rtl8821au_txpwr, rtl8821au_tables            │
  │ rtl_hal      chip-independent bring-up order over the ops    │
  │ rtl_rx       async bulk-IN engine, RX descriptor parsing     │
  │ rtl_tx       TX descriptor, bulk-OUT                          │
  │ rtl_ccmp     AES-CCM for WPA2 data frames                    │
  │ rtl_wpa      auth, association, 4-way handshake, clean leave │
  └──────────────────────────────────────────────────────────────┘
        │                                    │
        ▼                                    ▼
  pcap / Wireshark extcap            alfa-netd: utun <-> 802.11 + CCMP
```

## The chip ops table

Every chip-specific step lives behind `rtl_chip_ops` in
[`src/rtl_chip.h`](../src/rtl_chip.h): power-on, firmware download, MAC, BB
and RF init, channel switch, monitor filter, calibration, TX power, LEDs, and
the data-path geometry (RX and TX endpoints, descriptor sizes, RX descriptor
parser, TX inject).

`rtl_hal_full_init()` runs those steps in a fixed order for whichever chip
`rtl_chip_probe(vid, pid)` returns. Tools never call chip-specific functions
directly; they go through the bound table, for example
`rtl_rx_bound_ops()->tx_ep_mgmt` for the management endpoint.

Adding a chip means: a USB ID block in `rtl_ids.c`, a new `rtl_chip_ops`
instance, and its init code and tables. The RTL8821AU port is the worked
example; see [RTL8821AU.md](RTL8821AU.md).

## Bring-up order

1. Power-on sequence (chip power state machine), LLT table
2. Firmware download to the on-chip 8051, wait for ready
3. MAC init: register table, TX queue pages and priorities, WMAC filters,
   EDCA, beacon timing, USB burst settings
4. Baseband and AGC tables, crystal capacitance
5. RF tables, band switch
6. Channel and bandwidth
7. TX power from the efuse calibration, capped by regulatory limits
8. Optional calibration (IQK, and LCK on the RTL8812AU)
9. Receive filter: promiscuous for monitor mode, or station mode after
   `rtl_wpa_connect()`

`stagetest <stage>` runs these one at a time for porting work.

## Station mode

`rtl_wpa_connect()` sends open authentication, an association request with
an RSN element for CCMP and PSK, then performs the 4-way handshake with
PBKDF2 and the PRF from CommonCrypto. It installs the pairwise and group keys
in software; frames are encrypted and decrypted by `rtl_ccmp`, not by the
chip. `alfa-netd` then runs DHCP over the encrypted link, creates the `utun`,
answers ARP for its address and forwards IP in both directions.
