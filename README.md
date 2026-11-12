# LSC Smart Connect Indoor IP Camera (AK3918AV130) Custom Firmware

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![SoC](https://img.shields.io/badge/SoC-Anyka%20AK3918AV130-orange.svg)]()
[![Sensor](https://img.shields.io/badge/Sensor-GalaxyCore%20GC20C3%201080p-green.svg)]()
[![Stream](https://img.shields.io/badge/Stream-RTSP%20H.264%20%2B%20PCM%20Audio-red.svg)]()

A standalone, 100% cloud-free custom firmware and native RTSP server for the **LSC Smart Connect Smart Indoor IP Camera 1080p HD** (Action SKU `SI B26101`, model `3215672.2`).

This project completely removes the proprietary Tuya cloud agent (`anyka_ipc`) and replaces it with a lightweight, reverse-engineered C application (`ak_rtsp`) that streams pristine 1080p H.264 video and synchronized audio directly to your local network (Home Assistant, Synology Surveillance Station, Frigate, go2rtc, VLC).

---

![Camera Front & PCB](images/1.jpg)

---

## This fork: ak_rtsp with login, web admin, Home Assistant and recording

> [!WARNING]
> **AI-assisted work, tested on one camera only.** The code, reverse engineering and documentation in
> this fork were written with AI assistants (Anthropic Claude and DeepSeek) under human direction.
> It was tested on a single unit: LSC Smart Connect indoor camera **model `3215672.3`, `SI D26228`**.
> Other models or hardware revisions may differ. Read the code before trusting it, keep your stock
> firmware backup (step 0 below), and use it at your own risk.

This fork turns `ak_rtsp` into a complete camera firmware for local use. It runs on the
same decloud setup as the original project (only `bin/ak_rtsp` in the APP partition changes).

![Web admin page: live view with overlay and image settings](docs/screenshots/overview.jpg)

- **Login everywhere:** HTTP and RTSP Digest, per-IP login backoff, reset button (10 s) or SD file to restore `admin/admin`.
- **Web admin page** (port 80, HTTPS 443): live view with audio and push-to-talk, snapshot, image settings,
  day/night and IR, overlay, motion zones, recordings browser, Wi-Fi (with rollback), time/NTP,
  health, settings backup/restore, optional development terminal.
- **HTTPS and RTSPS** (port 322) with an on-camera CA or your own certificate; reverse proxy support
  (Traefik, see `docs/ak_rtsp-auth/traefik.md`).
- **Streams:** 1080p main stream and a 640x360 sub stream (`/sub`), up to 4 RTSP/RTSPS sessions.
- **Burned-in overlay:** date/time, day/night tag, frame rate and custom text drawn by the ISP.
- **Less storage:** adaptive frame rate (idle at 10/5/3 fps, full rate on motion in the zones or anywhere,
  about 70% less at 5 fps), smart encoding (long GOP / long-term reference), H.264 High profile.
- **Picture:** brightness, contrast, saturation and sharpness behave like the stock app (re-derived from its
  disassembly), plus WDR, noise reduction strength, anti-flicker, flip/mirror and privacy masks.
- **Day/night:** stock soft photosensor algorithm or exposure-based, IR LED auto/on/off, light level.
- **Events:** motion (software or ISP statistics, zones), sound level, tamper (covered/moved),
  delivered to MQTT (Home Assistant discovery, TLS with pinning or client certificates), a webhook and ONVIF.
- **ONVIF** (Profile S subset): discovery, media profiles, snapshot, imaging, events (PullPoint),
  audio back-channel; checked against what Home Assistant's ONVIF integration does.
- **SD card recording:** continuous or event-based fragmented MP4, pre/post-event buffer, timelapse, weekly
  schedules, privacy hours, retention by free space and age.
- **Speaker:** chimes, custom sound, webhook-triggered sounds, talk-back from the browser.
- **Microphone:** echo cancellation (SpeexDSP) with voice ducking so phone talk-back does not howl, mute while
  playing, gain, AGC, noise gate.
- **Status LED** activity feedback, microphone gain.

### Screenshots
The camera picture is replaced by cat photos.

| | |
|---|---|
| ![Overlay settings](docs/screenshots/overlay.jpg) | ![Motion detection and zones](docs/screenshots/events.jpg) |
| Video overlay: text, date/time, day/night tag, position | Motion detection: detector choice, sensitivity, zone grid |
| ![MQTT and Home Assistant](docs/screenshots/integrations.jpg) | ![Recordings browser](docs/screenshots/recordings.jpg) |
| MQTT with Home Assistant discovery | Recordings: motion/sound clips, pictures, timelapse, continuous |
| ![Wi-Fi](docs/screenshots/network.jpg) | ![HTTPS](docs/screenshots/security.jpg) |
| Wi-Fi scan and connect with automatic rollback | Login and HTTPS with the camera's own CA |
| ![Health](docs/screenshots/system.jpg) | ![Dark theme](docs/screenshots/dark.jpg) |
| System health and time | Follows the system dark theme |

<p align="center"><img src="docs/screenshots/mobile.jpg" alt="Phone layout" width="300"></p>

#### Recording to the SD card

| | |
|---|---|
| ![Recording settings](docs/screenshots/rec_settings.jpg) | ![Recording schedule](docs/screenshots/rec_schedule.jpg) |
| Record on motion, sound, both or continuously; pre/post-event time, sound, event pictures, timelapse, retention by space and age | Weekly hour grid for recording, alerts and privacy, with presets |
| ![Browse by day](docs/screenshots/rec_browse.jpg) | ![Select and download](docs/screenshots/rec_select.jpg) |
| Browse by day from the settings page | Recordings page: filter by type and date, select, download as .tar or delete |

![Player](docs/screenshots/rec_player.jpg)
The player: clips and pictures with previous/next and download.

Screenshots: `tools/dev/readme_shots.js` (Playwright) takes them from a running camera with the picture,
Wi-Fi scan and recording list replaced and personal values (addresses, MAC, Wi-Fi name, overlay text,
certificate fingerprints) rewritten, and refuses to save a shot where an original value is still visible.

How it was tested, how to try a build without flashing, guarded flashing/rollback, and the hardware
quirks found on the way: [TESTING.md](TESTING.md). Reverse-engineering notes: `docs/ak_rtsp-auth/re/`.

**Building this fork:** as below (Zig + SmolRTSP checkout), then
`cd src/ak_rtsp && make AR="zig ar"`. The first build downloads and checksum-verifies mbedTLS 3.6.7
(`third_party/fetch_mbedtls.sh`, needs `curl`, `bzip2`, `sha256sum`) and the default custom
speaker sound, the Windows 3.1 "Tada" (`third_party/fetch_sound.sh`, checksum-checked; `python3` and
`ffmpeg` convert it). Use your own with `src/ak_rtsp/sounds/custom.wav`, another download with
`make CUSTOM_SOUND_URL=… CUSTOM_SOUND_SHA256=…`, or none with `make CUSTOM_SOUND_URL=`.
`python3` also regenerates the web pages if you edit them. Host tests: `bash tests/run_host_tests.sh`.

---

## ⚠️ Important Disclaimer & Risks

> ⚠️ **CAUTION: FLASHING AND MODIFYING THIS DEVICE IS STRICTLY AT YOUR OWN RISK.**
>
> - **Experimental Codebase:** This is the public release of a much larger private reverse-engineering project. The reverse engineering itself — reading UART logs, decompiling `anyka_ipc` in Ghidra, strace-capturing the stock RTSP session, cross-referencing ioctl structs, etc. — was done by hand over many sessions. This public repository (its structure, docs, and some cleanup/refactoring) was then compiled and organized with AI assistance from that original project. While core streaming and hardware drivers have been verified on test units, the codebase has **not been exhaustively tested by hand across all conditions or manufacturing batches** and carries inherent experimental risks.
> - **Risk of Permanent Hardware Bricking:** Writing to the SPI NOR flash chip carries a real risk of permanently bricking your camera. In the worst case, a corrupted flash may result in an unrecoverable hard brick that can only be fixed by physically desoldering the flash chip and using an external SPI programmer (e.g. CH341A).
> - **No Liability:** The authors and contributors of this repository accept **zero liability or responsibility** for damaged hardware, bricked cameras, data loss, or voided warranties.
> - **Windows + WSL only.** Every tool, script, and workflow in this repo (the .NET serial console, the `zig cc` cross-build, the WSL `mksquashfs` packaging step) has only ever been developed and tested on Windows 11 with WSL/Ubuntu. Native Linux or macOS hosts, other WSL distros, or other cross-compilers may well work, but they are **completely untested** — you're on your own figuring out any path/tooling differences if you go that route.
> - **Always make a complete flash backup** before attempting to flash any custom image.
> - **Always double-check the flash verification logs** before disconnecting power.
> - If flashing fails or reports errors, **DO NOT power cycle**—attempt to flash your stock backup immediately!

---

## Key Features

- **100% Cloud-Free (De-Tuyafied):** Tuya cloud daemons, MQTT beacons, and telemetry binaries are completely eliminated.
- **Native SmolRTSP Streaming Engine:** Lightweight standalone C server (`ak_rtsp`, ~126 KB) built on [OpenIPC's SmolRTSP](https://github.com/OpenIPC/smolrtsp) library, interfacing directly with the Anyka hardware ISP and H.264 encoder.
- **1080p High-Quality Video:** Full 1920x1080 resolution at 15–25 FPS with calibrated ISP parameters (color correction matrix, white balance gains, noise reduction, and VBR+ rate control).
- **Synchronized Audio:** Captures from on-board microphone via `/dev/pcmC0D0c` streamed over RTP (L16 / 8000 Hz) with RTCP Sender Reports for jitter-free playback in VLC and NVRs.
- **IR-Cut & Night Mode:** Dual GPIO solenoid pulse control for daytime IR-cut filtration and night vision illumination.
- **Hardened MTD Flasher (`flash_tool`):** Chunked 4KB write/verify/retry flasher that avoids the SPI-NOR DMA corruption bugs common when writing flash over active network/camera drivers.
- **Hardware Toolkit:** Includes an automated ESP32 U-Boot interceptor bridge and a high-performance Windows .NET 9 WinUI serial console.

---

## 📷 Current Status & Image Quality

> ℹ️ **Image Quality Notice:**  
> The custom firmware is fully functional and stable for continuous 24/7 streaming. However, **the dynamic image tuning (contrast, wide-dynamic range curves, and auto-exposure adjustments) is still somewhat behind the stock Tuya firmware's proprietary closed-source tuning**.  
> We have calibrated the static white balance gains (`R=460, G=256, B=504`) to fix the raw sensor blue tint and tuned VBR+ rate control to minimize macroblocking, but there is ample room for future community improvement and tuning of the 24-module Anyka ISP pipeline! Pull requests and ISP tuning experiments are very welcome.

| | Stock Tuya (`anyka_ipc`) | This Firmware (`ak_rtsp`) |
|---|:---:|:---:|
| **Static scene** | ![Stock stable](images/video_out/anyka_stable.png) | ![Custom stable](images/video_out/ours_stable.png) |
| **Motion** | ![Stock movement](images/video_out/anyka_movement.png) | ![Custom movement](images/video_out/ours_movement.png) |

### Known Limitations

- **No dynamic auto white balance (AWB).** White balance uses fixed, hand-calibrated gains
  (see above) rather than tracking scene color temperature dynamically. In practice, the stock
  Tuya firmware doesn't appear to do dynamic AWB either in the scenes tested so far, so this may
  not be a real functional gap — but it's worth knowing if your lighting varies a lot.
- **Two-way audio (speaker output) is not supported.** Microphone capture → RTP streaming
  (camera-to-viewer audio) is fully working, but the reverse direction (playback through the
  camera's on-board speaker, e.g. for NVR "talk" features) is not implemented. Out of scope for
  now — may be revisited in the future.

---

## Hardware Overview

| Component | Specification |
|---|---|
| **Retail Model** | LSC Smart Connect Smart Indoor IP Camera 1080p HD (Action) |
| **Model / SKU** | `3215672.2` / `SI B26101` |
| **PCB Revision** | `IPC280KG2-GNA-MAIN-V1.0` (Circular PCB, 2025-08-28) |
| **SoC** | Anyka AK3918AV130 (ARM926EJ-S @ 400 MHz) |
| **Image Sensor** | GalaxyCore GC20C3 (2MP 1/2.9" CMOS, I2C bus) |
| **Flash Memory** | 8 MiB SPI NOR (`XM25QH64D` / Winbond SOIC-8) |
| **Wireless** | Altobeam ATBM6012BX / ATBM6x3x USB WiFi |
| **Power** | USB-C 5V / 1A |

> ℹ️ **NOTE:** This repository is tailored for **Revision 2** (circular PCB, Anyka AK3918AV130). Older revisions with rectangular boards used different SoCs (AK3918EV200 or GK7102) which have different register maps and flash partitioning.

---

## Step 0: MANDATORY — Dump Your Stock Firmware Backup!

Before doing anything else, connect to your camera shell (via UART or Telnet) and create a full backup of all 8 flash partitions:

```sh
# 1. Mount SD card and create backup directory
mount -t vfat /dev/mmcblk0p1 /mnt
mkdir -p /mnt/original_firmware

# 2. Dump all partitions
dd if=/dev/mtdblock0 of=/mnt/original_firmware/mtdblock0.bin bs=64k
dd if=/dev/mtdblock1 of=/mnt/original_firmware/mtdblock1.bin bs=64k
dd if=/dev/mtdblock2 of=/mnt/original_firmware/mtdblock2.bin bs=64k
dd if=/dev/mtdblock3 of=/mnt/original_firmware/mtdblock3.bin bs=64k
dd if=/dev/mtdblock4 of=/mnt/original_firmware/mtdblock4.bin bs=64k
dd if=/dev/mtdblock5 of=/mnt/original_firmware/mtdblock5.bin bs=64k
dd if=/dev/mtdblock6 of=/mnt/original_firmware/mtdblock6.bin bs=64k
dd if=/dev/mtdblock7 of=/mnt/original_firmware/mtdblock7.bin bs=64k
sync
```

Keep a copy of `/mnt/original_firmware/` safely on your computer!

---

## Quick Installation Guide

### Step 1: Prepare the MicroSD Card
Format a MicroSD card as FAT32 and copy the following files:
- `custom_firmware/ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs` (the padded firmware image — see
  [Building Your Own Firmware](docs/building_your_own_firmware.md) for how to produce this)
- `flash_tool` (pre-compiled binary from `src/ak_rtsp/`)
- `tools/flash_scripts/install_with_flash_tool.sh`
- `tools/flash_scripts/restore.sh` *(for safety fallback)*
- `original_firmware/mtdblock*.bin` *(your stock backup)*

### Step 2: Connect & Run Flashing Script
Insert the SD card, connect to the camera over Telnet (or UART), mount the card, and execute:
```sh
mount -t vfat /dev/mmcblk0p1 /mnt
sh /mnt/install_with_flash_tool.sh
```

### Step 3: Check Verification Output!
> 🛑 **IMPORTANT: Always verify the tool output before powering down:**
> ```text
> FAULT SUMMARY: 0/1280 chunks needed retry, 0 total failed verify attempts.
> Flashing and verification completed successfully!
> ```
> If any errors are displayed, **DO NOT DISCONNECT POWER!** Run `sh /mnt/restore.sh` to revert to your stock firmware image first.

### Step 4: Power Cycle by Hand
> ℹ️ **NOTE:** The software `reboot` command **does not work reliably** on this hardware platform.  
> **Unplug and reconnect the USB-C power cable by hand** to power cycle the camera.

---

## RTSP Stream URLs

Once booted into the custom firmware, the camera serves RTSP on standard port 554.

The custom `ak_rtsp` server is **path-agnostic** (it serves the 1080p video + audio stream on any requested path or root `/`):

| Format | RTSP URL Example | Notes |
|---|---|---|
| **Root (Simplest)** | `rtsp://<camera-ip>:554/` | 1920x1080 H.264 + Synchronized L16 Audio |
| **Named Path (Optional)** | `rtsp://<camera-ip>:554/live` or `/main_ch` | Any sub-path works interchangeably |

*(Note: `rtsp://<camera-ip>:554/main_ch` was the legacy path used by the original Tuya firmware, which continues to work seamlessly).*

---

## Repository Structure

```
.
├── docs/                                # Technical Documentation & Guides
│   ├── hardware_and_uart.md             # Disassembly, UART pads, U-Boot shell & photos
│   ├── flash_partition_map_and_rootfs.md# Flash layout, stock boot traps, de-Tuyafication
│   ├── firmware_building_and_flashing.md# Build guide & SPI-NOR DMA contention gotchas
│   ├── camera_and_audio_pipeline.md     # ISP registers, div0 kernel fix, audio capture
│   ├── building_your_own_firmware.md    # WSL setup, dump/unsquash/patch/rebuild your own image
│   └── ak_rtsp-auth/                    # This fork: reverse-engineering notes, Traefik guide
├── images/                              # Hardware photographs and diagrams
├── tests/                               # This fork: host tests (run_host_tests.sh) and camera test scripts
├── TESTING.md                           # This fork: test procedure, safe flashing, hardware findings
├── src/
│   └── ak_rtsp/                         # Standalone C RTSP server & flash_tool source
│       ├── Makefile                     # Cross-compilation Makefile (Zig / Anyka GCC)
│       ├── build_firmware.sh            # Patches your own app_extracted/ dump & repacks squashfs
│       ├── main.c                       # System bring-up and orchestration
│       ├── isp.c / isp.h                # GC20C3 134-reg init & 24-module ISP pipeline
│       ├── vi.c / vi.h                  # Video input channels (1080p slice mode)
│       ├── venc.c / venc.h              # H.264 hardware encoder setup (VBR+)
│       ├── rtsp.c / rtsp.h              # RTSP / RTP server & RTCP Sender Reports
│       ├── audio.c / audio.h            # ALSA PCM mic capture & ring buffer
│       ├── ircut.c / ircut.h            # IR-cut solenoid pulse controller
│       ├── ae.c / night.c               # Hardware auto-exposure & day/night switching
│       ├── flash_tool.c                 # Chunked verified MTD SPI-NOR flasher
│       └── arm_atomics.S                # ARMv5TE legacy atomic stubs
└── tools/
    ├── flash_scripts/                   # Deployment and flash management scripts
    │   ├── install_with_flash_tool.sh   # Primary proven Telnet/UART MTD flasher
    │   ├── install_ak_rtsp_firmware.sh  # Bare UART fallback flasher
    │   ├── pad_build.sh                 # SquashFS padding & timestamp naming script
    │   ├── restore.sh                   # Restore stock APP partition
    │   ├── restore_full_firmware.sh     # Restore ALL stock partitions
    │   └── ...
    ├── maint/                           # This fork: guarded APP flash/rollback (flash_image.sh, KNOWN_IMAGES)
    ├── dev/                             # This fork: camera shell, SD test-binary deploy, SD hook lines, README screenshots
    ├── firmware_patch_templates/        # The actual de-Tuyafication patch (process.ini + wrapper)
    └── hardware_debug/                  # Hardware tools
        ├── lolin32_uart_bridge/         # ESP32 auto U-Boot interception firmware
        └── serial_console_dotnet/       # WinUI 3 serial terminal with base64 transfer
```

---

## Building from Source

### Prerequisite: SmolRTSP
`ak_rtsp` is **not** linked against a prebuilt SmolRTSP library — its `Makefile` compiles
[OpenIPC's SmolRTSP](https://github.com/OpenIPC/smolrtsp) sources directly into the `ak_rtsp`
binary, and expects to find them as a sibling directory: `../smolrtsp` relative to
`src/ak_rtsp/` — i.e. checked out at **`src/smolrtsp`**, next to `src/ak_rtsp/` (not at the repo
root). This is *not* included in this repository. Before building, clone it there and let CMake
populate its own header-only dependencies (`slice99`, `datatype99`, `interface99`, `metalang99`)
via FetchContent — you only need the *configure* step, not a full build:

```bash
cd src
git clone https://github.com/OpenIPC/smolrtsp.git
cd smolrtsp
cmake -S . -B build   # populates build/_deps/{slice99,datatype99,interface99,metalang99}-src
```

If you don't have (or don't want to install) CMake, you can populate the same four dependencies
by hand instead — this was verified to produce an identical, working build:
```bash
cd src/smolrtsp
mkdir -p build/_deps
cd build/_deps
for pair in "slice99:v0.7.8" "metalang99:v1.13.5" "datatype99:v1.6.5" "interface99:v1.0.2"; do
    name="${pair%%:*}"; tag="${pair##*:}"
    curl -sL "https://github.com/hirrolot/$name/archive/refs/tags/$tag.tar.gz" -o "$name.tar.gz"
    mkdir -p "${name}-src"
    tar xzf "$name.tar.gz" -C "${name}-src" --strip-components=1
    rm "$name.tar.gz"
done
```
(These are the exact versions SmolRTSP's own `CMakeLists.txt` `FetchContent_Declare` calls
pinned at the time this project was built — check that file if the upstream repo has since
bumped them.)

The resulting `src/smolrtsp/` tree (with `include/` and the populated `build/_deps/`) is exactly
what `src/ak_rtsp/Makefile`'s include paths expect. **Verified working end-to-end** (both the
CMake-less dependency fetch above and the full `make` build below were run against this exact
repo and produced statically-linked ARM/EABI5 binaries with no errors). This has only been
tested against the SmolRTSP revision available at the time this project was built — if the
upstream repo has since diverged, some source-level adjustments in `src/ak_rtsp/rtsp.c`/`venc.c`
may be needed.

### Prerequisite: GNU Make for Windows
`make` for compiling `ak_rtsp` is run **natively on Windows, not inside WSL** (WSL is only used
for the separate squashfs-packaging step in
[Building Your Own Firmware](docs/building_your_own_firmware.md)). Windows doesn't ship `make`,
so you need a native GNU Make on your `PATH`. This project was built against
[GnuWin32 Make](https://gnuwin32.sourceforge.net/packages/make.htm) (GNU Make 3.81, installed to
`C:\Program Files (x86)\GnuWin32\bin\make.exe` via its standalone installer) — that's an old,
unmaintained package but it's a plain native `.exe` with no MSYS/Cygwin runtime needed, which is
why it was picked. If you'd rather use a maintained source, a package manager works too and any
recent GNU Make version should be fine:
```powershell
winget install GnuWin32.Make      # or: choco install make   /   scoop install make
```
Whichever you use, make sure `make.exe` ends up on `PATH` (open a new terminal after installing
and confirm with `make --version`).

### Compiling `ak_rtsp`
You can cross-compile using [Zig](https://ziglang.org/) without needing complex toolchains installed:

```powershell
cd src/ak_rtsp
make CC="zig cc -target arm-linux-musleabi -mcpu=arm926ej_s -static"
```

### Compiling `flash_tool`
```bash
cd src/ak_rtsp
zig cc -target arm-linux-musleabi -mcpu=arm926ej_s -static -O2 flash_tool.c -o flash_tool
```

### Running the .NET Serial Console & Tuning Suite
```powershell
cd tools/hardware_debug/serial_console_dotnet
dotnet run -c Release
```

---

## Detailed Documentation

- [Hardware & UART Connection Guide](docs/hardware_and_uart.md)
- [Flash Partition Map & De-Tuyafication Details](docs/flash_partition_map_and_rootfs.md)
- [Firmware Packaging & Flash DMA Gotchas](docs/firmware_building_and_flashing.md)
- [Camera ISP, Audio & Div0 Fix Architecture](docs/camera_and_audio_pipeline.md)
- [Serial Console & Live Camera Tuning App Guide](docs/serial_console_and_camera_tuning.md)
- [Building Your Own Firmware From Your Own Dump](docs/building_your_own_firmware.md)

---

## Methods & Approach

This firmware exists because of a fairly standard embedded-RE toolchain, applied by hand over
many sessions before this repo was compiled:

1. **UART access first.** Solder to the 4 UART pads, capture the boot log, intercept U-Boot's
   autoboot with a scripted Ctrl+C flood, and get a root shell via `init=/bin/sh` bootargs.
2. **strace the stock behavior.** With the vendor's `anyka_ipc` running its factory RTSP demo
   mode, `strace` every ioctl it makes against the ISP/VI/VENC/audio character devices. This is
   the ground truth for every hardware init sequence this project reimplements — reversing from
   a decompiler alone, without a real reference trace of a known-working session, would have
   been far slower and less reliable.
3. **Decompile in Ghidra.** Where the strace log alone doesn't explain a struct layout or ioctl
   semantics (e.g. why a particular field has to be non-zero), decompile the relevant function
   in `anyka_ipc` or a kernel module to confirm it directly.
4. **Reimplement from scratch, verify against the trace.** `ak_rtsp` doesn't call into any
   vendor `.so` — it drives the same character devices directly, with ioctl sequences derived
   from steps 2–3, and its output is repeatedly checked byte-for-byte against captures of the
   stock session (see `docs/camera_and_audio_pipeline.md`) whenever behavior diverged.
5. **De-Tuyafy at the application layer.** Rather than replacing the kernel or rootfs, only the
   APP partition's process list is patched to launch `ak_rtsp` instead of the vendor's Tuya
   cloud client — see `docs/flash_partition_map_and_rootfs.md`.

## Credits & Acknowledgements

- **Anyka Community & Researchers:** Inspiration and reference implementations from [VGerris's Anyka Hacking Journey](https://github.com/VGerris/Anyka_ak3918_hacking_journey) and [Lawliet95's ANYKA Tuya Journey](https://github.com/Lawliet95/ANYKA-Tuya-Hacking-Journey-AK3918v200EN080-v200). Both cover earlier-generation Anyka SoCs (`AK3918EV200`/`GK7102`) with different register maps and flash layouts than this Revision 2 (`AK3918AV130`) hardware, but were valuable for general ioctl naming conventions and orientation.
- **Original firmware:** [FluxAlchemist/LSC_AK3918AV130_cam_custom_firmware](https://github.com/FluxAlchemist/LSC_AK3918AV130_cam_custom_firmware), which this fork builds on.
- **Decloud wizard:** [4ddict/Action-Smart-Connect-Flasher](https://github.com/4ddict/Action-Smart-Connect-Flasher), used to decloud and install on the development unit.
- **mbedTLS** (Apache-2.0, fetched at build time) for HTTPS/RTSPS/MQTT TLS; **quirc** (ISC) for Wi-Fi QR codes.
- **Screenshot cats** (CC0, via Wikimedia Commons, cropped to 16:9): [Gaspard the too big cat](https://commons.wikimedia.org/wiki/File:Gaspard_the_too_big_cat_(Unsplash)_(2017;_cropped_2023).jpg)
  by Elsa Noblet and [Hairless cat and a plant](https://commons.wikimedia.org/wiki/File:Hairless_cat_and_a_plant_(Unsplash)_(2016;_cropped_2023).jpg) by Linnea Sandbakk (Unsplash).
- **Default custom sound:** the Windows 3.1 startup sound (© Microsoft), downloaded at build time from
  [winhistory.de](https://www.winhistory.de/more/winstart/winstart_en.htm); it is not part of this repository.
- **SmolRTSP (OpenIPC):** High-performance, lightweight RTSP library for embedded systems by [OpenIPC/smolrtsp](https://github.com/OpenIPC/smolrtsp).
