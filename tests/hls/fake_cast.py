"""Fake Chromecast for tests/hls/hls_test.sh: a Cast v2 receiver (TLS, length-
prefixed protobuf CastMessage, JSON payloads) and an mDNS responder for
_googlecast._tcp. On LOAD it fetches the HLS stream like a real device
(playlist, init, first segment -> MEDIA_STATUS PLAYING) and then plays it for
a while with hls_check.py. Every event goes to <log> as JSON lines.

  fake_cast.py <port> <cert.pem> <key.pem> <log> [--behavior ok|launch_error|load_failed|mp4_refused|noseek|nocatchup]
               [--mdns <ip>] [--preroll <s>] [--play-secs <s>]
               [--launch-delay <s>]   (seconds the receiver app takes to start, like a real TV)
"""
import json, os, socket, ssl, struct, subprocess, sys, threading, time, urllib.request, uuid

port, cert, key, logf = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
behavior = sys.argv[sys.argv.index("--behavior") + 1] if "--behavior" in sys.argv else "ok"
mdns_ip = sys.argv[sys.argv.index("--mdns") + 1] if "--mdns" in sys.argv else None
launch_delay = float(sys.argv[sys.argv.index("--launch-delay") + 1]) if "--launch-delay" in sys.argv else 0.0
preroll = float(sys.argv[sys.argv.index("--preroll") + 1]) if "--preroll" in sys.argv else 0.0      # live.mp4 start-up buffer
play_secs = float(sys.argv[sys.argv.index("--play-secs") + 1]) if "--play-secs" in sys.argv else 16.0
HERE = os.path.dirname(os.path.abspath(__file__))
lock = threading.Lock()
apps = []                                   # running receiver apps


def log(**kw):
    kw["t"] = round(time.time(), 3)
    with lock, open(logf, "a") as f:
        f.write(json.dumps(kw) + "\n")


# ---- protobuf CastMessage --------------------------------------------------------------
def varint(v):
    out = b""
    while True:
        b = v & 0x7f; v >>= 7
        out += bytes([b | (0x80 if v else 0)])
        if not v: return out


def encode(src, dst, ns, payload):
    def s(tag, x):
        x = x.encode(); return bytes([tag]) + varint(len(x)) + x
    body = b"\x08\x00" + s(0x12, src) + s(0x1a, dst) + s(0x22, ns) + b"\x28\x00" + s(0x32, json.dumps(payload))
    return struct.pack(">I", len(body)) + body


def decode(b):
    i, f = 0, {}
    while i < len(b):
        k = 0; sh = 0
        while True:
            c = b[i]; i += 1; k |= (c & 0x7f) << sh; sh += 7
            if not c & 0x80: break
        field, wt = k >> 3, k & 7
        if wt == 0:
            while b[i] & 0x80: i += 1
            i += 1
        elif wt == 2:
            l = 0; sh = 0
            while True:
                c = b[i]; i += 1; l |= (c & 0x7f) << sh; sh += 7
                if not c & 0x80: break
            f[field] = b[i:i + l]; i += l
        else:
            raise ValueError(f"wire type {wt}")
    return {"src": f.get(2, b"").decode(), "dst": f.get(3, b"").decode(), "ns": f.get(4, b"").decode(),
            "payload": json.loads(f.get(6, b"{}").decode())}


NS_CONN = "urn:x-cast:com.google.cast.tp.connection"
NS_HEART = "urn:x-cast:com.google.cast.tp.heartbeat"
NS_RECV = "urn:x-cast:com.google.cast.receiver"
NS_MEDIA = "urn:x-cast:com.google.cast.media"


def receiver_status(req):
    with lock:
        a = [dict(x) for x in apps]
    # odd key order and a first app that is not ours, as real devices may send
    return {"requestId": req, "status": {"applications": [
        {"displayName": "Backdrop {x}", "appId": "E8C28D3C", "transportId": "backdrop-1", "sessionId": "b-1"}] + [
        {"sessionId": x["sessionId"], "transportId": x["transportId"], "displayName": "Default \"Media\" Receiver",
         "namespaces": [{"name": NS_MEDIA}], "appId": x["appId"], "statusText": "Ready To Cast"} for x in a],
        "volume": {"level": 1.0, "muted": False}}, "type": "RECEIVER_STATUS"}


