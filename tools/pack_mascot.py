#!/usr/bin/env python3
"""Pack corner-mascot frames into the device's mascot.bin (see src/ui_mascot.cpp).

    python3 tools/pack_mascot.py <art-dir> [out.bin]
    python3 tools/pack_mascot.py --boot <art-dir> [boot.bin]

--boot packs the boot screen's states instead (enter_left, enter_right,
enter_top, enter_bottom: the mascot entering from that side, played once;
wait: the loop while the device boots), with a palette of its own, into
format version 2: version 1 plus each frame's offset from where the mascot
lands (animation.json states.<state>.offsets_px, [dx, dy] in art pixels,
-128..127), so an entrance can move as it was drawn.  The art lives in
assets/boot.

<art-dir> holds frames/<state>/*.png for the states idle, recognize,
understand, execute and error (frame order = file name order, at most 8 per
state, each 32x32 RGBA), and optionally animation.json with
states.<state>.durations_ms.  Fully transparent pixels become palette index 0;
every other pixel must be opaque; at most 63 colours.

The art lives in assets/mascot (fan art, not under the MIT license; see
its NOTICE.md).  `make mascot-upload` packs it and copies
it to the card, or by hand:

    curl -F "file=@mascot.bin" "$VAT_DEVICE/api/upload?path=/vat/assets/mascot.bin"
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

from PIL import Image

STATES = ["idle", "recognize", "understand", "execute", "error"]
BOOT_STATES = ["enter_left", "enter_right", "enter_top", "enter_bottom", "wait"]
SIZE = 32
MAX_FRAMES = 8
MAX_COLORS = 64
DEFAULT_MS = 200


def rgb565(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def main() -> int:
    args = sys.argv[1:]
    boot = bool(args) and args[0] == "--boot"
    if boot:
        args = args[1:]
    if not args:
        print(__doc__)
        return 2
    states = BOOT_STATES if boot else STATES
    art = Path(args[0])
    out = Path(args[1] if len(args) > 1 else ("boot.bin" if boot else "mascot.bin"))
    meta = {}
    if (art / "animation.json").exists():
        meta = json.loads((art / "animation.json").read_text(encoding="utf-8")).get("states", {})

    palette = [0]  # index 0: transparent
    index_of: dict[int, int] = {}
    counts, durations, offsets, frames = [], [], [], []
    for state in states:
        files = sorted((art / "frames" / state).glob("*.png"))
        if not 1 <= len(files) <= MAX_FRAMES:
            raise SystemExit(f"{state}: need 1-{MAX_FRAMES} frames, found {len(files)}")
        ms = list(meta.get(state, {}).get("durations_ms", []))
        ms = (ms + [DEFAULT_MS] * MAX_FRAMES)[:len(files)]
        moves = [tuple(m) for m in meta.get(state, {}).get("offsets_px", [])]
        moves = (moves + [(0, 0)] * MAX_FRAMES)[:len(files)]
        if any(not -128 <= v <= 127 for m in moves for v in m):
            raise SystemExit(f"{state}: offsets_px out of -128..127")
        offsets.append(moves + [(0, 0)] * (MAX_FRAMES - len(files)))
        counts.append(len(files))
        durations.append(ms + [0] * (MAX_FRAMES - len(files)))
        for path in files:
            image = Image.open(path).convert("RGBA")
            if image.size != (SIZE, SIZE):
                raise SystemExit(f"{path}: {image.size}, expected {SIZE}x{SIZE}")
            data = bytearray()
            raw = image.tobytes()
            for i in range(0, len(raw), 4):
                r, g, b, a = raw[i:i + 4]
                if a == 0:
                    data.append(0)
                    continue
                if a != 255:
                    raise SystemExit(f"{path}: semi-transparent pixel (alpha {a})")
                c = rgb565(r, g, b)
                if c not in index_of:
                    if len(palette) >= MAX_COLORS:
                        raise SystemExit(f"more than {MAX_COLORS - 1} colours")
                    index_of[c] = len(palette)
                    palette.append(c)
                data.append(index_of[c])
            frames.append(bytes(data))

    header = bytearray(b"VATM")
    header += bytes([2 if boot else 1, SIZE, len(states), len(palette)])
    header += bytes(counts + [0] * (8 - len(counts)))
    for state_ms in durations + [[0] * MAX_FRAMES] * (8 - len(durations)):
        header += struct.pack("<8H", *[min(int(v), 0xFFFF) for v in state_ms])
    header += struct.pack(f"<{MAX_COLORS}H", *(palette + [0] * (MAX_COLORS - len(palette))))
    assert len(header) == 272, len(header)
    if boot:  # version 2: int8 dx, dy per frame, [8 states][8 frames]
        for state_moves in offsets + [[(0, 0)] * MAX_FRAMES] * (8 - len(offsets)):
            for dx, dy in state_moves:
                header += struct.pack("<bb", int(dx), int(dy))
        assert len(header) == 400, len(header)
    out.write_bytes(bytes(header) + b"".join(frames))
    print(f"{out}: {len(frames)} frames, {len(palette) - 1} colours, {out.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
