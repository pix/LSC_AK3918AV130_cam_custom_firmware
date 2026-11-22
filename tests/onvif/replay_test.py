"""ONVIF replay (RTSP rtsp://camera/replay/<token>) end to end.

Usage: replay_test.py host user pass rec_dir
Needs the recordings profile_g_test.py leaves (the recorder's segment + the
crafted day pg_common.FIX_DAY); makes them itself if they are missing.
  - ffprobe / ffmpeg: frames decode, timestamps monotonic, duration and frame
    count match the file, UDP, seeking (-ss), no decoder errors
  - raw RTSP client: ONVIF replay header extension (NTP time, C/E/D/T, CSeq),
    Range npt= / clock= (start and end), PAUSE + resume, re-PLAY while playing,
    Scale / Frames, Rate-Control: no, RTCP BYE at the end, playlists of a day
    (gaps, a file cut short, an unplayable file skipped)
  - errors: unknown / traversal / timelapse tokens, unplayable file, no or
    wrong login, PLAY before SETUP, range past the end, concurrent limit (2),
    live streaming unaffected while 2 replays run
"""
import datetime, json, os, struct, subprocess, sys, tempfile, threading, time
sys.dont_write_bytecode = True                  # no __pycache__ in the source tree
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pg_common as C
from pg_common import check, Rtsp, parse_rtp, nal_type

H, U, P, REC = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
UTC = datetime.timezone.utc
http = C.Http(H, U, P)
http.config(**{"onvif.replay": "on", "time.tz": "UTC"})


def find_real():
    """the recorder's longest finished segment (stopping the recorder leaves a short one too)"""
    best = None
    for x in json.loads(http.get("/api/rec/all")):
        if not x.get("r") and x["n"].endswith("_cont.mp4") and x["d"] not in (C.FIX_DAY, C.FIX_DAY2):
            if not best or x["s"] > best["s"]:
                best = x
    return (best["d"], best["n"]) if best and best["s"] > 200000 else (None, None)


day, name = find_real()
if not day:                                         # run alone: make the recordings first
    http.config(**{"record.audio": "on", "record.segment": "1", "record.keep_free": "100", "record.mode": "continuous"})
    day, name = C.wait_recording(http)
    http.config(**{"record.mode": "off"})
    time.sleep(2)
if not os.path.isdir(os.path.join(REC, C.FIX_DAY)):
    C.make_fixture(REC, os.path.join(REC, day, name))
W = tempfile.mkdtemp()
ref = os.path.join(W, "ref.mp4")
with open(ref, "wb") as f:
    f.write(http.get(f"/rec/{day}/{name}", timeout=60))
FTOK = f"{day}_{name[:-4]}"
URI = f"rtsp://{H}:554/replay/{FTOK}"
AURI = f"rtsp://{U}:{P}@{H}:554/replay/{FTOK}"
start_utc = datetime.datetime.strptime(f"{day} {name[:8]}", "%Y-%m-%d %H-%M-%S").replace(tzinfo=UTC).timestamp()

j, _ = C.ffprobe_json(ref, "-select_streams", "v", "-show_entries", "packet=pts_time,flags")
ref_pts = [float(p["pts_time"]) for p in j.get("packets", [])]
ref_keys = sum(1 for p in j.get("packets", []) if "K" in p.get("flags", ""))
ref_dur = C.media_duration(ref)
print(f"   reference {day}/{name}: {len(ref_pts)} video frames, {ref_keys} keyframes, {ref_dur:.2f} s")

# ---- ffprobe / ffmpeg ------------------------------------------------------------------
t0 = time.time()
r = subprocess.run(["ffprobe", "-v", "error", "-rtsp_transport", "tcp", "-of", "json", "-show_entries",
                    "frame=pts_time,key_frame,media_type:stream=codec_name,codec_type,width,height", AURI],
                   capture_output=True, text=True, timeout=300)
