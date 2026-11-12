#!/usr/bin/env python3
"""Generate osd_font.c: printable ASCII (0x20..0x7e) glyph coverage maps for
the burned-in video OSD, rendered from DejaVu Sans Mono Bold.

Each pixel is 2 bits: 0 = empty, 1 = edge (anti-aliased), 2 = solid.
Glyphs are packed row-major, 4 pixels per byte (first pixel in the low bits).

DejaVu fonts are derived from Bitstream Vera; their licence allows embedding
and redistribution of derived glyph data (notice reproduced in the output).

Usage: gen_font.py <font.ttf> <out.c>
"""
import sys
from PIL import Image, ImageDraw, ImageFont

SIZES = [("main1", 30), ("main2", 60), ("sub1", 12), ("sub2", 24)]
NOTICE = """/* Glyphs rendered from DejaVu Sans Mono Bold.
 * Bitstream Vera Fonts Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved.
 * Bitstream Vera is a trademark of Bitstream, Inc. DejaVu changes are in the
 * public domain. Permission is hereby granted, free of charge, to any person
 * obtaining a copy of the fonts accompanying this license ("Fonts") and
 * associated documentation files, to reproduce and distribute the Font
 * Software (see https://dejavu-fonts.github.io/License.html for the full text). */
"""


def render(path, px):
    font = ImageFont.truetype(path, px)
    asc, desc = font.getmetrics()
    adv = round(font.getlength("M"))
    h = asc + desc
    glyphs = []
    for c in range(0x20, 0x7f):
        im = Image.new("L", (adv, h), 0)
        ImageDraw.Draw(im).text((0, 0), chr(c), font=font, fill=255)
        px_ = im.load()
        cov = []
        for y in range(h):
            for x in range(adv):
                v = px_[x, y]
                cov.append(2 if v >= 140 else 1 if v >= 50 else 0)
        glyphs.append(cov)
    return adv, h, glyphs


def main():
    ttf, out = sys.argv[1], sys.argv[2]
    lines = [NOTICE, '#include "osd_font.h"', ""]
    for name, px in SIZES:
        w, h, glyphs = render(ttf, px)
        per = (w * h + 3) // 4
        data = []
        for cov in glyphs:
            b = [0] * per
            for i, v in enumerate(cov):
                b[i >> 2] |= v << ((i & 3) * 2)
            data.extend(b)
        lines.append("static const unsigned char %s_bits[%d] = {" % (name, len(data)))
        for i in range(0, len(data), 24):
            lines.append("    " + ",".join("%d" % x for x in data[i:i + 24]) + ",")
        lines.append("};")
        lines.append("const osd_font_t osd_font_%s = { %d, %d, %d, %s_bits };" % (name, w, h, per, name))
        lines.append("")
    open(out, "w").write("\n".join(lines))


if __name__ == "__main__":
    main()
