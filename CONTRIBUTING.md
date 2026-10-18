# Contributing

Thanks for helping. Hardware reports are as valuable as code: if you have an
adapter with one of the supported chips, a run of the steps below on your Mac
tells us a lot.

## Build

```bash
brew install libusb
```

```bash
make
```

```bash
make check
```

`make check` runs the self-tests that need no hardware (AES-CCM and PBKDF2).
The build must stay free of warnings with `-Wall -Wextra`.

## Testing on hardware

Work through these in order and include the output in your issue or pull
request. Each step only makes sense if the previous one passed.

1. `./usbprobe` finds the adapter and claims the interface.
2. `./stagetest 7 6` brings the chip up to monitor mode on channel 6.
3. `./monitor 6 10 test.pcap 1` captures frames; open the file in Wireshark.
4. `./inject 6 30 1` sends probe requests and should record probe responses.
5. `./associate <channel> <bssid> "<ssid>" 0 4` authenticates and associates
   with a network you control, then leaves cleanly.
6. `./scripts/run-netd.sh "<ssid>"`, then ping the gateway through the
   tunnel.

Do not attach capture files from networks you do not own, and never paste a
Wi-Fi password into an issue.

## Adding a USB ID

If your adapter uses a supported chip but is not detected, add its VID:PID to
`src/rtl_ids.c` under the right chip family, with the vendor and model in the
note. Cite where the ID comes from, ideally the Linux `rtw88` or `rtw88xxau`
ID tables.

## Adding a chip family

See [docs/RTL8821AU.md](docs/RTL8821AU.md#porting-a-new-chip). Port from the
Linux `rtw88` driver where it supports the chip, one bring-up stage at a time,
and test each stage with `stagetest`.

## Code

- C99, built with clang for arm64.
- Chip-specific code goes behind `rtl_chip_ops`; tools must not call a chip's
  functions directly.
- Generated files (`rtl8821au_tables.c`, `fw_8821a_nic.c`, the limit rows)
  are regenerated from upstream sources, not edited by hand.
- Write comments and messages in English.
- Never log key material or passwords. Nonces and frames are fine in verbose
  output.

## Commits and pull requests

- One logical change per commit, with a message that says what changed and
  why, and what hardware it was verified on.
- Keep pull requests focused; split refactoring from behaviour changes.
- Fill in the pull request template's hardware section.

By contributing you agree that your contributions are licensed under the
GPL v2, like the rest of the project.
