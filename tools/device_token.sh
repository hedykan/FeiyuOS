#!/usr/bin/env bash
# Read the device's web API token from its USB serial console (printed once
# at boot) and keep it in ~/.config/feiyuos/device_token (mode 600) for make,
# tools/wait_ready.sh and tools/vat_e2e.py.  Resets the device.
#
#     tools/device_token.sh [port]
set -euo pipefail
PORT=${1:-/dev/ttyACM0}
OUT=$HOME/.config/feiyuos/device_token
ESPTOOL=$(find ~/.arduino15 -type f -name esptool -perm -u+x | head -1)
pkill -f "[c]at $PORT" 2>/dev/null || true
timeout 30 "$ESPTOOL" --chip esp32s3 -p "$PORT" run >/dev/null 2>&1
stty -F "$PORT" 115200 raw -echo
token=$(timeout 40 cat "$PORT" | strings | grep -m1 '"event":"web_token"' |
        sed -E 's/.*"detail":"([A-Z2-7]+)".*/\1/' || true)
if [ -z "$token" ]; then
    echo "no token on $PORT within 40 s" >&2
    exit 1
fi
mkdir -p "$(dirname "$OUT")" && chmod 700 "$(dirname "$OUT")"
( umask 077; printf %s "$token" > "$OUT" )
echo "token saved to $OUT (${#token} characters)"
