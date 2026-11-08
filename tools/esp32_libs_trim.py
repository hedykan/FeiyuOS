#!/usr/bin/env python3
"""Trim lib-builder to what the Cardputer firmware links (run inside lib-builder/).

Idempotent.  Removes managed components the firmware never uses, and builds
only the esp32s3 variant the m5stack_cardputer board selects (qio flash, quad
PSRAM slot, 80 MHz boot), so one build takes a fraction of the time.
The output is then only valid for Cardputer: `make libs-stock` for other boards.
"""
import json
import re
import sys
from pathlib import Path

DROP = {
    "espressif/esp32-camera", "espressif/esp-tflite-micro", "espressif/esp-sr", "espressif/esp_matter",
    "espressif/esp_modem", "espressif/esp-zboss-lib", "espressif/esp-zigbee-lib", "espressif/esp-dsp",
    "espressif/network_provisioning", "espressif/esp_rainmaker", "espressif/rmaker_common",
    "espressif/esp_insights", "espressif/esp_diag_data_store", "espressif/esp_diagnostics", "espressif/cbor",
    "espressif/qrcode", "espressif/libsodium", "espressif/esp-modbus", "chmorgan/esp-libhelix-mp3",
}


def drop_dependencies(path: Path) -> list[str]:
    """Removes DROP entries from the `dependencies:` block of an idf_component.yml."""
    lines = path.read_text().split("\n")
    out, removed, skipping, in_deps = [], [], False, False
    for line in lines:
        if re.match(r"^\S", line):
            in_deps = line.startswith("dependencies:")
            skipping = False
        elif in_deps and re.match(r"^  \S", line):
            name = line.strip().rstrip(":").strip("\"'")
            skipping = name in DROP
            if skipping:
                removed.append(name)
        if not skipping:
            out.append(line)
    path.write_text("\n".join(out))
    return removed


root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
for manifest in ("main/idf_component.yml", "components/arduino/idf_component.yml"):
    removed = drop_dependencies(root / manifest)
    print(f"{manifest}: removed {len(removed)}: {' '.join(sorted(removed)) or '-'}")

builds = root / "configs/builds.json"
data = json.loads(builds.read_text())
for target in data["targets"]:
    if target["target"] == "esp32s3":
        target["features"] = []
        target["idf_libs"] = ["qio", "80m"]
        target["bootloaders"] = [["qio", "80m"]]
        target["mem_variants"] = []
builds.write_text(json.dumps(data, indent=4) + "\n")
print("builds.json: esp32s3 -> one variant (qio, 80m), no esp_sr, no PSRAM")

# build.sh always builds the esp-sr speech models for esp32s3; without esp-sr that target does not exist.
script = root / "build.sh"
text = script.read_text()
old = 'if [ "$target" == "esp32s3" ]; then\n        idf.py -DIDF_TARGET="$target" -DSDKCONFIG_DEFAULTS="$idf_libs_configs" srmodels_bin'
new = 'if [ "$target" == "esp32s3" ] && [ -d managed_components/espressif__esp-sr ]; then\n        idf.py -DIDF_TARGET="$target" -DSDKCONFIG_DEFAULTS="$idf_libs_configs" srmodels_bin'
if old in text:
    script.write_text(text.replace(old, new))
    print("build.sh: esp-sr models only when esp-sr is present")
elif new in text:
    print("build.sh: already patched")
else:
    sys.exit("build.sh: srmodels block not found, check the lib-builder version")

left = [p.name for p in (root / "managed_components").glob("*")] if (root / "managed_components").exists() else []
print("managed_components now:", " ".join(sorted(left)) or "(none yet)")
