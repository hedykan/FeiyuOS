#!/usr/bin/env bash
# Patches third-party Arduino libraries the firmware depends on, in place and
# idempotently; `make build` runs it first.  Each patch says why.
set -euo pipefail

USER_DIR=$(arduino-cli config get directories.user 2>/dev/null || echo "$HOME/Arduino")
LIBSSH="$USER_DIR/libraries/LibSSH-ESP32/src/channels.c"

# LibSSH-ESP32: the largest SSH packet we accept, 32 KB -> 8 KB.  libssh grows
# its receive buffer by reallocs up to a whole packet; reading a long output
# the server sends full packets, and the 32 KB buffer chain cut the heap into
# 7 KB pieces (S8, 2026-11-09: eight 8 KB reallocs failed, then the model
# request lost its tool result).  8 KB packets cost nothing on a LAN.
if [ -f "$LIBSSH" ]; then
    if grep -q '^#define CHANNEL_MAX_PACKET 32768' "$LIBSSH"; then
        sed -i 's/^#define CHANNEL_MAX_PACKET 32768/#define CHANNEL_MAX_PACKET 8192  \/\/ FeiyuOS: tools\/patch_libs.sh/' "$LIBSSH"
        echo "patched: LibSSH CHANNEL_MAX_PACKET 8192"
    elif ! grep -q '^#define CHANNEL_MAX_PACKET 8192' "$LIBSSH"; then
        echo "patch_libs: CHANNEL_MAX_PACKET in $LIBSSH is neither 32768 nor 8192; check the library version" >&2
        exit 1
    fi
else
    echo "patch_libs: $LIBSSH not found (install LibSSH-ESP32)" >&2
    exit 1
fi
