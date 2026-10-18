# Troubleshooting

## Why sudo

Talking to the adapter needs no privileges: the USB interface is unclaimed,
so `monitor`, `scan`, `inject`, `associate` and the other tools run as you.

`alfa-netd` needs root for two things only:

- Creating the `utun` tunnel interface goes through a kernel control socket
  that macOS only opens for root.
- Assigning the interface address and changing routes with `ifconfig` and
  `route` are root-only.

### Using Touch ID instead of typing your password

macOS can accept Touch ID for `sudo`. Create `/etc/pam.d/sudo_local` from the
template and uncomment the `pam_tid.so` line:

```bash
sudo cp /etc/pam.d/sudo_local.template /etc/pam.d/sudo_local
```

```bash
sudo nano /etc/pam.d/sudo_local
```

### A passwordless rule for this one binary

You can let `alfa-netd` run without a password with a sudoers rule. Anyone who
can replace that file can then run code as root, so prefer Touch ID unless the
binary lives somewhere only an administrator can write.

```bash
sudo visudo -f /etc/sudoers.d/alfa-netd
```

```
yourname ALL=(root) NOPASSWD: /path/to/rtl8821au-macos/alfa-netd
```

## The Wi-Fi password and the Keychain

`scripts/run-netd.sh` reads the password from your login Keychain as you, and
pipes it to `alfa-netd --psk-stdin`. It is never written to a file or put on a
command line. Store it once per network:

```bash
security add-generic-password -s alfa-netd -a "MyNetwork" -w
```

The first read may show a macOS dialog asking to allow `security` access to
the item; choose Always Allow. To change the password, delete and re-add it:

```bash
security delete-generic-password -s alfa-netd -a "MyNetwork"
```

If you run `sudo` and are asked for a password, that is your **Mac login
password**, not the Wi-Fi password.

## Connection failures

`alfa-netd` and `rtl_wpa` print some messages in German, from the upstream
project. The common ones:

| Message | Meaning | What to do |
|---|---|---|
| `Auth fehlgeschlagen` | Open authentication got no answer | Check the management endpoint for your chip; move closer |
| `Assoc fehlgeschlagen` | Association got no answer or was refused | The access point may still hold the last session; wait a minute or make sure the previous run exited cleanly |
| `keine msg3 (msg1 seen N times...)` | The access point rejected our handshake message 2 | Wrong password, or the SSID differs in case or spacing |
| `Kein DHCP-OFFER` | No DHCP reply over the encrypted link | Usually follows a deauth; see the `[link]` lines above it |

### Deauthentication reason codes

`[link] AP hat uns deauthentifiziert (reason=N)` means the access point
dropped the connection.

| Reason | Meaning | Seen when |
|---|---|---|
| 1 | Unspecified | Reconnecting without leaving the previous session |
| 2 | Previous authentication no longer valid | Sending plaintext data after the keys were installed |
| 3 | Station is leaving | Our own clean exit |
| 6 | Class 2 frame from a non-authenticated station | Same as reason 1 |
| 7 | Class 3 frame from a non-associated station | Anything sent after an earlier deauth |
| 15 | 4-way handshake timeout | Handshake message 4 lost |
| 16 | Group key handshake timeout | Group rekey not acknowledged |

## Packet loss through the tunnel

On the RTL8821AU, roughly 40% of pings are lost at the moment. The `[stat]`
line that `alfa-netd` prints every second shows where:

- `icmp_out` rising but `icmp_in` not: replies are not reaching us.
- `arp_req_answered` rising: the gateway re-resolved our address and we
  answered. Replies should continue afterwards.
- `uni_decfail` rising: frames arrive but fail decryption, which points at
  key or replay-counter handling.
- `tim=1`: the access point thinks we are asleep and is holding frames.

## The adapter is not found

- Run `./usbprobe`. If the adapter is not listed, check the cable and hub,
  and that its VID:PID is in `src/rtl_ids.c`.
- If `claim if0` fails, another process holds the device. Only one tool can
  use the adapter at a time; stop any running `alfa-netd` or `monitor`.
