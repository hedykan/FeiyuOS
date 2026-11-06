#!/usr/bin/env python3
"""Keep only what the place parser reads in a recorded AMap response
(tests/host/fixtures/amap): POI names, addresses, distances and locations,
route steps, transit segments, weather, codes.  Phone numbers, photos, ids
and the rest of AMap's data are dropped before a fixture is committed.

    python3 tools/prune_amap_fixture.py <file.json>...
"""
import json
import sys

POI_KEYS = {"name", "address", "distance", "location", "type"}


def prune(value):
    if isinstance(value, dict):
        if "pois" in value and isinstance(value["pois"], list):
            value["pois"] = [{k: v for k, v in p.items() if k in POI_KEYS} for p in value["pois"]]
        for key in ("tel", "photos", "business", "biz_ext", "indoor_data", "navi", "children", "parent"):
            value.pop(key, None)
        for key in list(value):
            value[key] = prune(value[key])
    elif isinstance(value, list):
        value = [prune(v) for v in value]
    return value


def main() -> int:
    for path in sys.argv[1:]:
        data = json.load(open(path, encoding="utf-8"))
        json.dump(prune(data), open(path, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
        print("pruned", path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