player = {}                                 # the live.mp4 player model (plock)
plock = threading.Lock()


def player_pos(now):
    """Position of the modelled player: advances at its rate, never past the
    media received (then BUFFERING until more arrives). plock held."""
    p = player; have = p["rec"]["media"]
    pos = p["pos0"] + p["rate"] * (now - p["t_ref"])
    if pos >= have:
        p["pos0"], p["t_ref"] = have, now
        return have, "BUFFERING"
    return pos, "PLAYING"


def media_cmd(send, msg, t, p):
    """GET_STATUS / SEEK / SET_PLAYBACK_RATE on the media channel."""
    def reply(payload): send(msg["dst"], msg["src"], NS_MEDIA, payload)
    with plock:
        now = time.time()
        if not player.get("rec"):
            reply({"type": "MEDIA_STATUS", "requestId": p.get("requestId", 0), "status": []}); return
        pos, st = player_pos(now)
        if t == "SEEK":
            if behavior in ("noseek", "nocatchup"):
                reply({"type": "INVALID_REQUEST", "requestId": p.get("requestId", 0), "reason": "INVALID_COMMAND"}); return
            x = max(0.0, min(float(p.get("currentTime", pos)), player["rec"]["media"]))
            player.update(pos0=x, t_ref=now); player["seeks"].append(round(x, 2))
        elif t == "SET_PLAYBACK_RATE":
            if behavior == "nocatchup":
                reply({"type": "INVALID_REQUEST", "requestId": p.get("requestId", 0), "reason": "INVALID_COMMAND"}); return
            r = float(p.get("playbackRate", 1))
            player.update(pos0=pos, t_ref=now, rate=r); player["rates"].append(r)
        pos, st = player_pos(now)
        reply({"type": "MEDIA_STATUS", "requestId": p.get("requestId", 0), "status": [
            {"mediaSessionId": 1, "playerState": st, "currentTime": round(pos, 3), "playbackRate": player["rate"]}]})


