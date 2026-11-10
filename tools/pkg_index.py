#!/usr/bin/env python3
"""Make the online catalog (packages/index.json) that `read pkg://catalog`
reads, from the .lua files in a directory.

The header is read the way the device reads it (lua_meta.cpp): the device
installs a file only if it declares exactly the uses / package / target /
usage this catalog shows, and only if its SHA-256 and size match.

    python3 tools/pkg_index.py packages                    # writes packages/index.json
    python3 tools/pkg_index.py packages --base-url URL     # absolute urls
"""
import argparse
import hashlib
import json
import pathlib
import sys
import urllib.parse

SPACE = " \t\r\n\v\f"  # Arduino String::trim() (ASCII isspace)


def split_first(text):
    """lua_meta split_first: at the first space or tab, both sides trimmed."""
    at = min([i for i in (text.find(" "), text.find("\t")) if i >= 0], default=-1)
    first, rest = (text, "") if at < 0 else (text[:at], text[at + 1:])
    return first.strip(SPACE), rest.strip(SPACE)


def without_comment(value):
    at = value.find(" --")
    return (value[:at] if at >= 0 else value).strip(SPACE)


def header(source):
    h = {"about": "", "version": "", "package": "", "target": "", "usage": "", "uses": []}
    for line in source.split("\n"):
        line = line.strip(SPACE)
        if not line:
            continue
        if not line.startswith("--"):
            break
        body = line[2:].strip(SPACE)
        if not body.startswith("@"):
            continue
        key, value = split_first(body[1:])
        if key == "uses":
            tool, target = split_first(without_comment(value))
            h["uses"].append(tool + " " + target)
        elif key in ("version", "package", "target"):
            h[key] = without_comment(value)
        elif key in ("about", "usage"):
            h[key] = value
    return h


def entry(name, data, url):
    h = header(data.decode("utf-8"))
    item = {"name": name, "version": h["version"], "kind": "package" if h["package"] else "program",
            "about": h["about"]}
    if h["package"]:
        item.update(package=h["package"], target=h["target"], usage=h["usage"])
    item.update(uses=h["uses"], url=url, sha256=hashlib.sha256(data).hexdigest(), size=len(data))
    return item


def build(files, base_url=""):
    """files: {"名字.lua": bytes} -> the catalog (a dict)."""
    packages = []
    for file_name in sorted(files):
        if not file_name.endswith(".lua"):
            continue
        url = urllib.parse.quote(file_name)
        if base_url:
            url = base_url.rstrip("/") + "/" + url
        packages.append(entry(file_name[:-4], files[file_name], url))
    return {"version": 1, "packages": packages}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("directory")
    parser.add_argument("--base-url", default="")
    args = parser.parse_args()
    directory = pathlib.Path(args.directory)
    files = {p.name: p.read_bytes() for p in directory.glob("*.lua")}
    catalog = build(files, args.base_url)
    for item in catalog["packages"]:
        if item["size"] > 16384:
            sys.exit(f"{item['name']}: over 16 KB")
    (directory / "index.json").write_text(json.dumps(catalog, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    print(f"{len(catalog['packages'])} entries -> {directory / 'index.json'}")


if __name__ == "__main__":
    main()
