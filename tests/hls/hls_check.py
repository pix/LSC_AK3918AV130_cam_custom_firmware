"""HLS test client for the camera's /hls/ output (tests/hls/hls_test.sh).

  hls_check.py client <playlist url> <secs> [name]
      Plays like a live player (hls.js / RFC 8216 rules): starts 3 segments
      from the live edge, reloads the playlist (target duration, half of it
      when unchanged), downloads every new segment in order and checks
      - playlist syntax: EXTM3U, VERSION, TARGETDURATION (constant, >= every
        rounded EXTINF), MEDIA-SEQUENCE (never backwards, consistent mapping
        seq -> URI/duration between reloads), EXT-X-MAP, >= 3 target durations
        once segments are being removed; headers (content type, CORS)
      - init segment: ftyp, moov/trak/avc1/avcC, mvex/trex, 90 kHz
      - media segments: moof+mdat pairs only, mfhd increasing, tfdt continuous
        across segments (no gap, no overlap), trun sizes == mdat payload, data
        offsets inside mdat, NAL lengths fill each sample, first sample sync +
        IDR when EXT-X-INDEPENDENT-SEGMENTS, sum of durations == EXTINF (1 ms)
      - no missed segment (sequence numbers consecutive)
      Prints one summary line; exit 1 on any error.
  hls_check.py ffprobe <file.mp4> <min secs>
      Packet timestamps of an ffmpeg recording of the stream: monotonic DTS,
      no gap > 3 frame durations, total duration >= min secs.
  hls_check.py slow <segment url> <bytes/s>
      Reads one segment slowly with a tiny receive window; prints
      "slow: got N of M" (the server may cut it once the ring dropped it).
  hls_check.py get <url>
      Prints "status content-type cors length" (no body).
  hls_check.py tfdt <segment url>
      Prints the segment's first decode time and its length (90 kHz).
"""
import json, re, socket, ssl, struct, subprocess, sys, time, urllib.error, urllib.parse, urllib.request

import os
if os.environ.get("HLS_CA"):            # HTTPS: verify against the camera's CA (name = address)
    CTX = ssl.create_default_context(cafile=os.environ["HLS_CA"])
else:
    CTX = ssl.create_default_context()
    CTX.check_hostname = False
    CTX.verify_mode = ssl.CERT_NONE


def fetch(url, timeout=20):
    req = urllib.request.Request(url, headers={"User-Agent": "hls_check"})
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=CTX) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


# ---- MP4 boxes -----------------------------------------------------------------------
def boxes(b, off=0, end=None):
    end = len(b) if end is None else end
    while off < end:
        if end - off < 8:
            raise ValueError(f"truncated box header at {off}")
        size, typ = struct.unpack(">I4s", b[off:off + 8])
        hdr = 8
        if size == 1:
            size = struct.unpack(">Q", b[off + 8:off + 16])[0]; hdr = 16
        if size < hdr or off + size > end:
            raise ValueError(f"bad box size {size} ({typ}) at {off}")
        yield typ.decode("latin1"), off, size, hdr
        off += size


def find(b, path, start=0, end=None):
    """Box at path (list of types, each inside the previous); None if absent."""
    end = len(b) if end is None else end
    for t, o, s, h in boxes(b, start, end):
        if t == path[0]:
            return (o, s, h) if len(path) == 1 else find(b, path[1:], o + h, o + s)
    return None