def play(conn, send, msg):
    p = msg["payload"]; media = p.get("media", {})
    url = media.get("contentId", "")
    log(ev="load", media=media, keys=sorted(p), sessionId=p.get("sessionId"), dst=msg["dst"])
    is_mp4 = media.get("contentType") == "video/mp4"
    if behavior == "load_failed" or (behavior == "mp4_refused" and is_mp4):
        send(msg["dst"], msg["src"], NS_MEDIA, {"type": "LOAD_FAILED", "requestId": p.get("requestId")}); return
    send(msg["dst"], msg["src"], NS_MEDIA, {"type": "MEDIA_STATUS", "requestId": p.get("requestId"),
                                             "status": [{"mediaSessionId": 1, "playerState": "BUFFERING", "media": media}]})
    if is_mp4:          # plain media element: one endless GET; a player that buffers --preroll s before playing
        rec = {"first": None, "media": 0.0, "frags": 0, "keyfirst": None, "types": [], "err": None, "cors": None}

        def reader():
            try:
                r = urllib.request.urlopen(url, timeout=20); rec["cors"] = r.headers.get("Access-Control-Allow-Origin")
                dec0, t_end = None, time.time() + play_secs
                while time.time() < t_end:
                    h = r.read(8)
                    if len(h) < 8: break
                    n, ty = struct.unpack(">I4s", h); b = r.read(n - 8)
                    if len(rec["types"]) < 2: rec["types"].append(ty.decode("latin-1"))
                    if ty != b"moof": continue
                    o = b.find(b"tfdt"); tr = b.find(b"trun")
                    dec = struct.unpack(">Q", b[o + 8:o + 16])[0]; dur, _, fl = struct.unpack(">III", b[tr + 16:tr + 28])
                    if dec0 is None: dec0 = dec; rec["keyfirst"] = fl == 0x02000000; rec["first"] = time.time()
                    rec["media"] = (dec + dur - dec0) / 90000; rec["frags"] += 1
                r.close()
            except Exception as e:
                rec["err"] = str(e)
        th = threading.Thread(target=reader, daemon=True); t0 = time.time(); th.start()
        while rec["first"] is None and rec["err"] is None and time.time() - t0 < 15: time.sleep(0.05)
        if rec["first"] is None:
            log(ev="fetch_failed", error=rec["err"] or "no frame")
            send(msg["dst"], msg["src"], NS_MEDIA, {"type": "MEDIA_STATUS", "requestId": 0,
                                                     "status": [{"mediaSessionId": 1, "playerState": "IDLE", "idleReason": "ERROR"}]})
            return
        log(ev="fetched", mp4=True, first_ms=int((rec["first"] - t0) * 1000), segment=1, cors=rec["cors"])
        time.sleep(preroll)                     # the start-up buffer: this much delay unless the sender catches up
        with plock:
            player.update(pos0=0.0, t_ref=time.time(), rate=1.0, rec=rec, seeks=[], rates=[])
        send(msg["dst"], msg["src"], NS_MEDIA, {"type": "MEDIA_STATUS", "requestId": 0, "status": [
            {"mediaSessionId": 1, "playerState": "PLAYING", "currentTime": 0, "playbackRate": 1, "media": media}]})
        th.join()
        ok = rec["types"][:2] == ["ftyp", "moov"] and rec["keyfirst"] and rec["frags"] >= 10
        with plock:
            seeks, rates = list(player.get("seeks", [])), list(player.get("rates", []))
        log(ev="played", result=("PASS" if ok else "FAIL") + f" fake player: {rec['frags']} fragments, {rec['media']:.1f} s media,"
            f" starts {rec['types']}, keyframe first {rec['keyfirst']}", seeks=seeks, rates=rates)
        return
    try:
        t0 = time.time()
        pl = urllib.request.urlopen(url, timeout=20).read().decode()
        pl_ms = int((time.time() - t0) * 1000)
        base = url.rsplit("/", 1)[0] + "/"
        init = urllib.request.urlopen(base + "init.mp4", timeout=20).read()
        seg = [l for l in pl.splitlines() if l.startswith("seg")][0]
        r = urllib.request.urlopen(base + seg, timeout=20)
        data = r.read()
        log(ev="fetched", playlist=pl.count("seg"), playlist_ms=pl_ms, init=len(init), segment=len(data), cors=r.headers.get("Access-Control-Allow-Origin"))
    except Exception as e:
        log(ev="fetch_failed", error=str(e))
        send(msg["dst"], msg["src"], NS_MEDIA, {"type": "MEDIA_STATUS", "requestId": 0,
                                                 "status": [{"mediaSessionId": 1, "playerState": "IDLE", "idleReason": "ERROR"}]})
        return
    send(msg["dst"], msg["src"], NS_MEDIA, {"type": "MEDIA_STATUS", "requestId": 0,
                                             "status": [{"mediaSessionId": 1, "playerState": "PLAYING", "media": media}]})
    out = subprocess.run([sys.executable, "-I", os.path.join(HERE, "hls_check.py"), "client", url, "12", "chromecast"],
                         capture_output=True, text=True).stdout
    log(ev="played", result=out.strip())