took = time.time() - t0
fr = json.loads(r.stdout or "{}")
vf = [f for f in fr.get("frames", []) if f.get("media_type") == "video"]
af = [f for f in fr.get("frames", []) if f.get("media_type") == "audio"]
vp = [float(f["pts_time"]) for f in vf if "pts_time" in f]     # the very first frame may have none (before the first RTCP SR)
codecs = sorted(s["codec_name"] for s in fr.get("streams", []))
check(codecs == ["h264", "pcm_mulaw"], f"ffprobe streams {codecs}")
check(len(vf) == len(ref_pts), f"ffprobe decoded {len(vf)} video frames (file has {len(ref_pts)})")
check(all(b > a for a, b in zip(vp, vp[1:])), "video timestamps strictly increasing")
check(vp and abs((vp[-1] - vp[0]) - (ref_pts[-1] - ref_pts[0])) < 0.15,
      f"video span {vp[-1] - vp[0]:.3f} s (file {ref_pts[-1] - ref_pts[0]:.3f} s)" if vp else "video span")
check(len(af) > 0 and abs(took - ref_dur) < 6, f"audio frames {len(af)}; real-time pacing: {took:.1f} s for {ref_dur:.1f} s, ended by BYE")
check(not r.stderr.strip(), f"ffprobe without errors {r.stderr.strip()[:200]}")

def ff(*args, t=8):
    p = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", *args, "-t", str(t), "-f", "null", "-"], capture_output=True, text=True, timeout=120)
    return p.returncode, p.stderr.strip()

rc, err = ff("-rtsp_transport", "udp", "-i", AURI)
check(rc == 0 and not err, f"ffmpeg UDP 8 s: rc {rc} {err[:200]}")
rc, err = ff("-ss", "30", "-rtsp_transport", "tcp", "-i", AURI, "-fps_mode", "passthrough", t=4)
check(rc == 0 and not err, f"ffmpeg -ss 30 (PLAY Range npt=30-): rc {rc} {err[:200]}")
rc, err = ff("-rtsp_transport", "tcp", "-i", f"rtsp://{U}:{P}@{H}:554/replay/{C.FIX_DAY}", t=6)
check(rc == 0 and not err, f"ffmpeg day playlist: rc {rc} {err[:200]}")


# ---- raw client: whole file, fast, with the ONVIF extension -----------------------------------------
def collect(c, timeout=10.0, until_bye=True, limit=None):
    """RTP of a PLAY: list of (channel, parsed); bye seen?"""
    pk, bye = [], False
    for ch, d in c.frames(timeout):
        if ch in (1, 3):
            o, this = 0, False
            while o + 4 <= len(d):
                if d[o + 1] == 203:
                    this = True
                o += 4 + 4 * struct.unpack(">H", d[o + 2:o + 4])[0]
            bye = bye or this
            if this and until_bye and ch == 1:      # (a BYE left over from an earlier PLAY does not count here)
                break
            continue
        x = parse_rtp(d)
        if x:
            pk.append((ch, x))
        if limit and len(pk) >= limit:
            break
    return pk, bye


def aus(pk):
    """video access units: list of (packets) split at the marker bit"""
    out, cur = [], []
    for ch, x in pk:
        if ch != 0:
            continue
        cur.append(x)
        if x["marker"]:
            out.append(cur)
            cur = []
    return out


c = Rtsp(H, U, P)
code, sdp = c.open_replay(URI)
check(code == 200 and "a=range:npt=0-" in sdp and "sprop-parameter-sets=" in sdp and "PCMU/8000" in sdp, f"DESCRIBE/SETUP replay: {code}")
code, hdr, _ = c.request("PLAY", URI, {"Range": "npt=0-", "Require": "onvif-replay", "Rate-Control": "no"})
play_cseq = c.cseq
check(code == 200 and hdr.get("range", "").startswith("npt=0.000-") and "rtptime=" in hdr.get("rtp-info", ""),
      f"PLAY: {code} Range {hdr.get('range')} RTP-Info {hdr.get('rtp-info', '')[:60]}")
t0 = time.time()
pk, bye = collect(c, 20)
fast = time.time() - t0
v = [x for ch, x in pk if ch == 0]
a = [x for ch, x in pk if ch == 2]
units = aus(pk)
check(bye and fast < ref_dur / 2, f"Rate-Control: no: {len(v)} video / {len(a)} audio packets in {fast:.1f} s, RTCP BYE at the end")
check(len(units) == len(ref_pts), f"{len(units)} access units (file {len(ref_pts)})")
check(all(x["ext"] for _, x in pk), "every RTP packet has the 0xABAC extension")
ntp = [u[0]["ext"][0] for u in units]
check(ntp and abs(ntp[0] - start_utc) < 0.01 and all(b >= a_ for a_, b in zip(ntp, ntp[1:])),
      f"NTP times: first {ntp[0] - start_utc:+.3f} s from the file name, non-decreasing" if ntp else "NTP times")
