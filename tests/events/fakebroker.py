"""Tiny MQTT 3.1.1 test broker (single client, QoS 0) for the camera's MQTT client.
Logs CONNECT (client id, will, user), every PUBLISH (topic, size, short payload),
answers SUBSCRIBE/PINGREQ, and sends the commands given on the command line
(topic=payload) a few seconds after the client subscribed.
Usage: fakebroker.py port seconds [user:pass] [topic=payload ...]"""
import socket, struct, sys, time, threading, json
port, secs = int(sys.argv[1]), float(sys.argv[2])
cred = sys.argv[3] if len(sys.argv) > 3 and ":" in sys.argv[3] and "=" not in sys.argv[3] else None
cmds = [a.split("=", 1) for a in sys.argv[3 + (1 if cred else 0):]]
def rd(c, n):
    b = b""
    while len(b) < n:
        d = c.recv(n - len(b))
        if not d: raise EOFError
        b += d
    return b
def rlen(c):
    m, v = 1, 0
    while True:
        d = rd(c, 1)[0]; v += (d & 127) * m; m *= 128
        if not d & 128: return v
def s16(b, o): l = struct.unpack(">H", b[o:o+2])[0]; return b[o+2:o+2+l], o + 2 + l
def enc(t, body):
    l, out = len(body), b""
    while True:
        d = l % 128; l //= 128; out += bytes([d | (128 if l else 0)])
        if not l: break
    return bytes([t]) + out + body
def pub(topic, payload, retain=0):
    t = topic.encode(); return enc(0x30 | retain, struct.pack(">H", len(t)) + t + payload.encode())
import os, ssl
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); srv.bind(("0.0.0.0", port)); srv.listen(1)
srv.settimeout(secs); end = time.time() + secs
ctx = None
if os.environ.get("TLS_CERT"):                      # TLS mode: TLS_CERT/TLS_KEY, optional TLS_CLIENT_CA (require client cert)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(os.environ["TLS_CERT"], os.environ["TLS_KEY"])
    if os.environ.get("TLS_CLIENT_CA"):
        ctx.verify_mode = ssl.CERT_REQUIRED; ctx.load_verify_locations(os.environ["TLS_CLIENT_CA"])
while True:
    try:
        c, a = srv.accept()
    except socket.timeout:
        print("no client connected"); sys.exit(1)
    if not ctx: break
    try:
        c.settimeout(10); c = ctx.wrap_socket(c, server_side=True)
        pc = c.getpeercert()
        print("TLS", c.version(), c.cipher()[0], "client cert:", dict(x[0] for x in pc["subject"]) if pc else None)
        break
    except Exception as e:
        print("TLS handshake failed:", e); c.close()
        if time.time() > end: sys.exit(1)
print("client from", a[0]); c.settimeout(1)
base = None; subscribed_at = None; sent = False; disc = []
while time.time() < end:
    try:
        h = rd(c, 1)[0]; body = rd(c, rlen(c))
    except socket.timeout:
        h = None
    except EOFError:
        print("client closed"); break
    if h is not None:
        t = h >> 4
        if t == 1:
            o = 10; flags = body[7]
            cid, o = s16(body, o); info = {"client": cid.decode(), "keepalive": struct.unpack(">H", body[8:10])[0]}
            if flags & 4: wt, o = s16(body, o); wm, o = s16(body, o); info["will"] = f"{wt.decode()}={wm.decode()} retain={bool(flags & 32)}"
            if flags & 128: u, o = s16(body, o); info["user"] = u.decode()
            if flags & 64: p, o = s16(body, o); info["pass_ok"] = cred is None or f"{info.get('user')}:{p.decode()}" == cred
            print("CONNECT", json.dumps(info))
            rc = 0 if (cred is None or info.get("pass_ok")) else 4
            c.sendall(bytes([0x20, 2, 0, rc]))
            if rc: print("refused (bad credentials)"); break
        elif t == 3:
            tl = struct.unpack(">H", body[:2])[0]; topic = body[2:2+tl].decode(); payload = body[2+tl:]
            if topic.startswith("homeassistant/"):
                disc.append(topic.split("/")[1] + ":" + topic.split("/")[3])
            else:
                if topic.endswith("/availability") and base is None: base = topic.rsplit("/", 1)[0]
                show = payload[:60].decode(errors="replace") if not payload.startswith(b"\xff\xd8") else f"<JPEG {len(payload)} bytes>"
                print(f"PUB {'R' if h & 1 else ' '} {topic} = {show}")
        elif t == 8:
            pid = body[:2]; filters = []
            o = 2
            while o < len(body): f, o = s16(body, o); filters.append(f.decode()); o += 1
            print("SUBSCRIBE", filters); c.sendall(bytes([0x90, 2 + len(filters)]) + pid + bytes(len(filters)))
            subscribed_at = time.time()
        elif t == 12:
            c.sendall(bytes([0xd0, 0]))
        elif t == 14:
            print("DISCONNECT"); break
    if subscribed_at and not sent and time.time() - subscribed_at > 3 and base:
        print("discovery entities:", ", ".join(disc))
        for topic, payload in cmds:
            topic = topic.replace("BASE", base); print("SEND", topic, "=", payload); c.sendall(pub(topic, payload)); time.sleep(0.5)
        sent = True
print("done")