def check_init(b, errs):
    top = [t for t, *_ in boxes(b)]
    if top[:2] != ["ftyp", "moov"]:
        errs.append(f"init: top-level boxes {top}")
        return
    mdhd = find(b, ["moov", "trak", "mdia", "mdhd"])
    if not mdhd:
        errs.append("init: no trak/mdia/mdhd"); return
    ts = struct.unpack(">I", b[mdhd[0] + mdhd[2] + 12:mdhd[0] + mdhd[2] + 16])[0]
    if ts != 90000:
        errs.append(f"init: timescale {ts}")
    stsd = find(b, ["moov", "trak", "mdia", "minf", "stbl", "stsd"])
    if not stsd:
        errs.append("init: no stsd"); return
    o, s, h = stsd
    entries = list(boxes(b, o + h + 8, o + s))      # full box + entry count
    if not entries or entries[0][0] != "avc1":
        errs.append(f"init: sample entry {[e[0] for e in entries]}"); return
    t, ao, asz, ah = entries[0]
    inner = [x[0] for x in boxes(b, ao + ah + 78, ao + asz)]   # visual sample entry fields
    if "avcC" not in inner:
        errs.append(f"init: avc1 without avcC ({inner})")
    if not find(b, ["moov", "mvex", "trex"]):
        errs.append("init: no mvex/trex (not fragmented)")
    if find(b, ["moov", "trak", "mdia", "hdlr"]) is None:
        errs.append("init: no hdlr")


def parse_moof(b, o, s, h):
    r = {"seq": None, "tracks": []}
    for t, co, cs, ch in boxes(b, o + h, o + s):
        if t == "mfhd":
            r["seq"] = struct.unpack(">I", b[co + 12:co + 16])[0]
        elif t == "traf":
            tr = {"tfdt": None, "samples": [], "data_offset": None, "track": None, "tfhd_flags": None}
            for u, uo, us, uh in boxes(b, co + ch, co + cs):
                vf = struct.unpack(">I", b[uo + 8:uo + 12])[0]
                ver, fl = vf >> 24, vf & 0xffffff
                p = uo + 12
                if u == "tfhd":
                    tr["track"] = struct.unpack(">I", b[p:p + 4])[0]; tr["tfhd_flags"] = fl
                elif u == "tfdt":
                    tr["tfdt"] = struct.unpack(">Q", b[p:p + 8])[0] if ver == 1 else struct.unpack(">I", b[p:p + 4])[0]
                elif u == "trun":
                    n = struct.unpack(">I", b[p:p + 4])[0]; p += 4
                    if fl & 1:
                        tr["data_offset"] = struct.unpack(">i", b[p:p + 4])[0]; p += 4
                    first_flags = None
                    if fl & 4:
                        first_flags = struct.unpack(">I", b[p:p + 4])[0]; p += 4
                    for i in range(n):
                        smp = {}
                        for bit, key in ((0x100, "dur"), (0x200, "size"), (0x400, "flags"), (0x800, "cto")):
                            if fl & bit:
                                smp[key] = struct.unpack(">I", b[p:p + 4])[0]; p += 4
                        if i == 0 and first_flags is not None:
                            smp["flags"] = first_flags
                        tr["samples"].append(smp)
            r["tracks"].append(tr)
    return r