check(ntp and abs(ntp[-1] - start_utc - (ref_pts[-1] - ref_pts[0])) < 0.2, f"last frame at +{ntp[-1] - start_utc:.2f} s" if ntp else "last")
keys = [u for u in units if any(nal_type(x) in (5, 0x105) for x in u)]
cbits = all(all(x["ext"][1] & 0x80 for x in u) for u in keys) and not any(x["ext"][1] & 0x80 for u in units if u not in keys for x in u)
check(len(keys) == ref_keys and cbits, f"C bit exactly on the {len(keys)} keyframes")
check(all(x["ext"][2] == play_cseq & 0xFF for _, x in pk), "CSeq byte = PLAY's CSeq")
check(units and all(x["ext"][1] & 0x10 for x in units[-1]) and not any(x["ext"][1] & 0x10 for u in units[:-1] for x in u),
      "T (and E) bit only on the last frame")
samples = sum(len(x["payload"]) for x in a)
check(abs(samples / 8000 - ref_dur) < 1.5, f"audio: {samples / 8000:.2f} s of PCMU")
vseq = [x["seq"] for x in v]
check(all((b - a_) & 0xFFFF == 1 for a_, b in zip(vseq, vseq[1:])), "video RTP sequence numbers contiguous")
c.request("TEARDOWN", URI)
c.close()

# ---- npt seek, re-PLAY while playing, PAUSE / resume (paced) -----------------------------------------
c = Rtsp(H, U, P)
c.open_replay(URI)
code, hdr, _ = c.request("PLAY", URI, {"Range": "npt=30-", "Require": "onvif-replay"})
rs = float(hdr.get("range", "npt=-1-").split("=")[1].split("-")[0])
check(code == 200 and 25 <= rs <= 30, f"PLAY npt=30-: answered Range {hdr.get('range')}")
pk, _ = collect(c, 3, limit=40)
first = next((x for ch, x in pk if ch == 0), None)
check(first and abs(first["ext"][0] - start_utc - rs) < 0.05 and first["ext"][1] & 0x20,
      f"first frame at +{first['ext'][0] - start_utc:.2f} s with the D bit" if first else "no packet after the seek")
code, hdr, _ = c.request("PLAY", URI, {"Range": "npt=10-", "Require": "onvif-replay"})
cs2 = c.cseq & 0xFF
pk, _ = collect(c, 2, limit=400)
new = [x for ch, x in pk if ch == 0 and x["ext"][2] == cs2]
check(code == 200 and new and abs(new[0]["ext"][0] - start_utc - 10) < 5 and new[0]["ext"][1] & 0x20,
      f"re-PLAY npt=10- while playing: new CSeq, at +{new[0]['ext'][0] - start_utc:.2f} s" if new else f"re-PLAY: {code}")
code, _, _ = c.request("PAUSE", URI)
list(c.frames(0.5))
pk, _ = collect(c, 1.5)
check(code == 200 and not pk, f"PAUSE: {code}, {len(pk)} packets after it")
last_ntp = new[-1]["ext"][0] if new else 0
code, hdr, _ = c.request("PLAY", URI, {"Require": "onvif-replay"})
pk, _ = collect(c, 2, limit=50)
rv = [x for ch, x in pk if ch == 0]
check(code == 200 and rv and 0 <= rv[0]["ext"][0] - last_ntp < 3, f"resume: continues at +{rv[0]['ext'][0] - start_utc:.2f} s" if rv else "resume")
c.request("TEARDOWN", URI)
c.close()

# ---- the crafted day: clock ranges, gaps, files cut short / unplayable -----------------------------
DURI = f"rtsp://{H}:554/replay/{C.FIX_DAY}"
def at(hms):
    return datetime.datetime.strptime(f"{C.FIX_DAY} {hms}", "%Y-%m-%d %H:%M:%S").replace(tzinfo=UTC).timestamp()

