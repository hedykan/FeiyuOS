"""Rewrites retired addresses (docs/EXTENSION_PACKAGE_GUIDE.zh-CN.md §5.6) in text files.

    python3 /tmp/readdress.py FILE...
"""
import re
import sys

NAME = r"[^\s\"'，。、；：）)`|>]+"
RULES = [
    (re.compile(r"lora://settings/"), "settings://lora/"),
    (re.compile(r"lora://"), "device://local/lora/"),
    (re.compile(r"ble://power"), "settings://ble/power"),
    (re.compile(r"ble://devices(?![/\w])"), "device://local/ble/devices/list"),
    (re.compile(r"ble://scan/log"), "device://local/ble/log"),
    (re.compile(r"ble://"), "device://local/ble/"),
    (re.compile(r"wifi://"), "device://local/wifi/"),
    (re.compile(r"(?<![\w/])ir://"), "device://local/ir/"),
    (re.compile(r"device://local/display/brightness"), "settings://brightness"),
    (re.compile(r"device://local/display/timeout"), "settings://timeout"),
    (re.compile(r"device://local/audio/volume"), "settings://volume"),
    (re.compile(r"device://local/context/length"), "settings://context"),
    (re.compile(r"device://local/settings(?![/\w])"), "settings://"),
    (re.compile(r"device://boot(?![/\w])"), "device://local/event/boot"),
    (re.compile(r"pkg://(install|update|rollback)/(" + NAME + ")"), r"pkg://\2/\1"),
    (re.compile(r"pkg://install\?"), "pkg://new?"),
    (re.compile(r"settings://open/(" + NAME + ")"), r"settings://\1/open"),
    (re.compile(r"ui://apps(?![/\w])"), "ui://apps/list"),
    (re.compile(r"memory://cases(?![/\w])"), "memory://cases/list"),
    (re.compile(r"memory://out(?![/\w])"), "memory://out/list"),
    (re.compile(r"ui://canvas/"), "gfx://canvas/"),
]


def rewrite(text: str) -> str:
    for pattern, new in RULES:
        text = pattern.sub(new, text)
    return text


if __name__ == "__main__":
    for path in sys.argv[1:]:
        old = open(path, encoding="utf-8").read()
        new = rewrite(old)
        if new != old:
            open(path, "w", encoding="utf-8").write(new)
            print("rewrote", path)
