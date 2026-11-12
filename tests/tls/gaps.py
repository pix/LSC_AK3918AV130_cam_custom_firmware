"""Continuity of a talk-back tone in a camera-mic recording: 25 ms windows,
440 Hz level; reports the talk span and how many windows inside it drop out."""
import sys, wave, math, struct
w = wave.open(sys.argv[1]); n = w.getnframes(); d = struct.unpack("<%dh" % n, w.readframes(n)); sr = w.getframerate()
def g(seg, f):
    k = 2 * math.cos(2 * math.pi * f / sr); s1 = s2 = 0
    for x in seg: s0 = x + k * s1 - s2; s2, s1 = s1, s0
    return 10 * math.log10((s1 * s1 + s2 * s2 - k * s1 * s2) / len(seg) + 1)
win = sr // 40
lv = [g(d[i:i + win], 440) for i in range(0, n - win, win)]
loud = [i for i, v in enumerate(lv) if v > 85]
if not loud: print("no tone found"); sys.exit()
a, b = loud[0], loud[-1]
span = lv[a:b + 1]; med = sorted(span)[len(span) // 2]
gaps = [i for i, v in enumerate(span) if v < med - 20]
print(f"tone {a/40:.2f}-{b/40:.2f}s ({len(span)} windows), median {med:.0f} dB, dropouts(>20 dB below) {len(gaps)} = {100*len(gaps)/len(span):.0f}%")
print("".join("#" if v > med - 10 else ("+" if v > med - 20 else ".") for v in span))
