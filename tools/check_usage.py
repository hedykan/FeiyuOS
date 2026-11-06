#!/usr/bin/env python3
"""Every package's kUsage must be JSON-safe: it is streamed raw into the quoted
system prompt, and the registry drops an unsafe one entirely (a package then
silently disappears from the prompt: ir did in v0.5.2).  Run by make host-test.

    python3 tools/check_usage.py
"""
import re
import sys
from pathlib import Path

PACKAGES = Path(__file__).resolve().parents[1] / "firmware/standalone_agent/src/packages"


def main() -> int:
    bad = []
    for source in sorted(PACKAGES.glob("*_package.cpp")):
        text = source.read_text(encoding="utf-8")
        start = text.find("constexpr char kUsage[]")
        if start < 0:
            continue
        end = text.find(";\n", start)
        declaration = text[start:end]
        literal = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', declaration))
        if "\\" in literal:
            bad.append(f"{source.name}: kUsage contains a backslash or an escaped quote")
        if any(ord(c) < 0x20 for c in literal):
            bad.append(f"{source.name}: kUsage contains a control character")
    for line in bad:
        print("FAIL", line)
    print(f"usage check: {len(bad)} problem(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
