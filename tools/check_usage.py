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


def check(name: str, literal: str, bad: list) -> None:
    if "\\" in literal:
        bad.append(f"{name}: kUsage or index() contains a backslash or an escaped quote")
    if any(ord(c) < 0x20 for c in literal):
        bad.append(f"{name}: kUsage or index() contains a control character")


def main() -> int:
    bad = []
    for source in sorted(PACKAGES.glob("*_package.cpp")):
        text = source.read_text(encoding="utf-8")
        literals = []
        start = text.find("constexpr char kUsage[]")
        if start >= 0:
            declaration = text[start:text.find(";\n", start)]
            literals.append("".join(re.findall(r'"((?:[^"\\]|\\.)*)"', declaration)))
        # index(): the package's line in the system prompt's index, streamed the same way
        found = re.search(r'const char\* index\(\) const override \{[^"]*"((?:[^"\\]|\\.)*)"', text)
        if found:
            literals.append(found.group(1))
        for literal in literals:
            check(source.name, literal, bad)
    for line in bad:

        print("FAIL", line)
    print(f"usage check: {len(bad)} problem(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
