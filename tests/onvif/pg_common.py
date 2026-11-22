"""Shared helpers for the ONVIF Profile G tests (profile_g_test.py, replay_test.py).

Runs against host_server inside tests/host/run_sandboxed.sh (or a camera in SD
test mode, without the fixture part). The fixture writes recordings straight
into the SD card's rec/ folder (the sandbox's tmpfs), next to the one the
recorder made: other days, motion/sound/event kinds, a gap, a file cut short,
an unplayable file, names that must be ignored.
"""
import base64, datetime, hashlib, json, os, random, re, shutil, socket, struct, sys, time, urllib.request

UTC = datetime.timezone.utc
fails = 0


def check(ok, what):
    global fails
    if not ok:
        fails += 1
    print(("PASS" if ok else "FAIL"), what, flush=True)
    return ok


class Http:
    """camera web API with Digest login"""

    def __init__(self, host, user, pw):
        self.base = f"http://{host}"
        pm = urllib.request.HTTPPasswordMgrWithDefaultRealm()
        pm.add_password(None, self.base, user, pw)
        self.op = urllib.request.build_opener(urllib.request.HTTPDigestAuthHandler(pm))

    def get(self, path, timeout=20):
        req = urllib.request.Request(self.base + path, headers={"X-Requested-With": "lsc"})
        with self.op.open(req, timeout=timeout) as r:
            return r.read()

    def config(self, **kv):
        """registered settings (/api/config); time.tz is a control command (/api/set)"""
        tz = kv.pop("time.tz", None)
        if tz:
            req = urllib.request.Request(self.base + "/api/set", data=f"name=time.tz&value={tz}".encode(),
                                         headers={"X-Requested-With": "lsc"})
            with self.op.open(req, timeout=20) as r:
                r.read()
        if not kv:
            return b""
        data = "&".join(f"{k}={v}" for k, v in kv.items()).encode()
        req = urllib.request.Request(self.base + "/api/config", data=data, headers={"X-Requested-With": "lsc"})
        with self.op.open(req, timeout=20) as r:
            return r.read()


def wsse_header(user, pw):
    nonce = os.urandom(16)
    created = datetime.datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%S.000Z")
    digest = base64.b64encode(hashlib.sha1(nonce + created.encode() + pw.encode()).digest()).decode()
    return ('<s:Header><wsse:Security xmlns:wsse="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd" '
            'xmlns:wsu="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd"><wsse:UsernameToken>'
            f'<wsse:Username>{user}</wsse:Username><wsse:Password Type="http://docs.oasis-open.org/wss/2004/01/'
            f'oasis-200401-wss-username-token-profile-1.0#PasswordDigest">{digest}</wsse:Password>'
            f'<wsse:Nonce EncodingType="http://docs.oasis-open.org/wss/2004/01/oasis-200401-soap-message-security-1.0#Base64Binary">'
            f'{base64.b64encode(nonce).decode()}</wsse:Nonce><wsu:Created>{created}</wsu:Created></wsse:UsernameToken>'
            '</wsse:Security></s:Header>')