def check_segment(b, st, errs, label, independent):
    """st: running state {tfdt_next, mfhd}. Returns duration (90 kHz)."""
    items = list(boxes(b))
    types = [t for t, *_ in items]
    if not types or len(types) % 2 or any(types[i] != "moof" or types[i + 1] != "mdat" for i in range(0, len(types), 2)):
        errs.append(f"{label}: boxes {types[:8]}"); return 0
    total = 0
    for i in range(0, len(items), 2):
        _, o, s, h = items[i]
        _, mo, ms, mh = items[i + 1]
        m = parse_moof(b, o, s, h)
        if st.get("mfhd") is not None and m["seq"] <= st["mfhd"]:
            errs.append(f"{label}: mfhd {m['seq']} after {st['mfhd']}")
        st["mfhd"] = m["seq"]
        if len(m["tracks"]) != 1 or m["tracks"][0]["track"] != 1:
            errs.append(f"{label}: tracks {[t['track'] for t in m['tracks']]}"); continue
        tr = m["tracks"][0]
        if st.get("tfdt_next") is not None and tr["tfdt"] != st["tfdt_next"]:
            errs.append(f"{label}: tfdt {tr['tfdt']} expected {st['tfdt_next']} (gap {tr['tfdt'] - st['tfdt_next']})")
        sizes = sum(x.get("size", 0) for x in tr["samples"])
        if sizes != ms - mh:
            errs.append(f"{label}: trun sizes {sizes} != mdat payload {ms - mh}")
        if tr["data_offset"] != (mo + mh) - o:
            errs.append(f"{label}: data_offset {tr['data_offset']} != {(mo + mh) - o}")
        p = mo + mh
        for k, smp in enumerate(tr["samples"]):
            end = p + smp["size"]; q = p; nal_types = []
            while q < end:
                if q + 4 > end:
                    errs.append(f"{label}: NAL length runs past sample"); break
                ln = struct.unpack(">I", b[q:q + 4])[0]
                if ln == 0 or q + 4 + ln > end:
                    errs.append(f"{label}: NAL length {ln} past sample end"); break
                nal_types.append(b[q + 4] & 31); q += 4 + ln
            if k == 0 and i == 0 and independent:
                if smp.get("flags", 0) & 0x00010000 or 5 not in nal_types:
                    errs.append(f"{label}: independent segment does not start with an IDR ({nal_types}, flags {smp.get('flags', 0):#x})")
            if smp.get("dur", 0) == 0:
                errs.append(f"{label}: sample duration 0")
            p = end
        d = sum(x.get("dur", 0) for x in tr["samples"])
        st["tfdt_next"] = tr["tfdt"] + d
        total += d
    return total


# ---- playlist ----------------------------------------------------------------------------
def parse_playlist(text, errs):
    lines = [l.strip() for l in text.splitlines() if l.strip()]
    pl = {"segs": [], "target": None, "mseq": 0, "map": None, "version": None, "indep": False}
    if not lines or lines[0] != "#EXTM3U":
        errs.append("playlist: first line is not #EXTM3U"); return pl
    dur = None
    for l in lines[1:]:
        if l.startswith("#EXT-X-TARGETDURATION:"):
            pl["target"] = int(l.split(":", 1)[1])
        elif l.startswith("#EXT-X-MEDIA-SEQUENCE:"):
            pl["mseq"] = int(l.split(":", 1)[1])
        elif l.startswith("#EXT-X-VERSION:"):
            pl["version"] = int(l.split(":", 1)[1])
        elif l.startswith("#EXT-X-MAP:"):
            m = re.search(r'URI="([^"]+)"', l)
            pl["map"] = m.group(1) if m else None
        elif l == "#EXT-X-INDEPENDENT-SEGMENTS":
            pl["indep"] = True
        elif l.startswith("#EXTINF:"):
            dur = float(l[8:].split(",", 1)[0])
        elif l.startswith("#EXT-X-ENDLIST"):
            errs.append("playlist: live playlist has ENDLIST")
        elif l.startswith("#"):
            errs.append(f"playlist: unexpected tag {l}")
        else:
            if dur is None:
                errs.append(f"playlist: URI without EXTINF: {l}")
            pl["segs"].append((pl["mseq"] + len(pl["segs"]), l, dur)); dur = None
    if pl["target"] is None: errs.append("playlist: no TARGETDURATION")
    if pl["version"] is None or pl["version"] < 6: errs.append(f"playlist: VERSION {pl['version']} (EXT-X-MAP needs >= 6)")
    if not pl["map"]: errs.append("playlist: no EXT-X-MAP")
    for seq, uri, d in pl["segs"]:
        if pl["target"] is not None and d is not None and round(d) > pl["target"]:
            errs.append(f"playlist: EXTINF {d} > TARGETDURATION {pl['target']}")
        if uri != f"seg{seq}.m4s":
            errs.append(f"playlist: segment {seq} is named {uri}")
    return pl


