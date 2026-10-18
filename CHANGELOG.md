# Changelog

All notable changes to this fork are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- **RTL8821AU / RTL8811AU support**, ported from Linux `rtw88`: power-on,
  firmware, MAC/BB/RF init, 2.4 GHz channel switch, IQK, efuse TX power with
  regulatory limits, monitor mode, injection, scan, WPA2 station mode.
- USB ID table (`src/rtl_ids.c`) keyed by vendor and product, covering the
  rtw88 RTL8821AU list (TP-Link, D-Link, Netgear, Edimax, Buffalo, ELECOM and
  others).
- `scripts/run-netd.sh` and `alfa-netd --psk-stdin`: read the Wi-Fi password
  from the macOS Keychain.
- `rtl_wpa_leave()`: deauthenticate cleanly on exit.
- Diagnostics: `txtest`, `wpatest`, extra flags for `associate`, handshake
  tracing in `rtl_wpa`, beacon TIM and keepalive in `alfa-netd`.
- `make check` for the hardware-free self-tests, and a GitHub Actions build.
- Documentation in `docs/`.
- Comments and program output translated from German to English, except
  `src/deauth.c`.

### Changed

- The chip ops table carries a chip id; `rtl_chip_probe()` takes vendor and
  product ID.
- Tools send management frames on the bound chip's endpoint instead of the
  RTL8812AU constant.
- TX uses software sequence numbers instead of hardware sequencing.
- `alfa-netd`: the unencrypted ARP test is opt-in (`ALFA_PLAIN_TEST=1`).
- Handshake message 4 is re-sent for every copy of message 3.
- ARP replies are sent from the main loop, not from the RX callback.

### Fixed

- Realtek PIDs `0811`, `0820`, `0823` and `a811` were listed as RTL8812AU;
  per rtw88 they are RTL8821AU parts.

## Upstream baseline

This fork starts from
[janlueders/rtl8812au-macos](https://github.com/janlueders/rtl8812au-macos)
at commit `f08da13`: RTL8812AU monitor mode, injection, Wireshark extcap and
the WPA2 internet client.
