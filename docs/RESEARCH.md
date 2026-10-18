# Background research

Why there is no driver for the TP-Link Archer T2U Plus (RTL8821AU) on Apple
Silicon Macs, the three possible routes, and why this project took the
userspace route. Written on 2026-10-17, before the port started; for the
current state see [RTL8821AU.md](RTL8821AU.md).

## Bottom line

A homegrown driver is feasible, and the most realistic path is **not** a kernel extension.
A brand-new open-source project, `rtl8812au-macos`, already runs the sibling RTL8812AU chip
entirely in userspace on Apple Silicon macOS 26 with WPA2 station mode and internet access,
and its author explicitly structured the code so that an 8821AU port only needs a second
chip-ops table plus its register tables. Extending that project is the shortest route to a
working adapter in macOS itself. Expect weeks of focused work, not months, if you take that
route. A from-scratch kernel extension remains a months-long project with security trade-offs.

## What the hardware is

| Item | Value |
|---|---|
| USB ID | `2357:0120` (TP-Link vendor ID, Archer T2U Plus product ID) |
| Chip | Realtek RTL8821AU, 1x1 802.11ac, USB 2.0 high speed |
| USB string | "Realtek 802.11ac WLAN Adapter", serial `00e04c000001` (generic) |
| Interface class | `0xFF` vendor-specific, one interface, bulk IN/OUT plus control |
| Driver bound on macOS | Only `AppleUSBHostCompositeDevice`. Nothing claims interface 0. |

Because nothing claims the interface, a normal userspace process can open it through libusb
or IOUSBHost without any security changes. That is the foundation every userspace project
below relies on.

## Why no driver exists today

- TP-Link's newest Mac driver targets macOS 10.15 (dated 2020). Nothing for macOS 11+ or arm64.
- Realtek's macOS kernel extensions are x86_64 binaries. They cannot load on the arm64 kernel
  and Rosetta does not translate kexts.
- The community `chris1111/Wireless-USB-Big-Sur-Adapter` package supports Tahoe 26 but only
  on Intel Macs. Its README says it does not work on M1 through M4, and it does not list the
  RTL8821AU at all. It also requires fully disabling System Integrity Protection.
- Apple's DriverKit has `USBDriverKit` and `NetworkingDriverKit`, but NetworkingDriverKit is
  Ethernet-only. There is no 802.11 DriverKit family, and the Ethernet dext entitlement is
  Apple-approval-gated. So there is no sanctioned way to ship a Wi-Fi driver on Apple Silicon.

## The three routes

### Route A: Kernel extension (itlwm-style fake Ethernet)

How itlwm does it for Intel cards: the kext ports the Linux/OpenBSD driver into IOKit and
presents the card through Apple's public `IOEthernetController` class instead of the private
`IO80211Family`. macOS sees a wired Ethernet port. A userspace menu-bar app (HeliPort) talks
to the kext through an `IOUserClient` to scan, pick a network, and enter the password. The
802.11 state machine runs inside the kext.

What it costs on Apple Silicon:
- Boot to Recovery, set Reduced Security, enable "Allow user management of kernel extensions
  from identified developers".
- A kext from an identified developer needs a Developer ID with the kext-signing entitlement,
  which Apple grants on request. An unsigned or ad-hoc kext additionally needs SIP disabled.
- Third-party kexts load only from the Auxiliary Kernel Collection, rebuilt by `kmutil` and
  activated on the next reboot. Every macOS update can invalidate it.
- Apple has deprecated kexts in favor of system extensions. No sunset date, but no future.
- Nobody has ported any Realtek USB Wi-Fi driver to an arm64 kext. You would be first, with
  no reference implementation for the USB transport inside IOKit.

Verdict: technically possible, heavy, and it permanently lowers the Mac's security posture.

### Route B: Userspace driver over libusb (extend `rtl8812au-macos`)