def soap(host, path, body, user=None, pw=None, timeout=30):
    """raw SOAP 1.2 POST (WS-Security digest when user is given): (http status, text)"""
    env = ('<?xml version="1.0" encoding="UTF-8"?><s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" '
           'xmlns:tse="http://www.onvif.org/ver10/search/wsdl" xmlns:trc="http://www.onvif.org/ver10/recording/wsdl" '
           'xmlns:trp="http://www.onvif.org/ver10/replay/wsdl" xmlns:tt="http://www.onvif.org/ver10/schema" '
           'xmlns:wsnt="http://docs.oasis-open.org/wsn/b-2" xmlns:tds="http://www.onvif.org/ver10/device/wsdl">'
           + (wsse_header(user, pw) if user else "") + f"<s:Body>{body}</s:Body></s:Envelope>")
    req = urllib.request.Request(f"http://{host}{path}", data=env.encode(),
                                 headers={"Content-Type": "application/soap+xml; charset=utf-8"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read().decode(errors="replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")


# ---- recordings ----------------------------------------------------------------------
FIX_DAY = "2025-01-15"       # crafted days (UTC: the test sets time.tz=UTC)
FIX_DAY2 = "2025-01-14"


def wait_recording(http, timeout=200):
    """the recorder's first finished file: (day, name)"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            lst = json.loads(http.get("/api/rec/all"))
            done = [x for x in lst if not x.get("r") and x["n"].endswith("_cont.mp4")]
            if done:
                return done[0]["d"], done[0]["n"]
        except Exception as e:
            print("   (waiting:", e, ")")
        time.sleep(3)
    return None, None


def ffprobe_json(src, *args):
    import subprocess
    r = subprocess.run(["ffprobe", "-v", "error", "-of", "json", *args, src], capture_output=True, text=True, timeout=300)
    return json.loads(r.stdout or "{}"), r.stderr


def media_duration(path):
    j, _ = ffprobe_json(path, "-show_entries", "format=duration")
    return float(j.get("format", {}).get("duration", 0))


def make_fixture(rec_dir, real_path):
    """crafted recordings next to the real one; returns {name: (start, end)} of FIX_DAY"""
    d = os.path.join(rec_dir, FIX_DAY)
    os.makedirs(d, exist_ok=True)
    dur = media_duration(real_path)
    raw = open(real_path, "rb").read()
    files = {}

    def put(name, data, start, length):
        p = os.path.join(d if name[0] != "@" else os.path.join(rec_dir, FIX_DAY2), name.lstrip("@"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)
        end = start + length
        os.utime(p, (end, end))
        files[name.lstrip("@")] = (start, end)

    def t(day, hms):
        return datetime.datetime.strptime(f"{day} {hms}", "%Y-%m-%d %H:%M:%S").replace(tzinfo=UTC).timestamp()

    put("10-00-00_motion.mp4", raw, t(FIX_DAY, "10:00:00"), dur)
    put("10-30-00_sound.mp4", raw, t(FIX_DAY, "10:30:00"), dur)
    put("10-31-30_event.mp4", raw, t(FIX_DAY, "10:31:30"), dur)       # starts ~30 s after the sound clip ends
    put("11-00-00_cont.mp4", raw[: int(len(raw) * 0.6)], t(FIX_DAY, "11:00:00"), dur * 0.6)          # cut short
    random.seed(7)
    put("11-30-00_motion.mp4", bytes(random.getrandbits(8) for _ in range(50000)), t(FIX_DAY, "11:30:00"), 10)   # unplayable
    put("12-00-00_timelapse.mp4", raw, t(FIX_DAY, "12:00:00"), 5)                                     # never listed
    for junk in ("12-00-00_motion.jpg", "notavideo.txt", ".hidden.mp4", "12-10-00_other.mp4"):
        with open(os.path.join(d, junk), "wb") as f:
            f.write(b"x" * 100)
    os.makedirs(os.path.join(rec_dir, "not-a-day"), exist_ok=True)
    put("@23-59-30_cont.mp4", raw, t(FIX_DAY2, "23:59:30"), dur)                                       # past midnight
    return files, dur


# ---- a small RTSP client (Digest, interleaved TCP) --------------------------------------------
class Rtsp:
    def __init__(self, host, user, pw, port=554, timeout=15):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.host, self.user, self.pw = host, user, pw
        self.cseq = 0
        self.auth = None
        self.session = None
        self.buf = b""
        self.pending = []           # interleaved frames received while waiting for a response

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass

    def _digest(self, method, uri):
        if not self.auth:
            return None
        realm, nonce = self.auth
        ha1 = hashlib.md5(f"{self.user}:{realm}:{self.pw}".encode()).hexdigest()
        ha2 = hashlib.md5(f"{method}:{uri}".encode()).hexdigest()
        resp = hashlib.md5(f"{ha1}:{nonce}:{ha2}".encode()).hexdigest()
        return f'Digest username="{self.user}", realm="{realm}", nonce="{nonce}", uri="{uri}", response="{resp}"'

    def _read_response(self):
        while True:
            while self.buf[:1] == b"$":
                if len(self.buf) < 4:
                    break
                n = struct.unpack(">H", self.buf[2:4])[0]
                if len(self.buf) < 4 + n:
                    break
                self.pending.append((self.buf[1], self.buf[4:4 + n]))
                self.buf = self.buf[4 + n:]
            i = self.buf.find(b"\r\n\r\n")
            if self.buf[:1] != b"$" and i >= 0:
                head = self.buf[:i].decode(errors="replace")
                lines = head.split("\r\n")
                hdr = {}
                for l in lines[1:]:
                    if ":" in l:
                        k, v = l.split(":", 1)
                        hdr[k.strip().lower()] = v.strip()
                n = int(hdr.get("content-length", "0"))
                if len(self.buf) >= i + 4 + n:
                    body = self.buf[i + 4:i + 4 + n].decode(errors="replace")
                    self.buf = self.buf[i + 4 + n:]
                    code = int(lines[0].split()[1])
                    return code, hdr, body
            d = self.s.recv(65536)
            if not d:
                raise ConnectionError("closed")
            self.buf += d

    def request(self, method, uri, headers=None, auth=True):
        for attempt in range(2):
            self.cseq += 1
            h = {"CSeq": str(self.cseq), "User-Agent": "pg-test"}
            if self.session:
                h["Session"] = self.session
            if headers:
                h.update(headers)
            a = self._digest(method, uri) if auth else None
            if a:
                h["Authorization"] = a
            msg = f"{method} {uri} RTSP/1.0\r\n" + "".join(f"{k}: {v}\r\n" for k, v in h.items()) + "\r\n"
            self.s.sendall(msg.encode())
            code, hdr, body = self._read_response()
            if code == 401 and auth and attempt == 0 and "www-authenticate" in hdr:
                w = hdr["www-authenticate"]
                self.auth = (re.search(r'realm="([^"]*)"', w).group(1), re.search(r'nonce="([^"]*)"', w).group(1))
                continue
            if "session" in hdr and not self.session:
                self.session = hdr["session"].split(";")[0]
            return code, hdr, body
        return code, hdr, body

    def open_replay(self, uri, require=True, audio=True):
        """DESCRIBE + SETUP (interleaved 0-1 video, 2-3 audio): (code, sdp)"""
        rq = {"Require": "onvif-replay"} if require else {}
        code, hdr, sdp = self.request("DESCRIBE", uri, {"Accept": "application/sdp", **rq})
        if code != 200:
            return code, sdp
        code, _, _ = self.request("SETUP", uri + "/streamid=0", {"Transport": "RTP/AVP/TCP;unicast;interleaved=0-1", **rq})
        if code == 200 and audio and "m=audio" in sdp:
            code, _, _ = self.request("SETUP", uri + "/streamid=1", {"Transport": "RTP/AVP/TCP;unicast;interleaved=2-3", **rq})
        return code, sdp

    def frames(self, timeout=5.0):
        """interleaved frames until timeout without data: yields (channel, bytes)"""
        while self.pending:
            yield self.pending.pop(0)
        self.s.settimeout(timeout)
        while True:
            while len(self.buf) >= 4 and self.buf[:1] == b"$":
                n = struct.unpack(">H", self.buf[2:4])[0]
                if len(self.buf) < 4 + n:
                    break
                ch, pkt = self.buf[1], self.buf[4:4 + n]
                self.buf = self.buf[4 + n:]
                yield ch, pkt
            if self.buf[:1] not in (b"", b"$"):
                return                       # an RTSP message (answer): stop here
            try:
                d = self.s.recv(262144)
            except socket.timeout:
                return
            if not d:
                return
            self.buf += d


def parse_rtp(pkt):
    """-> dict(pt, marker, seq, ts, ext: (ntp_seconds_float, flags, cseq) or None, payload)"""
    if len(pkt) < 12 or pkt[0] >> 6 != 2:
        return None
    cc = pkt[0] & 15
    o = 12 + 4 * cc
    ext = None
    if pkt[0] & 0x10:
        prof, ln = struct.unpack(">HH", pkt[o:o + 4])
        data = pkt[o + 4:o + 4 + 4 * ln]
        if prof == 0xABAC and ln == 3:
            sec, frac = struct.unpack(">II", data[:8])
            ext = (sec - 2208988800 + frac / 2 ** 32, data[8], data[9])
        o += 4 + 4 * ln
    return {"pt": pkt[1] & 0x7F, "marker": pkt[1] >> 7, "seq": struct.unpack(">H", pkt[2:4])[0],
            "ts": struct.unpack(">I", pkt[4:8])[0], "ext": ext, "payload": pkt[o:]}


def nal_type(rtp):
    p = rtp["payload"]
    if not p:
        return None
    t = p[0] & 31
    if t == 28 and len(p) > 1:
        return (p[1] & 31) | (0x100 if p[1] & 0x80 else 0)     # FU-A: type, | 0x100 on the start fragment
    return t
