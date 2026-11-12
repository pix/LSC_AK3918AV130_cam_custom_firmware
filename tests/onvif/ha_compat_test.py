"""What Home Assistant's ONVIF integration does, against our firmware:
- capability detection (snapshot, imaging, ptz),
- Imaging service (its IR-lamp switch and brightness/saturation/contrast),
- events decoded with onvif_parsers (the package HA pins) into HA entities,
- the HA buttons' XML (SystemReboot, SetSystemDateAndTime) are only built here,
  not sent (a reboot would interrupt the test).
Usage: ha_compat_test.py host user pass   (needs onvif-zeep-async and onvif_parsers)
"""
import asyncio, datetime, sys, time, urllib.parse, urllib.request
from onvif import ONVIFCamera
import onvif_parsers

H, U, P = sys.argv[1], sys.argv[2], sys.argv[3]
BASE = f"http://{H}"
pm = urllib.request.HTTPPasswordMgrWithDefaultRealm()
pm.add_password(None, BASE, U, P)
op = urllib.request.build_opener(urllib.request.HTTPDigestAuthHandler(pm))
fails = 0


def check(ok, what):
    global fails
    fails += 0 if ok else 1
    print(("PASS" if ok else "FAIL"), what)


def param(name):
    with op.open(BASE + "/api/params", timeout=5) as r:
        for line in r.read().decode().splitlines():
            if line.startswith(name + "="):
                return line.split("=", 1)[1]
    return None


async def main():
    cam = ONVIFCamera(H, 80, U, P)
    await cam.update_xaddrs()

    # ---- capabilities, the way onvif/device.py detects them
    caps = await cam.get_capabilities()
    media = await cam.create_media_service()
    mc = await media.GetServiceCapabilities()
    check(bool(mc.SnapshotUri), "capability: snapshot URI")
    try:
        img = await cam.create_imaging_service()
        imaging = True
    except Exception as e:  # noqa: BLE001
        print("   imaging service:", e)
        imaging = False
    check(imaging, "capability: Imaging service (HA's IR lamp/brightness switches)")
    try:
        cam.get_definition("ptz")
        ptz = True
    except Exception:  # noqa: BLE001
        ptz = False
    check(not ptz, "capability: no PTZ (this camera cannot pan)")

    # ---- Imaging: HA switch "ir_lamp" (on -> IrCutFilter OFF) and picture sliders
    async def set_img(**kw):
        req = img.create_type("SetImagingSettings")
        req.VideoSourceToken = "vs"
        req.ImagingSettings = kw
        await img.SetImagingSettings(req)

    def get_img():
        req = img.create_type("GetImagingSettings")
        req.VideoSourceToken = "vs"
        return img.GetImagingSettings(req)

    opts = await img.GetOptions({"VideoSourceToken": "vs"})
    check(opts.Brightness.Min is not None and opts.Brightness.Max == 100, "Imaging GetOptions (0..100)")

    await set_img(ColorSaturation=100, Contrast=0, Brightness=50)
    check(param("isp.saturation") == "50" and param("isp.contrast") == "-50" and param("isp.brightness") == "0",
          "Imaging: ColorSaturation 100 / Contrast 0 / Brightness 50 hit the ISP sliders")
    st = await get_img()
    print("   sliders now:", param("isp.saturation"), param("isp.contrast"), param("isp.brightness"),
          "| ONVIF readback:", st.ColorSaturation, st.Contrast, st.Brightness)
    check(st.ColorSaturation == 100 and st.Contrast == 0 and st.Brightness == 50, "Imaging: GetImagingSettings reads them back")

    # A day/night switch is a full IR-cut + ISP reload and takes a few seconds;
    # space the changes out instead of queuing three switches, as HA does.
    await set_img(IrCutFilter="OFF")
    check(param("night.mode") == "night", "Imaging: IrCutFilter OFF -> night mode (IR lamp switch on)")
    await asyncio.sleep(6)
    await set_img(IrCutFilter="ON")
    check(param("night.mode") == "day", "Imaging: IrCutFilter ON -> day mode")
    await asyncio.sleep(6)
    await set_img(IrCutFilter="AUTO")
    check(param("night.mode") == "auto", "Imaging: IrCutFilter AUTO -> automatic")
    await asyncio.sleep(3)
    # restore the sliders
    await set_img(ColorSaturation=50, Contrast=50, Brightness=50)

    # ---- events through HA's parser; start a recording so JobState turns Active
    def cfg(**kv):
        body = urllib.parse.urlencode(kv).encode()
        req = urllib.request.Request(BASE + "/api/config", data=body, method="POST",
                                     headers={"X-Requested-With": "lsc",
                                              "Content-Type": "application/x-www-form-urlencoded"})
        with op.open(req, timeout=5) as r:
            r.read()
    cfg(**{"record.mode": "continuous"})
    time.sleep(3)
    mgr = await cam.create_pullpoint_manager(datetime.timedelta(seconds=60), lambda: None)
    pp = mgr.get_service()
    seen, t0 = {}, time.time()
    while time.time() - t0 < 15 and len(seen) < 5:
        msgs = await pp.PullMessages({"MessageLimit": 20, "Timeout": datetime.timedelta(seconds=5)})
        for m in msgs.NotificationMessage or []:
            topic = m.Topic._value_1
            try:
                ents = await onvif_parsers.parse(topic, f"{H}", m)   # onvif_parsers 2.5 API
            except Exception as e:  # noqa: BLE001  (HA logs and ignores topics it has no parser for)
                print("   unparsed:", topic, e)
                continue
            if not ents:
                print("   no parser for:", topic)
            for e in ents:
                seen.setdefault(e.name, e)
    for name in ("Last Reboot", "Processor Usage", "Storage Failure"):
        e = seen.get(name)
        check(e is not None, f"event parses to HA entity: {name}" + (f" = {e.value}" if e else ""))
    js = seen.get("Recording Job State")
    check(js is not None and js.value is True, "event: Recording Job State is Active while recording")
    print("   entities:", {k: (v.platform, v.value) for k, v in seen.items()})
    await mgr.shutdown()
    cfg(**{"record.mode": "off"})

    # ---- HA's buttons: the requests it would send (not sent)
    dev = await cam.create_devicemgmt_service()
    dev.create_type("SystemReboot")
    sd = dev.create_type("SetSystemDateAndTime")
    sd.DateTimeType = "Manual"
    sd.UTCDateTime = {"Date": {"Year": 2026, "Month": 11, "Day": 12}, "Time": {"Hour": 8, "Minute": 0, "Second": 0}}
    check(sd.DateTimeType == "Manual", "HA buttons build (SystemReboot, SetSystemDateAndTime)")
    await cam.close()
    print("FAILURES:", fails)
    sys.exit(1 if fails else 0)


asyncio.run(main())
