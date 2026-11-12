"""Close a real acoustic loop with this computer: the camera's live audio is
played on the computer speakers, the computer microphone is sent back as
talk-back, so camera speaker -> computer mic -> camera speaker forms the
Larsen loop. Usage: acoustic_loop.py <camera-ip> <seconds> [http_port]
Needs PulseAudio/PipeWire (aplay, ffmpeg). Keep the speaker volume low.
"""
import base64, json, os, socket, struct, subprocess, sys, threading, time

H = sys.argv[1]; SECS = float(sys.argv[2]); P = int(sys.argv[3]) if len(sys.argv) > 3 else 80
cmd = ["curl", "-s", "--digest", "-u", "admin:admin", "-H", "X-Requested-With: lsc",
       f"http://{H}:{P}/api/live/token"]
tok = json.loads(subprocess.run(cmd, capture_output=True, text=True).stdout)["token"]
s = socket.create_connection((H, P), timeout=5)
s.sendall((f"GET /api/live?t={tok} HTTP/1.1\r\nHost: {H}:{P}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           f"Sec-WebSocket-Key: {base64.b64encode(os.urandom(16)).decode()}\r\nSec-WebSocket-Version: 13\r\n"
           f"Origin: http://{H}:{P}\r\n\r\n").encode())
buf = b""
while b"\r\n\r\n" not in buf: buf += s.recv(4096)
hdr, buf = buf.split(b"\r\n\r\n", 1)
assert hdr.startswith(b"HTTP/1.1 101"), hdr

aplay = subprocess.Popen(["aplay", "-q", "-f", "S16_LE", "-r", "8000", "-c", "1"], stdin=subprocess.PIPE)
mic = subprocess.Popen(["ffmpeg", "-v", "error", "-f", "pulse", "-i", "default", "-ar", "8000", "-ac", "1",
                        "-f", "s16le", "-"], stdout=subprocess.PIPE)
end = time.time() + SECS
sent = played = 0

def rx():
    global buf, played
    s.settimeout(0.2)
    while time.time() < end:
        try: d = s.recv(65536)
        except socket.timeout: continue
        if not d: return
        buf += d
        while len(buf) >= 2:
            op = buf[0] & 0x0f
            ln = buf[1] & 0x7f; h = 2
            if ln == 126:
                if len(buf) < 4: break
                ln = struct.unpack(">H", buf[2:4])[0]; h = 4
            elif ln == 127:
                if len(buf) < 10: break
                ln = struct.unpack(">Q", buf[2:10])[0]; h = 10
            if len(buf) < h + ln: break
            pl = buf[h:h + ln]; buf = buf[h + ln:]
            if pl and (pl[0] & 2) and len(pl) > 9:          # audio: 1 byte flags + 8 byte ts
                try: aplay.stdin.write(pl[9:]); aplay.stdin.flush(); played += len(pl) - 9
                except Exception: return

def tx():
    global sent
    chunk = 1600                                        # 100 ms
    while time.time() < end:
        d = mic.stdout.read(chunk)
        if not d: break
        m = os.urandom(4)
        pl = bytes(b ^ m[i & 3] for i, b in enumerate(d))
        try:
            s.sendall(bytes([0x82, 0x80 | 126]) + struct.pack(">H", len(pl)) + m + pl); sent += len(pl)
        except Exception: break
        time.sleep(0.01)

t1 = threading.Thread(target=rx); t2 = threading.Thread(target=tx)
t1.start(); t2.start(); t1.join(); t2.join()
print(f"played {played/2/8000:.1f}s of camera audio, sent {sent/2/8000:.1f}s of microphone audio")
mic.terminate(); aplay.stdin.close(); aplay.terminate(); s.close()
