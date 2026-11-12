"""ONVIF RTSP audio backchannel client (what go2rtc does with #backchannel=1):
DESCRIBE with Require: www.onvif.org/ver20/backchannel, SETUP the a=sendonly
track over interleaved TCP, PLAY, then send G.711 u-law RTP (440 Hz tone).
Usage: backchannel_test.py host user pass seconds"""
import hashlib, math, re, socket, struct, sys, time

H, U, P, SECS = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
URL = f"rtsp://{H}:554/"
s = socket.create_connection((H, 554), timeout=10)
cseq = 0
auth = None


def lin2ulaw(x):
    BIAS, CLIP = 0x84, 32635
    sign = 0x80 if x < 0 else 0
    x = min(abs(x), CLIP) + BIAS
    exp = 7
    mask = 0x4000
    while not (x & mask) and exp > 0:
        exp -= 1
        mask >>= 1
    mant = (x >> (exp + 3)) & 0x0f
    return ~(sign | (exp << 4) | mant) & 0xff


def request(method, url, extra=""):
    global cseq
    for _ in range(2):
        cseq += 1
        a = ""
        if auth:
            realm, nonce = auth
            ha1 = hashlib.md5(f"{U}:{realm}:{P}".encode()).hexdigest()
            ha2 = hashlib.md5(f"{method}:{url}".encode()).hexdigest()
            r = hashlib.md5(f"{ha1}:{nonce}:{ha2}".encode()).hexdigest()
            a = f'Authorization: Digest username="{U}", realm="{realm}", nonce="{nonce}", uri="{url}", response="{r}"\r\n'
        s.sendall(f"{method} {url} RTSP/1.0\r\nCSeq: {cseq}\r\nUser-Agent: backchannel-test\r\n{a}{extra}\r\n".encode())
        data = b""
        while b"\r\n\r\n" not in data:
            data += s.recv(4096)
        head, body = data.split(b"\r\n\r\n", 1)
        head = head.decode()
        m = re.search(r"Content-Length:\s*(\d+)", head, re.I)
        n = int(m.group(1)) if m else 0
        while len(body) < n:
            body += s.recv(4096)
        if head.split()[1] == "401":
            realm = re.search(r'realm="([^"]+)"', head).group(1)
            nonce = re.search(r'nonce="([^"]+)"', head).group(1)
            globals()["auth"] = (realm, nonce)
            continue
        return head, body.decode(errors="replace")
    return head, ""


head, sdp = request("DESCRIBE", URL, "Accept: application/sdp\r\nRequire: www.onvif.org/ver20/backchannel\r\n")
print("DESCRIBE ->", head.splitlines()[0])
bc = [blk for blk in sdp.split("m=")[1:] if "sendonly" in blk]
print(("PASS" if bc else "FAIL"), "backchannel track offered:", bc[0].split("\r\n")[0] if bc else sdp)
head, _ = request("SETUP", URL + "streamid=2", "Transport: RTP/AVP/TCP;unicast;interleaved=4-5\r\n")
print("SETUP backchannel ->", head.splitlines()[0], "|", re.search(r"Transport:[^\r\n]*", head).group(0) if "Transport" in head else "")
sess = re.search(r"Session:\s*([^;\r\n]+)", head).group(1)
head, _ = request("PLAY", URL, f"Session: {sess}\r\nRange: npt=0.000-\r\n")
print("PLAY ->", head.splitlines()[0])
seq, ts, ssrc = 1, 0, 0x1234abcd
t0 = time.time()
k = 0
sent = 0
while time.time() - t0 < SECS:
    payload = bytes(lin2ulaw(int(16000 * math.sin(2 * math.pi * 440 * (k + i) / 8000))) for i in range(160))
    k += 160
    rtp = struct.pack(">BBHII", 0x80, 0, seq & 0xffff, ts & 0xffffffff, ssrc) + payload
    s.sendall(b"$" + bytes([4]) + struct.pack(">H", len(rtp)) + rtp)
    seq += 1
    ts += 160
    sent += 1
    time.sleep(max(0, t0 + sent * 0.02 - time.time()))
print(f"sent {sent} RTP packets ({sent * 0.02:.1f} s)")
request("TEARDOWN", URL, f"Session: {sess}\r\n")
s.close()
