"""Harmonic distortion of the 440 Hz talk-back PCM the page sent (s16le, 8 kHz)."""
import sys, math, struct
d = open(sys.argv[1], "rb").read(); n = len(d) // 2; x = struct.unpack("<%dh" % n, d)
x = x[4000:]                              # skip start-up
def p(f):
    k = 2 * math.cos(2 * math.pi * f / 8000); s1 = s2 = 0
    for v in x: s0 = v + k * s1 - s2; s2, s1 = s1, s0
    return (s1 * s1 + s2 * s2 - k * s1 * s2) / len(x)
h1 = p(440); hs = sum(p(440 * k) for k in (2, 3, 4, 5, 6, 7, 8, 9))
print(f"{len(x)/8000:.1f} s analysed, peak {max(abs(v) for v in x)}, THD {100*math.sqrt(hs/h1):.1f}%  (3rd harmonic {10*math.log10(p(1320)/h1):.0f} dBc)")
