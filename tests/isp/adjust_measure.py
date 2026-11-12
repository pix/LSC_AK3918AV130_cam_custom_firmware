"""Measure what each picture control does on the camera.
For each control: -50, 0, +50 (then back to 0); after a settle time, grab full-resolution frames over RTSP and
compute the property the control should change:
  brightness -> mean luma, contrast -> luma standard deviation, saturation -> mean chroma (|Cb|+|Cr|),
  sharpness -> mean |Laplacian| of luma (high-frequency energy).
Usage: adjust_measure.py host [controls...]   (admin/admin; leaves each control at 0)"""
import subprocess, sys, time, urllib.parse, urllib.request
import numpy as np

H = sys.argv[1]
CTRL = sys.argv[2:] or ["brightness", "contrast", "saturation", "sharpness"]
B = f"http://{H}"
pm = urllib.request.HTTPPasswordMgrWithDefaultRealm(); pm.add_password(None, B, "admin", "admin")
op = urllib.request.build_opener(urllib.request.HTTPDigestAuthHandler(pm))


def setp(name, value):
    req = urllib.request.Request(B + "/api/set", data=urllib.parse.urlencode({"name": name, "value": str(value)}).encode(),
                                 headers={"X-Requested-With": "lsc"})
    return op.open(req, timeout=10).read().decode().strip()


def frames(n=3):
    """n raw YUV420p 1920x1080 frames from the main stream"""
    w, h = 1920, 1080
    p = subprocess.run(["ffmpeg", "-v", "error", "-rtsp_transport", "tcp", "-i", f"rtsp://admin:admin@{H}:554/",
                        "-frames:v", str(n), "-f", "rawvideo", "-pix_fmt", "yuv420p", "-"],
                       capture_output=True, timeout=60)
    sz = w * h * 3 // 2
    out = []
    for i in range(len(p.stdout) // sz):
        b = np.frombuffer(p.stdout[i * sz:(i + 1) * sz], np.uint8)
        y = b[:w * h].reshape(h, w).astype(np.float32)
        u = b[w * h:w * h + w * h // 4].reshape(h // 2, w // 2).astype(np.float32) - 128
        v = b[w * h + w * h // 4:].reshape(h // 2, w // 2).astype(np.float32) - 128
        out.append((y, u, v))
    return out


def metric(ctrl, fr):
    vals = []
    for y, u, v in fr:
        y = y[40:-120]                               # leave out the burned-in overlay rows
        if ctrl == "brightness": vals.append(y.mean())
        elif ctrl == "contrast": vals.append(y.std())
        elif ctrl == "saturation": vals.append((np.abs(u) + np.abs(v)).mean())
        else:
            lap = np.abs(4 * y[1:-1, 1:-1] - y[:-2, 1:-1] - y[2:, 1:-1] - y[1:-1, :-2] - y[1:-1, 2:])
            vals.append(lap.mean())
    return float(np.mean(vals))


for c in CTRL:
    row = []
    for val in (-50, 0, 50, 0):
        r = setp("isp." + c, val)
        time.sleep(5 if c == "brightness" else 2.5)      # AE needs a moment to reach a new target
        row.append((val, round(metric(c, frames()), 2), r))
    print(f"{c:11s}", "  ".join(f"{v:+d}: {m:7.2f}" for v, m, _ in row), "| replies:", ",".join(x[2].split()[0] for x in row), flush=True)
