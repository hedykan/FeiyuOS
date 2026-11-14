#!/usr/bin/env python3
"""Nothing draws on the screen except through the compositor
(docs/V075_PLAN.zh-CN.md, test T2).  A painter that drew on the display
directly would bring back the flicker the compositor removes, and a painter
that set its own clip on a band could write outside the strip's memory.
Run by make host-test.

    python3 tools/check_ui.py

Rules, for every .cpp/.h/.ino under the firmware:
  1. no drawing call on M5Cardputer.Display (fill*, draw*, push*, write*,
     qrcode, print*, startWrite) outside the files allowed below;
  2. no reference to M5Cardputer.Display kept in a variable outside them,
     unless the line says "// measure only" (text width) or "// read back
     only" (the screenshot);
  3. setClipRect / clearClipRect only in the files allowed below: painters
     clip with compose::clip() / compose::unclip().
"""
import re
import sys
from pathlib import Path

FIRMWARE = Path(__file__).resolve().parents[1] / "firmware/standalone_agent"

# The compositor itself, and the boot splash (it runs before the compositor
# exists and owns the whole screen).
ALLOWED = {"src/ui_compose.cpp", "src/ui_splash.cpp"}

DRAW = re.compile(r"M5Cardputer\.Display\s*\.\s*(fill\w*|draw\w*|push\w*|write\w*|qrcode|print\w*|startWrite)\s*\(")
BIND = re.compile(r"(?:auto|Gfx|lgfx::\w+|M5GFX)\s*&\s*\w+\s*=\s*M5Cardputer\.Display\b")
CLIP = re.compile(r"\b(setClipRect|clearClipRect)\s*\(")


def strip_comment(line: str) -> str:
    return line.split("//", 1)[0]


def main() -> int:
    bad = []
    for path in sorted(FIRMWARE.rglob("*")):
        if path.suffix not in (".cpp", ".h", ".ino") or "third_party" in path.parts:
            continue
        name = path.relative_to(FIRMWARE).as_posix()
        if name in ALLOWED:
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            code = strip_comment(line)
            where = f"{name}:{number}"
            if DRAW.search(code):
                bad.append(f"{where}: draws on M5Cardputer.Display directly (paint through compose::frame / area)")
            if BIND.search(code) and "measure only" not in line and "read back only" not in line:
                bad.append(f"{where}: keeps M5Cardputer.Display in a variable (mark '// measure only' if it only measures)")
            if CLIP.search(code) and "compose::" not in code:
                bad.append(f"{where}: sets a clip itself (use compose::clip / compose::unclip)")
    for line in bad:
        print("FAIL", line)
    print(f"ui check: {len(bad)} problem(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
