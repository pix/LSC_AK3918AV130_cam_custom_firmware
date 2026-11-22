"""The first steps of Home Assistant's ONVIF setup (onvif-zeep-async, the library
HA uses): xaddrs, device information, media profiles. Prints OK / FAIL and the
error as HA would show it (e.g. "ClientOSError: [Errno 104] Connection reset
by peer", which is what HA reported when pointed at the HTTPS port).
Usage: ha_setup.py <host> <port> <user> <password>"""
import asyncio, sys
from onvif import ONVIFCamera

H, P, U, W = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]


async def main():
    cam = ONVIFCamera(H, P, U, W)
    try:
        await cam.update_xaddrs()
        dev = await cam.create_devicemgmt_service()
        info = await dev.GetDeviceInformation()
        media = await cam.create_media_service()
        profiles = await media.GetProfiles()
        print(f"OK port {P}: {info.Manufacturer} {info.Model}, {len(profiles)} profiles")
        return 0
    except Exception as e:
        print(f"FAIL port {P}: {type(e).__name__}: {e}")
        return 1
    finally:
        await cam.close()

sys.exit(asyncio.run(main()))
