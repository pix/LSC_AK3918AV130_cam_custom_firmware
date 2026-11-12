"""ONVIF checks with the library Home Assistant uses (onvif-zeep-async).
Usage: onviftest.py host user pass   (motion self-test needs md settings, see TESTING.md)"""
import asyncio, socket, sys, time, uuid, datetime, subprocess
from onvif import ONVIFCamera
H, U, P = sys.argv[1], sys.argv[2], sys.argv[3]

def probe():
    m = f'''<?xml version="1.0" encoding="UTF-8"?><e:Envelope xmlns:e="http://www.w3.org/2003/05/soap-envelope" xmlns:w="http://schemas.xmlsoap.org/ws/2004/08/addressing" xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery" xmlns:dn="http://www.onvif.org/ver10/network/wsdl"><e:Header><w:MessageID>uuid:{uuid.uuid4()}</w:MessageID><w:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To><w:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action></e:Header><e:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe></e:Body></e:Envelope>'''
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(3); s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
    s.sendto(m.encode(), ("239.255.255.250", 3702))
    try:
        while True:
            d, a = s.recvfrom(8192)
            if a[0] == H:
                t = d.decode(errors="replace"); x = t.split("<d:XAddrs>")[1].split("</d:XAddrs>")[0] if "<d:XAddrs>" in t else "?"
                print("PASS discovery: ProbeMatch from", a[0], "XAddrs", x); return
    except socket.timeout:
        print("FAIL discovery: no ProbeMatch")

async def main():
    probe()
    cam = ONVIFCamera(H, 80, U, P)
    await cam.update_xaddrs()
    dev = await cam.create_devicemgmt_service()
    i = await dev.GetDeviceInformation(); print("PASS device info:", i.Manufacturer, "|", i.Model, "|", i.FirmwareVersion, "| serial", i.SerialNumber)
    t = await dev.GetSystemDateAndTime(); print("PASS time:", t.UTCDateTime.Date.Year, t.UTCDateTime.Time.Hour, ":", t.UTCDateTime.Time.Minute)
    n = await dev.GetNetworkInterfaces(); print("PASS network:", n[0].Info.HwAddress)
    media = await cam.create_media_service()
    prof = await media.GetProfiles(); p = prof[0]
    print("PASS profiles:", len(prof), p.token, p.VideoEncoderConfiguration.Encoding, p.VideoEncoderConfiguration.Resolution.Width, "x", p.VideoEncoderConfiguration.Resolution.Height, "gov", p.VideoEncoderConfiguration.H264.GovLength)
    su = await media.GetStreamUri({"StreamSetup": {"Stream": "RTP-Unicast", "Transport": {"Protocol": "RTSP"}}, "ProfileToken": p.token}); print("PASS stream uri:", su.Uri)
    sn = await media.GetSnapshotUri({"ProfileToken": p.token}); print("PASS snapshot uri:", sn.Uri)
    uri = su.Uri.replace("rtsp://", f"rtsp://{U}:{P}@")
    r = subprocess.run(["ffprobe", "-v", "error", "-rtsp_transport", "tcp", "-show_entries", "stream=codec_name", "-of", "csv=p=0", uri], capture_output=True, text=True, timeout=30)
    print(("PASS" if "h264" in r.stdout else "FAIL"), "stream plays:", r.stdout.split())
    # events
    ev = await cam.create_events_service()
    props = await ev.GetEventProperties(); print("PASS event properties")
    # the same path Home Assistant takes: PullPointManager (CreatePullPointSubscription + pullpoint service)
    mgr = await cam.create_pullpoint_manager(datetime.timedelta(seconds=60), lambda: print("   subscription lost"))
    pp = mgr.get_service()
    got_init, got_motion = False, False
    t0 = time.time()
    while time.time() - t0 < 40 and not got_motion:
        msgs = await pp.PullMessages({"MessageLimit": 10, "Timeout": datetime.timedelta(seconds=8)})
        for m in msgs.NotificationMessage or []:
            topic = m.Topic._value_1; data = m.Message._value_1.Data.SimpleItem[0]
            op = m.Message._value_1.PropertyOperation
            if op == "Initialized": got_init = True
            if "CellMotionDetector" in topic and data.Value == "true" and op == "Changed": got_motion = True
            print(f"   event {op:11s} {topic} {data.Name}={data.Value}")
    print(("PASS" if got_init else "FAIL"), "initial state messages;", ("PASS" if got_motion else "FAIL"), "motion event delivered")
    await mgr.shutdown()
    await cam.close()
    bad = ONVIFCamera(H, 80, U, P + "x")
    try:
        await bad.update_xaddrs(); d2 = await bad.create_devicemgmt_service(); await d2.GetDeviceInformation(); print("FAIL wrong password accepted")
    except Exception as e:
        print("PASS wrong password refused:", str(e)[:60])
    await bad.close()
asyncio.run(main())
