#!/usr/bin/env bash
# Switch the precompiled ESP-IDF libraries arduino-cli links against.
#
#   tools/esp32_libs.sh status
#   tools/esp32_libs.sh custom <dir>   # e.g. lib-builder/out/tools/esp32-arduino-libs
#   tools/esp32_libs.sh stock          # back to the board package's own copy
#
# The board package's directory becomes a symlink; the original is kept next
# to it as <dir>.stock.  The custom build comes from tools/build_esp32_libs.sh
# (same ESP-IDF and arduino-esp32 commits as the stock copy).
set -euo pipefail
libs=${ESP32_LIBS_DIR:-$HOME/.arduino15/packages/m5stack/tools/esp32-arduino-libs/idf-release_v5.4-858a988d-v1}
stock="$libs.stock"

status() {
  if [ -L "$libs" ]; then echo "custom -> $(readlink "$libs")"; else echo "stock"; fi
  grep -E "ASYMMETRIC_CONTENT_LEN|SSL_(IN|OUT)_CONTENT_LEN|SSL_MAX_CONTENT_LEN|KEEP_PEER_CERTIFICATE" "$libs/esp32s3/sdkconfig" || true
}

case "${1:-status}" in
  status) status ;;
  custom)
    dir=$(realpath "${2:?usage: esp32_libs.sh custom <dir>}")
    [ -f "$dir/esp32s3/sdkconfig" ] || { echo "$dir/esp32s3/sdkconfig missing" >&2; exit 1; }
    [ -L "$libs" ] || { [ -e "$stock" ] && { echo "$stock already exists" >&2; exit 1; }; mv "$libs" "$stock"; }
    # Keep files the board package ships beside the per-chip folders (tools.json...).
    for f in "$stock"/*; do [ -e "$dir/$(basename "$f")" ] || cp -r "$f" "$dir/"; done
    ln -sfn "$dir" "$libs"
    status ;;
  stock)
    [ -L "$libs" ] && rm "$libs" && mv "$stock" "$libs"
    status ;;
  *) echo "usage: $0 status | custom <dir> | stock" >&2; exit 2 ;;
esac
