#!/usr/bin/env bash
# Patches third-party Arduino libraries the firmware depends on, in place and
# idempotently; `make build` runs it first.  Each patch says why.
set -euo pipefail

USER_DIR=$(arduino-cli config get directories.user 2>/dev/null || echo "$HOME/Arduino")
LIBSSH="$USER_DIR/libraries/LibSSH-ESP32/src/channels.c"

# LibSSH-ESP32: the largest SSH packet we accept 32 KB -> 4 KB, and the channel
# window 8000 -> 4096 bytes.  libssh keeps up to a window of received data in
# three buffers (the socket's, the packet being decrypted, the channel's stdout),
# each grown by reallocs to the next power of two.  Reading a long output the
# server keeps them full: with 32 KB packets the buffer chain cut the heap into
# 7 KB pieces (S8, 2026-11-09); with 8 KB packets and an 8000-byte window the
# read loop of a 312 KB fetch still took 26-36 KB; with 4 KB it takes about 20 KB,
# steadily (2026-11-12).  The price: that fetch takes 11-13 s instead of 6-9 s.
set_define() {  # name, value, the values the library may have before patching
    local name=$1 value=$2; shift 2
    if grep -q "^#define $name $value\b" "$LIBSSH"; then return; fi
    for old in "$@"; do
        if grep -q "^#define $name $old\b" "$LIBSSH"; then
            sed -i "s/^#define $name $old\b.*/#define $name $value  \/\/ FeiyuOS: tools\/patch_libs.sh/" "$LIBSSH"
            echo "patched: LibSSH $name $value"
            return
        fi
    done
    echo "patch_libs: $name in $LIBSSH is none of $value $*; check the library version" >&2
    exit 1
}
if [ -f "$LIBSSH" ]; then
    set_define CHANNEL_MAX_PACKET 4096 32768 8192
    set_define WINDOW_DEFAULT 4096 8000
else
    echo "patch_libs: $LIBSSH not found (install LibSSH-ESP32)" >&2
    exit 1
fi
