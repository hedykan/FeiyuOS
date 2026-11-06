#!/usr/bin/env bash
# Rebuild the precompiled ESP-IDF libraries (esp32-arduino-libs) for esp32s3
# with a smaller TLS output buffer, pinned to the exact versions of the stock
# m5stack 3.2.5 board package (its esp32-arduino-libs versions.txt):
#   lib-builder ac45e2d · ESP-IDF release/v5.4 858a988d6e · arduino-esp32 ca8d26286 · tinyusb cd96277bd
#
#   tools/build_esp32_libs.sh <work-dir>      # needs docker; ~1-2 h, several GB
#   HTTPS_PROXY=http://127.0.0.1:PORT tools/build_esp32_libs.sh <work-dir>
#   make libs-custom CUSTOM_LIBS=<work-dir>/lib-builder/out/tools/esp32-arduino-libs
#
# mbedTLS changes (per TLS connection: -12 KB output buffer, peer certificate
# freed after the handshake):
#   CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y, IN 16384, OUT 4096
#   CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE off
set -euo pipefail
work=$(mkdir -p "${1:?usage: build_esp32_libs.sh <work-dir>}" && cd "$1" && pwd)
cd "$work"
[ -d lib-builder ] || git clone -q https://github.com/espressif/esp32-arduino-lib-builder lib-builder
cd lib-builder
git checkout -q ac45e2d
git checkout -q -- tools/update-components.sh tools/install-arduino.sh configs/defconfig.esp32s3
# Pin tinyusb; fetch the arduino-esp32 commit by full SHA (it is no longer on a
# branch) and do not pull a detached HEAD.
sed -i 's|^    git clone "$TINYUSB_REPO_URL" "$TINYUSB_REPO_DIR"$|    git clone "$TINYUSB_REPO_URL" "$TINYUSB_REPO_DIR" \&\& git -C "$TINYUSB_REPO_DIR" checkout -q cd96277bd|; s|^    git -C "$TINYUSB_REPO_DIR" pull --ff-only$|    git -C "$TINYUSB_REPO_DIR" checkout -q cd96277bd|' tools/update-components.sh
sed -i 's|^\tgit -C "$AR_COMPS/arduino" fetch --all \&\& \\$|\tgit -C "$AR_COMPS/arduino" fetch --all \&\& { git -C "$AR_COMPS/arduino" fetch -q origin "$AR_BRANCH" \|\| true; } \&\& \\|; s|^\tgit -C "$AR_COMPS/arduino" pull --ff-only$|\tgit -C "$AR_COMPS/arduino" log --oneline -1|' tools/install-arduino.sh
cat >> configs/defconfig.esp32s3 <<'CONF'
# voice-agent-terminal: smaller TLS output buffer, drop the peer certificate after the handshake
CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y
CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096
# CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE is not set
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
  -v "$work/ccache:/root/.ccache" -w /lb ubuntu:24.04 bash -c "
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
  skip=''
  for attempt in 1 2 3; do
    ./build.sh \$skip -t esp32s3 -I release/v5.4 -i 858a988d6e -A ca8d26286c1a326b1fa2142d5f9b580f9d942f4e && exit 0
    echo \"build attempt \$attempt failed\"
    skip=-s
  done
  exit 1
"
echo "done: $PWD/out/tools/esp32-arduino-libs"
