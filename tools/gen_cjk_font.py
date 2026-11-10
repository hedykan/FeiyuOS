#!/usr/bin/env python3
"""Re-pack M5GFX's efontCN (u8g2, 10/12/14 px) for flash: the 6763 hanzi
as fixed-cell bitmaps, everything else as a smaller u8g2 font.

u8g2 run-length codes dense CJK glyphs poorly (28 bytes a 12 px glyph against
18 for its bitmap) and keeps a 5-byte header per glyph.  Here the hanzi share
one cell per size and one code table, so the three fonts take ~400 KB
instead of 619 KB, every glyph kept and drawn pixel for pixel the same
(cjk_font.cpp; checked by comparing renders in tests/host).

    python3 tools/gen_cjk_font.py <M5GFX>/src/lgfx/Fonts/efont/lgfx_efont_cn.c \\
        firmware/standalone_agent/src/cjk_font_data.cpp
"""
import re
import sys

SIZES = (10, 12, 14)


def c_bytes(source, name):
    """The bytes of `PROGMEM const uint8_t name[n] = "..." "...";`."""
    m = re.search(r"\b%s\[(\d+)\]\s*=\s*((?:\s*\"(?:[^\"\\]|\\.)*\")+)\s*;" % re.escape(name), source, re.S)
    if not m:
        raise SystemExit("array %s not found" % name)
    out = bytearray()
    for lit in re.findall(r"\"((?:[^\"\\]|\\.)*)\"", m.group(2), re.S):
        i = 0
        while i < len(lit):
            c = lit[i]
            if c != "\\":
                out += c.encode("latin-1")
                i += 1
                continue
            n = lit[i + 1]
            if n in "01234567":
                j = i + 1
                while j < len(lit) and j < i + 4 and lit[j] in "01234567":
                    j += 1
                out.append(int(lit[i + 1:j], 8))
                i = j
            elif n == "x":
                j = i + 2
                while j < len(lit) and lit[j] in "0123456789abcdefABCDEF":
                    j += 1
                out.append(int(lit[i + 2:j], 16))
                i = j
            else:
                out.append({"n": 10, "t": 9, "r": 13, "\\": 92, "\"": 34, "'": 39, "?": 63, "a": 7, "b": 8, "f": 12, "v": 11}[n])
                i += 2
    out.append(0)  # the literal's terminating NUL, counted in the declared size
    if len(out) != int(m.group(1)):
        raise SystemExit("%s: parsed %d bytes, declared %s" % (name, len(out), m.group(1)))
    return bytes(out)


class Bits:
    """u8g2's bit reader: LSB first."""

    def __init__(self, data, pos):
        self.data, self.pos, self.bit = data, pos, 0

    def get(self, n):
        v = 0
        for i in range(n):
            v |= ((self.data[self.pos + (self.bit >> 3)] >> (self.bit & 7)) & 1) << i
            self.bit += 1
        return v

    def sget(self, n):
        return self.get(n) - (1 << (n - 1))


def is_hanzi(code):
    return 0x4E00 <= code <= 0x9FFF


def parse(font):
    """Header fields, the ASCII section (raw), unicode records [(code, record bytes)]."""
    h = {k: font[i] for i, k in enumerate(["cnt", "bbx", "b0", "b1", "bw", "bh", "bx", "by", "bd"])}
    uni = (font[21] << 8) | font[22]
    ascii_part = font[23:23 + uni]
    p = 23 + uni
    while True:  # skip the lookup table
        last = (font[p + 2] << 8) | font[p + 3]
        p += 4
        if last == 0xFFFF:
            break
    records = []
    while True:
        code = (font[p] << 8) | font[p + 1]
        if code == 0:
            break
        size = font[p + 2]
        records.append((code, font[p:p + size]))
        p += size
    return h, ascii_part, records


def decode_glyph(h, record):
    """(w, h, x, y, dx, rows of 0/1) of a unicode record."""
    r = Bits(record, 3)
    w, hh = r.get(h["bw"]), r.get(h["bh"])
    x, y, dx = r.sget(h["bx"]), r.sget(h["by"]), r.sget(h["bd"])
    px = []
    while len(px) < w * hh:
        a, b = r.get(h["b0"]), r.get(h["b1"])
        while True:
            px += [0] * a + [1] * b
            if not r.get(1):
                break
    px = px[:w * hh]
    return w, hh, x, y, dx, [px[i * w:(i + 1) * w] for i in range(hh)]


