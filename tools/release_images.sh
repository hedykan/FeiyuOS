#!/usr/bin/env bash
# Release images from a `make release` build directory, checked before upload.
#
#   tools/release_images.sh <build-dir> <out-dir> <version>
#
#   feiyuos-<version>-cardputer-adv.bin  first install, flash at 0x0: bootloader,
#                                        partition table and app, cut at the end of
#                                        the app so the storage partitions are untouched
#   feiyuos-<version>-app.bin            upgrade, flash at 0x10000
#   mascot.bin                           corner mascot, copy to /vat/assets/mascot.bin
#   ac.irdb                              air-conditioner IR codes, copy to /vat/ir/ac.irdb
#   SHA256SUMS.txt
#
# Fails when gitleaks or ../.publish_denylist (private hostnames, addresses,
# user names) match anything in the images.
set -euo pipefail
build=${1:?usage: tools/release_images.sh <build-dir> <out-dir> <version>}
out=${2:?}
version=${3:?}
root=$(git rev-parse --show-toplevel)
name=$(basename "$(ls "$build"/*.ino.bin | head -1)" .ino.bin)
app="$build/$name.ino.bin"
merged="$build/$name.ino.merged.bin"
full="$out/feiyuos-$version-cardputer-adv.bin"

rm -rf "$out"
mkdir -p "$out"
app_size=$(stat -c %s "$app")
head -c $((0x10000 + app_size)) "$merged" > "$full"
cmp <(tail -c "$app_size" "$full") "$app"
cmp <(head -c "$(stat -c %s "$build/$name.ino.bootloader.bin")" "$full") "$build/$name.ino.bootloader.bin"
cmp <(tail -c +$((0x8000 + 1)) "$full" | head -c "$(stat -c %s "$build/$name.ino.partitions.bin")") \
    "$build/$name.ino.partitions.bin"
cp "$app" "$out/feiyuos-$version-app.bin"
python3 "$root/tools/pack_mascot.py" "$root/assets/mascot" "$out/mascot.bin" >/dev/null
cp "$root/assets/irdb/ac.irdb" "$out/ac.irdb"
(cd "$out" && sha256sum ./*.bin ./*.irdb | sed 's# \./# #' > SHA256SUMS.txt)

command -v gitleaks >/dev/null || { echo "gitleaks not installed" >&2; exit 1; }
# gitleaks skips binary files: scan the text inside the images instead.
text=$(mktemp -d)
trap 'rm -rf "$text"' EXIT
for image in "$out"/*.bin "$out"/*.irdb; do strings -n 8 "$image" > "$text/$(basename "$image").txt"; done
gitleaks dir --no-banner --exit-code 0 --report-format json --report-path "$text/report.json" "$text" >/dev/null 2>&1
# The SSH and TLS libraries carry PEM markers ("-----BEGIN OPENSSH PRIVATE KEY-----")
# to recognise key formats; a marker with no base64 body after it is not a key.
python3 - "$text/report.json" <<'PY'
import json, re, sys
findings = json.load(open(sys.argv[1])) or []
real = []
for f in findings:
    body = re.sub(r"-----(BEGIN|END)[^-]*-----", " ", f.get("Secret") or f.get("Match") or "")
    marker_only = f.get("RuleID") == "private-key" and not re.search(r"[A-Za-z0-9+/=]{40,}", body)
    label = "library PEM marker, no key body" if marker_only else "LEAK"
    print(f"gitleaks: {f.get('RuleID')} in {f.get('File', '').split('/')[-1]}: {label}")
    if not marker_only:
        real.append(f)
sys.exit(1 if real else 0)
PY
denylist="$root/../.publish_denylist"
hits=0
if [ -f "$denylist" ]; then
  while read -r pattern; do
    case "$pattern" in ''|'#'*) continue ;; esac
    n=$(strings -n 6 "$out"/*.bin "$out"/*.irdb | grep -cE -- "$pattern" || true)
    hits=$((hits + n))
  done < "$denylist"
fi
n=$(strings -n 6 "$out"/*.bin "$out"/*.irdb | grep -cF -- "$HOME" || true)
hits=$((hits + n))
[ "$hits" -eq 0 ] || { echo "private strings in the images: $hits (denylist or $HOME)" >&2; exit 1; }
ls -l "$out" | awk 'NR > 1 {print $5, $9}'
echo "release images ready: $out"
