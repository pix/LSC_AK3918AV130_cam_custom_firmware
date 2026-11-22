# 📷 LSC Smart Connect Decloud Wizard

> **Copy in this repository.** This directory is the Decloud Wizard by the
> [Action-Smart-Connect-Flasher](https://github.com/4ddict/Action-Smart-Connect-Flasher) contributors,
> release 0.3.2 (commit `3b79b60`), MIT licensed (see `LICENSE` here). It is vendored so the wizard
> installs **this repository's** firmware. See [Changes in this copy](#-changes-in-this-copy).

**Turn the €9.95 Action LSC Smart Connect indoor camera into a local RTSP camera — without needing to know Telnet, MTD flash layouts, cross-compilers, or Linux device names.**

> ✅ Originally tested on **3215672.2 / SI B26101**; additional community reports listed below\
> 📡 Local 1080p RTSP via `ak_rtsp`  
> ☁️ Removes the Tuya `anyka_ipc` cloud application from the APP partition  
> 💾 Creates a full stock firmware backup before flashing  
> 🐧 Interactive installer for Linux

---

## 🚀 Quick Install

Run this on your Linux computer:

```bash
curl -fsSL https://raw.githubusercontent.com/pix/LSC_AK3918AV130_cam_custom_firmware/main/tools/decloud_wizard/install.sh -o /tmp/lsc-decloud-wizard.sh && bash /tmp/lsc-decloud-wizard.sh
```

Run this way, the wizard builds the firmware from this repository's `main` branch (it prints the exact
commit it uses). Run from a clone (see *Manual install* below), it builds the committed code of that clone.

That is all you need to download manually. The wizard downloads its helper files and the required upstream projects automatically.

**Do not run the complete wizard with `sudo`.** It will ask for your sudo password only when it needs to format/mount the SD card or install packages.

### Version 0.3.2

- Recognises the two additional camera labels reported in [issue #2](https://github.com/4ddict/Action-Smart-Connect-Flasher/issues/2), while retaining the exact MTD layout and backup checks.
- Downloads an official, SHA-256-verified Zig compiler when Zig is missing, including on Debian/Q4OS ;).
- Temporarily suppresses desktop automount for the selected SD card when udev/UDisks is available, and checks that it is unmounted before formatting.
- Documents the firmware's existing local camera controls below.

Version 0.3.1 already fixed detection of tools in `/sbin` and the separate Debian `fdisk` package. The complete wizard now refuses to run as root; start it as your regular user.

---

## ✨ What the wizard does

The installer is deliberately written for people who have never modified an IP camera before.

It walks you through the complete process using numbered choices and plain-English instructions:

1. 🔎 **Confirm the camera model** — refuses to continue for the wrong article number.
2. 🏷️ **Name the camera** — for example `Front Door` → `front-door`.
3. 🧰 **Check your computer** — installs missing build/tools where supported.
4. 📶 **Ask for Wi-Fi details** — password is hidden and entered twice for verification.
5. 💾 **Choose the microSD card** — shown as an easy numbered list with size/model information.
6. 🛟 **Back up all 8 MTD partitions** before anything is flashed.
7. 📡 **Save Wi-Fi permanently** so the finished camera boots without the SD card.
8. 🏗️ **Build your own cloud-free firmware** from the APP dump of your camera.
9. ⚡ **Flash + read-back verify every 4 KiB block** of the APP partition.
10. ✅ **Check the final RTSP service** and display the stream URL.


---

## 📦 What you need

| Item | Requirement |
| --- | --- |
| Camera | One of the exact labels in the compatibility table below |
| Computer | Linux PC/laptop |
| microSD | Any small card is sufficient; its contents will be erased |
| Wi-Fi | 2.4 GHz network |
| Network | Computer and camera must be able to reach each other locally |

The wizard currently supports dependency installation on **Arch/CachyOS**, **Debian/Ubuntu** and **Fedora-family** systems where the required packages are available.

The wizard checks the available tools before choosing `pacman`, APT or `dnf`. If everything is installed, it skips package installation.
---

## ⚠️ Check the model before you start

Choose the exact article number and SI code printed on your camera or box:

| Article number | SI code | Evidence |
| --- | --- | --- |
| **3215672.2** | **B26101** | Original hardware-tested camera, Anyka AK3918AV130 |
| **3215672.2** | **C26101** | A successful installation with wizard 0.3.0 was reported in [issue #2](https://github.com/4ddict/Action-Smart-Connect-Flasher/issues/2) |
| **3215672.3** | **D26228** | A successful installation with wizard 0.3.0 was reported in [issue #2](https://github.com/4ddict/Action-Smart-Connect-Flasher/issues/2) |

The additional variants have not been independently hardware-tested by this project. Their inclusion is based on that user's successful installations. A matching label alone does not bypass the camera-side checks.

It is **not** for:

```text
3215672
3215672.1
```

Do not assume other SI codes or revisions are compatible. The wizard asks for the exact label and verifies the same MTD partition map on the camera before allowing a flash. A different layout is rejected.

---

## 🧠 What changes on the camera?

The camera keeps its original:

- U-Boot bootloader
- Linux kernel
- hardware drivers
- root filesystem
- CONFIG partition

The **APP** partition is rebuilt from your own stock dump. The original Tuya/LSC `anyka_ipc` application is removed and replaced by the lightweight local `ak_rtsp` application.

The expected flash map is:

```text
mtd0: 00032000 00001000 "UBOOT"
mtd1: 00001000 00001000 "ENV"
mtd2: 00001000 00001000 "ENVBK"
mtd3: 00010000 00001000 "DTB"
mtd4: 00180000 00001000 "KERNEL"
mtd5: 000fc000 00001000 "ROOTFS"
mtd6: 00040000 00001000 "CONFIG"
mtd7: 00500000 00001000 "APP"
```

If the layout differs, the wizard stops **before writing anything**.

---

## 🛟 Automatic backup

Before modifying the camera, the wizard dumps and verifies all eight original MTD partitions.

Backups are stored per camera name, for example:

```text
~/.cache/lsc-3215672-decloud-wizard/backups/front-door/20260914_153000/
```

This is useful when modifying several cameras. Give each camera a unique name such as:

```text
front-door
backyard
garage
hallway
```

Keep these backups somewhere safe.

---

## 📡 RTSP

After the final boot, the camera should be reachable without an SD card.

```text
rtsp://<camera-ip>:554/
```

The server is path-agnostic, so this also works:

```text
rtsp://<camera-ip>:554/main_ch
```

You can use the stream with software such as **Frigate, Scrypted, Home Assistant, VLC, ffmpeg or go2rtc**.

For multiple cameras, create a DHCP reservation for each camera in your router. The wizard also changes the DHCP hostname to the camera name you entered during setup.

---

## 🎛️ Live camera settings

The pinned upstream `ak_rtsp` firmware already includes a local, text-based control service on **TCP port 8091**. You can change settings after installation without rebuilding or reflashing firmware. This is not an HTTP API.

For example, connect from a computer on the same local network using netcat:

```bash
nc <camera-ip> 8091
```

If `nc` is missing on Debian/Q4OS/Ubuntu, install it with `sudo apt-get install netcat-openbsd`. On Arch/CachyOS the package is `openbsd-netcat`; on Fedora, `nmap-ncat` supplies the `ncat` command, which can be used in place of `nc`.

Type a command and press Enter. Start by reading the available settings:

```text
LIST
GET night.mode
GET night.state
```

| Command | Effect |
| --- | --- |
| `SET night.mode day` | Force day mode: IR illumination off and the IR-cut filter in its daytime position |
| `SET night.mode night` | Force night mode: IR illumination on and the IR-cut filter in its night position |
| `SET night.mode auto` | Return to automatic day/night switching |
| `GET night.state` | Read the current day/night state; this value is read-only |
| `SET isp.brightness 10` | Adjust brightness; accepted range is -50 to 50, with 0 as the neutral setting |

`isp.contrast`, `isp.saturation` and `isp.sharpness` also accept values from -50 to 50. `LIST` shows the exposure and automatic day/night tuning parameters as well. Change one setting at a time and record its previous value.

Day/night mode coordinates the IR LEDs, IR-cut filter and sensor settings; it is not an independent IR-LED switch. A mode change is applied by the monitor thread, so allow a moment before reading `night.state` again.

Replies beginning with `OK` acknowledge a change; `ERR` indicates a rejected command. Lines beginning with `LOG` are live camera logs. Press Ctrl+C to disconnect.

These are **runtime settings**: they reset when `ak_rtsp` restarts or the camera reboots. The service has no authentication or encryption. Keep port 8091 on a trusted local network and do not expose it to the Internet.

The protocol is implemented by upstream [control.c](https://github.com/FluxAlchemist/LSC_AK3918AV130_cam_custom_firmware/blob/3250e0f/src/ak_rtsp/control.c).

---

## 🔐 Wi-Fi handling

The installer:

- stores the temporary credentials on the SD card during installation;
- persists the working `wpa_supplicant` configuration to the camera's CONFIG partition;
- does **not** intentionally upload your Wi-Fi credentials anywhere.

After installation, reformat the SD card because it contains both your Wi-Fi setup files and your private stock firmware dump.

Never commit SD-card contents, firmware dumps, or Wi-Fi configuration files to GitHub.

---

## 🧪 Flash safety checks

Before writing `mtd7`, the camera-side flash script verifies:

- exact supported MTD layout;
- stock APP backup exists and is exactly 5 MiB;
- custom APP image is exactly 5 MiB;
- current camera APP MD5 still matches the backup created in Stage 1;
- `flash_tool` is present.

The image is then erased/written directly to SPI NOR and every 4 KiB block is read back and verified.

A successful run ends with a message similar to:

```text
VERIFICATION SUCCESSFUL
0/1280 chunks needed at least one retry
Flashing and verification completed successfully
```

Only after the wizard has confirmed success should the camera be power-cycled.

---

## 🔄 Updating a camera (`--update`)

A camera that already runs this firmware is updated without redoing the installation: no Wi-Fi setup, no
backup stage, settings kept (Wi-Fi, login, image settings and recordings are not in the APP partition).

```bash
bash /tmp/lsc-decloud-wizard.sh --update      # or ./install.sh --update from a checkout
```

1. Pick the camera (the ones installed or updated from this computer are listed) or name another one.
2. The wizard fetches and builds the newest firmware, as for an installation.
3. Maintenance access, two choices:
   - **the camera already has a maintenance SD card in** (the card from the installation, which still opens
     the maintenance port 24), or
   - **make a new maintenance SD card**: the wizard erases a card and writes a minimal one that only opens
     port 24 (no Wi-Fi change, no backup stage, no vendor settings). Insert it, power-cycle, wait 45 s.
4. The wizard copies the camera's current firmware to this computer over the LAN (checksum-verified; kept in
   the camera's backup folder as `before_update_<date>`), checks it is an ak_rtsp build (a stock camera needs
   the full installation), and builds the new image from this camera's stock backup if this computer has it,
   else from that current firmware.
5. It sends the image, `flash_tool` and `camera/decloud_update.sh` to the SD card (checksum-verified), asks
   once more, then the camera-side script: checks the exact flash layout, the image checksum and squashfs
   header, that the APP is still exactly the one read in step 4, backs it up to the SD card
   (`update/mtd7_before_<md5>.bin`), writes with per-block verification, checks the whole partition, and
   restores the backup automatically if anything does not verify.
6. The camera restarts; the wizard waits for the web page and RTSP.

Remove the maintenance card afterwards: while it is in the camera, port 24 is a root shell without a password
for anyone on your network.

---

## 💡 Stage 1 notes

During the first camera boot with the prepared SD card:

- the **blue LED may continue blinking**;
- the factory voice prompt is intentionally muted, so you will normally **not hear the Chinese factory-mode message**;
- leave the camera powered for the full period shown by the wizard.

The wizard checks the SD card afterwards and will tell you whether Stage 1 actually succeeded.

---

## 🛠️ Troubleshooting

### Zig is not available in Debian/Q4OS repositories

Version 0.3.2 can install the checksum-verified official compiler automatically after you accept dependency installation. You do not need to add a third-party repository. The first compiler run can take a long time on an older computer, especially while Zig prepares its ARM target libraries. Later runs can benefit from Zig's compiler cache; the wizard still rebuilds the executables.

### The SD card remounts while being prepared

The wizard uses a temporary udev rule to tell UDisks to ignore only the selected card while the wizard works on it. It removes the rule before card removal or when the wizard exits. It checks for remaining mounts before wiping or formatting.

Close file-manager windows using the card. If your desktop uses another automounter, or `udevadm` is unavailable, disable that desktop's automount option temporarily and rerun the wizard. Do not force formatting of a mounted card.

### Stage 1 says it did not complete

The most common cause is an incorrect Wi-Fi SSID or password. The wizard lets you re-enter the Wi-Fi details and retry without starting over.

### The camera gets a different IP address

That is normal with DHCP. Check your router's client list. After installation, creating a DHCP reservation is recommended.

### The camera is online but the wizard cannot reach maintenance port 24

Make sure the SD card prepared by the wizard is still inserted during the flashing stage and that your PC and camera are on the same LAN/VLAN.

### Flashing reports an error

**Do not disconnect power.** Read the error shown by the wizard. The camera-side script attempts a stock APP restore if the custom write fails, but power should remain connected until the result is known.

---

## 🧑‍💻 Manual install / development

Instead of the one-line installer:

```bash
git clone https://github.com/pix/LSC_AK3918AV130_cam_custom_firmware.git
cd LSC_AK3918AV130_cam_custom_firmware/tools/decloud_wizard
./install.sh
```

To build another commit or another fork instead: `LSC_FIRMWARE_COMMIT=<commit> ./install.sh`
(a clone of `LSC_WIZARD_REPO`, default `pix/LSC_AK3918AV130_cam_custom_firmware`).

---

## 🔁 Changes in this copy

Compared with upstream release 0.3.2:

- **Firmware source:** this repository instead of FluxAlchemist's firmware pinned at `3250e0f`: the
  checkout the wizard runs from (its committed `HEAD`), or this repository's `main` resolved to an exact
  commit when started with the one-line command. The toolkit and SmolRTSP pins are unchanged.
- **No source patching:** upstream 0.3.2 patched the FluxAlchemist source while building (GC20C3
  exposure cap, blocking RTSP socket, day/night thresholds 3000/1200, day/night mode-change fix). These
  fixes are part of this repository's source; the wizard now only checks they are present and stops if not.
- **Stripped binary:** the image is built with `ak_rtsp_stripped` (~1.2 MB instead of ~5 MB with debug
  symbols), so it fits the 5 MiB APP partition; `pad_build.sh` refuses an oversized image.
- **Helper download URL** (one-line mode): `tools/decloud_wizard/camera/` of this repository.
- **Zig:** a system Zig is used as before (installed with the other packages on Arch and Fedora). When none
  is available, the wizard no longer downloads one silently: it shows how to install Zig for your
  distribution and asks whether to quit (install it yourself) or download the official, SHA-256 checked
  Zig 0.14.1 into its own cache. Both Zig 0.14.1 and 0.16 build this firmware.
- **Stage 1 records the camera's address:** `decloud_stage1.sh` read the IP right after Wi-Fi association,
  before the DHCP lease, so `IP=` in `STAGE1_STATUS` could stay empty and stage 2 had no address to propose.
  It now waits up to 30 s for the address. `camera/decloud_flash.sh` is unchanged.

Tested: a complete installation on a stock 3215672.3 / SI D26228 camera (stage 1, build, stage 2 flash),
then HTTPS from scratch (certificates removed) verified with OpenSSL, GnuTLS, ffmpeg and Python. Also:
the wizard's source and build steps (`update_sources`, `build_firmware`) run end to end on Linux
against a stock APP dump and produced an image identical in content to the hardware-tested v204 image
(except the freshly built binary and the chosen hostname).

Useful commands:

```bash
./install.sh --version
./install.sh --help
```

Generated firmware, camera dumps, extracted files and Wi-Fi configs are ignored by `.gitignore` and should never be committed.

---

## ❤️ Credits

This wizard is an orchestration/user-experience layer built on the reverse-engineering work of other projects:

- **tasarren / lsc-tuya-toolkit** — SD boot hook, camera access, Wi-Fi and maintenance services  
  <https://github.com/tasarren/lsc-tuya-toolkit>
- **FluxAlchemist / LSC_AK3918AV130_cam_custom_firmware** — AK3918AV130 reverse engineering, `ak_rtsp`, firmware build and verified flash tooling  
  <https://github.com/FluxAlchemist/LSC_AK3918AV130_cam_custom_firmware>
- **OpenIPC / SmolRTSP** — RTSP library used by `ak_rtsp`  
  <https://github.com/OpenIPC/smolrtsp>

No stock LSC/Tuya firmware is distributed by this repository. The custom APP image is built locally from firmware dumped from the user's own camera.

---

## ⚖️ Disclaimer

Flashing embedded devices always carries risk. This project comes with no warranty. Unexpected hardware revisions, power loss, broken SD cards or software changes can potentially brick a camera.

The installer includes model/layout checks, a full stock backup and read-back verification to reduce that risk, but you use it at your own responsibility.

---

## 📄 License

The wizard/orchestration code is released under the **MIT License** (Copyright (c) 2026 LSC 3215672.2
Decloud Wizard contributors, `LICENSE` in this directory); the changes in this copy are under the same
license. Downloaded upstream projects remain subject to their own licenses.
