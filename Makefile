FQBN := m5stack:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=disabled
PORT ?= /dev/ttyACM0
SKETCH ?= firmware/standalone_agent
BUILD_NAME := $(notdir $(SKETCH))
BUILD_DIR := build/$(BUILD_NAME)
DEVICE ?= http://vat.local
# The device's web API token (tools/device_token.sh saves it).
DEVICE_TOKEN ?= $(shell cat $(HOME)/.config/feiyuos/device_token 2>/dev/null)
AUTH = -H 'Authorization: Bearer $(DEVICE_TOKEN)'
PHASES ?= auto

.PHONY: build upload flash wait-ready host-test mascot mascot-upload irdb-upload device-token release m5burner monitor clean e2e serial-log serial-stop probe-hardware probe-sd libs-status libs-custom libs-stock

build:
	arduino-cli compile -b '$(FQBN)' --build-path '$(BUILD_DIR)' '$(SKETCH)'

upload: build serial-stop
	arduino-cli upload -b '$(FQBN)' -p '$(PORT)' --input-dir '$(BUILD_DIR)'

# Flash, keep the serial log, and return once the device has rebooted and
# loaded its configuration (no fixed sleep).
flash: upload serial-log wait-ready

wait-ready:
	tools/wait_ready.sh '$(DEVICE)'

# Portable firmware sources (parsing, formatting, text helpers) built and
# checked on this machine against recorded responses: seconds, no flashing.
# cJSON comes from the same ESP-IDF tree as the custom libraries.
CJSON_DIR ?= ../../esp-libbuild/lib-builder/esp-idf/components/json/cJSON
HOST_SRC := firmware/standalone_agent/src
HOST_UNITS := $(HOST_SRC)/common_text.cpp $(HOST_SRC)/place_format.cpp $(HOST_SRC)/wifi_saved.cpp $(HOST_SRC)/prog_engine.cpp $(HOST_SRC)/ac_core.cpp $(HOST_SRC)/arena_ledger.cpp $(HOST_SRC)/ui_panel.cpp $(HOST_SRC)/web_token_core.cpp $(HOST_SRC)/ui_canvas.cpp
IREXT_C := $(wildcard $(HOST_SRC)/third_party/irext/*.c)
host-test:
	mkdir -p build/host
	cc -O1 -c '$(CJSON_DIR)/cJSON.c' -o build/host/cJSON.o
	cd build/host && cc -O1 -c $(addprefix $(CURDIR)/,$(IREXT_C))
	c++ -std=c++17 -O1 -Wall -Wextra -Werror -Itests/host/shim -I'$(HOST_SRC)' -I'$(CJSON_DIR)' \
		tests/host/host_test.cpp $(HOST_UNITS) build/host/cJSON.o $(patsubst %.c,build/host/%.o,$(notdir $(IREXT_C))) -o build/host/host_test
	build/host/host_test tests/host/fixtures
	python3 tools/check_usage.py

# Corner mascot and boot animation: pack assets/mascot (and assets/boot) and
# copy them to the card; the firmware shows them when /vat/assets/mascot.bin
# (boot.bin) exists.
mascot:
	mkdir -p build
	python3 tools/pack_mascot.py assets/mascot build/mascot.bin
	if [ -d assets/boot/frames/wait ]; then python3 tools/pack_mascot.py --boot assets/boot build/boot.bin; fi

mascot-upload: mascot
	@curl -sS $(AUTH) -F "file=@build/mascot.bin" '$(DEVICE)/api/upload?path=/vat/assets/mascot.bin'
	@if [ -f build/boot.bin ]; then curl -sS $(AUTH) -F "file=@build/boot.bin" '$(DEVICE)/api/upload?path=/vat/assets/boot.bin'; fi

# Air-conditioner IR codes (IRext, MIT): assets/irdb/ac.irdb, packed by
# tools/pack_irdb.py from the IRext database; the firmware reads it from the card.
irdb-upload:
	@curl -sS $(AUTH) -F "file=@assets/irdb/ac.irdb" '$(DEVICE)/api/upload?path=/vat/ir/ac.irdb'

# Read the web API token from the serial console (resets the device).
device-token:
	tools/device_token.sh '$(PORT)'

# Release images (GitHub Release assets).  A separate build directory, with
# source paths mapped so __FILE__ in library asserts does not carry the build
# machine's home directory; tools/release_images.sh checks the images.
VERSION ?= $(shell git describe --tags --abbrev=0)
PREFIX_MAP := -fmacro-prefix-map=$(HOME)/=~/ -fmacro-prefix-map=$(CURDIR)/=./
release:
	arduino-cli compile -b '$(FQBN)' --build-path build/release/build \
		--build-property 'compiler.c.extra_flags=$(PREFIX_MAP)' \
		--build-property 'compiler.cpp.extra_flags=$(PREFIX_MAP)' '$(SKETCH)'
	tools/release_images.sh build/release/build 'build/release/$(VERSION)' '$(VERSION)'

# Upload the release image to M5Burner, kept private: get a share code with
# node tools/m5burner.mjs share, test it, then publish on.  Log in once with
# node tools/m5burner.mjs login.
m5burner:
	python3 tools/m5burner_cover.py assets/mascot 'build/release/$(VERSION)/m5burner_cover.png'
	node tools/m5burner.mjs upload '$(VERSION)' 'build/release/$(VERSION)/feiyuos-$(VERSION)-cardputer-adv.bin' 'build/release/$(VERSION)/m5burner_cover.png'

monitor:
	arduino-cli monitor -p '$(PORT)' --config baudrate=115200

clean:
	rm -rf '$(BUILD_DIR)'

# Keep the device's serial console in logs/serial.log during tests: a
# watchdog or panic prints the stuck task and backtrace only there.
serial-log: serial-stop
	mkdir -p logs
	stty -F '$(PORT)' 115200 raw -echo
	nohup sh -c "cat '$(PORT)' | grep --line-buffered -av web_token >> logs/serial.log" >/dev/null 2>&1 &

serial-stop:
	-pkill -x -f "cat $(PORT)" || true

# Acceptance over Wi-Fi; run on the host the device's SSH profiles point at.
e2e:
	python3 tools/vat_e2e.py --device '$(DEVICE)' --phases '$(PHASES)'

probe-hardware:
	$(MAKE) upload SKETCH=firmware/hardware_probe

probe-sd:
	$(MAKE) upload SKETCH=firmware/sd_probe

# Precompiled ESP-IDF libraries: the board package's own, or a lib-builder
# build from tools/build_esp32_libs.sh (smaller TLS output buffer, see firmware/README.md).
CUSTOM_LIBS ?= ../../esp-libbuild/lib-builder/out/tools/esp32-arduino-libs
libs-status:
	tools/esp32_libs.sh status
libs-custom:
	tools/esp32_libs.sh custom '$(CUSTOM_LIBS)' && rm -rf '$(BUILD_DIR)'
libs-stock:
	tools/esp32_libs.sh stock && rm -rf '$(BUILD_DIR)'