def client(url, secs, name):
    errs, notes = [], []
    st = {}
    seen = {}               # seq -> (uri, dur)
    next_seq = None
    target = None
    last_mseq = -1
    first_mseq = None
    fetched = 0; media90 = 0; bytes_ = 0
    t_end = time.time() + secs
    init_ok = False
    reloads = 0
    while time.time() < t_end and len(errs) < 20:
        t0 = time.time()
        code, hd, body = fetch(url)
        reloads += 1
        if code != 200:
            errs.append(f"playlist HTTP {code}: {body[:80]!r}"); break
        ct = hd.get("Content-Type", "")
        if ct != "application/vnd.apple.mpegurl": errs.append(f"playlist content type {ct}")
        if hd.get("Access-Control-Allow-Origin") != "*": errs.append("playlist without CORS header")
        pl = parse_playlist(body.decode("utf-8", "replace"), errs)
        if target is None: target = pl["target"]
        elif pl["target"] != target: errs.append(f"TARGETDURATION changed {target} -> {pl['target']}")
        if pl["mseq"] < last_mseq: errs.append(f"MEDIA-SEQUENCE went back {last_mseq} -> {pl['mseq']}")
        if first_mseq is None: first_mseq = pl["mseq"]
        last_mseq = pl["mseq"]
        for seq, uri, d in pl["segs"]:
            if seq in seen and seen[seq] != (uri, d):
                errs.append(f"segment {seq} changed between reloads: {seen[seq]} -> {(uri, d)}")
            seen[seq] = (uri, d)
        total = sum(d for _, _, d in pl["segs"])
        if pl["mseq"] > first_mseq and target and total < 3 * target - 0.3:
            errs.append(f"playlist only {total:.2f} s after removals (< 3 x {target})")
        if not init_ok:
            code, hd, ib = fetch(urllib.parse.urljoin(url, pl["map"]))
            if code != 200: errs.append(f"init HTTP {code}"); break
            check_init(ib, errs); init_ok = True
            if hd.get("Access-Control-Allow-Origin") != "*": errs.append("init without CORS header")
        segs = pl["segs"]
        if next_seq is None and segs:
            next_seq = segs[max(0, len(segs) - 3)][0]      # hls.js: 3 segments from the live edge
        new = 0
        for seq, uri, d in segs:
            if next_seq is None or seq < next_seq: continue
            if seq > next_seq:
                errs.append(f"missed segments {next_seq}..{seq - 1}")
            code, hd, sb = fetch(urllib.parse.urljoin(url, uri))
            if code != 200:
                errs.append(f"{uri}: HTTP {code}"); next_seq = seq + 1; continue
            if int(hd.get("Content-Length", -1)) != len(sb): errs.append(f"{uri}: short body {len(sb)}")
            d90 = check_segment(sb, st, errs, uri, pl["indep"])
            if abs(d90 / 90000 - d) > 0.0015: errs.append(f"{uri}: media {d90 / 90000:.4f} s, EXTINF {d}")
            fetched += 1; media90 += d90; bytes_ += len(sb); new += 1
            next_seq = seq + 1
        wait = (target or 2) if new else (target or 2) / 2
        time.sleep(max(0.05, wait - (time.time() - t0)))
    secs_media = media90 / 90000
    status = "PASS" if not errs else "FAIL"
    print(f"{status} client {name}: {fetched} segments, {secs_media:.1f} s media, {bytes_ * 8 / max(secs_media, 0.1) / 1e6:.2f} Mbit/s, "
          f"target {target}, seq {first_mseq}..{next_seq}, {reloads} reloads" + "".join(f"\n  {e}" for e in errs[:10]))
    if not errs and secs_media < secs * 0.6:
        print(f"FAIL client {name}: only {secs_media:.1f} s of media in {secs} s"); return 1
    return 0 if not errs else 1


