# alfa-driver — RTL8812AU native on macOS Apple Silicon (userspace)

Monitor mode + packet injection with the **Alfa AWUS036ACH (RTL8812AU)**
**natively on macOS (Apple Silicon, M-series)** — no VM, no Linux, no kext,
no DriverKit. Pure userspace over libusb.

> For authorized WLAN auditing, CTFs, and security research on networks you own
> or have permission to test.

## Why userspace instead of a driver

On macOS Apple Silicon there is **no** viable kernel-driver path for a USB Wi-Fi
device:

- Kexts are effectively gone; the `IO80211` KPIs are private/undocumented.
- DriverKit offers `USBDriverKit` and (Ethernet-only) `NetworkingDriverKit`, but
  **no 802.11/Wi-Fi family**. Apple's Wi-Fi stack is private.

Monitor mode and injection do not need that OS integration. On the RTL8812AU they
are **chip functions**: configure registers, then send/receive raw 802.11 frames
over the USB bulk endpoints. A normal macOS userspace program does all of that
itself via **libusb**.

Upside: runs on a fully locked-down Apple Silicon Mac — no SIP disable, no reduced
security, no signing/notarization required. macOS ships no RTL8812AU driver, so
the device is free for userspace to claim.

## Architecture

```
  Alfa AWUS036ACH (RTL8812AU)
        │  USB (bulk-in = RX, bulk-out = TX, control = registers/firmware)
        ▼
  ┌────────────────────────────────────────────┐
  │  alfa-driver (arm64 macOS, libusb)          │
  │  rtl_usb   USB vendor-request register I/O   │
  │  rtl_init  power-on sequence (card enable)   │
  │  rtl_fw    firmware download                 │
  │  rtl_efuse efuse read (real MAC)             │
  │  rtl_mac   MAC init + monitor RCR            │
  │  rtl_bb    baseband/PHY init                 │
  │  rtl_rf    RF init + channel (2.4/5 GHz)     │
  │  rtl_cal   IQK/LCK calibration               │
  │  rtl_txpwr efuse-calibrated TX power         │
  │  rtl_rx    bulk-IN → 802.11 + radiotap        │
  │  rtl_tx    802.11 + TX descriptor → bulk-OUT  │
  │  rtl_ccmp  AES-CCM (WPA2 data crypto)         │
  │  rtl_wpa   WPA2-PSK connect (auth/assoc/4-way)│
  └────────────────────────────────────────────┘
        │
        ▼
  Wireshark / Kismet / aircrack (pcap)   |   utun (internet client)
```

## What works (verified on hardware: Alfa AWUS036ACH, M2 Pro, macOS 26.6.2)

- **Device bring-up:** claim device (no kext/SIP), register R/W, power-on,
  efuse read (real ALFA MAC), firmware download (checksum OK, firmware running).
- **MAC/BB/RF init + channel** on 2.4 and 5 GHz (tables byte-identical to the
  Linux reference).
- **Monitor mode:** captures real 802.11 traffic to pcap+radiotap. Verified on
  channel 6 and channel 100 (beacons, data, ACK/Block-ACK), decoded by tcpdump.
- **Packet injection:** verified via probe-request → probe-response to our MAC
  from the AP.
- **TX power:** read from the adapter's own efuse calibration and applied at
  init (the AWUS036ACH's external PA/LNA front-end on both bands is detected
  from efuse and correctly engaged — this, not just the digital gain index,
  is what actually gets the adapter to its rated output power).
- **Wireshark integration** via extcap (`alfa-extcap`).
- **Status LED** (register `LEDCFG0`): on after init, blinks on traffic.

### Internet client — working

`alfa-netd` connects as a real WPA2 station and bridges IP through a `utun`
interface, so the Mac gets full internet access through the Alfa adapter.
Verified end-to-end on real hardware: WPA2-PSK 4-way handshake, CCMP
encrypt/decrypt of live traffic, DHCP, routing, and sustained 0% packet loss
pinging the public internet. You only need the network's SSID — `alfa-netd`
scans 2.4GHz for it and finds the channel/BSSID itself. Note: because it uses
`utun`, it appears as a tunnel, not as an Ethernet adapter in Network settings
(that would require the entitlement-gated DriverKit path). 5GHz networks and
WPA3/SAE are not supported yet.

## Tools

