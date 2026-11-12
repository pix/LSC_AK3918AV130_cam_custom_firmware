"""Remote client without echo cancellation, emulated digitally: the camera's live
audio is sent straight back as talk-back with gain G (dB), so the only acoustic
link in the loop is the camera's own speaker -> mic (what audio.aec removes).
loop3.py <camera-ip> <seconds> <gain_db>. Aborts if the camera output stays > -12 dBFS for 1 s."""
import base64, json, math, os, socket, struct, subprocess, sys, threading, time
H = sys.argv[1]; SECS = float(sys.argv[2]); G = 10 ** (float(sys.argv[3]) / 20)
tok = json.loads(subprocess.run(["curl", "-s", "--digest", "-u", "admin:admin", "-H", "X-Requested-With: lsc",
      f"http://{H}/api/live/token"], capture_output=True, text=True).stdout)["token"]
s = socket.create_connection((H, 80), timeout=5)
s.sendall((f"GET /api/live?t={tok} HTTP/1.1\r\nHost: {H}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           f"Sec-WebSocket-Key: {base64.b64encode(os.urandom(16)).decode()}\r\nSec-WebSocket-Version: 13\r\n"
           f"Origin: http://{H}\r\n\r\n").encode())
buf = b""
while b"\r\n\r\n" not in buf: buf += s.recv(4096)
buf = buf.split(b"\r\n\r\n", 1)[1]
s.settimeout(0.2)
end = time.time() + SECS; levels = []; acc = [0.0, 0]; loud = 0; aborted = False; pend = b""
# a kick so there is something to loop: 0.3 s of speech-like noise burst
import random
kick = b"".join(struct.pack("<h", int(random.gauss(0, 3000))) for _ in range(2400))
m = os.urandom(4); s.sendall(bytes([0x82, 0xfe]) + struct.pack(">H", len(kick)) + m + bytes(b ^ m[i & 3] for i, b in enumerate(kick)))
while time.time() < end:
    try: d = s.recv(65536)
    except socket.timeout: continue
    if not d: break
    buf += d
    while len(buf) >= 2:
        ln = buf[1] & 0x7f; h = 2
        if ln == 126:
            if len(buf) < 4: break
            ln = struct.unpack(">H", buf[2:4])[0]; h = 4
        elif ln == 127:
            if len(buf) < 10: break
            ln = struct.unpack(">Q", buf[2:10])[0]; h = 10
        if len(buf) < h + ln: break
        pl = buf[h:h + ln]; buf = buf[h + ln:]
        if pl and (pl[0] & 2) and len(pl) > 9:
            a = pl[9:]; out = bytearray()
            for (v,) in struct.iter_unpack("<h", a[:len(a) // 2 * 2]):
                acc[0] += v * v; acc[1] += 1
                out += struct.pack("<h", max(-32768, min(32767, int(v * G))))
            if acc[1] >= 4000:
                db = 10 * math.log10(acc[0] / acc[1] + 1) - 90.3; levels.append(db); acc[0] = 0; acc[1] = 0
                loud = loud + 1 if db > -12 else 0
                if loud >= 2: aborted = True; end = 0
            pend += bytes(out)
            while len(pend) >= 1600:
                d2, pend = pend[:1600], pend[1600:]
                m = os.urandom(4)
                s.sendall(bytes([0x82, 0xfe]) + struct.pack(">H", 1600) + m + bytes(b ^ m[i & 3] for i, b in enumerate(d2)))
s.close()
print(("ABORTED (howl) " if aborted else "") + "camera out per 0.5 s (dBFS): " + " ".join("%.0f" % x for x in levels))
