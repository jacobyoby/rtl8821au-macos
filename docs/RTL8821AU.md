# RTL8821AU / RTL8811AU port

The RTL8821AU is a 1x1 802.11ac chip on USB 2.0. This port adds it as a
second chip family next to the upstream RTL8812AU. The reference is the Linux
`rtw88` driver (`rtw8821a.c`, `rtw8821a_table.c`, `rtw88xxa.c`), not the
older aircrack-ng vendor driver.

Test hardware: TP-Link Archer T2U Plus (`2357:0120`), Apple Silicon,
macOS 27.0.

## Status

| Step | Status | Evidence |
|---|---|---|
| USB ID match, claim from userspace | Works | `usbprobe` |
| Register access, chip version | Works | `chipinfo` |
| Hardware reset, power-on sequence | Works | `initchip`, both polling steps pass |
| Firmware download (`rtw8821a_fw.bin`) | Works | checksum OK, firmware ready |
| LLT, MAC init | Works | `stagetest 3`, `CR=0x06ff`, `TXDMA_PQ_MAP=0xc5a0` |
| Efuse decode | Works | MAC, crystal cap, PA/LNA options, country code |
| BB and AGC tables with the rtw88 condition parser | Works | `stagetest 4` |
| RF path A table, band switch to 2.4 GHz | Works | `stagetest 5` |
| Channel switch, 20 MHz | Works | `stagetest 7` |
| Monitor capture, 2.4 GHz | Works | 4,340 frames in 10 s on channel 6 |
| IQK | Partial | TX result converges; RX result scatters on this unit and falls back to the default, as rtw88 would |
| LCK | Not applicable | rtw88 does none on the 8821A |
| Efuse TX power with regulatory caps | Works | CCK 28, OFDM 30 to 32 at worldwide limits |
| Injection | Works | 30 probe requests, 94 probe responses |
| Scan, 2.4 GHz | Works | `scan` |
| Open auth and association | Works | 5 of 5 with the clean leave |
| WPA2-PSK 4-way handshake | Works | MIC verified against an independent computation |
| DHCP, gateway ARP, `utun` | Works | lease obtained, gateway MAC resolved |
| IP traffic through `utun` | Partial | 70 of 120 pings answered, 2 to 26 ms, sustained over several ARP refreshes |
| 5 GHz | Untested | `switch_band(5G)` is ported; 5 GHz TX power and limits are not |
| WPA3-SAE, HT/VHT rates, 40/80 MHz | Not yet | |

## Files

| File | Contents |
|---|---|
| `src/rtl_ids.[ch]` | USB VID:PID to chip family; small enough for the probe tools to link alone |
| `src/rtl8821au.c` | Ops table, hardware reset, power sequences, CR enable, LLT, firmware download, MAC init. Port checklist at the top |
| `src/rtl8821au_phy.c` | Efuse board decode, RF path A access, the `rtw_parse_tbl_phy_cond` condition parser, BB/AGC/RF table load, band and channel switch, post-RF MAC bits |
| `src/rtl8821au_cal.c` | IQK, initial gain (`RTL8821AU_IGI=<hex>` overrides), CCK power-detect threshold |
| `src/rtl8821au_txpwr.c` | Efuse power-group block to TX AGC registers, with 2.4 GHz 20 MHz limit rows |
| `src/rtl8821au_tables.c` | AGC, BB and RF tables, generated from `rtw8821a_table.c` |
| `src/fw_8821a_nic.c` | `rtw88/rtw8821a_fw.bin` from linux-firmware as a C array, SHA-256 in the header |

`rtl8821au_tables.c`, `fw_8821a_nic.c` and the limit rows in
`rtl8821au_txpwr.c` are generated. Regenerate them from the upstream sources
rather than editing them by hand; the generation rules are in each file's
header.

## USB endpoints

The 8821AU has a different layout from the AWUS036ACH:

| Endpoint | Direction | Use |
|---|---|---|
| `0x84` | bulk IN | RX |
| `0x05` | bulk OUT | high priority, beacon, H2C |
| `0x06` | bulk OUT | VO and VI |
| `0x08` | bulk OUT | BE and BK |
| `0x09` | bulk OUT | management |
| `0x87` | interrupt IN | unused |

Always use `rtl_rx_bound_ops()->tx_ep_mgmt`. The 8812AU macro
`RTL_TX_EP_MGMT` (`0x02`) is not an endpoint on this chip.

## Lessons from the first access point

The test network is a WPA2/WPA3 transition network with protected management
frames capable (CCMP, PSK and SAE). Each of these cost a debugging round:

1. **Leave with a deauth.** Without it the access point keeps the station in
   its table and answers the next attempt with deauth reason 1, then 6.
   `rtl_wpa_leave()` runs on every exit path.
2. **Never send plaintext data after the keys are installed.** The access
   point deauths with reason 2 and then 7 for everything. `alfa-netd`'s
   ARP isolation test is opt-in (`ALFA_ARP_PROBE=<router IP>`), and its
   unencrypted half needs `ALFA_PLAIN_TEST=1` on top, for that reason.
3. **Use software sequence numbers.** With hardware sequencing, our
   retransmissions reached the access point as duplicates and were dropped;
   unicast died after a few frames.
4. **Do not transmit from inside the libusb RX callback.** ARP replies sent
   from the callback never arrived; once queued and sent from the main loop,
   the gateway's ARP refreshes stopped breaking the link.
5. **Answer every copy of handshake message 3.** The access point re-sends it
   when it misses our ACK or message 4, and times out with reason 15 if
   message 4 never arrives.

## Next steps

1. **Link quality.** The access point retries many frames to us, the
   FCS-clean rate in monitor mode is about 47%, and the RX IQK result
   scatters. A dynamic initial gain (`rtw_phy_dig`, false-alarm driven) is
   the next lever, then a closer look at RX IQK.
2. **ACK visibility.** The `acks` counter in `alfa-netd` stays at 0 even with
   `RCR_ACF` set during the bridge; `REG_RXFLTMAP1` probably filters control
   frames. Knowing whether our unicast frames are ACKed would separate TX
   from RX loss.
3. **5 GHz.** Test `switch_band(5G)`, then port 5 GHz TX power and limits.
4. **Rates.** Power-by-rate offsets (`rtw8821a_bb_pg`) and power training,
   then HT/VHT TX.
5. **Clean up output.** Strip the handshake hex dumps from `rtl_wpa.c` once
   the data path is stable. They print nonces and frames, never the password.

## Porting a new chip

The same steps apply to other Realtek USB chips handled by `rtw88`:

1. Add the USB IDs to `rtl_ids.c` with a new `RTL_CHIP_*` value.
2. Create an `rtl_chip_ops` instance and register it in `rtl_chip_for()`.
3. Port in this order, testing each stage with `stagetest`: power sequence,
   firmware, MAC init, BB/AGC tables, RF tables, channel, monitor filter.
4. Run `monitor`; frames with valid FCS mean RX works.
5. Port calibration and TX power, then test with `inject` and `txtest`.
6. Station mode needs no chip-specific code beyond the endpoint map.