c = Rtsp(H, U, P)
code, sdp = c.open_replay(DURI)
code, hdr, _ = c.request("PLAY", DURI, {"Range": "clock=20250115T095000Z-", "Require": "onvif-replay", "Rate-Control": "no"})
check(code == 200 and hdr.get("range", "").startswith("clock=20250115T100000.000Z-"), f"day PLAY from before the first file: {hdr.get('range')}")
pk, bye = collect(c, 30)
units = aus(pk)
ntp = [u[0]["ext"][0] for u in units]
segs, s0 = [], None
for i, t in enumerate(ntp):
    if s0 is None or t - ntp[i - 1] > 2:
        segs.append([t, t, i])
        s0 = t
    segs[-1][1] = t
starts = [round(s[0] - at("10:00:00")) for s in segs]
check(bye and starts[:3] == [0, 1800, 1890] and len(segs) == 4, f"day playlist: segments start at +{starts} s (motion, sound, event, cut-short cont; unplayable skipped)")
gap_ok = all(all(x["ext"][1] & 0x40 for x in units[s[2] - 1]) and all(x["ext"][1] & 0x20 for x in units[s[2]]) for s in segs[1:3])
check(gap_ok, "E bit before each gap, D bit after it")
check(len(units) > 3 * len(ref_pts) and len(units) < 4 * len(ref_pts), f"{len(units)} frames: 3 whole files + the cut-short one")
check(all(b >= a_ for a_, b in zip(ntp, ntp[1:])), "NTP times never go back across files")
c.close()

c = Rtsp(H, U, P)
c.open_replay(DURI)
code, hdr, _ = c.request("PLAY", DURI, {"Range": "clock=20250115T103010Z-20250115T103020Z", "Require": "onvif-replay", "Rate-Control": "no"})
pk, bye = collect(c, 10)
units = aus(pk)
ntp = [u[0]["ext"][0] for u in units]
check(code == 200 and bye and ntp and at("10:30:10") - 4 <= ntp[0] <= at("10:30:10") and at("10:30:19") <= ntp[-1] <= at("10:30:20") + 0.5 if ntp else False,
      f"clock=10:30:10-10:30:20: {hdr.get('range')}, frames +{ntp[0] - at('10:30:00'):.1f}..+{ntp[-1] - at('10:30:00'):.1f} s" if ntp else f"clock range: {code}")
check(units and all(x["ext"][1] & 0x10 for x in units[-1]), "T bit at the range end")
c.close()

# ---- Scale / Frames ---------------------------------------------------------------------------
c = Rtsp(H, U, P)
c.open_replay(URI)
code, hdr, _ = c.request("PLAY", URI, {"Range": "npt=0-", "Scale": "4.0", "Require": "onvif-replay"})
pk, _ = collect(c, 3)
units = aus(pk)
check(code == 200 and hdr.get("scale") == "4.00" and units and all(any(nal_type(x) in (5, 0x105) for x in u) for u in units),
      f"Scale 4: {len(units)} frames, all keyframes")
code, _, _ = c.request("PLAY", URI, {"Scale": "-1"})
check(code == 456, f"Scale -1 (reverse) refused: {code}")
code, _, _ = c.request("PLAY", URI, {"Scale": "0"})
check(code == 456, f"Scale 0 refused: {code}")
code, _, _ = c.request("PLAY", URI, {"Range": "npt=99999-"})
check(code == 457, f"Range past the end: {code}")
code, _, _ = c.request("PLAY", URI, {"Range": "smpte=0:00:10-"})
check(code == 457, f"Range smpte refused: {code}")
code, _, _ = c.request("PLAY", URI, {"Range": "npt=0-", "Frames": "intra"})
pk, _ = collect(c, 2)
units = aus(pk)
check(code == 200 and units and all(any(nal_type(x) in (5, 0x105) for x in u) for u in units) and not any(ch == 2 for ch, _ in pk),
      f"Frames: intra: {len(units)} keyframes, no audio")
c.close()

# ---- errors ----------------------------------------------------------------------------------
def describe(path, user=U, pw=P, auth=True):
    c = Rtsp(H, user, pw)
    try:
        code, _, _ = c.request("DESCRIBE", f"rtsp://{H}:554{path}", {"Accept": "application/sdp"}, auth=auth)
    except Exception as e:
        code = str(e)
    c.close()
    return code

