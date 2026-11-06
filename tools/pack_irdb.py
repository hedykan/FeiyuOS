#!/usr/bin/env python3
"""Pack the IRext air-conditioner codes into one file for the TF card.

    python3 tools/pack_irdb.py <irext_db.sqlite> <irext-binaries dir> assets/irdb/ac.irdb

Source: IRext (https://github.com/irext, MIT): the SQLite dump of the code
database and the binaries archive.  Only the AC category is kept: brands in
priority order, each with its remotes in priority order.

Layout (the firmware reads it with ranged reads, never whole):

    IRAC1 <index bytes>\n
    <brand>|<brand_en>|<remote>@<offset>+<length>,...\n     one line per brand
    ...
    <code files, concatenated; offsets count from the end of the index>
"""
import sqlite3
import sys
from pathlib import Path


def main(db_path, bin_dir, out_path):
    db = sqlite3.connect(db_path)
    brands = db.execute(
        "SELECT id, name, name_en FROM brand WHERE category_id = 1 AND status != 0 AND name != '样例' "
        "ORDER BY priority, id"
    ).fetchall()
    blob = bytearray()
    placed = {}  # remote_map -> (offset, length)
    lines = []
    for brand_id, name, name_en in brands:
        remotes = db.execute(
            "SELECT remote, remote_map FROM remote_index WHERE category_id = 1 AND brand_id = ? AND status != 0 "
            "ORDER BY priority, id",
            (brand_id,),
        ).fetchall()
        entries, seen = [], set()
        for remote, remote_map in remotes:
            if remote_map in seen:
                continue
            seen.add(remote_map)
            if remote_map not in placed:
                data = (Path(bin_dir) / f"irda_{remote_map}.bin").read_bytes()
                placed[remote_map] = (len(blob), len(data))
                blob += data
            offset, length = placed[remote_map]
            entries.append(f"{remote}@{offset}+{length}")
        if not entries:
            continue
        for field in (name, name_en or ""):
            if any(c in field for c in "|,\n"):
                sys.exit(f"brand name with a separator: {field!r}")
        lines.append(f"{name.strip()}|{(name_en or '').strip()}|{','.join(entries)}\n")
    index = "".join(lines).encode()
    out = Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(f"IRAC1 {len(index)}\n".encode() + index + bytes(blob))
    print(f"{out}: {len(lines)} brands, {len(placed)} code files, index {len(index)} B, total {out.stat().st_size} B")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