def rest_font(font, h, ascii_part, records):
    """A u8g2 font of the same header and ASCII part, unicode part without hanzi."""
    keep = [rec for code, rec in records if not is_hanzi(code)]
    groups = [keep[i:i + 32] for i in range(0, len(keep), 32)]
    lut = bytearray()
    body = bytearray()
    offset = 4 * len(groups)  # from the table's start to the first group
    for gi, g in enumerate(groups):
        last = 0xFFFF if gi == len(groups) - 1 else (g[-1][0] << 8) | g[-1][1]
        lut += bytes([offset >> 8, offset & 0xFF, last >> 8, last & 0xFF])
        size = sum(len(rec) for rec in g)
        offset = size  # next entry: from this group's start
        for rec in g:
            body += rec
    body += b"\0\0"
    header = bytearray(font[:23])
    header[0] = min(255, len(keep))
    return bytes(header) + ascii_part + bytes(lut) + bytes(body)


def main():
    src = open(sys.argv[1], encoding="latin-1").read()
    out_path = sys.argv[2]
    fonts = {s: c_bytes(src, "lgfx_efont_cn_%d" % s) for s in SIZES}
    parsed = {s: parse(fonts[s]) for s in SIZES}
    codes = sorted(code for code, _ in parsed[SIZES[0]][2] if is_hanzi(code))
    for s in SIZES:
        if sorted(c for c, _ in parsed[s][2] if is_hanzi(c)) != codes:
            raise SystemExit("hanzi differ between sizes")

    lines = ["// Generated by tools/gen_cjk_font.py from M5GFX's efontCN (efont, BSD-style",
             "// licence: see M5GFX src/lgfx/Fonts/efont/COPYRIGHT.txt).  Do not edit.",
             "#include \"cjk_font_data.h\"", "", "namespace vat::cjk_data {", ""]

    def array(name, data, kind="uint8_t"):
        lines.append("const %s %s[%d] = {" % (kind, name, len(data)))
        per = 16 if kind == "uint16_t" else 24
        for i in range(0, len(data), per):
            chunk = data[i:i + per]
            fmt = "0x%04x" if kind == "uint16_t" else "%d"
            lines.append("    " + ",".join(fmt % v for v in chunk) + ",")
        lines.append("};")
        lines.append("")

    array("kHanzi", codes, "uint16_t")
    total = 2 * len(codes)
    report = []
    for s in SIZES:
        h, ascii_part, records = parsed[s]
        glyphs = {code: decode_glyph(h, rec) for code, rec in records if is_hanzi(code)}
        left = min(g[2] for g in glyphs.values())
        right = max(g[2] + g[0] for g in glyphs.values())
        bottom = min(g[3] for g in glyphs.values())  # u8g2 y: the box's bottom above the baseline
        top = max(g[3] + g[1] for g in glyphs.values())
        W, H = right - left, top - bottom
        advances = {g[4] for g in glyphs.values()}
        if len(advances) != 1:
            raise SystemExit("%d px: hanzi advances differ %s" % (s, sorted(advances)))
        adv = advances.pop()
        if left + W > adv:
            raise SystemExit("%d px: the cell (%d..%d) passes the advance %d" % (s, left, left + W, adv))
        if left < 0:
            raise SystemExit("%d px: negative x offset" % s)
        cell = (W * H + 7) // 8
        bitmap = bytearray()
        for code in codes:
            w, hh, x, y, dx, rows = glyphs[code]
            grid = [[0] * W for _ in range(H)]
            for r in range(hh):
                cy = (top - 1) - (y + hh - 1 - r)  # cell row 0 is the top
                for c in range(w):
                    grid[cy][x - left + c] = rows[r][c]
            bits = 0
            n = 0
            out = bytearray()
            for row in grid:
                for v in row:
                    bits |= v << (n & 7)
                    n += 1
                    if (n & 7) == 0:
                        out.append(bits)
                        bits = 0
            if n & 7:
                out.append(bits)
            assert len(out) == cell
            bitmap += out
        rest = rest_font(fonts[s], h, ascii_part, records)
        array("kRest%d" % s, rest)
        array("kBits%d" % s, bitmap)
        lines.append("const Cell kCell%d = {%d, %d, %d, %d, %d, kBits%d};" % (s, W, H, left, bottom, adv, s))
        lines.append("")
        total += len(rest) + len(bitmap)
        report.append("%d px: cell %dx%d (x %d, y %d, advance %d), rest %d B + bitmaps %d B (was %d B)"
                      % (s, W, H, left, bottom, adv, len(rest), len(bitmap), len(fonts[s])))
    lines.append("}  // namespace vat::cjk_data")
    open(out_path, "w").write("\n".join(lines) + "\n")
    for line in report:
        print(line)
    print("total %d KB (was %d KB)" % (total // 1024, sum(len(f) for f in fonts.values()) // 1024))


if __name__ == "__main__":
    main()