def handle(raw):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    try:
        conn = ctx.wrap_socket(raw, server_side=True)
    except Exception as e:
        log(ev="tls_failed", error=str(e)); return
    wl = threading.Lock()

    def send(src, dst, ns, payload):
        with wl:
            conn.sendall(encode(src, dst, ns, payload))
    log(ev="connected", cipher=conn.cipher()[0], version=conn.version())
    conn.settimeout(60)
    buf = b""; pinged = False
    try:
        while True:
            d = conn.recv(65536)
            if not d: break
            buf += d
            while len(buf) >= 4 and len(buf) >= 4 + struct.unpack(">I", buf[:4])[0]:
                n = struct.unpack(">I", buf[:4])[0]
                msg = decode(buf[4:4 + n]); buf = buf[4 + n:]
                p = msg["payload"]; t = p.get("type")
                log(ev="msg", ns=msg["ns"], src=msg["src"], dst=msg["dst"], type=t)
                if msg["ns"] == NS_CONN and t == "CONNECT" and not pinged:
                    pinged = True
                    send("receiver-0", msg["src"], NS_HEART, {"type": "PING"})      # the sender must answer PONG
                elif msg["ns"] == NS_HEART and t == "PING":
                    send(msg["dst"], msg["src"], NS_HEART, {"type": "PONG"})
                elif msg["ns"] == NS_RECV and t == "LAUNCH":
                    if behavior == "launch_error":
                        send("receiver-0", msg["src"], NS_RECV, {"type": "LAUNCH_ERROR", "reason": "NOT_FOUND", "requestId": p.get("requestId")})
                        continue
                    time.sleep(launch_delay)            # the receiver app starting (a few s on a real TV)
                    with lock:
                        apps[:] = [{"appId": p.get("appId"), "sessionId": str(uuid.uuid4()), "transportId": "web-7"}]
                    send("receiver-0", msg["src"], NS_RECV, receiver_status(p.get("requestId")))
                elif msg["ns"] == NS_RECV and t == "GET_STATUS":
                    send("receiver-0", msg["src"], NS_RECV, receiver_status(p.get("requestId")))
                elif msg["ns"] == NS_RECV and t == "STOP":
                    with lock:
                        ok = any(a["sessionId"] == p.get("sessionId") for a in apps)
                        apps[:] = [a for a in apps if a["sessionId"] != p.get("sessionId")]
                    log(ev="stop", sessionId=p.get("sessionId"), matched=ok)
                    send("receiver-0", msg["src"], NS_RECV, receiver_status(p.get("requestId")))
                elif msg["ns"] == NS_MEDIA and t in ("GET_STATUS", "SEEK", "SET_PLAYBACK_RATE"):
                    media_cmd(send, msg, t, p)
                elif msg["ns"] == NS_MEDIA and t == "LOAD":
                    threading.Thread(target=play, args=(conn, send, msg), daemon=True).start()
    except Exception as e:
        log(ev="conn_error", error=str(e))
    log(ev="closed")
    try: conn.close()
    except Exception: pass


def mdns():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", 5353))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton("224.0.0.251") + socket.inet_aton(mdns_ip))

    def name(n): return b"".join(bytes([len(x)]) + x.encode() for x in n.split(".")) + b"\0"

    def rr(owner, typ, rdata, cls=0x8001, ttl=120):
        return name(owner) + struct.pack(">HHIH", typ, cls, ttl, len(rdata)) + rdata

    def answer(q, inst, fn, model, port):
        full = inst + "._googlecast._tcp.local"
        txt = b"".join(bytes([len(x)]) + x for x in [b"id=" + inst.encode(), b"md=" + model.encode(), b"fn=" + fn.encode()])
        recs = [rr("_googlecast._tcp.local", 12, name(full), cls=1),
                rr(full, 16, txt), rr(full, 33, struct.pack(">HHH", 0, 0, port) + name(inst + ".local")),
                rr(inst + ".local", 1, socket.inet_aton(mdns_ip))]
        return struct.pack(">HHHHHH", 0, 0x8400, 1, 1, 0, 3) + q + b"".join(recs)
    while True:
        d, a = s.recvfrom(9000)
        if b"_googlecast" not in d or d[2] & 0x80: continue
        qend = 12
        while d[qend]: qend += 1 + d[qend]
        q = d[12:qend + 5]
        log(ev="mdns_query", src=a[0], port=a[1])
        s.sendto(answer(q, "Test-TV-1", 'Living "Room" TV', "Chromecast", 8009), a)
        s.sendto(answer(q, "Group-2", "All speakers\\", "Google Cast Group", 32187), a)


if mdns_ip:
    threading.Thread(target=mdns, daemon=True).start()
ls = socket.socket()
ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
ls.bind(("0.0.0.0", port)); ls.listen(4)
log(ev="ready", port=port, behavior=behavior)
while True:
    c, _ = ls.accept()
    threading.Thread(target=handle, args=(c,), daemon=True).start()