def ffprobe(path, min_secs):
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "packet=dts_time,duration_time,flags",
                          "-of", "csv=p=0", path], capture_output=True, text=True).stdout.split("\n")
    pk = []
    for l in out:
        f = l.split(",")
        if len(f) >= 2 and f[0] not in ("", "N/A"):
            pk.append((float(f[0]), float(f[1]) if f[1] not in ("", "N/A") else 0.0))
    errs = []
    if len(pk) < 10: errs.append(f"only {len(pk)} packets")
    gaps = 0
    for (a, da), (b, db) in zip(pk, pk[1:]):
        if b <= a: errs.append(f"DTS not increasing {a} -> {b}")
        elif b - a > 3 * max(da, 0.04): gaps += 1; errs.append(f"gap {a:.3f} -> {b:.3f}")
    dur = pk[-1][0] - pk[0][0] if pk else 0
    if dur < min_secs: errs.append(f"duration {dur:.1f} s < {min_secs}")
    print(("PASS" if not errs else "FAIL") + f" ffprobe {path.split('/')[-1]}: {len(pk)} frames, {dur:.1f} s, {gaps} gaps"
          + "".join(f"\n  {e}" for e in errs[:8]))
    return 0 if not errs else 1


def livemp4(url, secs, name, out=None, slack=0.6):
    """Read the endless live.mp4 for secs seconds like a player would (no
    faster than it arrives) and check: ftyp + moov first; then moof+mdat, one
    frame each, the first a keyframe; tfdt contiguous (each fragment starts
    where the previous ended); media time keeps pace with the wall clock
    (nothing old dumped at the start, no drift): |media - wall| <= slack."""
    errs = []
    t0 = time.time()
    try:
        r = urllib.request.urlopen(url, timeout=20)
    except Exception as e:
        print(f"FAIL livemp4 {name}: {e}"); return 1
    ctype = r.headers.get("Content-Type", ""); clen = r.headers.get("Content-Length")
    if ctype != "video/mp4": errs.append(f"Content-Type {ctype}")
    if clen is not None: errs.append(f"Content-Length {clen} on an endless stream")
    if r.headers.get("Access-Control-Allow-Origin") != "*": errs.append("no CORS header")
    sink = open(out, "wb") if out else None
    def box():
        h = r.read(8)
        if len(h) < 8: return None, None
        n, t = struct.unpack(">I4s", h)
        body = r.read(n - 8) if n >= 8 else b""
        if sink: sink.write(h + body)
        return t.decode("latin-1"), body
    first_byte = None; types = []; frags = 0; next_dec = None; t_first = None; dec0 = None
    lead = lag = 0.0; scale = 90000; keyfirst = None; durs = []; trace = []
    while time.time() - t0 < secs:
        t, b = box()
        if t is None: errs.append("stream ended"); break
        if first_byte is None: first_byte = time.time() - t0
        if len(types) < 2: types.append(t)
        if t == "moov":
            m = b.find(b"mdhd")
            if m > 0: scale = struct.unpack(">I", b[m + 16:m + 20])[0] if b[m + 4] == 0 else struct.unpack(">I", b[m + 24:m + 28])[0]
        if t != "moof": continue
        o = b.find(b"tfdt"); tr = b.find(b"trun")
        if o < 0 or tr < 0: errs.append("moof without tfdt/trun"); continue
        dec = struct.unpack(">Q", b[o + 8:o + 16])[0]
        n = struct.unpack(">I", b[tr + 8:tr + 12])[0]
        if n != 1: errs.append(f"fragment {frags}: {n} samples (want 1)")
        dur, _, fl = struct.unpack(">III", b[tr + 16:tr + 28])
        if keyfirst is None: keyfirst = fl == 0x02000000
        if next_dec is not None and dec != next_dec: errs.append(f"fragment {frags}: tfdt {dec}, expected {next_dec}")
        next_dec = dec + dur; durs.append(dur); frags += 1
        now = time.time()
        if t_first is None: t_first, dec0 = now, dec
        media = (dec - dec0) / scale; wall = now - t_first
        lead = max(lead, media - wall); lag = max(lag, wall - media); trace.append((wall, wall - media))
    r.close()
    if sink: sink.close()
    if types[:2] != ["ftyp", "moov"]: errs.append(f"starts with {types}")
    if not keyfirst: errs.append("first fragment is not a keyframe")
    if frags < 10: errs.append(f"only {frags} fragments")
    # the newest frame is sent when the one after it arrives: one frame of lag is normal
    if lead > slack: errs.append(f"media ran {lead:.2f} s ahead of the wall clock (backlog dumped)")
    if lag > slack + (max(durs) / scale if durs else 0): errs.append(f"media fell {lag:.2f} s behind the wall clock")
    st = "PASS" if not errs else "FAIL"
    print(f"{st} livemp4 {name}: {frags} fragments, first byte {first_byte or 0:.2f} s, "
          f"max lead {lead:.2f} s, max lag {lag:.2f} s, end lag {trace[-1][1] if trace else 0:.2f} s, "
          f"lag per 5 s: {' '.join(f'{max(l for w, l in trace if k * 5 <= w < k * 5 + 5):.2f}' for k in range(int(trace[-1][0] // 5) + 1) if any(k * 5 <= w < k * 5 + 5 for w, l in trace)) if trace else ''}, "
          f"durations {min(durs, default=0)}..{max(durs, default=0)}"
          + "".join(f"\n  {e}" for e in errs[:8]))
    return 0 if not errs else 1


