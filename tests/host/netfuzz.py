"""Light network fuzzer for host_server (run inside run_sandboxed.sh).
Mutates realistic requests on HTTP (80), HTTPS (443), RTSP (554), RTSPS (322),
ONVIF SOAP (/onvif/), WS-Discovery (UDP 3702) and the control port (8091),
before and after a valid Digest login (so authenticated handlers are reached).
Every 100 cases it checks that the server still answers; on failure the last
cases are written to <outdir> as reproducers.
Usage: netfuzz.py <host> <seconds> <outdir> [seed]"""
import hashlib, os, random, socket, ssl, sys, time

H, SECS, OUT = sys.argv[1], float(sys.argv[2]), sys.argv[3]
R = random.Random(int(sys.argv[4]) if len(sys.argv) > 4 else 1)
os.makedirs(OUT, exist_ok=True)
U, P = "admin", "admin"
CTX = ssl.create_default_context(); CTX.check_hostname = False; CTX.verify_mode = ssl.CERT_NONE
last = []

def md5(s): return hashlib.md5(s.encode()).hexdigest()

def send(port, data, tls=False, read=True, timeout=1.0):
    s = socket.create_connection((H, port), timeout=timeout)
    if tls: s = CTX.wrap_socket(s)
    try:
        s.sendall(data)
        if not read: return b""
        out = b""
        while len(out) < 65536:
            b = s.recv(16384)
            if not b: break
            out += b
        return out
    except (OSError, ssl.SSLError):
        return b""
    finally:
        try: s.close()
        except OSError: pass

NONCES = {}
def nonce(port=80, tls=False, path="/"):
    # a nonce stays valid 5 min and each request gets a new cnonce: reuse it for 100 cases
    c = NONCES.get((port, tls))
    if c and c[1] < 100:
        NONCES[(port, tls)] = (c[0], c[1] + 1)
        return c[0]
    r = send(port, f"GET {path} HTTP/1.1\r\nHost: {H}\r\nConnection: close\r\n\r\n".encode(), tls, timeout=3.0)
    t = r.decode("latin1")
    i = t.find('nonce="')
    n = t[i + 7:t.find('"', i + 7)] if i >= 0 else "x"
    NONCES[(port, tls)] = (n, 0)
    return n

def digest(method, uri, n, realm="LSC Camera", nc="00000001"):
    cn = "%08x" % R.getrandbits(32)
    ha1, ha2 = md5(f"{U}:{realm}:{P}"), md5(f"{method}:{uri}")
    resp = md5(f"{ha1}:{n}:{nc}:{cn}:auth:{ha2}")
    return (f'Digest username="{U}", realm="{realm}", nonce="{n}", uri="{uri}", qop=auth, nc={nc}, '
            f'cnonce="{cn}", response="{resp}", algorithm=MD5')

PATHS = ["/", "/api/status", "/api/config", "/api/set", "/api/osd", "/api/tls", "/api/tls/custom", "/api/events",
         "/api/rec/list?day=2026-01-01", "/api/rec/delete", "/api/backup", "/api/restore", "/api/mqtt/upload?what=ca",
         "/api/acme", "/api/acme/ca", "/hook/sound?token=x", "/snapshot.jpg", "/api/live?t=abc", "/api/wifi/connect",
         "/rec/2026-01-01/00-00-00_motion.mp4", "/rec-tar/a", "/onvif/device_service", "/api/isp/get?m=nr1"]
BODIES = [b"", b"name=image.jpeg_quality&value=80", b"mqtt.host=127.0.0.1&mqtt.port=1883", b"names=a.lan&redirect=1",
          b"text=hello&enabled=1", b"files=2026-01-01/00-00-00_motion.mp4", b"-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n",
          b"RIFF\x24\x00\x00\x00WAVEfmt \x10\x00\x00\x00\x01\x00\x01\x00\x40\x1f\x00\x00\x80\x3e\x00\x00\x02\x00\x10\x00data\x00\x00\x00\x00"]
