# Security and responsible use

## Responsible use

This project can capture traffic, inject frames and send deauthentication
frames. Use those features only on networks you own or have written permission
to test. Doing otherwise is illegal in most countries.

## Credentials

- The Wi-Fi password is read with `getpass` or from the macOS Keychain
  (`scripts/run-netd.sh`). It is never written to disk or passed on the
  command line.
- Verbose handshake output prints nonces, MAC addresses and EAPOL frames. It
  never prints the password, the PMK or the session keys. Even so, avoid
  posting that output publicly: a captured handshake allows offline guessing
  of weak passwords.
- `wpatest` reads a passphrase from the `WPA_TEST_PSK` environment variable.
  It is meant for throwaway test passphrases only.

## Privileges

Only `alfa-netd` needs root, to create the `utun` interface and change
routes. See [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md#why-sudo).

## Reporting a vulnerability

Please report security problems privately through GitHub's
[private vulnerability reporting](../../security/advisories/new) rather than
in a public issue. Include the affected tool, the steps to reproduce and your
macOS version.
