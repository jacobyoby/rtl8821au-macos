# rtl8821au-macos

**Realtek USB Wi-Fi adapters on Apple Silicon Macs, in userspace.**
Monitor mode, packet injection and a WPA2 internet client for RTL8812AU and
RTL8821AU adapters, with no kext, no DriverKit, no SIP changes and no VM.

[![Build](https://github.com/jacobyoby/rtl8821au-macos/actions/workflows/build.yml/badge.svg)](https://github.com/jacobyoby/rtl8821au-macos/actions/workflows/build.yml)
![Platform](https://img.shields.io/badge/platform-macOS%20arm64-lightgrey)
[![License: GPL v2](https://img.shields.io/badge/license-GPL--2.0-blue)](LICENSE)

> For authorized Wi-Fi testing, CTFs and research on networks you own or have
> permission to test. See [SECURITY.md](SECURITY.md).

macOS on Apple Silicon has no driver for these chips and no supported way to
write one: kexts need reduced security, and DriverKit has no Wi-Fi family.
Everything a USB Wi-Fi chip needs, though, is register writes, firmware upload
and raw 802.11 frames over USB bulk endpoints. This project does all of that
from a normal userspace program over libusb, then bridges IP traffic to macOS
through a `utun` tunnel interface.

This is a fork of [janlueders/rtl8812au-macos](https://github.com/janlueders/rtl8812au-macos),
which supports the RTL8812AU. The fork adds a second chip family, the
**RTL8821AU / RTL8811AU**, ported from the Linux `rtw88` driver.

## Supported hardware

| Chip | Example adapters | Status |
|---|---|---|
| RTL8812AU | Alfa AWUS036ACH | Upstream: monitor, injection, WPA2 internet client |
| RTL8821AU / RTL8811AU | TP-Link Archer T2U Plus, Archer T2U Nano, Archer T2U v3, D-Link DWA-171, Netgear A6100, Edimax EW-7811UTC | Experimental: see below |

The full USB ID list is in [`src/rtl_ids.c`](src/rtl_ids.c). If your adapter
uses one of these chips but is not listed, please
[open an adapter report](../../issues/new?template=adapter_report.yml).

### RTL8821AU feature status

Verified on a TP-Link Archer T2U Plus (`2357:0120`), Apple Silicon, macOS 27.

| Feature | Status |
|---|---|
| Probe, power-on, firmware, MAC/BB/RF init | Works |
| Monitor capture, 2.4 GHz | Works, about 47% of frames FCS-clean |
| Packet injection | Works: 30 probe requests drew 94 probe responses |
| Scan, 2.4 GHz | Works |
| Open auth and association | Works, 5 of 5 |
| WPA2-PSK handshake, DHCP, gateway ARP | Works, including WPA2/WPA3 transition networks |
| IP traffic through `utun` | Partial: about 58% of pings answered at 2 to 26 ms |
| 5 GHz, WPA3-SAE, HT/VHT rates | Not yet |

The remaining packet loss is a link-quality problem, not a protocol one. The
details and next steps are in [docs/RTL8821AU.md](docs/RTL8821AU.md).

## Quick start

**1. Install the prerequisites** (once):

```bash
xcode-select --install
```

```bash
brew install libusb
```

**2. Build:**

```bash
git clone https://github.com/jacobyoby/rtl8821au-macos.git
cd rtl8821au-macos
make
```

**3. Check that the adapter is found and claimable:**

```bash
./usbprobe
```

You should see your adapter's chip family and `claim if0: OK`.

**4. Capture in monitor mode** (channel 6, 10 seconds, with calibration):

```bash
./monitor 6 10 capture.pcap 1
```

Open `capture.pcap` in Wireshark. For live capture inside Wireshark, see
[Wireshark integration](#wireshark-integration).

**5. Connect to a WPA2 network.** Store the Wi-Fi password in your login
Keychain once; it prompts for the password and never writes it to a file:

```bash
security add-generic-password -s alfa-netd -a "MyNetwork" -w
```

Then start the client. `sudo` asks for your Mac login password:

```bash
./scripts/run-netd.sh "MyNetwork"
```

It scans for the network, connects, runs DHCP and brings up a `utun`
interface. Add `--default` to route all traffic through the adapter. Stop it
with Ctrl-C; it leaves the network cleanly and restores your routes.

## Tools

| Tool | Purpose | Needs sudo |
|---|---|---|
| `scripts/run-netd.sh` | WPA2 client with the password from the Keychain | yes |
| `alfa-netd` | WPA2 client and IP bridge through `utun` | yes |
| `scan` | List nearby networks | no |
| `monitor` | Monitor-mode capture to pcap with radiotap | no |
| `alfa-extcap` | Wireshark extcap capture source | no |
| `inject` | Injection test: probe requests and responses | no |
| `deauth` | Send deauthentication frames, **only on networks you may test** | no |
| `associate` | Open auth and association test, with flags for bisecting | no |
| `connect` | WPA2 handshake test without IP bridging | no |
| `usbprobe`, `chipinfo`, `efuseinfo` | Find the device, read chip version and efuse | no |
| `stagetest` | Bring-up stage by stage, for porting work | no |
| `txtest`, `wpatest` | TX path and handshake diagnostics | no |

`sudo` is only needed to create the `utun` interface and change routes; see
[Troubleshooting](docs/TROUBLESHOOTING.md#why-sudo) for why, and how to use
Touch ID instead of typing your password.

## Wireshark integration

```bash
mkdir -p ~/.config/wireshark/extcap
```

```bash
ln -sf "$(pwd)/alfa-extcap" ~/.config/wireshark/extcap/alfa-extcap
```

## Documentation

- [Architecture](docs/ARCHITECTURE.md): why userspace, how the pieces fit, the chip ops table.
- [RTL8821AU port](docs/RTL8821AU.md): status, file layout, endpoints, lessons learned, next steps.
- [Troubleshooting](docs/TROUBLESHOOTING.md): sudo, Keychain, deauth reason codes, log messages.
- [Background research](docs/RESEARCH.md): why there is no driver for Apple Silicon, and the routes compared.
- [Changelog](CHANGELOG.md) and [Contributing](CONTRIBUTING.md).

## Limitations

- The connection shows up as a tunnel, not as a Wi-Fi or Ethernet adapter in
  System Settings. A real network interface would need an Apple-approved
  DriverKit entitlement.
- RTL8821AU: 2.4 GHz only so far, legacy rates only, no WPA3-SAE.
- Throughput is limited by doing CCMP encryption in software and by the
  current link quality on the RTL8821AU.

## Credits and license

- RTL8812AU support: [Jan Lüders](https://github.com/janlueders), ported from
  [aircrack-ng/rtl8812au](https://github.com/aircrack-ng/rtl8812au) (Realtek, GPL-2.0).
- RTL8821AU support: ported from the Linux kernel's `rtw88` driver by Bitterblue
  Smith and Realtek (GPL-2.0 OR BSD-3-Clause).
- Firmware: `rtw8821a_fw.bin` and the RTL8812AU NIC firmware are © Realtek,
  redistributable under [LICENCE.rtlwifi_firmware.txt](LICENCE.rtlwifi_firmware.txt).

The code is licensed under the [GNU GPL v2](LICENSE). Not affiliated with
Apple, Realtek, TP-Link or Alfa Network.
