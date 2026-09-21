#!/usr/bin/env python3
"""Write src/font_data.h: DejaVu Sans glyph outlines for isocost.

Development-time only. The isocost build and binary never run Python.

    python3 tools/gen_font.py src/font_data.h

Reads the TrueType tables directly (head, hhea, maxp, hmtx, cmap, loca,
glyf) and stores each glyph's quadratic outline in font units, so the PNG,
SVG, and PDF backends draw identical text at any size. Kerning pairs are
measured with Pillow when it is installed; without it the header has no
kerning. DejaVu fonts are distributed under the Bitstream Vera license,
which allows redistribution of modified glyph data.
"""
import struct
import sys

FACES = [
    ("regular", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"),
    ("bold", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf"),
]
# ASCII plus the few symbols the figures use (times, plus-minus, degree, micro,
# less/greater-or-equal, middle dot for the legend separator).
CHARS = [chr(c) for c in range(32, 127)] + list("×±°µ≤≥·−")


class Font:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        num_tables = struct.unpack(">H", self.data[4:6])[0]
        self.tables = {}
        for i in range(num_tables):
            tag, _, offset, length = struct.unpack(">4sIII", self.data[12 + 16 * i: 28 + 16 * i])
            self.tables[tag.decode("latin-1")] = (offset, length)
        head = self.table("head")
        self.units_per_em = struct.unpack(">H", head[18:20])[0]
        self.loca_long = struct.unpack(">h", head[50:52])[0] == 1
        hhea = self.table("hhea")
        self.ascent, self.descent = struct.unpack(">hh", hhea[4:8])
        self.num_hmetrics = struct.unpack(">H", hhea[34:36])[0]
        self.num_glyphs = struct.unpack(">H", self.table("maxp")[4:6])[0]
        self.cmap = self.read_cmap()
        self.loca = self.read_loca()

    def table(self, tag):
        offset, length = self.tables[tag]
        return self.data[offset:offset + length]

    def read_cmap(self):
        cmap = self.table("cmap")
        count = struct.unpack(">H", cmap[2:4])[0]
        for i in range(count):
            platform, encoding, offset = struct.unpack(">HHI", cmap[4 + 8 * i: 12 + 8 * i])
            if platform == 3 and encoding == 1 and struct.unpack(">H", cmap[offset:offset + 2])[0] == 4:
                return self.cmap_format4(cmap[offset:])
        raise SystemExit("no Unicode BMP cmap")

    @staticmethod
    def cmap_format4(sub):
        segx2 = struct.unpack(">H", sub[6:8])[0]
        segs = segx2 // 2
        ends = struct.unpack(">%dH" % segs, sub[14:14 + segx2])
        starts = struct.unpack(">%dH" % segs, sub[16 + segx2:16 + 2 * segx2])
        deltas = struct.unpack(">%dh" % segs, sub[16 + 2 * segx2:16 + 3 * segx2])
        range_base = 16 + 3 * segx2
        ranges = struct.unpack(">%dH" % segs, sub[range_base:range_base + segx2])
        mapping = {}
        for s in range(segs):
            for cp in range(starts[s], ends[s] + 1):
                if cp == 0xFFFF:
                    continue
                if ranges[s] == 0:
                    gid = (cp + deltas[s]) & 0xFFFF
                else:
                    at = range_base + 2 * s + ranges[s] + 2 * (cp - starts[s])
                    gid = struct.unpack(">H", sub[at:at + 2])[0]
                    if gid:
                        gid = (gid + deltas[s]) & 0xFFFF
                mapping[cp] = gid
        return mapping

    def read_loca(self):
        loca = self.table("loca")
        n = self.num_glyphs + 1
        if self.loca_long:
            return list(struct.unpack(">%dI" % n, loca[:4 * n]))
        return [2 * v for v in struct.unpack(">%dH" % n, loca[:2 * n])]

    def advance(self, gid):
        hmtx = self.table("hmtx")
        i = min(gid, self.num_hmetrics - 1)
        return struct.unpack(">H", hmtx[4 * i:4 * i + 2])[0]

    def outline(self, gid):
        """Return a list of contours; each contour is a list of (x, y, on_curve)."""
        glyf_offset = self.tables["glyf"][0]
        start, end = self.loca[gid], self.loca[gid + 1]
        if start == end:
            return []
        g = self.data[glyf_offset + start:glyf_offset + end]
        ncontours = struct.unpack(">h", g[0:2])[0]
        if ncontours >= 0:
            return self.simple(g, ncontours)
        return self.composite(g)

    @staticmethod
    def simple(g, ncontours):
        ends = struct.unpack(">%dH" % ncontours, g[10:10 + 2 * ncontours])
        npoints = ends[-1] + 1 if ncontours else 0
        at = 10 + 2 * ncontours
        ilen = struct.unpack(">H", g[at:at + 2])[0]
        at += 2 + ilen
        flags = []
        while len(flags) < npoints:
            f = g[at]
            at += 1
            flags.append(f)
            if f & 8:
                repeat = g[at]
                at += 1
                flags.extend([f] * repeat)
        xs, ys = [], []
        for axis, short_bit, same_bit, out in ((0, 2, 16, xs), (1, 4, 32, ys)):
            value = 0
            for f in flags:
                if f & short_bit:
                    delta = g[at]
                    at += 1
                    value += delta if f & same_bit else -delta
                elif not f & same_bit:
                    value += struct.unpack(">h", g[at:at + 2])[0]
                    at += 2
                out.append(value)
        contours, first = [], 0
        for e in ends:
            contours.append([(xs[i], ys[i], bool(flags[i] & 1)) for i in range(first, e + 1)])
            first = e + 1
        return contours

    def composite(self, g):
        at = 10
        contours = []
        while True:
            flags, gid = struct.unpack(">HH", g[at:at + 4])
            at += 4
            if flags & 1:
                dx, dy = struct.unpack(">hh", g[at:at + 4])
                at += 4
            else:
                dx, dy = struct.unpack(">bb", g[at:at + 2])
                at += 2
            a, b, c, d = 1.0, 0.0, 0.0, 1.0
            if flags & 8:
                a = d = struct.unpack(">h", g[at:at + 2])[0] / 16384.0
                at += 2
            elif flags & 0x40:
                a, d = (v / 16384.0 for v in struct.unpack(">hh", g[at:at + 4]))
                at += 4
            elif flags & 0x80:
                a, b, c, d = (v / 16384.0 for v in struct.unpack(">hhhh", g[at:at + 8]))
                at += 8
            for contour in self.outline(gid):
                contours.append([(round(a * x + c * y + dx), round(b * x + d * y + dy), on) for x, y, on in contour])
            if not flags & 0x20:
                break
        return contours


def kerning(path, units):
    try:
        from PIL import ImageFont
    except ImportError:
        return []
    font = ImageFont.truetype(path, units)
    pairs = []
    ascii_chars = CHARS[:95]
    for a in ascii_chars:
        for b in ascii_chars:
            k = font.getlength(a + b) - font.getlength(a) - font.getlength(b)
            if abs(k) >= 8:
                pairs.append((ord(a), ord(b), round(k)))
    return pairs


def emit(out, name, path):
    font = Font(path)
    glyphs, points, contour_ends = [], [], []
    for ch in CHARS:
        gid = font.cmap.get(ord(ch), 0)
        contours = font.outline(gid)
        point_start, contour_start = len(points), len(contour_ends)
        for contour in contours:
            points.extend(contour)
            contour_ends.append(len(points) - point_start)
        glyphs.append((ord(ch), font.advance(gid), point_start, len(points) - point_start,
                       contour_start, len(contours)))
    kern = kerning(path, font.units_per_em)
    # font.c stores glyph fields as unsigned short and kerning pairs as ASCII bytes plus a short.
    assert all(0 <= v < 65536 for g in glyphs for v in g), "glyph field does not fit in 16 bits"
    assert all(32 <= a < 127 and 32 <= b < 127 and -32768 <= k < 32768 for a, b, k in kern), "kerning pair outside ASCII"
    # 4 bytes per point: x * 2 + on-curve flag, then y (font units fit easily in 16 bits).
    out.write("static const FontPoint font_%s_points[%d] = {\n" % (name, max(1, len(points))))
    for i in range(0, len(points), 8):
        out.write("".join("{%d,%d}," % (x * 2 + (1 if on else 0), y) for x, y, on in points[i:i + 8]) + "\n")
    out.write("};\n")
    out.write("static const unsigned short font_%s_contour_ends[%d] = {\n" % (name, max(1, len(contour_ends))))
    for i in range(0, len(contour_ends), 20):
        out.write(",".join(str(v) for v in contour_ends[i:i + 20]) + ",\n")
    out.write("};\n")
    out.write("static const FontGlyph font_%s_glyphs[%d] = {\n" % (name, len(glyphs)))
    for g in glyphs:
        out.write("{%d,%d,%d,%d,%d,%d},\n" % g)
    out.write("};\n")
    out.write("static const FontKern font_%s_kern[%d] = {\n" % (name, max(1, len(kern))))
    for k in kern or [(0, 0, 0)]:
        out.write("{%d,%d,%d}," % k)
    out.write("\n};\n")
    out.write("static const FontFace font_%s = {%d, %d, %d, %d, font_%s_glyphs, font_%s_points, "
              "font_%s_contour_ends, font_%s_kern, %d};\n\n"
              % (name, font.units_per_em, font.ascent, font.descent, len(glyphs), name, name, name, name, len(kern)))
    print("%s: %d glyphs, %d points, %d contours, %d kerning pairs" % (name, len(glyphs), len(points), len(contour_ends), len(kern)))


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else "src/font_data.h"
    with open(target, "w") as out:
        out.write("/*\n * Generated by tools/gen_font.py from DejaVu Sans. Do not edit.\n"
                  " * Glyph outlines: Copyright (c) 2003 Bitstream, Inc.; DejaVu changes are in the public domain.\n"
                  " * Distributed under the Bitstream Vera license: see third_party/dejavu/LICENSE.\n */\n\n")
        for name, path in FACES:
            emit(out, name, path)


if __name__ == "__main__":
    main()