Project: https://github.com/janlueders/rtl8812au-macos (GPL-2.0, C, ~490 KB of source,
created September 2026, single author, 0 stars, last push 2026-09-21).

What it already does, verified by its author on an M2 Pro running macOS 26.6.2:
- Claims the device over libusb. Register I/O via vendor control transfers, firmware upload,
  bulk IN for RX, bulk OUT for TX. No kext, no SIP change, no signing, no DriverKit.
- Monitor mode with radiotap pcap output, packet injection, scanning, a Wireshark extcap.
- Station mode: `alfa-netd` does the WPA2-PSK 4-way handshake and CCMP in software
  (`rtl_wpa.c`, `rtl_ccmp.c`), gets a DHCP lease, opens a `utun` interface, and routes the
  Mac's default route through it. Needs `sudo` for utun and routing.
- Chip abstraction: `src/rtl_chip.h` defines an `rtl_chip_ops` table. The header comment
  (in German) says a further chip such as the 8811AU/8821AU needs only a second instance of
  `rtl_chip_ops` plus its own table modules. The probe function matches on USB product ID;
  today the only entry is the 8812AU with PIDs 0x8812, 0x881a, 0x881b, 0x881c, 0x8813.

What an 8821AU port needs:
1. A second `rtl_chip_ops` instance keyed on vendor `0x2357` / product `0x0120` (the probe
   currently matches PID only, so vendor matching must be added).
2. 8821A register, baseband, RF, and TX-power tables. These exist in Linux as
   `rtw88/rtw8821a_table.c` (2,350 lines) under GPL-2.0, compatible with the project's license.
3. The 8821A firmware blob, `rtw88/rtw8821a_fw.bin`, 31,898 bytes, in linux-firmware
   (confirmed downloadable from gitlab.com and git.kernel.org).
4. Chip-specific bring-up differences. In Linux rtw88 the two chips share `rtw88xxa.c`
   (1,990 lines) and differ in `rtw8821a.c` (1,231 lines) versus `rtw8812a.c` (1,125 lines).
   The USB glue is 78 and 94 lines respectively. So the delta between the chips is roughly
   a thousand lines of chip-specific code, mostly init sequences, power tracking, and the
   1x1 versus 2x2 antenna configuration.

Known limitations of the base project that carry over:
- The interface is a `utun` tunnel, not an Ethernet adapter. Network.framework classifies
  it as `other`, so some apps (Tailscale is the documented example) treat it as non-primary.
  The `feth` pair plus BPF trick used by DroidTether/TetherKit would make it appear as wired
  Ethernet; that is a contained follow-up, not a blocker.
- 5 GHz station mode and WPA3/SAE are not implemented yet. 2.4 GHz WPA2 only.
- CCMP encryption runs in software on the CPU. Fine for an 802.11ac 1x1 link, but the
  author of wfb-link reports that reliable TX/RX timing over USB was their biggest hurdle.
- Root is required for the internet-client daemon.
- Several userspace USB projects report libusb instability on recent macOS. wfb-link moved
  to IOUSBHost on macOS 26 because "libusb is not reliable there". mt76-usb-macos ships a
  pure-IOKit C driver for the same reason. Plan for an IOUSBHost transport layer; the
  `rtl_usb.c` file in rtl8812au-macos is only 2.3 KB, so swapping it is cheap.
- The base project is three weeks old with one contributor. Treat it as a starting point you
  own, not a dependency.

Verdict: the realistic path. Survives OS updates, no security changes, and most of the hard
reverse-engineering (firmware load, TX/RX descriptors, WPA2, CCMP, DHCP, routing) is done.

### Route C: Linux VM with USB passthrough

Linux kernel 6.14 and newer supports the RTL8821AU in-tree via `rtw88_8821au` (author
Bitterblue Smith, merged around 6.13/6.14, described by the morrownr project as good quality
from 6.14). UTM or Parallels can pass the USB device to the guest. Zero driver work, but the
adapter is only usable inside the VM.

