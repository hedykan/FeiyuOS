FQBN := m5stack:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=disabled
PORT ?= /dev/ttyACM0
SKETCH ?= firmware/standalone_agent
BUILD_NAME := $(notdir $(SKETCH))
BUILD_DIR := build/$(BUILD_NAME)
DEVICE ?= http://vat.local
PHASES ?= auto

.PHONY: build upload flash wait-ready host-test mascot mascot-upload monitor clean e2e serial-log serial-stop probe-hardware probe-sd libs-status libs-custom libs-stock

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
HOST_UNITS := $(HOST_SRC)/common_text.cpp $(HOST_SRC)/place_format.cpp $(HOST_SRC)/wifi_saved.cpp $(HOST_SRC)/prog_engine.cpp
host-test:
	mkdir -p build/host
	cc -O1 -c '$(CJSON_DIR)/cJSON.c' -o build/host/cJSON.o
	c++ -std=c++17 -O1 -Wall -Wextra -Werror -Itests/host/shim -I'$(HOST_SRC)' -I'$(CJSON_DIR)' \
		tests/host/host_test.cpp $(HOST_UNITS) build/host/cJSON.o -o build/host/host_test
	build/host/host_test tests/host/fixtures
	python3 tools/check_usage.py

# Corner mascot: pack assets/mascot (not in the public release) and copy it to
# the card; the firmware shows it when /vat/assets/mascot.bin exists.
mascot:
	mkdir -p build
	python3 tools/pack_mascot.py assets/mascot build/mascot.bin

mascot-upload: mascot
	curl -sS -F "file=@build/mascot.bin" '$(DEVICE)/api/upload?path=/vat/assets/mascot.bin'

monitor:
	arduino-cli monitor -p '$(PORT)' --config baudrate=115200

clean:
	rm -rf '$(BUILD_DIR)'

# Keep the device's serial console in logs/serial.log during tests: a
# watchdog or panic prints the stuck task and backtrace only there.
serial-log: serial-stop
	mkdir -p logs
	stty -F '$(PORT)' 115200 raw -echo
	nohup sh -c "cat '$(PORT)' >> logs/serial.log" >/dev/null 2>&1 &

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
