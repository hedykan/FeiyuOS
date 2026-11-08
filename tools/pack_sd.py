#!/usr/bin/env python3
"""Zip a card tree (vat/...) for the release: unzip at the card root.

    python3 tools/pack_sd.py <dir holding vat/> <out.zip>

Entries are sorted and dated 2020-01-01 so the same files give the same zip.
"""
import sys
import zipfile
from pathlib import Path

root, out = Path(sys.argv[1]), Path(sys.argv[2])
files = sorted(p for p in root.rglob("*") if p.is_file())
if not files or not all(p.relative_to(root).parts[0] == "vat" for p in files):
    sys.exit("expected files under vat/")
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for p in files:
        info = zipfile.ZipInfo(p.relative_to(root).as_posix(), date_time=(2020, 1, 1, 0, 0, 0))
        info.external_attr = 0o644 << 16
        info.compress_type = zipfile.ZIP_DEFLATED
        z.writestr(info, p.read_bytes())
for p in files:
    print(p.relative_to(root).as_posix(), p.stat().st_size)
