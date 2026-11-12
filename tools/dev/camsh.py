"""Run a shell command on the camera's maintenance telnetd (port 24, no login).
Usage: camsh.py <host> <timeout_s> '<command>'"""
import socket, sys, time, re
host, timeout, cmd = sys.argv[1], float(sys.argv[2]), sys.argv[3]
if cmd == "-": cmd = sys.stdin.read().strip()
IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240
s = socket.create_connection((host, 24), timeout=5)
buf = bytearray()
def pump(deadline):
    s.settimeout(0.3)
    while time.time() < deadline:
        try:
            d = s.recv(4096)
        except socket.timeout:
            yield; continue
        if not d: return
        i = 0
        while i < len(d):
            b = d[i]
            if b == IAC and i + 1 < len(d):
                c = d[i + 1]
                if c in (DO, DONT, WILL, WONT) and i + 2 < len(d):
                    opt = d[i + 2]
                    s.sendall(bytes([IAC, WONT if c in (DO, DONT) else DONT, opt]))
                    i += 3; continue
                if c == SB:
                    j = d.find(bytes([IAC, SE]), i)
                    i = j + 2 if j >= 0 else len(d); continue
                i += 2; continue
            buf.append(b); i += 1
        yield
for _ in pump(time.time() + 1.5): pass
buf.clear()
marker = "__END_%d__" % int(time.time() * 1000)
s.sendall((cmd + "\necho; echo " + marker + "\n").encode())
for _ in pump(time.time() + timeout):
    if buf.decode(errors="replace").count(marker) >= 2: break
out = buf.decode(errors="replace").replace("\r", "")
# drop echoed command lines, prompts and the end marker
out = out.split(marker)
body = marker.join(out[:-1]) if len(out) >= 3 else "".join(out)
echo = [l.strip() for l in cmd.split("\n")]
lines = []
for l in body.split("\n"):
    t = l.strip()
    if t.startswith("~ #") or t.startswith("/ #") or t.startswith("# ") or marker in t: continue
    if echo and t == echo[0]: echo.pop(0); continue
    lines.append(l)
print("\n".join(lines).strip("\n"))
s.close()