SOAP = (b'<?xml version="1.0" encoding="UTF-8"?><s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"><s:Header>'
        b'<Security xmlns="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd"><UsernameToken>'
        b'<Username>admin</Username><Password Type="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest">'
        b'AAAAAAAAAAAAAAAAAAAAAAAAAAA=</Password><Nonce>AAAAAAAAAAAAAAAAAAAAAA==</Nonce><Created>2026-11-22T10:00:00Z</Created>'
        b'</UsernameToken></Security></s:Header><s:Body><GetProfiles xmlns="http://www.onvif.org/ver10/media/wsdl"/></s:Body></s:Envelope>')
SOAP_OPS = [b"GetProfiles", b"GetStreamUri", b"GetSnapshotUri", b"GetDeviceInformation", b"GetCapabilities", b"GetServices",
            b"CreatePullPointSubscription", b"PullMessages", b"GetSystemDateAndTime", b"SetSystemDateAndTime", b"SystemReboot",
            b"GetVideoEncoderConfigurations", b"GetAudioOutputs", b"GetEventProperties", b"Unsubscribe", b"Renew"]
TOKENS = [b"\r\n", b"\r\n\r\n", b"\x00", b"%", b"%00", b"%zz", b"..", b"/", b"\"", b"'", b"<", b">", b"&", b"=", b";", b",",
          b"Content-Length: 99999999\r\n", b"Content-Length: -1\r\n", b"Transfer-Encoding: chunked\r\n", b"Host: evil.com\r\n",
          b"A" * 300, b"A" * 5000, b"\xff\xfe", b"Authorization: Digest x\r\n", b"Range: bytes=-5\r\n", b"Range: bytes=99999999999-\r\n",
          b"Upgrade: websocket\r\n", b"Sec-WebSocket-Key: x\r\n", b"X-Forwarded-For: 1.2.3.4\r\n", b"CSeq: 99999999999999\r\n"]

def mutate(b):
    b = bytearray(b)
    for _ in range(R.randint(1, 6)):
        op = R.randrange(7)
        pos = R.randrange(len(b) + 1)
        if op == 0 and b: b[R.randrange(len(b))] = R.randrange(256)
        elif op == 1: b[pos:pos] = R.choice(TOKENS)
        elif op == 2 and b: del b[pos:pos + R.randint(1, 40)]
        elif op == 3 and b:
            s = R.randrange(len(b)); e = min(len(b), s + R.randint(1, 200)); b[pos:pos] = b[s:e] * R.randint(1, 20)
        elif op == 4: b[pos:pos] = bytes(R.randrange(256) for _ in range(R.randint(1, 30)))
        elif op == 5 and b: b = b[:R.randrange(len(b) + 1)]
        elif op == 6: b[pos:pos] = str(R.choice([0, -1, 2**31, 2**32 + 1, 2**63, 10**20])).encode()
    return bytes(b)

def http_case(tls):
    port = 443 if tls else 80
    path = R.choice(PATHS)
    m = R.choice(["GET", "POST", "POST", "PUT", "OPTIONS", "HEAD"])
    body = SOAP.replace(b"GetProfiles", R.choice(SOAP_OPS)) if path.startswith("/onvif") else R.choice(BODIES)
    hdr = f"{m} {path} HTTP/1.1\r\nHost: {H}\r\nX-Requested-With: lsc\r\n"
    if R.random() < 0.9: hdr += "Connection: close\r\n"
    if path.startswith("/onvif"): hdr += "Content-Type: application/soap+xml\r\n"
    if R.random() < 0.6:                     # authenticated: the mutation hits the handler, not the login check
        n = nonce(port, tls)
        hdr += "Authorization: " + digest(m, path, n) + "\r\n"
        req = hdr.encode() + b"Content-Length: %d\r\n\r\n" % len(body) + body
        cut = len(hdr)                       # keep request line + auth intact most of the time
        if R.random() < 0.8: req = req[:cut] + mutate(req[cut:])
        else: req = mutate(req)
    else:
        req = mutate(hdr.encode() + b"Content-Length: %d\r\n\r\n" % len(body) + body)
    return port, req, tls

