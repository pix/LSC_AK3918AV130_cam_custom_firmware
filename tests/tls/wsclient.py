"""WebSocket live-view client for tests: http or https, counts video/audio
messages, sends talk-back audio frames. Usage:
wsclient.py host http_or_https_port secs [cafile] [user:pass]"""
import base64, json, os, socket, ssl, struct, subprocess, sys, time
H, P, secs = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
ca = sys.argv[4] if len(sys.argv) > 4 and sys.argv[4] != "-" else None
up = sys.argv[5] if len(sys.argv) > 5 else "admin:admin"
scheme = "https" if ca else "http"
cmd = ["curl", "-s", "--digest", "-u", up, "-H", "X-Requested-With: lsc", f"{scheme}://{H}:{P}/api/live/token"] + (["--cacert", ca] if ca else [])
tok = json.loads(subprocess.run(cmd, capture_output=True, text=True).stdout)["token"]
s = socket.create_connection((H, P), timeout=5)
if ca:
    ctx = ssl.create_default_context(cafile=ca)
    s = ctx.wrap_socket(s, server_hostname=H)
s.sendall((f"GET /api/live?t={tok} HTTP/1.1\r\nHost: {H}:{P}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           f"Sec-WebSocket-Key: {base64.b64encode(os.urandom(16)).decode()}\r\nSec-WebSocket-Version: 13\r\n"
           f"Origin: {scheme}://{H}:{P}\r\n\r\n").encode())
buf = b""
while b"\r\n\r\n" not in buf: buf += s.recv(4096)
hdr, buf = buf.split(b"\r\n\r\n", 1)
assert hdr.startswith(b"HTTP/1.1 101"), hdr
s.settimeout(0.2)
video = key = audio = nbytes = sent = 0
end = time.time() + secs; last_tx = 0
tone = b"".join(struct.pack("<h", int(8000 * __import__("math").sin(i * 2 * 3.14159 * 440 / 8000))) for i in range(800))
while time.time() < end:
    if time.time() - last_tx > 0.1:          # talk-back: 100 ms of tone, masked
        m = os.urandom(4); pl = bytes(b ^ m[i & 3] for i, b in enumerate(tone))
        s.sendall(bytes([0x82, 0x80 | 126]) + struct.pack(">H", len(pl)) + m + pl); sent += 1; last_tx = time.time()
    try:
        d = s.recv(65536)
        if not d: break
        buf += d
    except (socket.timeout, ssl.SSLWantReadError): pass
    while len(buf) >= 2:
        ln = buf[1] & 0x7f; h = 2
        if ln == 126:
            if len(buf) < 4: break
            ln = struct.unpack(">H", buf[2:4])[0]; h = 4
        elif ln == 127:
            if len(buf) < 10: break
            ln = struct.unpack(">Q", buf[2:10])[0]; h = 10
        if len(buf) < h + ln: break
        payload = buf[h:h + ln]; buf = buf[h + ln:]; nbytes += ln
        if payload[0] & 2: audio += 1
        else:
            video += 1; key += payload[0] & 1
s.close()
print(f"{scheme}: video={video} (key {key}) audio={audio} talkback_sent={sent} {nbytes*8/secs/1000:.0f} kbit/s")
