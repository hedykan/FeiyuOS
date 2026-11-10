#!/usr/bin/env python3
"""Find what stays allocated: trace allocations on the device while a workload
runs, then group what was never freed by the code that allocated it.

Needs firmware built with the heap-tracing IDF libraries (never a release):
    make libs-custom CUSTOM_LIBS=~/Develope/esp-libbuild/out-heaptrace/tools/esp32-arduino-libs
    make flash
    python3 tools/heaptrace.py --warmup s6,s10 --phases s10      # E2E phases as the workload
    python3 tools/heaptrace.py --dump-only                        # just read and group the last trace
    make libs-custom                                              # back to the production libraries

A warm-up round first keeps one-time allocations (caches filled on first use)
out of the trace; what a second round leaves behind is what keeps growing.
"""
import argparse
import collections
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
import vat_e2e  # noqa: E402

ROOT = vat_e2e.ROOT
ELF = ROOT / "build" / "standalone_agent" / "standalone_agent.ino.elf"
# Frames that only pass the allocation on: the site is the first frame below them.
PLUMBING = ("malloc", "calloc", "realloc", "heap_caps_", "operator new", "String::", "std::", "__gnu_cxx",
            "cJSON_", "strdup", "trace_malloc", "multi_heap", "tlsf_", "_malloc_r", "_calloc_r", "_realloc_r",
            "esp_heap_", "heap_trace", "??")


def addr2line(addresses: list[str]) -> dict[str, str]:
    from pathlib import Path
    tool = next(iter(sorted(Path(os.path.expanduser("~/.arduino15/packages/m5stack/tools/esp-x32")).glob("*/bin/xtensa-esp-elf-addr2line"))), None)
    if not tool:
        sys.exit("xtensa-esp-elf-addr2line not found")
    out = subprocess.run([str(tool), "-fC", "-e", str(ELF), *addresses], capture_output=True, text=True).stdout.split("\n")
    names, i = {}, 0
    for address in addresses:  # two lines per address: function, file:line
        name, where = out[i].strip(), out[i + 1].strip() if i + 1 < len(out) else ""
        names[address] = f"{name} ({where.split('/')[-1]})"
        i += 2
    return names


def site(frames: list[str]) -> str:
    for frame in frames:
        if not frame.startswith(PLUMBING):
            return frame
    return frames[-1] if frames else "?"


def pins(device, records: list, names: dict, top: int) -> None:
    """Pins: blocks still allocated that sit between free blocks.  Freeing one
    would join its neighbours; ranked by the free run that would make, the
    allocations worth moving (allocate at boot, pool, or keep elsewhere)."""
    if not records or "a" not in records[0]:
        print("\npins: this firmware's trace has no addresses")
        return
    mem = json.loads(device._request("GET", "/api/mem")[1].decode())
    free = sorted((int(b["a"], 16), b["s"]) for b in mem["blocks"] if not b["u"])
    largest = mem["heap"]["largest"]
    slack = 16  # block headers between neighbours
    found = []
    for r in records:
        a, size = int(r["a"], 16), r["s"]
        before = next((s for fa, s in free if a - slack <= fa + s <= a), 0)
        after = next((s for fa, s in free if a + size <= fa <= a + size + slack), 0)
        if before or after:
            found.append((before + size + after, before, after, r))
    found.sort(key=lambda f: -f[0])
    print(f"\npins (largest free block now {largest} B): what freeing each would join")
    for joined, before, after, r in found[:top]:
        frames = [names.get(x, x) for x in r["by"]]
        print(f"  {joined:6d} B = {before:6d} + [{r['s']:5d} at {r['a']}] + {after:6d}   {site(frames)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--device", default=os.environ.get("VAT_DEVICE", "http://vat.local"))
    parser.add_argument("--warmup", default="", help="E2E phases run before tracing starts")
    parser.add_argument("--phases", default="s10", help="E2E phases run while tracing")
    parser.add_argument("--records", type=int, default=300)
    parser.add_argument("--dump-only", action="store_true")
    parser.add_argument("--top", type=int, default=25)
    args = parser.parse_args()
    device = vat_e2e.Device(args.device)
    e2e = [sys.executable, str(ROOT / "tools" / "vat_e2e.py"), "--no-sync-profiles", "--phases"]
    if not args.dump_only:
        if args.warmup:
            print("warm-up:", args.warmup, flush=True)
            subprocess.run(e2e + [args.warmup], cwd=ROOT, stdout=subprocess.DEVNULL)
        status, body = device._request("POST", f"/api/heaptrace?action=start&records={args.records}")
        print("start:", body.decode(), flush=True)
        if b"error" in body:
            return 1
        try:
            subprocess.run(e2e + [args.phases], cwd=ROOT, stdout=subprocess.DEVNULL)
        finally:
            status, body = device._request("POST", "/api/heaptrace?action=stop")
            print("stop:", body.decode(), flush=True)
    dump = json.loads(device._request("GET", "/api/heaptrace")[1].decode())
    if "error" in dump:
        sys.exit(dump["error"])
    records = dump["records"]
    print(f"still allocated: {len(records)} blocks, {sum(r['s'] for r in records)} B"
          + ("  (records full: some allocations were not traced)" if dump["count"] >= dump["capacity"] else ""))
    names = addr2line(sorted({a for r in records for a in r["by"]}))
    groups = collections.defaultdict(lambda: [0, 0, None])
    for r in records:
        frames = [names[a] for a in r["by"]]
        g = groups[site(frames)]
        g[0] += 1
        g[1] += r["s"]
        g[2] = g[2] or frames
    for where, (n, size, frames) in sorted(groups.items(), key=lambda kv: -kv[1][1])[: args.top]:
        print(f"\n{size:7d} B in {n:3d} blocks  {where}")
        for f in frames[:6]:
            print("        " + f)
    pins(device, records, names, args.top)
    out = ROOT / "logs" / "heaptrace-last.json"
    out.write_text(json.dumps({"dump": dump, "names": names}, ensure_ascii=False))
    print(f"\nraw: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