for path in ("/replay/2030-01-01", "/replay/../../etc/passwd", "/replay/..%2F..%2Fetc%2Fpasswd", "/replay/2025-01-15/../../x",
             f"/replay/{C.FIX_DAY}_12-00-00_timelapse", f"/replay/{C.FIX_DAY}_10-00-00_motion.mp4", "/replay/", "/replay/" + "9" * 300,
             f"/replay/{C.FIX_DAY}_../../../etc/passwd", "/replay/2025-13-45"):
    check(describe(path) == 404, f"DESCRIBE {path[:50]} -> 404")
check(describe(f"/replay/{C.FIX_DAY}_11-30-00_motion") == 415, "unplayable file -> 415")
check(describe(f"/replay/{C.FIX_DAY}", auth=False) == 401, "no login -> 401")
check(describe(f"/replay/{C.FIX_DAY}", pw=P + "x") == 401, "wrong password -> 401")
c = Rtsp(H, U, P)
c.request("DESCRIBE", DURI)
code, _, _ = c.request("PLAY", DURI)
check(code == 455, f"PLAY before SETUP -> {code}")
c.close()

# ---- concurrency: 2 replays at once, the third refused, live unaffected -----------------------------
cs = []
for i in range(2):
    x = Rtsp(H, U, P)
    code, _ = x.open_replay(URI if i else DURI)
    code2, _, _ = x.request("PLAY", URI if i else DURI, {"Range": "npt=0-"})
    cs.append(x)
    check(code == 200 and code2 == 200, f"replay session {i + 1} playing")
check(describe(f"/replay/{C.FIX_DAY}") == 503, "third replay session -> 503")
stop = threading.Event()
def drain(x):
    while not stop.is_set():
        for _ in x.frames(0.2):
            pass
th = [threading.Thread(target=drain, args=(x,)) for x in cs]
for t in th:
    t.start()
rc, err = ff("-rtsp_transport", "tcp", "-i", f"rtsp://{U}:{P}@{H}:554/", t=5)
check(rc == 0 and not err, f"live stream while 2 replays run: rc {rc} {err[:200]}")
stop.set()
for t in th:
    t.join()
cs[0].request("TEARDOWN", DURI)
time.sleep(0.3)
check(describe(f"/replay/{C.FIX_DAY}") == 200, "after a TEARDOWN a new replay session is accepted")
for x in cs:
    x.close()
time.sleep(1)

# ---- the file being recorded: replay follows it, ends when the recorder closes it ---------------------
http.config(**{"record.mode": "continuous"})
live = None
for _ in range(40):
    live = next((x for x in json.loads(http.get("/api/rec/all")) if x.get("r")), None)
    if live and live["s"] > 100000:
        break
    time.sleep(0.5)
if check(live is not None, f"recorder writing {live['d'] + '/' + live['n'] if live else None}"):
    LURI = f"rtsp://{H}:554/replay/{live['d']}_{live['n'][:-4]}"
    c = Rtsp(H, U, P)
    code, _ = c.open_replay(LURI)
    code2, _, _ = c.request("PLAY", LURI, {"Range": "npt=0-", "Require": "onvif-replay", "Rate-Control": "no"})
    t_end = time.time() + 6
    seen, bye = [], False
    while time.time() < t_end and not bye:
        pk, bye = collect(c, 0.5, limit=300)
        seen += [x["ext"][0] for ch, x in pk if ch == 0]
    check(code == 200 and code2 == 200 and not bye and seen and max(seen) - min(seen) > 4,
          f"live file: still streaming after 6 s, {max(seen) - min(seen):.1f} s of video (followed the writer)" if seen else "live file: nothing")
    http.config(**{"record.mode": "off"})
    pk, bye = collect(c, 8)
    check(bye, "recorder stopped: the replay ends (BYE)")
    c.close()

# ---- switched off ------------------------------------------------------------------------------
http.config(**{"onvif.replay": "off"})
check(describe(f"/replay/{C.FIX_DAY}") == 404, "onvif.replay=off -> 404")
http.config(**{"onvif.replay": "on"})
check(subprocess.run(["curl", "-s", "-o", "/dev/null", "-m", "10", "-w", "%{http_code}", f"http://{H}/"],
                     capture_output=True, text=True).stdout == "401", "server alive")
print(f"replay: {C.fails} failed")
sys.exit(C.fails)
