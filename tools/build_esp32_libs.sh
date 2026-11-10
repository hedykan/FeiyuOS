#!/usr/bin/env bash
# Rebuild the precompiled ESP-IDF libraries (esp32-arduino-libs) for esp32s3
# with a smaller TLS output buffer, pinned to the exact versions of the stock
# m5stack 3.2.5 board package (its esp32-arduino-libs versions.txt):
#   lib-builder ac45e2d · ESP-IDF release/v5.4 858a988d6e · arduino-esp32 ca8d26286 · tinyusb cd96277bd
#
#   tools/build_esp32_libs.sh <work-dir>      # needs docker; first run ~1 h (downloads), then minutes
#   HTTPS_PROXY=http://127.0.0.1:PORT tools/build_esp32_libs.sh <work-dir>
#   make libs-custom CUSTOM_LIBS=<work-dir>/lib-builder/out/tools/esp32-arduino-libs
#
# Settings (appended to configs/defconfig.esp32s3, see the CONF block below):
#   mbedTLS: output buffer 4 KB (input 16 KB), peer certificate freed after the
#   handshake (-12 KB per TLS connection)
#   internal RAM for BLE (v0.6.1): FreeRTOS, heap and ring buffer code in flash,
#   esp_timer stack 4 KB, 6 static Wi-Fi RX buffers, no PSRAM (the Cardputer has
#   none; Wi-Fi TX buffers then become dynamic), BLE controller only (the host is
#   NimBLE-Arduino) with its code in flash, unused features off
# tools/esp32_libs_trim.py then drops unused components and builds only the
# Cardputer variant (qio flash, 80 MHz): minutes instead of hours, but the
# output is for the Cardputer only (make libs-stock for other boards).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mkdir -p "${1:?usage: build_esp32_libs.sh <work-dir>}" && cd "$1" && pwd)
cd "$work"
[ -d lib-builder ] || git clone -q https://github.com/espressif/esp32-arduino-lib-builder lib-builder
cd lib-builder
git checkout -q ac45e2d
git checkout -q -- tools/update-components.sh tools/install-arduino.sh configs/defconfig.esp32s3 configs/builds.json main/idf_component.yml build.sh
[ -d components/arduino/.git ] && git -C components/arduino checkout -q -- idf_component.yml
# Pin tinyusb; fetch the arduino-esp32 commit by full SHA (it is no longer on a
# branch) and do not pull a detached HEAD.
sed -i 's|^    git clone "$TINYUSB_REPO_URL" "$TINYUSB_REPO_DIR"$|    git clone "$TINYUSB_REPO_URL" "$TINYUSB_REPO_DIR" \&\& git -C "$TINYUSB_REPO_DIR" checkout -q cd96277bd|; s|^    git -C "$TINYUSB_REPO_DIR" pull --ff-only$|    git -C "$TINYUSB_REPO_DIR" checkout -q cd96277bd|' tools/update-components.sh
sed -i 's|^\tgit -C "$AR_COMPS/arduino" fetch --all \&\& \\$|\tgit -C "$AR_COMPS/arduino" fetch --all \&\& { git -C "$AR_COMPS/arduino" fetch -q origin "$AR_BRANCH" \|\| true; } \&\& \\|; s|^\tgit -C "$AR_COMPS/arduino" pull --ff-only$|\tgit -C "$AR_COMPS/arduino" log --oneline -1|' tools/install-arduino.sh
sed -i '/^CONFIG_SPIRAM=y$/d; /^CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y$/d' configs/defconfig.esp32s3
cat >> configs/defconfig.esp32s3 <<'CONF'
# CONFIG_SPIRAM is not set
# voice-agent-terminal: smaller TLS output buffer, drop the peer certificate after the handshake
CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y
CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096
# CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE is not set
# voice-agent-terminal 0.6.1: internal RAM for BLE (the Cardputer has no PSRAM).
# Kernel, heap and ring buffer code out of IRAM (IRAM and the heap share one SRAM).
CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH=y
CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH=y
CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH=y
# esp_timer task used 1.3 KB of 8 KB (/api/mem, 2026-11-08)
CONFIG_ESP_TIMER_TASK_STACK_SIZE=4096
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=6
# Without PSRAM the IDF default is dynamic TX buffers (allocated while sending)
# CONFIG_ESP_WIFI_CSI_ENABLED is not set
# CONFIG_ESP_WIFI_FTM_ENABLE is not set
# BLE: controller only (the host is NimBLE-Arduino), controller code in flash, fewer activities
CONFIG_BT_ENABLED=y
CONFIG_BT_CONTROLLER_ONLY=y
CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY=y
CONFIG_BT_CTRL_BLE_MAX_ACT=4
CONFIG_BT_CTRL_SCAN_DUPL_CACHE_SIZE=20
# CONFIG_BT_BLE_BLUFI_ENABLE is not set
# CONFIG_BLE_MESH is not set
# Unused here
# CONFIG_ZB_ENABLED is not set
# CONFIG_ESP_INSIGHTS_ENABLED is not set
# CONFIG_LWIP_PPP_SUPPORT is not set
# CONFIG_LWIP_IP_FORWARD is not set
# CONFIG_LWIP_IPV4_NAPT is not set
# CONFIG_ETH_SPI_ETHERNET_DM9051 is not set
# CONFIG_ETH_SPI_ETHERNET_W5500 is not set
# CONFIG_ETH_SPI_ETHERNET_KSZ8851SNL is not set
# CONFIG_MBEDTLS_SSL_PROTO_DTLS is not set
# Arduino libraries compiled by lib-builder itself (the IDE still compiles the core from source)
CONFIG_ARDUINO_SELECTIVE_COMPILATION=y
# CONFIG_ARDUINO_SELECTIVE_ESP_SR is not set
# CONFIG_ARDUINO_SELECTIVE_Zigbee is not set
# CONFIG_ARDUINO_SELECTIVE_Matter is not set
# CONFIG_ARDUINO_SELECTIVE_WiFiProv is not set
# CONFIG_ARDUINO_SELECTIVE_BLE is not set
# CONFIG_ARDUINO_SELECTIVE_BluetoothSerial is not set
# CONFIG_ARDUINO_SELECTIVE_SimpleBLE is not set
# CONFIG_ARDUINO_SELECTIVE_RainMaker is not set
# CONFIG_ARDUINO_SELECTIVE_OpenThread is not set
# CONFIG_ARDUINO_SELECTIVE_Insights is not set
# CONFIG_ARDUINO_SELECTIVE_Ethernet is not set
# CONFIG_ARDUINO_SELECTIVE_PPP is not set
# CONFIG_ARDUINO_SELECTIVE_NetBIOS is not set
# CONFIG_ARDUINO_SELECTIVE_SD_MMC is not set
# CONFIG_ARDUINO_SELECTIVE_SPIFFS is not set
# CONFIG_ARDUINO_SELECTIVE_FFat is not set
# mbedTLS uses plain calloc until net.cpp installs its allocator (mbedtls_platform_set_calloc_free),
# which serves record buffers from the TLS reserve; the stock INTERNAL_MEM_ALLOC works the same way
# CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC is not set
CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC=y
CONF
git diff --stat
# Behind a local HTTP proxy (component registry and GitHub downloads), set
# HTTPS_PROXY=http://127.0.0.1:PORT: the container then uses the host network.
proxy=()
[ -n "${HTTPS_PROXY:-}" ] && proxy=(--network host -e HTTPS_PROXY -e HTTP_PROXY="${HTTP_PROXY:-$HTTPS_PROXY}" -e https_proxy="$HTTPS_PROXY" -e http_proxy="${HTTP_PROXY:-$HTTPS_PROXY}")
# Download caches, the ESP-IDF tools and ccache live in <work-dir> so that a retry
# (the component manager fetches git components again on every pass, and one
# dropped connection fails the whole build) costs minutes, not an hour.
mkdir -p "$work/cache" "$work/espressif" "$work/ccache"
docker run --rm "${proxy[@]}" -v "$PWD:/lb" -v "$work/cache:/root/.cache" -v "$work/espressif:/root/.espressif" \
  -v "$work/ccache:/root/.ccache" -v "$root/tools/esp32_libs_trim.py:/trim.py:ro" -w /lb ubuntu:24.04 bash -c "
  set -e
  export DEBIAN_FRONTEND=noninteractive
  # apt: a China mirror, directly (the proxy is for git/components, and dozens of
  # .deb downloads through it just drop the connection).
  sed -i 's|http://archive.ubuntu.com|http://mirrors.aliyun.com|g; s|http://security.ubuntu.com|http://mirrors.aliyun.com|g' /etc/apt/sources.list /etc/apt/sources.list.d/* 2>/dev/null || true
  for i in 1 2 3 4 5; do
    http_proxy= https_proxy= apt-get -o Acquire::http::Proxy=false update -qq && \
    http_proxy= https_proxy= apt-get -o Acquire::http::Proxy=false install -y -qq --no-install-recommends \
      bison ccache cmake curl flex git gperf jq libncurses-dev libssl-dev libusb-1.0-0 ninja-build patch \
      python3 python3-pip python3-venv python3-setuptools wget ca-certificates >/dev/null && break
    echo \"apt attempt \$i failed, retrying\"; sleep 10
  done
  command -v git >/dev/null || { echo 'apt failed: no git'; exit 1; }
  git config --global --add safe.directory '*'
  trap 'chown -R $(id -u):$(id -g) /lb /root/.cache /root/.espressif /root/.ccache' EXIT
  export IDF_BRANCH=release/v5.4 IDF_COMMIT=858a988d6e AR_BRANCH=ca8d26286c1a326b1fa2142d5f9b580f9d942f4e
  for attempt in 1 2 3; do
    ./tools/update-components.sh && ./tools/install-arduino.sh && source ./tools/install-esp-idf.sh && break
    echo \"environment attempt \$attempt failed\"
  done
  python3 /trim.py /lb
  for attempt in 1 2 3; do
    ./build.sh -s -t esp32s3 && exit 0
    echo \"build attempt \$attempt failed\"
  done
  exit 1
"
echo "done: $PWD/out/tools/esp32-arduino-libs"
