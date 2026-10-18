#!/bin/sh
# Start alfa-netd with the Wi-Fi passphrase taken from the macOS Keychain.
#
# One-time setup (prompts for the passphrase, stores it in your login keychain):
#   security add-generic-password -s alfa-netd -a "<ssid>" -w
# Then:
#   ./scripts/run-netd.sh "<ssid>" [--default]
#
# The passphrase is read as you (not root), piped to the daemon's stdin and
# never written to a file or put on a command line.
SSID="${1:?usage: ./scripts/run-netd.sh <ssid> [--default]}"; shift
PSK=$(security find-generic-password -s alfa-netd -a "$SSID" -w 2>/dev/null) || {
    echo "No Keychain item for \"$SSID\". Add it once with:"
    echo "  security add-generic-password -s alfa-netd -a \"$SSID\" -w"
    exit 1
}
printf '%s\n' "$PSK" | sudo "$(cd "$(dirname "$0")/.." && pwd)/alfa-netd" "$SSID" --psk-stdin "$@"