| Tool | Purpose |
|------|---------|
| `alfa-netd` | **internet-client daemon** — connect to WPA2 Wi-Fi and get real internet access (needs sudo) |
| `scan`      | scan 2.4/5 GHz for nearby networks |
| `monitor`   | monitor-mode capture to pcap |
| `inject`    | packet injection (probe-request test) |
| `alfa-extcap` | Wireshark extcap capture source |
| `connect`   | WPA2-PSK connect + handshake test (no internet bridging) |
| `deauth`    | send 802.11 deauthentication frames (same technique as `aireplay-ng --deauth`) — **only against networks you're authorized to test** |
| `efuseinfo` | read efuse → real MAC, TX power calibration, PA/LNA/frontend info |
| `usbprobe`  | find/claim the device, dump endpoints |
| `chipinfo`  | read/decode chip version |
| `fwload`    | download firmware |

## Quick start (just want internet through the Alfa adapter?)

1. **Install prerequisites** (once):
   ```bash
   xcode-select --install        # Xcode Command Line Tools (provides cc, make)
   brew install libusb           # if you don't have Homebrew: https://brew.sh
   ```
2. **Get the code and build it:**
   ```bash
   git clone https://github.com/janlueders/rtl8812au-macos.git
   cd rtl8812au-macos
   make
   ```
3. **Plug in the Alfa AWUS036ACH**, then connect to your Wi-Fi by name:
   ```bash
   sudo ./alfa-netd "MyNetworkName" --default
   ```
   You'll be asked for your Wi-Fi password. `alfa-netd` finds the channel and
   BSSID itself, connects, and bridges internet through the adapter.
   `--default` makes it your Mac's default route (and briefly turns off
   Apple's own Wi-Fi so the two radios don't interfere on the same channel —
   it's restored automatically when you stop the daemon).
4. **Stop it any time with Ctrl-C** — it always restores your normal
   networking (routes, Apple Wi-Fi) on exit, even if the connection failed.

If you just want monitor mode / packet capture instead of internet access, see
the `monitor`/`inject`/`alfa-extcap`/`scan` tools above — no `sudo`-daemon or
`--default` needed for those.

## Build & use (other tools)

```bash
make                       # builds all tools (arm64, against libusb)
./monitor 6 15 out.pcap    # monitor mode: channel 6, 15s -> pcap (Wireshark/tshark)
./inject 6 30              # injection: 30 probe requests on channel 6
./scan                     # list nearby networks
```

For Wireshark, link the extcap helper:

```bash
mkdir -p ~/.config/wireshark/extcap
ln -sf "$(pwd)/alfa-extcap" ~/.config/wireshark/extcap/alfa-extcap
```

## License & origin

Register definitions, power-on/init sequences, tables (MAC/BB/RF), efuse decoding,
and the embedded firmware are **ported from** the Linux kernel driver
<https://github.com/aircrack-ng/rtl8812au> (originally © Realtek Corporation,
**GPL v2**). This project is a derivative work and is therefore also licensed under
**GPL v2** (see `LICENSE`). The Linux driver itself is not redistributed here; it
is used only as a porting reference.

No kext, no DriverKit, no Anthropic/Apple affiliation — pure userspace USB over
libusb.

## Honest note

This is a real reverse-engineering effort (reimplementing chip logic from the
Linux source), not a weekend project. But every milestone is independently
testable, and nothing about it is blocked on Apple Silicon — it is all userspace
USB work.

---

## RTL8821AU port (TP-Link Archer T2U Plus and friends) — work in progress

