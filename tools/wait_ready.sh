#!/usr/bin/env bash
# Wait until the device has rebooted and answers /api/status with its
# configuration loaded.  Replaces a fixed sleep after `make upload`: a fresh
# boot is recognised by uptime_s being no larger than the time waited.
#
#     tools/wait_ready.sh [device-url] [timeout-seconds]
set -u
DEVICE=${1:-${VAT_DEVICE:-http://vat.local}}
LIMIT=${2:-90}
start=$(date +%s)
while :; do
    waited=$(( $(date +%s) - start ))
    if body=$(curl -s -m 3 "$DEVICE/api/status") && python3 -c '
import json, sys
d = json.loads(sys.argv[1], strict=False)
sys.exit(0 if d.get("config_ready") and d.get("uptime_s", 1 << 30) <= int(sys.argv[2]) + 5 else 1)
' "$body" "$waited" 2>/dev/null; then
        echo "ready after ${waited} s"
        exit 0
    fi
    if (( waited >= LIMIT )); then
        echo "not ready after ${LIMIT} s" >&2
        exit 1
    fi
    sleep 1
done
