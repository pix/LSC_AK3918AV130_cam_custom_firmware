"""RTSP request framing checks against host_server or a camera.
Usage: rtsp_edge.py <host> [port]
- two requests sent in one packet (pipelined): both answered at once
- a request split right after "RTSP/" (two TCP segments): answered
- a request split in the middle of a header and in the final CRLF: answered
- interleaved $-frame (RTCP) followed by a request in the same packet: answered"""
import socket, sys, time
H = sys.argv[1]; P = int(sys.argv[2]) if len(sys.argv) > 2 else 554
fails = 0

def conn():
    s = socket.create_connection((H, P), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s

def replies(s, want, wait=3.0):
    s.settimeout(wait); data = b""; t0 = time.time()
    while data.count(b"RTSP/1.0 ") < want and time.time() - t0 < wait:
        try:
            b = s.recv(65536)
            if not b: break
            data += b
        except socket.timeout:
            break
    return data.count(b"RTSP/1.0 "), time.time() - t0

def check(name, ok, info=""):
    global fails
    print(("PASS " if ok else "FAIL ") + name + (f" ({info})" if info else ""))
    fails += not ok

def opt(cseq): return f"OPTIONS rtsp://{H}:{P}/ RTSP/1.0\r\nCSeq: {cseq}\r\n\r\n".encode()

s = conn(); s.sendall(opt(1) + opt(2)); n, dt = replies(s, 2); s.close()
check("pipelined: 2 requests in one packet, 2 replies", n == 2, f"{n} replies in {dt:.2f} s")
s = conn(); s.sendall(opt(1) + opt(2) + opt(3)); n, dt = replies(s, 3); s.close()
check("pipelined: 3 requests in one packet, 3 replies", n == 3, f"{n} replies in {dt:.2f} s")
for cut in (b"RTSP/", b"RTSP/1.", b"CSeq: ", b"\r\n\r"):
    r = opt(7); i = r.index(cut) + len(cut)
    s = conn(); s.sendall(r[:i]); time.sleep(0.3); s.sendall(r[i:]); n, dt = replies(s, 1); s.close()
    check(f"split after {cut!r}", n == 1, f"{n} replies")
s = conn(); s.sendall(b"$\x01\x00\x08" + b"\x81\xc9\x00\x01\x00\x00\x00\x00" + opt(9)); n, dt = replies(s, 1); s.close()
check("$-frame then request in one packet", n == 1, f"{n} replies")
s = conn(); s.sendall(b"GARBAGE \x00\x01\r\n\r\n"); time.sleep(0.3); s.sendall(opt(11)); n, dt = replies(s, 1); s.close()
check("garbage (with a NUL), then a valid request on the same connection", n == 1, f"{n} replies")
print(f"rtsp_edge: {fails} failed")
sys.exit(1 if fails else 0)