## Reference code sizes (Linux rtw88, mainline, counted 2026-10-17)

| Component | Lines |
|---|---|
| Shared 8812A/8821A chip code (`rtw88xxa.c/.h`) | 2,165 |
| 8821A-specific (`rtw8821a.c`, `rtw8821au.c`, `rtw8821a.h`) | 1,319 |
| 8821A tables (`rtw8821a_table.c`) | 2,350 |
| USB transport (`usb.c/.h`) | 1,495 |
| Core (main, mac, phy, fw, tx, rx, efuse, sec, ps, util, regd) | 11,608 |
| Coexistence with Bluetooth (`coex.c`, skippable) | 4,205 |

You do not port the core. The userspace project replaces it with its own, much smaller,
station-mode implementation. You port the tables, the firmware, and the chip-specific init.

## Recommended plan

1. Fork `rtl8812au-macos`. Build it unchanged and confirm `usbprobe` and `chipinfo` see the
   Archer T2U Plus once the probe accepts vendor 0x2357 / PID 0x0120 (expect the 8812AU ops
   to fail at bring-up, which tells you where the chip differences start).
2. Add an `rtl8821au_ops` instance. Fill it from `rtw8821a.c` in Linux: power-on sequence,
   MAC init, baseband/RF table load, efuse layout, TX power.
3. Convert `rtw8821a_table.c` into the project's table-module format.
4. Embed `rtw8821a_fw.bin` the way `fwload.c` embeds the 8812A firmware, and confirm the
   firmware checksum and ready flag.
5. Get monitor mode working first (no crypto, no association). Then `scan`, then `connect`,
   then `alfa-netd`.
6. Only after station mode works: replace libusb with IOUSBHost if you hit the macOS 26
   reliability problems, and consider the `feth` bridge so macOS sees Ethernet.

Rough effort for someone comfortable in C with Linux driver source open beside them:
two to four weeks to monitor mode and association on 2.4 GHz, longer for 5 GHz tuning.

## Sources

- TP-Link Archer T2U Plus downloads: https://www.tp-link.com/us/support/download/archer-t2u-plus/
- chris1111 Wireless-USB-Big-Sur-Adapter: https://github.com/chris1111/Wireless-USB-Big-Sur-Adapter
- ALFA on Apple Silicon compatibility report: https://yupitek.com/en/blog/alfa-wireless-card-apple-mac-compatibility/
- rtl8812au-macos (userspace, Apple Silicon): https://github.com/janlueders/rtl8812au-macos
- mt76-usb-macos (userspace MediaTek, IOKit transport): https://github.com/Network-Weather/mt76-usb-macos
- DroidTether (feth + BPF bridge pattern): https://github.com/jason5545/DroidTether
- TetherKit (feth + BPF bridge pattern): https://github.com/XiaoMiku01/TetherKit
- wfb-link on macOS 26 and libusb: https://news.ycombinator.com/item?id=48053252
- itlwm FAQ (fake Ethernet design): https://openintelwireless.github.io/itlwm/FAQ.html
- rtw88 RTL8821AU/RTL8812AU patch series: https://lwn.net/Articles/985235/
- rtw88 rtw8821au.c in mainline: https://github.com/torvalds/linux/blob/master/drivers/net/wireless/realtek/rtw88/rtw8821au.c
- morrownr 8821au notes on in-kernel driver: https://github.com/morrownr/8821au-20210708
- aircrack-ng rtl8812au (deprecated, source of the macOS port): https://github.com/aircrack-ng/rtl8812au
- Kexts on Apple Silicon (community write-up): https://karol-mazurek.medium.com/kernel-extensions-on-macos-1b0f38b632ea
- Kexts on Apple Silicon (Sweetwater guide): https://www.sweetwater.com/sweetcare/articles/kernel-extensions-on-mac-with-apple-silicon/
- Apple developer forum, IOUSBHost regressions: https://developer.apple.com/forums/thread/774497