def slow(url, rate):
    u = urllib.parse.urlsplit(url)
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    s.connect((u.hostname, u.port or 80))
    s.sendall(f"GET {u.path} HTTP/1.1\r\nHost: {u.netloc}\r\nConnection: close\r\n\r\n".encode())
    s.settimeout(30)
    buf = b""
    while b"\r\n\r\n" not in buf:
        d = s.recv(4096)
        if not d: break
        buf += d
    head, body = buf.split(b"\r\n\r\n", 1) if b"\r\n\r\n" in buf else (buf, b"")
    m = re.search(rb"Content-Length: (\d+)", head)
    total = int(m.group(1)) if m else -1
    got = len(body); t0 = time.time()
    while True:
        try:
            d = s.recv(1024)
        except socket.timeout:
            break
        if not d: break
        got += len(d)
        ahead = got / rate - (time.time() - t0)
        if ahead > 0: time.sleep(ahead)
    print(f"slow: got {got} of {total} in {time.time() - t0:.1f} s ({head.split(b' ')[1].decode()})")


def get(url):
    code, hd, body = fetch(url)
    print(code, hd.get("Content-Type", "-"), hd.get("Access-Control-Allow-Origin", "-"), len(body))


def tfdt(url):
    """First decode time and length (90 kHz) of a segment: "tfdt dur"."""
    code, hd, b = fetch(url)
    errs = []; st = {}
    d = check_segment(b, st, errs, url, False) if code == 200 else 0
    first = parse_moof(b, *next((o, s, h) for t, o, s, h in boxes(b) if t == "moof"))["tracks"][0]["tfdt"] if code == 200 else -1
    print(first, d)


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "client": sys.exit(client(sys.argv[2], float(sys.argv[3]), sys.argv[4] if len(sys.argv) > 4 else "1"))
    if cmd == "ffprobe": sys.exit(ffprobe(sys.argv[2], float(sys.argv[3])))
    if cmd == "livemp4": sys.exit(livemp4(sys.argv[2], float(sys.argv[3]), sys.argv[4],
                                          sys.argv[5] if len(sys.argv) > 5 and sys.argv[5] != "-" else None,
                                          float(sys.argv[6]) if len(sys.argv) > 6 else 0.6))
    if cmd == "slow": slow(sys.argv[2], float(sys.argv[3])); sys.exit(0)
    if cmd == "get": get(sys.argv[2]); sys.exit(0)
    if cmd == "tfdt": tfdt(sys.argv[2]); sys.exit(0)
    sys.exit(__doc__)
