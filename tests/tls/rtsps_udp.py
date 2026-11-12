"""RTSPS SETUP with UDP transport must get 461. Usage: rtsps_udp.py host ca.crt"""
import hashlib, re, socket, ssl, sys
H, CA = sys.argv[1], sys.argv[2]; U, P = "admin", "admin"
s = ssl.create_default_context(cafile=CA).wrap_socket(socket.create_connection((H, 322), timeout=10), server_hostname=H)
def req(method, uri, cseq, extra=""):
    s.sendall(f"{method} {uri} RTSP/1.0\r\nCSeq: {cseq}\r\n{extra}\r\n".encode())
    d = b""
    while b"\r\n\r\n" not in d: d += s.recv(4096)
    return d.decode(errors="replace")
url = f"rtsps://{H}:322/"
r = req("DESCRIBE", url, 1)
realm = re.search(r'realm="([^"]+)"', r).group(1); nonce = re.search(r'nonce="([^"]+)"', r).group(1)
def auth(method, uri):
    ha1 = hashlib.md5(f"{U}:{realm}:{P}".encode()).hexdigest(); ha2 = hashlib.md5(f"{method}:{uri}".encode()).hexdigest()
    resp = hashlib.md5(f"{ha1}:{nonce}:{ha2}".encode()).hexdigest()
    return f'Authorization: Digest username="{U}", realm="{realm}", nonce="{nonce}", uri="{uri}", response="{resp}"\r\n'
r = req("SETUP", url + "streamid=0", 2, auth("SETUP", url + "streamid=0") + "Transport: RTP/AVP;unicast;client_port=40000-40001\r\n")
print("UDP SETUP over RTSPS ->", r.splitlines()[0])
r = req("SETUP", url + "streamid=0", 3, auth("SETUP", url + "streamid=0") + "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
print("TCP SETUP over RTSPS ->", r.splitlines()[0])
