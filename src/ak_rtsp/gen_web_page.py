#!/usr/bin/env python3
"""Embed a web page into a .c file as a NUL-terminated C string.
Usage: gen_web_page.py src.html dst.c [symbol]  (default symbol web_index_html)"""
import sys
src, dst = sys.argv[1], sys.argv[2]
sym = sys.argv[3] if len(sys.argv) > 3 else "web_index_html"
data = open(src, "rb").read()
if b"\0" in data:
    sys.exit("page must not contain NUL bytes")
with open(dst, "w") as f:
    f.write("/* Generated from %s by gen_web_page.py - do not edit. */\n" % src)
    f.write("const char %s[] = {\n" % sym)
    for i in range(0, len(data), 16):
        f.write("  " + ",".join("0x%02x" % b for b in data[i:i + 16]) + ",\n")
    f.write("  0x00\n};\n")