def rtsp_case(tls):
    port = 322 if tls else 554
    url = f"rtsp://{H}:{port}/" + R.choice(["", "sub", "backchannel", "x" * 50])
    m = R.choice(["OPTIONS", "DESCRIBE", "SETUP", "PLAY", "TEARDOWN", "GET_PARAMETER", "SET_PARAMETER", "ANNOUNCE", "RECORD"])
    lines = [f"{m} {url} RTSP/1.0", "CSeq: 2", "User-Agent: Lavf"]
    if m == "SETUP": lines.append(R.choice(["Transport: RTP/AVP/TCP;unicast;interleaved=0-1", "Transport: RTP/AVP;unicast;client_port=5000-5001"]) )
    if m in ("PLAY", "TEARDOWN", "GET_PARAMETER"): lines.append("Session: 12345678")
    if m == "DESCRIBE": lines.append("Accept: application/sdp")
    if R.random() < 0.6:
        r = send(port, f"DESCRIBE {url} RTSP/1.0\r\nCSeq: 1\r\n\r\n".encode(), tls).decode("latin1")
        i = r.find('nonce="'); n = r[i + 7:r.find('"', i + 7)] if i >= 0 else "x"
        ha1, ha2 = md5(f"{U}:LSC Camera:{P}"), md5(f"{m}:{url}")
        lines.append(f'Authorization: Digest username="{U}", realm="LSC Camera", nonce="{n}", uri="{url}", response="{md5(ha1 + ":" + n + ":" + ha2)}"')
    req = ("\r\n".join(lines) + "\r\n\r\n").encode()
    if R.random() < 0.3: req += b"$" + bytes([R.randrange(4)]) + R.randrange(65536).to_bytes(2, "big") + mutate(b"\x80\x60" + b"\x00" * 30)
    return port, (mutate(req) if R.random() < 0.85 else req), tls

def control_case():
    lines = [b"AUTH admin admin\n" if R.random() < 0.7 else b"", R.choice([b"LIST\n", b"GET image.jpeg_quality\n",
             b"SET image.jpeg_quality 80\n", b"SET mqtt.host x\n", b"GET\n", b"SET\n", b"HELP\n"])]
    return 8091, mutate(b"".join(lines)), False

def discovery_case():
    d = (b'<?xml version="1.0"?><e:Envelope xmlns:e="http://www.w3.org/2003/05/soap-envelope" xmlns:w="http://schemas.xmlsoap.org/ws/2004/08/addressing" '
         b'xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery"><e:Header><w:MessageID>uuid:1</w:MessageID><w:Action>'
         b'http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action></e:Header><e:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter'
         b'</d:Types></d:Probe></e:Body></e:Envelope>')
    return mutate(d)

def alive():
    for _ in range(3):
        r = send(80, f"GET / HTTP/1.1\r\nHost: {H}\r\nConnection: close\r\n\r\n".encode(), timeout=10)
        if r.startswith(b"HTTP/1.1 401"): return True
        time.sleep(2)
    return False

t0, n, kinds = time.time(), 0, {}
while time.time() - t0 < SECS:
    k = R.choices(["http", "https", "rtsp", "rtsps", "control", "discovery"], [40, 10, 25, 5, 10, 5])[0]
    try:
        if k == "discovery":
            data = discovery_case()
            u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.sendto(data, (H, 3702)); u.close()
            case = (k, 3702, data)
        else:
            port, data, tls = {"http": lambda: http_case(False), "https": lambda: http_case(True), "rtsp": lambda: rtsp_case(False),
                               "rtsps": lambda: rtsp_case(True), "control": control_case}[k]()
            case = (k, port, data)
            send(port, data, tls, read=R.random() < 0.9)
    except (OSError, ssl.SSLError):
        pass
    last.append(case); last[:] = last[-60:]
    kinds[k] = kinds.get(k, 0) + 1
    n += 1
    if n % 100 == 0 and not alive():
        for i, (kk, pp, dd) in enumerate(last):
            open(os.path.join(OUT, f"last_{i:02d}_{kk}_{pp}.bin"), "wb").write(dd)
        print(f"SERVER DOWN after {n} cases; last {len(last)} cases saved in {OUT}", flush=True)
        sys.exit(1)
print(f"netfuzz: {n} cases in {time.time() - t0:.0f} s, server alive ({kinds})", flush=True)
