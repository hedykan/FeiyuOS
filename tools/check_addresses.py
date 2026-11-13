#!/usr/bin/env python3
"""Resource addresses follow docs/EXTENSION_PACKAGE_GUIDE.zh-CN.md section 5.

The rules come from three tables in that section (one copy only): the
registered schemes (5.5), the actions (5.2) and the retired addresses (5.6).
Checked: every address literal in the firmware's packages, the core prompt and
prog://help.  Run by make host-test.

    python3 tools/check_addresses.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GUIDE = ROOT / "docs/EXTENSION_PACKAGE_GUIDE.zh-CN.md"
SRC = ROOT / "firmware/standalone_agent/src"
SOURCES = sorted((SRC / "packages").glob("*.cpp")) + [SRC / "agent_prompt.h"]
ADDRESS = re.compile(r"\b([a-z]+)://([^\s\"'，。、；：（）()“”`|…<>]*)")
NO_VERBS = {"get", "set"}
NOUNS = {"help", "run"}  # also nouns (5.2): prog://help/examples, debug://run/编号


def table(section: str) -> list[str]:
    """First-column `code` cells of the table under `### section`."""
    text = GUIDE.read_text(encoding="utf-8")
    start = text.index(f"### {section}")
    end = text.find("\n### ", start + 4)
    cells = []
    for line in text[start:end if end > 0 else None].splitlines():
        m = re.match(r"\|\s*`([^`]+)`", line)
        if m:
            cells.append(m.group(1))
    if not cells:
        sys.exit(f"check_addresses: no table under ### {section} in {GUIDE.name}")
    return cells


def literals(path: Path) -> list[tuple[int, str]]:
    """(line, text) of every C++ string literal (raw ones too) holding ://."""
    text = path.read_text(encoding="utf-8")
    found = []
    for m in re.finditer(r'R"(\w*)\((.*?)\)\1"|"((?:[^"\\\n]|\\.)*)"', text, re.S):
        body = m.group(2) if m.group(2) is not None else m.group(3)
        if "://" in body:
            found.append((text.count("\n", 0, m.start()) + 1, body))
    return found


def main() -> int:
    schemes = set(table("5.5"))
    verbs = set(table("5.2"))
    retired = table("5.6")
    problems = []
    for path in SOURCES:
        for line, body in literals(path):
            for m in ADDRESS.finditer(body):
                scheme, rest = m.group(1), m.group(2).split("?")[0]
                address = f"{scheme}://{m.group(2)}"
                where = f"{path.relative_to(ROOT)}:{line}: {address}"
                if scheme not in schemes:
                    problems.append(f"{where}: scheme {scheme}:// is not registered (5.5)")
                    continue
                for old in retired:
                    prefix = old.endswith(("/", "?"))
                    if (address.startswith(old) if prefix else address == old):
                        problems.append(f"{where}: retired address {old} (5.6)")
                segments = [s for s in rest.split("/") if s]
                for i, s in enumerate(segments[:-1]):
                    if s in verbs and s not in NOUNS:
                        problems.append(f"{where}: action {s} must be the last segment (5.1)")
                if any(s in NO_VERBS for s in segments):
                    problems.append(f"{where}: no get/set segments; use read and write (5.1)")
    for p in problems:
        print("FAIL", p)
    print(f"address check: {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