This fork adds a second chip to the ops table: the **RTL8821AU / RTL8811AU**
(1x1 802.11ac, USB 2.0), as found in the TP-Link Archer T2U Plus (`2357:0120`),
Archer T2U Nano, D-Link DWA-171, Netgear A6100, Edimax EW-7811UTC and others.
The full USB ID list is in `src/rtl_ids.c` (taken from the Linux `rtw88`
driver's `rtw8821au` table).

Reference for the port is Linux `rtw88` (`rtw8821a.c`, `rtw8821a_table.c`,
`rtw88xxa.c`), not the old aircrack-ng vendor driver.

### What works today (verified on an Archer T2U Plus, Apple Silicon, macOS 27)

| Step | Status |
|---|---|
| USB ID match, claim in userspace (`usbprobe`) | works |
| Register access, chip version read (`chipinfo`) | works |
| Hardware reset + power-on sequence (`initchip`) | works, both polling steps pass |
| 8051 firmware upload, `rtw8821a_fw.bin` (`fwload`) | works, checksum OK, firmware ready |
| LLT table, MAC init: register table, queue pages, priorities, WMAC, EDCA, beacon, USB burst, ARFB (`stagetest 3`) | works, `CR=0x06ff`, `TXDMA_PQ_MAP=0xc5a0` |
| Efuse decode: MAC, crystal cap, PA/LNA options, BB swing (`rtl8821a_board_info`) | works |
| BB + AGC tables with the rtw88 condition parser, crystal cap (`stagetest 4`) | works |
| RF path A table, band switch to 2.4 GHz, post-RF MAC bits (`stagetest 5`) | works, `RF_A[0x18]` reads back the table value |
| Channel switch 20 MHz, monitor RCR (`stagetest 7`) | works |
| **Monitor-mode capture on 2.4 GHz (`monitor 6 10 out.pcap`)** | **works**: 4,340 frames in 10 s on channel 6, beacons with SSIDs, probe responses, RTS/CTS, block-acks. 42% FCS-clean (the RCR accepts CRC-error frames on purpose; no calibration yet) |
| LCK / IQK calibration | stubbed, returns 0 so RX-only bring-up continues |
| TX power from efuse, injection, scan / connect / internet | **not ported yet** |

Unported steps print `[8821au] <step>: not ported yet`. Calibration is skipped
with a message; TX power apply returns `ENOSYS` (it is a best-effort step).

### Layout of the port

- `src/rtl_ids.[ch]` — VID:PID -> chip family. Small enough for the probe tools
  to link without the chip core.
- `src/rtl_chip.[ch]` — `rtl_chip_ops` now carries a chip id instead of a PID
  list; `rtl_chip_probe(vid, pid)` picks the ops table.
- `src/rtl8821au.c` — the 8821AU ops: hw reset, power sequences, CR enable,
  LLT, firmware download, MAC init (MAC table generated from rtw88's
  `rtw8821a_table.c`); checklist at the top.
- `src/rtl8821au_phy.c` — efuse board decode, RF path A read/write (8821A
  timing), the `rtw_parse_tbl_phy_cond` condition parser, BB/AGC/RF table
  load, `switch_band`, `set_channel` (20 MHz path exercised, 40/80 carried
  but untested), post-RF MAC bits.
- `src/rtl8821au_tables.c` — `rtw8821a_agc`, `rtw8821a_bb`, `rtw8821a_rf_a`
  generated from rtw88 (comments stripped, `0X` prefixes handled).
- `src/fw_8821a_nic.c` — `rtw88/rtw8821a_fw.bin` from linux-firmware as a C
  array (sha256 in the file header; Realtek redistributable firmware licence).
- `src/rtl_init.c` — the power-sequence interpreter is now generic
  (`rtl_pwr_seq_run`), `src/rtl_fw.c` can load any 8051 image
  (`rtl_fw_download_blob`).
- `rtl_open_first_ex()` matches any vendor ID in the table; every tool prints
  the real `vid:pid`.

### Endpoints on the 8821AU

The USB 2.0 part has a different endpoint layout from the AWUS036ACH:
bulk IN `0x84`, bulk OUT `0x05 0x06 0x08 0x09`, interrupt IN `0x87`.
Per `rtw88` the four OUT pipes are high/beacon, VO/VI, BE/BK and management
(`REG_TXDMA_PQ_MAP = 0xc5a0`).
`rtl8821au_ops` carries that; the tools still pass the 8812AU macro
`RTL_TX_EP_MGMT` directly and need to switch to `ops->tx_ep_mgmt`.

### Next steps

1. Calibration (`rtw8821a_phy_calibration`: LCK + IQK) and the initial
   gain / DIG setup, which should raise the FCS-clean rate.
2. Efuse TX power (`rtw88xxa_set_tx_power_index`, `rtw8821a_txpwr_lmt`) and
   a TX descriptor check for the 1x1 part, then `inject` and `scan`.
3. Switch the tools from the `RTL_TX_EP_MGMT` macro to `ops->tx_ep_mgmt`
   (0x09 on this part), then `connect` / `alfa-netd`.
4. 5 GHz: `switch_band(5G)` is ported but untested.
