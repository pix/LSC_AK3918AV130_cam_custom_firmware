#!/usr/bin/env bash
set -Eeuo pipefail
# LSC 3215672.2 Decloud Wizard
# Beginner-friendly Linux installer for the Action LSC Smart Connect 3215672.2
# This project does NOT redistribute vendor firmware. It builds a custom APP
# image from the user's own camera dump.
#
# Vendored from https://github.com/4ddict/Action-Smart-Connect-Flasher
# (MIT, see LICENSE in this directory), release 0.3.2, commit 3b79b60.
# Changes for this repository (see README.md "Changes in this copy"):
#   - builds this repository's ak_rtsp (the checkout the wizard runs from, or
#     the fork's main branch resolved to an exact commit) instead of the
#     FluxAlchemist upstream pinned at 3250e0f;
#   - the on-the-fly source patches for that upstream revision are replaced by
#     checks: the same fixes are part of this repository's source.
# Debian-derived desktops may omit sbin directories from a regular user's PATH.
# Include them for tool detection without running the whole wizard as root.
# Append them so the user's existing tool choices keep their precedence.
export PATH="${PATH:-/usr/local/bin:/usr/bin:/bin}:/usr/local/sbin:/usr/sbin:/sbin"
VERSION="0.3.2"
ZIG_VERSION="0.14.1"
WORK_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/lsc-3215672-decloud-wizard"
SRC_DIR="$WORK_DIR/sources"
BACKUP_DIR="$WORK_DIR/backups"
BUILD_DIR="$WORK_DIR/build"
MOUNT_DIR="$WORK_DIR/sd"
STATE_FILE="$WORK_DIR/state.env"
RUNTIME_DIR="$WORK_DIR/runtime"
GITHUB_REPO="${LSC_WIZARD_REPO:-pix/LSC_AK3918AV130_cam_custom_firmware}"
GITHUB_BRANCH="${LSC_WIZARD_BRANCH:-main}"
RAW_BASE="https://raw.githubusercontent.com/${GITHUB_REPO}/${GITHUB_BRANCH}/tools/decloud_wizard"
TOOLKIT_REPO="https://github.com/tasarren/lsc-tuya-toolkit.git"
FIRMWARE_REPO="https://github.com/${GITHUB_REPO}.git"
SMOLRTSP_REPO="https://github.com/OpenIPC/smolrtsp.git"

# Known-good upstream revisions, hardware-tested with this wizard release.
# Keep these fixed until a newer upstream revision has been reviewed and tested.
TOOLKIT_COMMIT="396db8a"
SMOLRTSP_COMMIT="8ae3b51"
# The firmware is this repository. Resolved in resolve_firmware_source():
# the local checkout's HEAD when the wizard runs from one, else the commit
# that $GITHUB_BRANCH points to right now (printed, then used as the pin).
FIRMWARE_COMMIT="${LSC_FIRMWARE_COMMIT:-}"
LSC_FIRMWARE_COMMIT_FROM_ENV="${LSC_FIRMWARE_COMMIT:-}"
SD_LABEL="LSC_DECLOUD"
EXPECTED_APP_SIZE=5242880
CAMERA_NAME=""
CAMERA_HOSTNAME=""
CAMERA_BACKUP_DIR=""
CAMERA_BUILD_DIR=""
LATEST_BACKUP_FILE=""
CAMERA_MODEL="3215672.2"
CAMERA_SI="B26101"
AUTOMOUNT_RULE=""
AUTOMOUNT_DISK=""
# When run from a cloned/downloaded repository, use the local helper scripts.
# When launched with the one-line curl command, fetch those tiny helpers into
# the cache directory so the user still only has to run one command.
SCRIPT_SOURCE="${BASH_SOURCE[0]:-}"
if [[ -n "$SCRIPT_SOURCE" && -f "$SCRIPT_SOURCE" && -d "$(cd "$(dirname "$SCRIPT_SOURCE")" 2>/dev/null && pwd)/camera" ]]; then
    SELF_DIR="$(cd "$(dirname "$SCRIPT_SOURCE")" && pwd)"
else
    SELF_DIR="$RUNTIME_DIR"
fi
# ── Appearance ───────────────────────────────────────────────────────────────
if command -v tput >/dev/null 2>&1 && [[ -t 1 ]]; then
    BOLD="$(tput bold)"; DIM="$(tput dim)"; RESET="$(tput sgr0)"
    GREEN="$(tput setaf 2)"; YELLOW="$(tput setaf 3)"; RED="$(tput setaf 1)"
    CYAN="$(tput setaf 6)"; BLUE="$(tput setaf 4)"
else
    BOLD=""; DIM=""; RESET=""; GREEN=""; YELLOW=""; RED=""; CYAN=""; BLUE=""
fi
CHECK="${GREEN}✔${RESET}"
CROSS="${RED}✖${RESET}"
ARROW="${CYAN}➜${RESET}"
WARNICON="${YELLOW}⚠${RESET}"
INFOICON="${BLUE}●${RESET}"
QUESTION="${CYAN}?${RESET}"

say() { printf '%b\n' "$*"; }
info() { say "${INFOICON} $*"; }
ok() { say "${CHECK} $*"; }
warn() { say "${WARNICON} ${YELLOW}$*${RESET}"; }
die() { say "${CROSS} ${RED}$*${RESET}" >&2; exit 1; }
step() { say "${ARROW} ${BOLD}$*${RESET}"; }
pause() { printf '\n%b Press Enter to continue... ' "${ARROW}"; read -r _; }
hr() {
    say "${DIM}────────────────────────────────────────────────────────────${RESET}"
}

section() {
    local num="$1" total="$2" title="$3"
    say ""
    hr
    say "${CYAN}${BOLD}[${num}/${total}] ${title}${RESET}"
    hr
}
ask_menu() {
    # Usage: ask_menu "Prompt" min max
    local prompt="$1" min="$2" max="$3" value
    while true; do
        printf "%b %s" "$QUESTION" "$prompt"
        read -r value
        if [[ "$value" =~ ^[0-9]+$ ]] && (( value >= min && value <= max )); then
            REPLY="$value"
            return 0
        fi
        warn "Please enter a number from $min to $max."
    done
}
ask_yes_no() {
    # Returns 0 for yes and 1 for no. Invalid input is simply asked again.
    local prompt="$1" default="${2:-yes}" answer
    while true; do
        if [[ "$default" == "yes" ]]; then
            printf "%b %s [Y/n]: " "$QUESTION" "$prompt"
        else
            printf "%b %s [y/N]: " "$QUESTION" "$prompt"
        fi
        read -r answer
        answer="${answer,,}"
        if [[ -z "$answer" ]]; then
            [[ "$default" == "yes" ]] && return 0 || return 1
        fi
        case "$answer" in
            y|yes) return 0 ;;
            n|no) return 1 ;;
            *) warn "Please answer yes or no." ;;
        esac
    done
}
quit_cleanly() {
    say ""
    info "Nothing else was changed. You can run the wizard again whenever you are ready."
    exit 0
}

cleanup_mount() {
    local part target

    # First unmount the wizard's own mount point.
    if mountpoint -q "$MOUNT_DIR" 2>/dev/null; then
        sudo umount "$MOUNT_DIR" >/dev/null 2>&1 || true
    fi

    # Then unmount every mount of the selected SD card. Desktop environments
    # may auto-mount the same partition under /run/media/... after reinsertion.
    if [[ -n "${SD_PART:-}" && -b "${SD_PART:-}" ]]; then
        while IFS= read -r target; do
            [[ -n "$target" ]] || continue
            sudo umount "$target" >/dev/null 2>&1 || true
        done < <(findmnt -rn -S "$SD_PART" -o TARGET 2>/dev/null || true)
    fi

    if [[ -n "${SD_DEV:-}" && -b "${SD_DEV:-}" ]]; then
        while read -r part; do
            [[ -b "$part" ]] || continue
            while IFS= read -r target; do
                [[ -n "$target" ]] || continue
                sudo umount "$target" >/dev/null 2>&1 || true
            done < <(findmnt -rn -S "$part" -o TARGET 2>/dev/null || true)
        done < <(lsblk -lnpo NAME,TYPE "$SD_DEV" 2>/dev/null | awk '$2=="part" {print $1}')
    fi
}

prepare_sd_for_removal() {
    sync
    cleanup_mount

    # Refuse to tell the user to remove the card while Linux still has one of
    # its partitions mounted.
    if [[ -n "${SD_DEV:-}" && -b "${SD_DEV:-}" ]]; then
        local part
        while read -r part; do
            if findmnt -rn -S "$part" >/dev/null 2>&1; then
                die "The SD card is still mounted. Close any file-manager window using it and run this step again."
            fi
        done < <(lsblk -lnpo NAME,TYPE "$SD_DEV" 2>/dev/null | awk '$2=="part" {print $1}')
    fi

    # Do not retrigger this still-connected card: that could mount it again
    # immediately before the user removes it. Reinsertion refreshes its state.
    restore_sd_automount
    ok "SD card synced and safely unmounted."
}
cleanup() {
    cleanup_mount || true
    restore_sd_automount refresh || true
}
banner() {
    clear 2>/dev/null || true
    cat <<'BANNER'
╔══════════════════════════════════════════════════════════════╗
║              LSC 3215672.2 DECLOUD WIZARD                  ║
║              Tuya cloud  ➜  Local RTSP                     ║
╚══════════════════════════════════════════════════════════════╝
BANNER
    say "${DIM}Beginner-friendly installer for the Action LSC Smart Connect camera${RESET}"
    say "${DIM}Anyka AK3918AV130 • ak_rtsp • Linux${RESET}"
    say ""
    say "Version ${BOLD}${VERSION}${RESET}"
    say "${DIM}Pinned sources: toolkit ${TOOLKIT_COMMIT} • firmware ${FIRMWARE_COMMIT} • SmolRTSP ${SMOLRTSP_COMMIT}${RESET}"
    say ""
    say "This wizard will guide you through every physical step."
    say "No Telnet commands, cross-compiling knowledge, or MTD knowledge is required."
    say ""
    info "Supported variants: 3215672.2 / SI B26101 or C26101; 3215672.3 / SI D26228."
    info "C26101 and D26228 are community-reported; the exact flash layout is still verified."
}
ensure_wizard_assets() {
    local missing=0 f
    mkdir -p "$SELF_DIR/camera"
    for f in decloud_stage1.sh decloud_flash.sh decloud_update.sh; do
        [[ -s "$SELF_DIR/camera/$f" ]] || missing=1
    done
    (( missing == 0 )) && return 0
    info "Downloading the wizard helper files..."
    command -v curl >/dev/null 2>&1 || die "curl is required for the one-line installer."
    for f in decloud_stage1.sh decloud_flash.sh decloud_update.sh; do
        curl -fsSL --retry 3 "$RAW_BASE/camera/$f" -o "$SELF_DIR/camera/$f" \
            || die "Could not download $f from $GITHUB_REPO."
        chmod +x "$SELF_DIR/camera/$f"
    done
    ok "Wizard helper files downloaded."
}
confirm_model() {
    say ""
    say "Check the sticker on the camera box or camera."
    say ""
    say "  1) 3215672.2 / SI B26101 — original hardware-tested variant"
    say "  2) 3215672.2 / SI C26101 — successful installation reported by a user"
    say "  3) 3215672.3 / SI D26228 — successful installation reported by a user"
    say "  0) Another model / not sure — quit"
    say ""
    ask_menu "Choose the exact label on your camera: " 0 3
    case "$REPLY" in
        1) CAMERA_MODEL="3215672.2"; CAMERA_SI="B26101" ;;
        2) CAMERA_MODEL="3215672.2"; CAMERA_SI="C26101" ;;
        3) CAMERA_MODEL="3215672.3"; CAMERA_SI="D26228" ;;
        0) quit_cleanly ;;
    esac
    ok "Camera label confirmed: $CAMERA_MODEL / SI $CAMERA_SI."
    say "The camera's exact MTD partition layout must also match before flashing."
}
ask_camera_name() {
    say ""
    say "${BOLD}Give this camera a name${RESET}"
    say "This is useful if you install several cameras."
    say "The name is used for backup folders and as the camera's DHCP hostname."
    say "Example: ${BOLD}Front Door${RESET} becomes ${BOLD}front-door${RESET}."
    say ""

    local raw normalized
    while true; do
        printf "Camera name: "
        read -r raw
        [[ -n "${raw//[[:space:]]/}" ]] || { warn "The camera name cannot be empty."; continue; }
        normalized="$(printf '%s' "$raw" | tr '[:upper:]' '[:lower:]' | sed -E 's/[^a-z0-9]+/-/g; s/^-+//; s/-+$//; s/-+/-/g' | cut -c1-63)"
        [[ -n "$normalized" ]] || { warn "Please use at least one letter or number in the camera name."; continue; }
        say "I will use: ${BOLD}$normalized${RESET}"
        if [[ -d "$BACKUP_DIR/$normalized" ]] && find "$BACKUP_DIR/$normalized" -mindepth 1 -maxdepth 1 -type d -print -quit 2>/dev/null | grep -q .; then
            warn "Backups already exist for a camera named '$normalized'."
            say "If this is a different camera, using a unique name will make the backups easier to identify."
            if ! ask_yes_no "Use '$normalized' anyway?" no; then
                continue
            fi
        fi
        if ask_yes_no "Is this name okay?" yes; then
            CAMERA_NAME="$normalized"
            CAMERA_HOSTNAME="$normalized"
            CAMERA_BACKUP_DIR="$BACKUP_DIR/$CAMERA_NAME"
            CAMERA_BUILD_DIR="$BUILD_DIR/$CAMERA_NAME"
            LATEST_BACKUP_FILE="$WORK_DIR/cameras/$CAMERA_NAME/latest_backup_path"
            mkdir -p "$CAMERA_BACKUP_DIR" "$CAMERA_BUILD_DIR" "$(dirname "$LATEST_BACKUP_FILE")"
            ok "Camera name saved as '$CAMERA_NAME'."
            return
        fi
    done
}
have() { command -v "$1" >/dev/null 2>&1; }
zig_release() {
    case "$(uname -m)" in
        x86_64|amd64)
            if [[ "$(getconf LONG_BIT 2>/dev/null || true)" == 32 ]]; then
                ZIG_PLATFORM="x86-linux"
            else
                ZIG_PLATFORM="x86_64-linux"
            fi ;;
        i386|i486|i586|i686|x86) ZIG_PLATFORM="x86-linux" ;;
        aarch64|arm64) ZIG_PLATFORM="aarch64-linux" ;;
        armv7l|armv8l) ZIG_PLATFORM="armv7a-linux" ;;
        riscv64) ZIG_PLATFORM="riscv64-linux" ;;
        *) return 1 ;;
    esac
    # SHA-256 values published at https://ziglang.org/download/index.json.
    case "$ZIG_PLATFORM" in
        x86_64-linux) ZIG_SHA256="24aeeec8af16c381934a6cd7d95c807a8cb2cf7df9fa40d359aa884195c4716c" ;;
        x86-linux) ZIG_SHA256="4bce6347fa112247443cb0952c19e560d1f90b910506cf895fd07a7b8d1c4a76" ;;
        aarch64-linux) ZIG_SHA256="f7a654acc967864f7a050ddacfaa778c7504a0eca8d2b678839c21eea47c992b" ;;
        armv7a-linux) ZIG_SHA256="1b34d9ecfaeb3b360e86c0bc233e1a8a2bbed2d40f2d4f20c12bde2128714324" ;;
        riscv64-linux) ZIG_SHA256="005f214f74dbafb7b4d8bd305f4e9d25048f711d9ec6fa7b3d4fca177e11b882" ;;
    esac
    ZIG_INSTALL_DIR="$WORK_DIR/tools/zig-$ZIG_VERSION-$ZIG_PLATFORM"
}
activate_cached_zig() {
    zig_release || return 1
    [[ -x "$ZIG_INSTALL_DIR/zig" ]] || return 1
    [[ "$("$ZIG_INSTALL_DIR/zig" version 2>/dev/null || true)" == "$ZIG_VERSION" ]] || return 1
    export PATH="$ZIG_INSTALL_DIR:$PATH"
}
ensure_zig() {
    if have zig; then
        zig version >/dev/null 2>&1 || die "The installed Zig cannot run on this computer."
        return
    fi
    activate_cached_zig && return 0
    # No system Zig: ask rather than download silently (change in this copy).
    warn "Zig (the C cross-compiler used to build the camera firmware) is not installed."
    say "Install it with your package manager, for example:"
    say "  Arch / CachyOS / Manjaro: ${BOLD}sudo pacman -S zig${RESET}"
    say "  Fedora:                   ${BOLD}sudo dnf install zig${RESET}"
    say "  Debian / Ubuntu:          ${BOLD}sudo snap install zig --classic --beta${RESET}  (no apt package)"
    say "  Any Linux:                official archive from ${BOLD}https://ziglang.org/download/${RESET}"
    say "Or let the wizard download the official Zig $ZIG_VERSION into its own cache"
    say "(SHA-256 checked, nothing installed system-wide, about 50 MB)."
    say ""
    say "  1) I will install Zig myself (quit now, run the wizard again afterwards)"
    say "  2) Download the official Zig $ZIG_VERSION for the wizard"
    ask_menu "Choose: " 1 2
    [[ "$REPLY" == 2 ]] || { say "Install Zig, then start the wizard again."; quit_cleanly; }
    zig_release || die "No automatic Zig download is configured for $(uname -m). Install Zig manually."
    info "Downloading official Zig $ZIG_VERSION for $ZIG_PLATFORM (about 50 MB)..."
    python3 - "$ZIG_INSTALL_DIR" "$ZIG_VERSION" "$ZIG_PLATFORM" "$ZIG_SHA256" <<'PY_ZIG' || die "Official Zig installation failed. Check the download error above and run the wizard again."
from pathlib import Path
import hashlib
import shutil
import subprocess
import sys
import tempfile

target = Path(sys.argv[1])
version, platform, expected = sys.argv[2:]
url = f"https://ziglang.org/download/{version}/zig-{platform}-{version}.tar.xz"
target.parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix=".zig-download-", dir=target.parent) as work:
    work = Path(work)
    archive = work / "zig.tar.xz"
    subprocess.run(["curl", "-fL", "--retry", "3", "--connect-timeout", "15",
                    "--max-time", "600", url, "-o", str(archive)], check=True)
    digest = hashlib.sha256()
    with archive.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    if digest.hexdigest() != expected:
        raise SystemExit("ERROR: Zig checksum mismatch; nothing was installed.")
    extracted = work / "extracted"
    extracted.mkdir()
    subprocess.run(["tar", "-xJf", str(archive), "--no-same-owner", "--strip-components=1",
                    "-C", str(extracted)], check=True)
    actual = subprocess.check_output([str(extracted / "zig"), "version"], text=True).strip()
    if actual != version:
        raise SystemExit(f"ERROR: Zig reported {actual}, expected {version}.")
    # Replace only this wizard-owned, versioned compiler cache, after validation.
    if target.is_symlink():
        raise SystemExit("ERROR: Zig cache is a symlink; refusing to replace it.")
    if target.exists():
        shutil.rmtree(target)
    extracted.rename(target)
PY_ZIG
    activate_cached_zig || die "The downloaded Zig could not be activated."
    ok "Verified Zig $ZIG_VERSION is ready."
}
install_dependencies() {
    local missing=() c need_packages=0
    if have zig; then
        zig version >/dev/null 2>&1 || die "The installed Zig cannot run on this computer."
    else
        activate_cached_zig || true
    fi
    local cmds=(curl git cmake make zig python3 unsquashfs mksquashfs mkfs.vfat sfdisk lsblk findmnt expect telnet file tar xz)
    for c in "${cmds[@]}"; do have "$c" || missing+=("$c"); done
    if ((${#missing[@]} == 0)); then
        ok "All required tools are installed."
        return
    fi
    warn "Missing tools: ${missing[*]}"
    say "The wizard can try to install the required packages."
    if ! ask_yes_no "Install dependencies now?" yes; then
        say "The wizard cannot continue without these tools."
        quit_cleanly
    fi
    for c in "${missing[@]}"; do [[ "$c" == zig ]] || need_packages=1; done
    local native_zig=() native_packages=()
    have zig || native_zig=(zig)
    if have pacman; then
        native_packages=(curl git cmake make python squashfs-tools dosfstools util-linux expect inetutils file tar xz)
        if ! sudo pacman -S --needed --noconfirm "${native_packages[@]}" "${native_zig[@]}"; then
            ((${#native_zig[@]})) || die "Could not install Arch/CachyOS dependencies."
            warn "Package installation with Zig failed; retrying the other tools (Zig is handled next)."
            sudo pacman -S --needed --noconfirm "${native_packages[@]}"
        fi
    elif have apt-get; then
        if (( need_packages )); then
            sudo apt-get update
            # Debian/Ubuntu package sfdisk separately in fdisk, not util-linux.
            # Telnet package naming differs slightly between releases.
            sudo apt-get install -y curl git cmake make python3 squashfs-tools dosfstools util-linux fdisk expect telnet file tar xz-utils || \
            sudo apt-get install -y curl git cmake make python3 squashfs-tools dosfstools util-linux fdisk expect inetutils-telnet file tar xz-utils
        fi
    elif have dnf; then
        native_packages=(curl git cmake make python3 squashfs-tools dosfstools util-linux expect telnet file tar xz)
        if ! sudo dnf install -y "${native_packages[@]}" "${native_zig[@]}"; then
            ((${#native_zig[@]})) || die "Could not install Fedora dependencies."
            warn "Package installation with Zig failed; retrying the other tools (Zig is handled next)."
            sudo dnf install -y "${native_packages[@]}"
        fi
    elif (( need_packages == 0 )); then
        : # Only Zig is missing; its official download needs no package manager.
    else
        die "Unsupported package manager. Install the missing tools manually: ${missing[*]}"
    fi
    ensure_zig
    for c in "${cmds[@]}"; do have "$c" || die "Required command '$c' is still missing."; done
    ok "Dependencies installed."
}

checkout_pinned_repo() {
    # Usage: checkout_pinned_repo NAME URL DIR BRANCH COMMIT
    local name="$1" url="$2" dir="$3" branch="$4" commit="$5"
    local actual shallow

    if [[ -d "$dir/.git" ]]; then
        git -C "$dir" remote set-url origin "$url" >/dev/null 2>&1 || true

        # Throw away previous build patches/artifacts in this disposable cache.
        git -C "$dir" reset --hard --quiet
        git -C "$dir" clean -fdx --quiet

        # Old wizard versions used --depth 1. If the pinned object is not in
        # that shallow cache, expand it once so the historical pin is reachable.
        if ! git -C "$dir" cat-file -e "${commit}^{commit}" 2>/dev/null; then
            shallow="$(git -C "$dir" rev-parse --is-shallow-repository 2>/dev/null || echo false)"
            if [[ "$shallow" == "true" ]]; then
                info "Expanding old shallow cache for $name..."
                git -C "$dir" fetch --quiet --unshallow origin "$branch" \
                    || die "Could not fetch full history for pinned $name source."
            else
                git -C "$dir" fetch --quiet origin "$branch" \
                    || die "Could not fetch pinned $name source."
            fi
        fi
    else
        rm -rf "$dir"
        info "Cloning pinned $name source..."
        # Full history is intentional: it makes the abbreviated known-good
        # commit ID resolvable even after upstream moves its default branch.
        git clone --quiet "$url" "$dir" \
            || die "Could not clone $name."
    fi

    # The pin must exist locally. Never silently fall back to branch HEAD.
    git -C "$dir" cat-file -e "${commit}^{commit}" 2>/dev/null \
        || die "Pinned $name commit $commit is not available."

    git -C "$dir" checkout --detach --quiet "$commit" \
        || die "Could not check out pinned $name commit $commit."
    git -C "$dir" reset --hard --quiet "$commit"
    git -C "$dir" clean -fdx --quiet

    actual="$(git -C "$dir" rev-parse HEAD)"
    [[ "$actual" == "$commit"* ]] \
        || die "Pinned $name verification failed: expected $commit, got $actual."
    actual="${actual:0:${#commit}}"

    ok "$name pinned at $actual."
}

resolve_firmware_source() {
    local root
    root="$(git -C "$SELF_DIR" rev-parse --show-toplevel 2>/dev/null || true)"
    if [[ -z "$LSC_FIRMWARE_COMMIT_FROM_ENV" && -n "$root" && -f "$root/src/ak_rtsp/Makefile" \
          && -f "$root/tools/decloud_wizard/install.sh" ]]; then
        if [[ -n "$(git -C "$root" status --porcelain -- src tools 2>/dev/null)" ]]; then
            warn "Your checkout has uncommitted changes in src/ or tools/; they are NOT built (only committed code is)."
        fi
        FIRMWARE_REPO="$root"
        FIRMWARE_BRANCH="HEAD"           # fetchable from a local repository on any branch
        FIRMWARE_COMMIT="$(git -C "$root" rev-parse --short=12 HEAD)"
        ok "Firmware source: this checkout ($root), commit $FIRMWARE_COMMIT."
        return
    fi
    if [[ -z "$FIRMWARE_COMMIT" ]]; then
        FIRMWARE_COMMIT="$(git ls-remote "$FIRMWARE_REPO" "refs/heads/$GITHUB_BRANCH" 2>/dev/null | cut -c1-12)"
        [[ -n "$FIRMWARE_COMMIT" ]] || die "Could not resolve $GITHUB_BRANCH of $FIRMWARE_REPO."
    fi
    ok "Firmware source: $FIRMWARE_REPO, commit $FIRMWARE_COMMIT."
}

update_sources() {
    info "Preparing hardware-tested pinned upstream sources..."
    resolve_firmware_source

    checkout_pinned_repo \
        "LSC toolkit" \
        "$TOOLKIT_REPO" \
        "$SRC_DIR/toolkit" \
        "main" \
        "$TOOLKIT_COMMIT"

    checkout_pinned_repo \
        "AK3918 custom firmware" \
        "$FIRMWARE_REPO" \
        "$SRC_DIR/firmware" \
        "${FIRMWARE_BRANCH:-$GITHUB_BRANCH}" \
        "$FIRMWARE_COMMIT"

    # SmolRTSP lives inside the firmware workspace because ak_rtsp's Makefile
    # expects ../smolrtsp relative to src/ak_rtsp.
    checkout_pinned_repo \
        "SmolRTSP" \
        "$SMOLRTSP_REPO" \
        "$SRC_DIR/firmware/src/smolrtsp" \
        "master" \
        "$SMOLRTSP_COMMIT"

    local required=(
        "$SRC_DIR/toolkit/sd_card/hack.sh"
        "$SRC_DIR/toolkit/sd_card/custom/scripts/entrypoint.sh"
        "$SRC_DIR/toolkit/sd_card/custom/configs/wifi.conf"
        "$SRC_DIR/firmware/src/ak_rtsp/Makefile"
        "$SRC_DIR/firmware/src/ak_rtsp/build_firmware.sh"
        "$SRC_DIR/firmware/src/ak_rtsp/arm_atomics.S"
        "$SRC_DIR/firmware/src/ak_rtsp/ae.c"
        "$SRC_DIR/firmware/src/ak_rtsp/rtsp.c"
        "$SRC_DIR/firmware/src/ak_rtsp/night.c"
        "$SRC_DIR/firmware/tools/firmware_patch_templates/ak_rtsp_wrapper.sh"
        "$SRC_DIR/firmware/src/smolrtsp/CMakeLists.txt"
    )
    for f in "${required[@]}"; do
        [[ -f "$f" ]] || die "Pinned upstream layout is incomplete; missing: $f"
    done

    ok "All pinned upstream sources verified."
}

show_disks() {
    say ""
    say "Storage devices currently visible to Linux:"
    lsblk -o NAME,SIZE,TYPE,FSTYPE,LABEL,MOUNTPOINTS,MODEL | sed 's/^/  /'
    say ""
}

root_disk() {
    local rootsrc pk
    rootsrc="$(findmnt -no SOURCE / 2>/dev/null || true)"
    [[ -n "$rootsrc" ]] || return 0
    pk="$(lsblk -no PKNAME "$rootsrc" 2>/dev/null | head -n1 || true)"
    if [[ -n "$pk" ]]; then echo "/dev/$pk"; else echo "$rootsrc"; fi
}
collect_sd_candidates() {
    SD_CANDIDATES=()
    local rd d type bytes rm tran mounts
    rd="$(root_disk)"

    while read -r d type; do
        [[ "$type" == "disk" ]] || continue
        [[ "$d" != "$rd" ]] || continue
        bytes="$(lsblk -bdnro SIZE "$d" 2>/dev/null | head -n1 || echo 0)"
        [[ "${bytes:-0}" =~ ^[0-9]+$ ]] || bytes=0
        (( bytes >= 33554432 )) || continue
        rm="$(lsblk -dnro RM "$d" 2>/dev/null | head -n1 || echo 0)"
        tran="$(lsblk -dnro TRAN "$d" 2>/dev/null | head -n1 || true)"
        mounts="$(lsblk -nrpo MOUNTPOINTS "$d" 2>/dev/null | tr '\n' ' ')"

        if [[ "$rm" == "1" || "$tran" == "usb" || "$tran" == "mmc" || "$mounts" == *"/run/media/"* || "$mounts" == *"/media/"* ]]; then
            SD_CANDIDATES+=("$d")
        fi
    done < <(lsblk -dpno NAME,TYPE)
}
describe_disk() {
    local d="$1" size model mounts
    size="$(lsblk -dnro SIZE "$d" | xargs)"
    model="$(lsblk -dno MODEL "$d" 2>/dev/null | sed 's/^[[:space:]]*//; s/[[:space:]]*$//' || true)"
    mounts="$(lsblk -nrpo MOUNTPOINTS "$d" 2>/dev/null | awk 'NF' | paste -sd ', ' -)"
    printf '%s — %s' "$size" "${model:-removable storage}"
    if [[ -n "$mounts" ]]; then
        printf ' — mounted at %s' "$mounts"
    fi
    return 0
}
choose_sd_device() {
    say ""
    say "${BOLD}Insert the microSD card now${RESET}"
    say ""
    say "1. Put the microSD card in your computer or card reader."
    say "2. Wait a few seconds for Linux to detect it."
    say "3. Close any file-manager window using the card; the wizard will mount it."
    say "   If it is blank or cannot be opened, that is okay; the wizard will format it."
    say "4. Come back to this terminal."
    say ""
    say "You will choose the card from a simple numbered list. You do NOT need to"
    say "understand Linux device names such as /dev/sdc or partitions such as /dev/sdc1."
    pause
    local i choice selected confirm
    while true; do
        collect_sd_candidates
        say ""
        say "${BOLD}Removable storage found:${RESET}"
        if ((${#SD_CANDIDATES[@]} == 0)); then
            warn "No removable SD card was detected."
            say "Make sure the card is inserted, then choose Rescan."
        else
            for i in "${!SD_CANDIDATES[@]}"; do
                printf "  %d) " "$((i+1))"
                describe_disk "${SD_CANDIDATES[$i]}"
                printf '\n'
            done
        fi
        say "  0) Rescan after inserting/remounting the SD card"
        say ""
        printf "Choose the microSD card by number: "
        read -r choice
        if [[ "$choice" == "0" ]]; then
            continue
        fi
        if ! [[ "$choice" =~ ^[0-9]+$ ]] || (( choice < 1 || choice > ${#SD_CANDIDATES[@]} )); then
            warn "That is not one of the available options. Please choose a number from the list."
            continue
        fi
        selected="${SD_CANDIDATES[$((choice-1))]}"
        say ""
        say "Selected: ${BOLD}$(describe_disk "$selected")${RESET}"
        warn "The wizard will ERASE everything currently stored on this card."
        say ""
        say "  1) Yes, this is the correct microSD card — erase it and continue"
        say "  2) No, let me choose a different device"
        say "  0) Quit without erasing anything"
        ask_menu "Choose: " 0 2
        confirm="$REPLY"
        case "$confirm" in
            1) SD_DEV="$selected"; return ;;
            2) continue ;;
            0) quit_cleanly ;;
        esac
    done
}
partition_path() {
    if [[ "$1" =~ [0-9]$ ]]; then printf '%sp1' "$1"; else printf '%s1' "$1"; fi
}
suppress_sd_automount() {
    # A desktop can remount newly-created partitions between sfdisk and mkfs.
    # UDisks honours this temporary udev hint for only the selected SD device.
    local disk name
    disk="${SD_DEV##*/}"
    [[ "$disk" =~ ^[A-Za-z0-9._-]+$ ]] || die "Invalid SD device name."
    if ! have udevadm; then
        warn "udevadm is unavailable. Disable desktop automount if the SD card keeps remounting."
        return 0
    fi
    [[ "$AUTOMOUNT_DISK" != "$disk" || -z "$AUTOMOUNT_RULE" ]] || return 0
    restore_sd_automount refresh
    sudo mkdir -p /run/udev/rules.d
    AUTOMOUNT_RULE="$(sudo mktemp /run/udev/rules.d/99-lsc-decloud-XXXXXX.rules)"
    AUTOMOUNT_DISK="$disk"
    printf 'SUBSYSTEM=="block", KERNEL=="%s", ENV{UDISKS_IGNORE}="1"\nSUBSYSTEM=="block", KERNELS=="%s", ENV{UDISKS_IGNORE}="1"\n' \
        "$disk" "$disk" | sudo tee "$AUTOMOUNT_RULE" >/dev/null
    sudo chmod 644 "$AUTOMOUNT_RULE"
    sudo udevadm control --reload-rules
    while IFS= read -r name; do
        [[ "$name" =~ ^[A-Za-z0-9._-]+$ ]] || die "Invalid SD partition name."
        sudo udevadm trigger --action=change "/sys/class/block/$name"
    done < <(lsblk -lnro KNAME "$SD_DEV")
    sudo udevadm settle
    ok "Desktop automount temporarily suppressed for $SD_DEV."
}
restore_sd_automount() {
    [[ -n "$AUTOMOUNT_RULE" ]] || return 0
    local disk="$AUTOMOUNT_DISK" name
    if ! sudo unlink "$AUTOMOUNT_RULE"; then
        warn "Could not remove the temporary automount rule: $AUTOMOUNT_RULE"
        return 1
    fi
    AUTOMOUNT_RULE=""
    AUTOMOUNT_DISK=""
    sudo udevadm control --reload-rules
    if [[ "${1:-}" == refresh && -d "/sys/class/block/$disk" ]]; then
        while IFS= read -r name; do
            [[ "$name" =~ ^[A-Za-z0-9._-]+$ ]] || continue
            sudo udevadm trigger --action=change "/sys/class/block/$name" || true
        done < <(lsblk -lnro KNAME "/dev/$disk" 2>/dev/null || true)
        sudo udevadm settle || true
    fi
}
ensure_sd_unmounted() {
    local part
    cleanup_mount
    while IFS= read -r part; do
        if findmnt -rn -S "$part" >/dev/null 2>&1; then
            die "$part is still mounted. Close file-manager windows or disable desktop automount, then try again."
        fi
    done < <(lsblk -lnpo NAME "$SD_DEV")
}
format_sd() {
    info "Formatting the SD card as one MBR/FAT32 partition..."
    suppress_sd_automount
    ensure_sd_unmounted
    sudo wipefs -a "$SD_DEV" >/dev/null
    printf 'label: dos\n, , c, *\n' | sudo sfdisk "$SD_DEV" >/dev/null
    sudo partprobe "$SD_DEV" 2>/dev/null || true
    sudo udevadm settle 2>/dev/null || sleep 2
    SD_PART="$(partition_path "$SD_DEV")"
    [[ -b "$SD_PART" ]] || { sleep 2; [[ -b "$SD_PART" ]] || die "Partition $SD_PART did not appear."; }
    ensure_sd_unmounted
    sudo mkfs.vfat -F 32 -n "$SD_LABEL" "$SD_PART" >/dev/null
    mount_sd
    ok "SD card formatted."
}
mount_sd() {
    mkdir -p "$MOUNT_DIR"
    if [[ -z "${SD_PART:-}" || ! -b "${SD_PART:-/nonexistent}" ]]; then
        SD_PART="$(lsblk -rpno NAME,LABEL,TYPE | awk -v l="$SD_LABEL" '$2==l && $3=="part" {print $1; exit}')"
    fi
    [[ -n "${SD_PART:-}" && -b "$SD_PART" ]] || die "Could not find the $SD_LABEL SD partition."
    local parent
    parent="$(lsblk -dnro PKNAME "$SD_PART")"
    [[ "$parent" =~ ^[A-Za-z0-9._-]+$ ]] || die "Could not identify the SD card's parent device."
    SD_DEV="/dev/$parent"
    suppress_sd_automount
    ensure_sd_unmounted
    sudo mount -o uid="$(id -u)",gid="$(id -g)",umask=022 "$SD_PART" "$MOUNT_DIR"
}
wait_for_sd_return() {
    say ""
    while true; do
        say "Put the prepared SD card back into this computer."
        say "The wizard will mount it; close any file-manager window using it."
        pause
        for _ in {1..10}; do
            SD_PART="$(lsblk -rpno NAME,LABEL,TYPE | awk -v l="$SD_LABEL" '$2==l && $3=="part" {print $1; exit}')"
            if [[ -n "$SD_PART" && -b "$SD_PART" ]]; then
                mount_sd
                return 0
            fi
            sleep 1
        done
        warn "I cannot find the prepared SD card yet."
        say "Check that it is inserted, then try again."
        say "  1) Try again"
        say "  0) Quit for now"
        ask_menu "Choose: " 0 1
        [[ "$REPLY" == "1" ]] || quit_cleanly
    done
}
shell_quote() {
    # Produce a POSIX-shell-safe single-quoted value.
    printf "'%s'" "${1//\'/\'\\\'\'}"
}
ask_wifi() {
    say ""
    say "${BOLD}Wi-Fi setup${RESET}"
    say "The camera uses 2.4 GHz Wi-Fi."
    say "Your password is hidden while you type."

    while true; do
        printf "Wi-Fi SSID: "
        read -r WIFI_SSID
        [[ -n "${WIFI_SSID//[[:space:]]/}" ]] && break
        warn "The SSID cannot be empty. Please try again."
    done
    local first second
    while true; do
        printf "Wi-Fi password: "
        read -rs first
        printf '\n'
        if [[ -z "$first" ]]; then
            warn "The Wi-Fi password cannot be empty. Please try again."
            continue
        fi
        printf "Wi-Fi password again: "
        read -rs second
        printf '\n'
        if [[ "$first" != "$second" ]]; then
            warn "The two passwords did not match. Please enter them again."
            continue
        fi
        WIFI_PASS="$first"
        ok "Wi-Fi password confirmed."
        break
    done
}
prepare_stage1_sd() {
    info "Preparing the SD card for safe camera access and backup..."
    rm -rf "$MOUNT_DIR"/* "$MOUNT_DIR"/.[!.]* "$MOUNT_DIR"/..?* 2>/dev/null || true
    cp -a "$SRC_DIR/toolkit/sd_card/." "$MOUNT_DIR/"
    # Skip the toolkit's large filesystem copy. We create a complete raw MTD backup instead.
    mkdir -p "$MOUNT_DIR/custom/state" "$MOUNT_DIR/logs" "$MOUNT_DIR/original_firmware"
    : > "$MOUNT_DIR/custom/state/dump.done"
    rm -f "$MOUNT_DIR/custom/state/shadow.done"

    cat > "$MOUNT_DIR/custom/configs/wifi.conf" <<EOF_WIFI
# Generated by LSC 3215672.2 Decloud Wizard
MODE=sta
SSID=$(shell_quote "$WIFI_SSID")
PASS=$(shell_quote "$WIFI_PASS")
SECURITY=3
DHCP=1
EOF_WIFI
    sed -i -E 's/^TELNET=.*/TELNET=1/; s/^TELNET_PORT=.*/TELNET_PORT=24/; s/^OFFLINE=.*/OFFLINE=1/' "$MOUNT_DIR/custom/configs/hack.conf"

    cp "$SELF_DIR/camera/decloud_stage1.sh" "$MOUNT_DIR/custom/scripts/decloud_stage1.sh"
    cp "$SELF_DIR/camera/decloud_flash.sh" "$MOUNT_DIR/custom/scripts/decloud_flash.sh"
    sed -i "s/MODEL=3215672.2/MODEL=$CAMERA_MODEL/" "$MOUNT_DIR/custom/scripts/decloud_stage1.sh"
    chmod +x "$MOUNT_DIR/custom/scripts/decloud_stage1.sh" "$MOUNT_DIR/custom/scripts/decloud_flash.sh"
    # Make entrypoint idempotent: stock firmware may call it as well as our direct hook.
    if ! grep -q 'DECLOUD_ENTRYPOINT_LOCK' "$MOUNT_DIR/custom/scripts/entrypoint.sh"; then
        sed -i '2i\# DECLOUD_ENTRYPOINT_LOCK\nmkdir /tmp/decloud_entrypoint.lock 2>/dev/null || exit 0' "$MOUNT_DIR/custom/scripts/entrypoint.sh"
    fi
    # The direct hook is important after anyka_ipc is removed: it keeps Wi-Fi + telnet available.
    if ! grep -q 'DECLOUD_WIZARD_HOOK' "$MOUNT_DIR/hack.sh"; then
        cat >> "$MOUNT_DIR/hack.sh" <<'EOF_HOOK'
# DECLOUD_WIZARD_HOOK
# Start our services directly. entrypoint.sh has an atomic once-per-boot lock,
# so this remains safe while the stock anyka_ipc wrapper also exists.
/bin/sh "${SCRIPTS_DIR}/entrypoint.sh" >/dev/null 2>&1 &
/bin/sh "${SCRIPTS_DIR}/decloud_stage1.sh" >/dev/null 2>&1 &
EOF_HOOK
    fi

    sync
    ok "Stage 1 SD card is ready."
}
stage1_physical_instructions() {
    prepare_sd_for_removal
    say ""
    say "${BOLD}STAGE 1 — Back up the camera and save Wi-Fi permanently${RESET}"
    say ""
    say "1. Make sure the camera is unplugged."
    say "2. Remove the SD card from the computer and insert it into the camera."
    say "3. Connect USB-C power to the camera."
    say "4. Leave it powered on for ${BOLD}two full minutes${RESET}."
    say "   The blue LED may keep blinking during this step; that is normal."
    say "   The factory voice prompt is intentionally muted by the wizard, so"
    say "   you will normally ${BOLD}not${RESET} hear the Chinese startup message."
    say "5. After two minutes, unplug USB-C power."
    say "6. Remove the SD card and put it back into this computer."
    wait_for_sd_return
}
verify_stage1() {
    info "Checking the camera backup..."
    if [[ ! -f "$MOUNT_DIR/STAGE1_SUCCESS" ]]; then
        warn "Stage 1 did not finish successfully."
        if [[ -f "$MOUNT_DIR/logs/decloud_stage1.log" ]]; then
            say ""
            say "The camera reported:"
            say "------------------"
            tail -n 80 "$MOUNT_DIR/logs/decloud_stage1.log"
            say "------------------"
        fi
        return 1
    fi
    local sizes=(204800 4096 4096 65536 1572864 1032192 262144 5242880)
    for i in {0..7}; do
        local f="$MOUNT_DIR/original_firmware/mtdblock${i}.bin"
        [[ -f "$f" ]] || die "Backup file missing: $f"
        local got
        got="$(stat -c%s "$f")"
        [[ "$got" -eq "${sizes[$i]}" ]] || die "Wrong size for mtdblock${i}.bin: $got"
    done
    local stamp backup_target
    stamp="$(date +%Y%m%d_%H%M%S)"
    backup_target="$CAMERA_BACKUP_DIR/$stamp"
    mkdir -p "$backup_target"
    cp -a "$MOUNT_DIR/original_firmware/." "$backup_target/"
    printf '%s\n' "$backup_target" > "$LATEST_BACKUP_FILE"
    ok "Full 8 MiB stock backup verified and copied to: $backup_target"
    if [[ -f "$MOUNT_DIR/STAGE1_STATUS" ]]; then
        say ""
        say "Camera information:"
        sed 's/^/  /' "$MOUNT_DIR/STAGE1_STATUS"
        CAMERA_IP="$(awk -F= '$1=="IP" {print $2}' "$MOUNT_DIR/STAGE1_STATUS" | tail -n1)"
    fi
}

run_stage1_until_success() {
    while true; do
        prepare_stage1_sd
        stage1_physical_instructions
        if verify_stage1; then
            return 0
        fi
        say ""
        say "Nothing has been flashed. You can safely try again."
        say "A wrong Wi-Fi SSID/password is the most common reason for this step to fail."
        say ""
        say "  1) Enter the Wi-Fi details again and retry"
        say "  2) Retry with the same Wi-Fi details"
        say "  0) Quit for now"
        ask_menu "Choose: " 0 2
        case "$REPLY" in
            1) ask_wifi ;;
            2) ;;
            0) quit_cleanly ;;
        esac
        # The card is still mounted after verification, so prepare_stage1_sd can
        # rebuild it immediately for the next attempt.
    done
}
patch_native_linux_build() {
    # Upstream 0.3.2 patched the FluxAlchemist source (pinned 3250e0f) here:
    # gen_version.sh, GC20C3 exposure cap, blocking RTSP socket, day/night
    # thresholds and the day/night state machine. All of them are part of
    # this repository's source, so only check that they are still there:
    # never patch this source blindly.
    local akdir="$SRC_DIR/firmware/src/ak_rtsp"
    [[ -x "$akdir/gen_version.sh" ]] && grep -q '\./gen_version\.sh' "$akdir/Makefile" \
        || die "Firmware source check failed: gen_version.sh is not used by the Makefile."
    grep -Eq '\.exp_max[[:space:]]*=[[:space:]]*2([[:space:]]*,)' "$akdir/ae.c" \
        || die "Firmware source check failed: GC20C3 exposure cap (ae.c exp_max = 2) missing."
    grep -Fq 'fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);' "$akdir/rtsp.c" \
        && die "Firmware source check failed: RTSP client socket is non-blocking again (corrupts RTP over TCP)."
    grep -Eq '^#define[[:space:]]+NIGHT_TRIGGER_HW_EXP[[:space:]]+3000([[:space:]]|/\*|$)' "$akdir/night.c" \
        && grep -Eq '^#define[[:space:]]+DAY_TRIGGER_HW_EXP[[:space:]]+1200([[:space:]]|/\*|$)' "$akdir/night.c" \
        || die "Firmware source check failed: day/night thresholds are not 3000/1200."
    grep -Fq 'static unsigned night_get_snapshot(night_tuning_t *out)' "$akdir/night.c" \
        && grep -Fq 'g_mode_generation' "$akdir/night.c" \
        || die "Firmware source check failed: day/night mode-change state machine missing."
    ok "Firmware source has the exposure, RTSP/TCP and day/night fixes."

    # Give each camera a useful DHCP hostname (for example front-door).
    # CAMERA_HOSTNAME is strictly sanitized by ask_camera_name().
    local wrapper="$SRC_DIR/firmware/tools/firmware_patch_templates/ak_rtsp_wrapper.sh"
    sed -i -E "s/hostname:[A-Za-z0-9._-]+/hostname:${CAMERA_HOSTNAME}/" "$wrapper"
    grep -q "hostname:${CAMERA_HOSTNAME} " "$wrapper" || die "Could not set the camera hostname in the wrapper."
}
build_firmware() {
    info "Building the cloud-free RTSP firmware from YOUR camera backup..."
    info "The first Zig build can take several minutes on older computers; later builds can reuse its compiler cache."
    local base_app akdir smol app_extract outdir
    # BUILD_BASE_APP: the APP image to start from (update mode: the stock
    # backup if this computer has it, else the camera's current APP); default:
    # the Stage 1 stock backup.
    base_app="${BUILD_BASE_APP:-$(cat "$LATEST_BACKUP_FILE")/mtdblock7.bin}"
    akdir="$SRC_DIR/firmware/src/ak_rtsp"
    smol="$SRC_DIR/firmware/src/smolrtsp"
    app_extract="$CAMERA_BUILD_DIR/app_extracted"
    outdir="$CAMERA_BUILD_DIR/output"
    [[ -d "$smol/.git" ]] || die "Pinned SmolRTSP checkout is missing."
    [[ "$(git -C "$smol" rev-parse --short=7 HEAD)" == "$SMOLRTSP_COMMIT" ]]         || die "SmolRTSP moved away from pinned commit $SMOLRTSP_COMMIT."
    cmake -S "$smol" -B "$smol/build" >/dev/null

    patch_native_linux_build
    find "$akdir" "$smol" -name '*.o' -delete 2>/dev/null || true
    rm -f "$akdir/ak_rtsp" "$akdir/ak_rtsp_stripped" "$akdir/flash_tool"

    make -C "$akdir" ak_rtsp ak_rtsp_stripped flash_tool 'CC=zig cc -target arm-linux-musleabi -mcpu=arm926ej_s' \
        || die "Cross-compilation failed. See the compiler output above."
    file "$akdir/ak_rtsp" | grep -q 'ARM' || die "ak_rtsp is not an ARM executable."
    file "$akdir/ak_rtsp" | grep -q 'statically linked' || die "ak_rtsp is not statically linked."
    file "$akdir/ak_rtsp_stripped" | grep -q ", stripped" || die "ak_rtsp_stripped is not stripped."
    file "$akdir/flash_tool" | grep -q 'ARM' || die "flash_tool is not an ARM executable."
    file "$akdir/flash_tool" | grep -q 'statically linked' || die "flash_tool is not statically linked."
    rm -rf "$app_extract" "$outdir"
    mkdir -p "$outdir" #removed "$app_extract" since unsquashfs doesn't override existing dirs
    unsquashfs -d "$app_extract" "$base_app" >/dev/null \
        || die "Could not unpack the APP partition image $base_app."

    APP_EXTRACTED="$app_extract" FW_DIR="$outdir" bash "$akdir/build_firmware.sh" \
        || die "Firmware packaging failed."
    local fw
    fw="$(find "$outdir" -maxdepth 1 -type f -name 'ak_rtsp_firmware_*.squashfs' | sort | tail -n1)"
    [[ -n "$fw" && -f "$fw" ]] || die "No firmware image was produced."
    [[ "$(stat -c%s "$fw")" -eq "$EXPECTED_APP_SIZE" ]] || die "Firmware image has the wrong size."
    BUILT_FIRMWARE="$fw"
    if [[ -z "${BUILD_NO_SD:-}" ]]; then           # update mode sends it over the LAN instead
        mkdir -p "$MOUNT_DIR/custom_firmware"
        rm -f "$MOUNT_DIR/custom_firmware/ak_rtsp_firmware_"*.squashfs
        cp "$fw" "$MOUNT_DIR/custom_firmware/"
        cp "$akdir/flash_tool" "$MOUNT_DIR/flash_tool"
        chmod +x "$MOUNT_DIR/flash_tool"
        sync
    fi

    FIRMWARE_NAME="$(basename "$fw")"
    ok "Firmware built and verified: $FIRMWARE_NAME"
}
stage2_physical_instructions() {
    prepare_sd_for_removal
    say ""
    say "${BOLD}STAGE 2 — Flash the cloud-free firmware${RESET}"
    say ""
    say "1. Make sure the camera is unplugged."
    say "2. Insert the prepared SD card into the camera."
    say "3. Connect USB-C power."
    say "4. Wait about 45 seconds for the camera to join Wi-Fi."
    say ""
    say "The wizard will connect to the camera on the special local service port"
    say "and show the complete flash/verification output on this screen."
    pause
}
check_tcp() {
    local host="$1" port="$2"
    timeout 2 bash -c "</dev/tcp/$host/$port" >/dev/null 2>&1
}
ask_camera_ip() {
    local default_ip="${CAMERA_IP:-}"
    while true; do
        if [[ -n "$default_ip" ]]; then
            printf "Camera IP [%s]: " "$default_ip"
            read -r ip
            ip="${ip:-$default_ip}"
        else
            printf "Camera IP (check your router's DHCP/client list): "
            read -r ip
        fi
        if [[ "$ip" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]] && check_tcp "$ip" 24; then
            CAMERA_IP="$ip"
            ok "Camera found at $CAMERA_IP; maintenance port 24 is open."
            return
        fi
        warn "I cannot reach port 24 at '$ip'. Check the IP in your router and try again."
        default_ip=""
    done
}
flash_over_telnet() {
    say ""
    warn "The next step writes the APP partition. Do not unplug the camera while flashing."
    say "The script verifies the exact partition layout and checks that the camera's"
    say "current APP partition matches the stock backup before it writes anything."
    say ""
    say "  1) Flash the verified custom firmware now"
    say "  2) Not yet — show this confirmation again"
    say "  0) Quit without flashing"
    while true; do
        ask_menu "Choose: " 0 2
        confirm="$REPLY"
        case "$confirm" in
            1) break ;;
            2)
                say "Nothing has been written yet. When you are ready:"
                say "  1) Flash the verified custom firmware now"
                say "  2) Not yet"
                say "  0) Quit without flashing"
                ;;
            0) quit_cleanly ;;
        esac
    done
    info "Connecting to $CAMERA_IP and flashing. This can take a minute..."
    export CAMERA_IP
    set +e
    expect <<'EOF_EXPECT'
set timeout 300
log_user 1
spawn telnet $env(CAMERA_IP) 24
expect {
    -re {Connected|Escape character|[#\$] ?$} {}
    timeout { puts "\nERROR: Telnet connection timed out."; exit 10 }
    eof { puts "\nERROR: Telnet connection closed."; exit 11 }
}
after 1000
send -- "sh /tmp/sd/custom/scripts/decloud_flash.sh\r"
expect {
    "DECLOUD_FLASH_SUCCESS" { puts "\nWizard detected successful flash + verification."; send -- "exit\r"; exit 0 }
    "DECLOUD_RESTORE_SUCCESS" { puts "\nCustom flash failed, but stock APP was restored successfully. DO NOT continue as if flashed."; send -- "exit\r"; exit 20 }
    "DECLOUD_FLASH_FAILED" { puts "\nFlash/verification failed. Keep the camera powered and review the output."; exit 21 }
    timeout { puts "\nERROR: Timed out waiting for flash verification. KEEP CAMERA POWERED."; exit 22 }
    eof { puts "\nERROR: Connection closed before success was confirmed. KEEP CAMERA POWERED."; exit 23 }
}
EOF_EXPECT
    local rc=$?
    set -e
    case "$rc" in
        0) ok "Flash and per-block verification completed successfully." ;;
        20) die "Stock APP was restored. Camera is safe, but custom firmware was not installed." ;;
        *) die "Flashing was not confirmed successful. KEEP THE CAMERA POWERED until you understand the failure." ;;
    esac
    # remembered for ./install.sh --update
    md5sum "$MOUNT_DIR/custom_firmware/$FIRMWARE_NAME" 2>/dev/null | cut -c1-32 > "$WORK_DIR/cameras/$CAMERA_NAME/installed_md5" || true
    printf '%s\n' "$CAMERA_IP" > "$WORK_DIR/cameras/$CAMERA_NAME/ip" || true
}
final_boot() {
    say ""
    say "${BOLD}FINAL BOOT${RESET}"
    say ""
    say "1. Unplug USB-C power now."
    say "2. Remove the SD card from the camera."
    say "3. Wait 5 seconds."
    say "4. Plug USB-C power back in WITHOUT the SD card."
    say "5. Wait about 45 seconds."
    pause
    local ip="$CAMERA_IP"
    if check_tcp "$ip" 554; then
        ok "RTSP port 554 is reachable at $ip."
    else
        warn "RTSP was not found at $ip. DHCP may have assigned a different address."
        printf "Enter the camera's current IP from your router (or press Enter to skip): "
        read -r newip
        if [[ -n "$newip" ]] && check_tcp "$newip" 554; then
            CAMERA_IP="$newip"
            ok "RTSP port 554 is reachable at $CAMERA_IP."
        else
            warn "I could not automatically verify RTSP. The flash itself was verified successfully."
        fi
    fi
    say ""
    say "${GREEN}${BOLD}Done.${RESET} Camera ${BOLD}$CAMERA_NAME${RESET} is now running ak_rtsp instead of the Tuya anyka_ipc APP."
    say ""
    say "Camera name: ${BOLD}$CAMERA_NAME${RESET}"
    say "DHCP hostname: ${BOLD}$CAMERA_HOSTNAME${RESET}"
    say "RTSP URL: ${BOLD}rtsp://${CAMERA_IP}:554/${RESET}"
    say "Alternative path: rtsp://${CAMERA_IP}:554/main_ch"
    say ""
    say "Live settings: day/night, exposure and image adjustments are available on TCP port 8091."
    say "See 'Live camera settings' in the GitHub README for commands; no reflash is needed."
    say ""
    say "Recommended next steps:"
    say "  • Create a DHCP reservation for the camera in your router."
    say "  • Put the camera in an isolated camera/IoT VLAN if you use one."
    say "  • After you are happy with the result, reformat the SD card: it still"
    say "    contains your Wi-Fi credentials and your private stock firmware backup."
    say "  • Keep the backup stored at: $(cat "$LATEST_BACKUP_FILE")"
}
# ── Update an already decloud camera (./install.sh --update) ────────────────
# The camera must offer the maintenance shell on port 24: an SD card with the
# toolkit's hack.sh in the camera (the card from the first installation, or a
# new maintenance card made here). Nothing in Stage 1 / Wi-Fi is redone: the
# firmware keeps its own Wi-Fi and settings (CONFIG partition, not rewritten).
UPDATE_DIR_CAM=/tmp/sd/update
cam_exec() {
    # Run one command in the camera's maintenance shell; prints its output,
    # returns its exit status (90+: no shell).
    local cmd="$1" t="${2:-60}"
    case "$cmd" in *'&') cmd="$cmd :" ;; esac          # "cmd & ; echo" is a syntax error
    CAMERA_IP="$CAMERA_IP" CAM_CMD="$cmd" CAM_T="$t" expect <<'EOF_EXPECT' | tr -d '\r'
set timeout $env(CAM_T)
log_user 0
match_max 200000
spawn -noecho telnet $env(CAMERA_IP) 24
expect {
    -re {[#$] $} {}
    timeout { puts stderr "No answer from the camera's maintenance port."; exit 90 }
    eof { puts stderr "The camera closed the maintenance connection."; exit 91 }
}
send -- "echo __OUT_B__; $env(CAM_CMD); echo __OUT_E_\$?__\r"
expect {
    -re {__OUT_B__\r\n(.*?)__OUT_E_([0-9]+)__} {
        set out $expect_out(1,string)
        set rc $expect_out(2,string)
    }
    timeout { puts stderr "The camera did not finish the command in time."; exit 92 }
    eof { puts stderr "The camera closed the maintenance connection."; exit 91 }
}
send -- "exit\r"
puts -nonewline $out
exit $rc
EOF_EXPECT
    return "${PIPESTATUS[0]}"
}
cam_fetch_app() {
    # Copy the camera's current APP partition to $1 over the LAN; verify it.
    local out="$1" cur got
    cur="$(cam_exec 'md5sum /dev/mtd7 | cut -c1-32')" || die "Could not read the camera's APP checksum."
    cur="$(printf '%s' "$cur" | tr -dc '0-9a-f')"
    [[ ${#cur} -eq 32 ]] || die "Unexpected answer from the camera: '$cur'."
    cam_exec 'pkill -x nc 2>/dev/null; setsid sh -c "dd if=/dev/mtd7 bs=64k 2>/dev/null | nc -l -p 9187" </dev/null >/dev/null 2>&1 &' >/dev/null \
        || die "Could not start the transfer on the camera."
    sleep 1
    python3 -I - "$CAMERA_IP" 9187 "$out" <<'EOF_PY' || die "Could not copy the camera's APP partition."
import socket, sys, time
host, port, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
for i in range(20):
    try:
        s = socket.create_connection((host, port), timeout=15); break
    except OSError:
        time.sleep(0.5)
else:
    sys.exit("cannot connect to the camera")
with open(out, "wb") as f:
    while True:
        d = s.recv(65536)
        if not d: break
        f.write(d)
EOF_PY
    got="$(md5sum "$out" | cut -c1-32)"
    [[ "$(stat -c%s "$out")" -eq "$EXPECTED_APP_SIZE" && "$got" == "$cur" ]] \
        || die "The copy of the camera's APP is incomplete (checksum $got, camera $cur). Nothing was changed."
    CURRENT_APP_MD5="$cur"
}
cam_send() {
    # Send local file $1 to the camera as $2 (on its SD card) over the LAN; verify it.
    local src="$1" dst="$2" want got size
    want="$(md5sum "$src" | cut -c1-32)"
    size="$(stat -c%s "$src")"
    for _ in 1 2 3; do
        cam_exec "pkill -x nc 2>/dev/null; rm -f '$dst'; setsid sh -c \"nc -l -p 9188 > '$dst'\" </dev/null >/dev/null 2>&1 &" >/dev/null \
            || die "Could not start the transfer on the camera."
        sleep 1
        # Send in chunks: a socket timeout bounds a whole sendall(), so one
        # 5 MiB sendall() with a 15 s timeout fails on links under ~350 KB/s.
        python3 -I - "$CAMERA_IP" 9188 "$src" <<'EOF_PY' || true
import socket, sys, time
host, port, src = sys.argv[1], int(sys.argv[2]), sys.argv[3]
for i in range(20):
    try:
        s = socket.create_connection((host, port), timeout=15); break
    except OSError:
        time.sleep(0.5)
else:
    sys.exit("cannot connect to the camera")
s.settimeout(60)
try:
    with open(src, "rb") as f:
        while True:
            b = f.read(32768)
            if not b: break
            s.sendall(b)
    s.shutdown(socket.SHUT_WR)
    s.settimeout(120)
    s.recv(1)
except OSError as e:
    sys.exit("transfer to the camera interrupted: %s" % e)
s.close()
EOF_PY
        # Let the camera's nc finish writing before checking the file.
        got="$(cam_exec "i=0; while pidof nc >/dev/null && [ \$i -lt 60 ]; do sleep 1; i=\$((i+1)); done; sync; md5sum '$dst' | cut -c1-32" 90 | tr -dc '0-9a-f')" || true
        [[ "$got" == "$want" ]] && return 0
        warn "The copy of $(basename "$src") on the camera did not verify ($(cam_exec "wc -c < '$dst'; df -k /tmp/sd | tail -n1" | tr '\n' ' ' | tr -s ' ') of $size bytes); retrying."
    done
    die "Could not copy $(basename "$src") to the camera. Nothing was changed."
}
choose_update_camera() {
    say ""
    say "${BOLD}Which camera do you want to update?${RESET}"
    local names=() d i
    for d in "$WORK_DIR"/cameras/*/; do
        [[ -d "$d" ]] && names+=("$(basename "$d")")
    done
    if (( ${#names[@]} )); then
        say "Cameras installed or updated from this computer:"
        for i in "${!names[@]}"; do
            say "  $((i + 1))) ${names[$i]}"
        done
        say "  $(( ${#names[@]} + 1 ))) Another camera (installed elsewhere)"
        say "  0) Quit"
        ask_menu "Choose: " 0 $(( ${#names[@]} + 1 ))
        [[ "$REPLY" == 0 ]] && quit_cleanly
        if (( REPLY <= ${#names[@]} )); then
            CAMERA_NAME="${names[$((REPLY - 1))]}"
            CAMERA_HOSTNAME="$CAMERA_NAME"
            CAMERA_BACKUP_DIR="$BACKUP_DIR/$CAMERA_NAME"
            CAMERA_BUILD_DIR="$BUILD_DIR/$CAMERA_NAME"
            LATEST_BACKUP_FILE="$WORK_DIR/cameras/$CAMERA_NAME/latest_backup_path"
            mkdir -p "$CAMERA_BACKUP_DIR" "$CAMERA_BUILD_DIR"
            [[ -f "$WORK_DIR/cameras/$CAMERA_NAME/ip" ]] && CAMERA_IP="$(cat "$WORK_DIR/cameras/$CAMERA_NAME/ip")"
            ok "Updating '$CAMERA_NAME'."
            return
        fi
    fi
    ask_camera_name
}
prepare_maintenance_sd() {
    # A card that only opens the maintenance shell (Telnet port 24): no Wi-Fi
    # change (the firmware keeps its own), no backup stage, no vendor overrides.
    info "Preparing the maintenance SD card..."
    rm -rf "${MOUNT_DIR:?}"/* "${MOUNT_DIR:?}"/.[!.]* "${MOUNT_DIR:?}"/..?* 2>/dev/null || true
    cp -a "$SRC_DIR/toolkit/sd_card/." "$MOUNT_DIR/"
    mkdir -p "$MOUNT_DIR/custom/state" "$MOUNT_DIR/logs"
    : > "$MOUNT_DIR/custom/state/dump.done"            # no filesystem copy
    printf '# Generated by the Decloud Wizard (update): Wi-Fi is left as it is\nMODE=none\n' > "$MOUNT_DIR/custom/configs/wifi.conf"
    sed -i -E 's/^TELNET=.*/TELNET=1/; s/^TELNET_PORT=.*/TELNET_PORT=24/; s/^ONVIF=.*/ONVIF=0/; s/^FTP=.*/FTP=0/;
               s/^OVERRIDE_HW_SETTINGS=.*/OVERRIDE_HW_SETTINGS=0/; s/^OVERRIDE_SW_SETTINGS=.*/OVERRIDE_SW_SETTINGS=0/' \
        "$MOUNT_DIR/custom/configs/hack.conf"
    if ! grep -q 'DECLOUD_ENTRYPOINT_LOCK' "$MOUNT_DIR/custom/scripts/entrypoint.sh"; then
        sed -i '2i\# DECLOUD_ENTRYPOINT_LOCK\nmkdir /tmp/decloud_entrypoint.lock 2>/dev/null || exit 0' "$MOUNT_DIR/custom/scripts/entrypoint.sh"
    fi
    if ! grep -q 'DECLOUD_WIZARD_HOOK' "$MOUNT_DIR/hack.sh"; then
        # hack.sh does not end with a newline: start a new line, or the hook joins its last command
        printf '\n' >> "$MOUNT_DIR/hack.sh"
        cat >> "$MOUNT_DIR/hack.sh" <<'EOF_HOOK'
# DECLOUD_WIZARD_HOOK - maintenance card: telnet on port 24 only
/bin/sh "${SCRIPTS_DIR}/entrypoint.sh" >/dev/null 2>&1 &
EOF_HOOK
    fi
    sync
    ok "Maintenance SD card is ready."
}
ask_maintenance_access() {
    say ""
    say "${BOLD}Maintenance access${RESET}"
    say "The update runs through the camera's maintenance port (Telnet 24), which is open"
    say "while a maintenance SD card is in the camera."
    say ""
    say "  1) The camera already has a maintenance card in (for example the card from the"
    say "     first installation) and is running"
    say "  2) Make a new maintenance SD card now (the card will be ERASED)"
    say "  0) Quit"
    ask_menu "Choose: " 0 2
    case "$REPLY" in
        0) quit_cleanly ;;
        2)
            choose_sd_device
            format_sd
            prepare_maintenance_sd
            prepare_sd_for_removal
            say ""
            say "1. Unplug the camera's USB-C power."
            say "2. Insert the maintenance SD card into the camera."
            say "3. Plug the power back in and wait about 45 seconds."
            pause
            ;;
    esac
    ask_camera_ip
    printf '%s\n' "$CAMERA_IP" > "$WORK_DIR/cameras/$CAMERA_NAME/ip"
}
read_current_firmware() {
    info "Reading the firmware currently on the camera (5 MiB over the LAN)..."
    local rc=0
    cam_exec 'grep -q "^mtd7: 00500000 00001000 \"APP\"$" /proc/mtd && grep -q "^mtd6: 00040000 00001000 \"CONFIG\"$" /proc/mtd' >/dev/null || rc=$?
    (( rc < 90 )) || die "Cannot use the camera's maintenance shell on port 24. Nothing was changed."
    (( rc == 0 )) || die "The camera's flash layout is not the supported one. Nothing was changed."
    local stamp dir
    stamp="$(date +%Y%m%d_%H%M%S)"
    dir="$CAMERA_BACKUP_DIR/before_update_$stamp"
    mkdir -p "$dir"
    cam_fetch_app "$dir/mtdblock7.bin"
    CURRENT_APP_FILE="$dir/mtdblock7.bin"
    ok "Current firmware copied and verified (MD5 $CURRENT_APP_MD5): $dir"
    local x="$CAMERA_BUILD_DIR/current_app_check"
    rm -rf "$x"
    unsquashfs -d "$x" "$CURRENT_APP_FILE" >/dev/null || die "The camera's APP partition is not a readable squashfs. Nothing was changed."
    if [[ -f "$x/bin/anyka_ipc" || ! -f "$x/bin/ak_rtsp" ]]; then
        rm -rf "$x"
        die "This camera still runs the stock firmware: use the full wizard (./install.sh) for the first installation."
    fi
    rm -rf "$x"
    ok "The camera runs an ak_rtsp build: it can be updated."
}
update_flash() {
    local fw="$BUILT_FIRMWARE" fwmd5
    fwmd5="$(md5sum "$fw" | cut -c1-32)"
    info "Sending the new firmware to the camera's SD card (nothing is written to flash yet)..."
    cam_exec "mkdir -p $UPDATE_DIR_CAM" >/dev/null || die "Could not create $UPDATE_DIR_CAM on the camera's SD card."
    cam_send "$SELF_DIR/camera/decloud_update.sh" "$UPDATE_DIR_CAM/decloud_update.sh"
    cam_send "$SRC_DIR/firmware/src/ak_rtsp/flash_tool" "$UPDATE_DIR_CAM/flash_tool"
    cam_send "$fw" "$UPDATE_DIR_CAM/new_app.squashfs"
    ok "New firmware, flash tool and update script are on the camera and verified."
    say ""
    warn "The next step rewrites the APP partition. Do not unplug the camera while it runs."
    say "Your settings (Wi-Fi, login, image, recordings) are kept: they are not in that partition."
    say "The camera keeps a copy of the current firmware on its SD card and restores it"
    say "automatically if anything does not verify."
    say ""
    say "  1) Update the camera now"
    say "  0) Quit without changing anything"
    ask_menu "Choose: " 0 1
    [[ "$REPLY" == 1 ]] || quit_cleanly
    info "Updating. This can take a minute..."
    export CAMERA_IP UPD_CMD="sh $UPDATE_DIR_CAM/decloud_update.sh $UPDATE_DIR_CAM/new_app.squashfs $fwmd5 $CURRENT_APP_MD5 $UPDATE_DIR_CAM/flash_tool"
    set +e
    expect <<'EOF_EXPECT'
set timeout 300
log_user 1
spawn telnet $env(CAMERA_IP) 24
expect {
    -re {[#$] $} {}
    timeout { puts "\nERROR: Telnet connection timed out."; exit 10 }
    eof { puts "\nERROR: Telnet connection closed."; exit 11 }
}
send -- "$env(UPD_CMD)\r"
expect {
    -re {DECLOUD_UPDATE_SUCCESS\r} { puts "\nWizard detected a successful update + verification."; send -- "exit\r"; exit 0 }
    -re {DECLOUD_UPDATE_RESTORED\r} { puts "\nThe update failed, but the previous firmware was restored."; send -- "exit\r"; exit 20 }
    -re {DECLOUD_UPDATE_FAILED\r} { puts "\nUpdate failed. Keep the camera powered and review the output."; exit 21 }
    timeout { puts "\nERROR: Timed out waiting for the update. KEEP CAMERA POWERED."; exit 22 }
    eof { puts "\nERROR: Connection closed before success was confirmed. KEEP CAMERA POWERED."; exit 23 }
}
EOF_EXPECT
    local rc=$?
    set -e
    case "$rc" in
        0) ok "Firmware written and verified." ;;
        20) die "The previous firmware was restored. The camera is safe; it was not updated." ;;
        *) die "The update was not confirmed. KEEP THE CAMERA POWERED until you understand the failure." ;;
    esac
    printf '%s\n' "$fwmd5" > "$WORK_DIR/cameras/$CAMERA_NAME/installed_md5"
}
update_reboot() {
    info "Restarting the camera..."
    cam_exec 'sync; setsid sh -c "sleep 1; reboot" </dev/null >/dev/null 2>&1 &' >/dev/null || true
    sleep 20
    local i
    for i in $(seq 1 40); do
        check_tcp "$CAMERA_IP" 80 && check_tcp "$CAMERA_IP" 554 && break
        sleep 3
    done
    if check_tcp "$CAMERA_IP" 80 && check_tcp "$CAMERA_IP" 554; then
        ok "The camera is back: web page and RTSP answer at $CAMERA_IP."
    else
        warn "The camera did not answer yet. If it stays silent, unplug it for 5 seconds and plug it back in."
    fi
    say ""
    say "${GREEN}${BOLD}Done.${RESET} Camera ${BOLD}$CAMERA_NAME${RESET} runs firmware ${BOLD}$FIRMWARE_COMMIT${RESET}."
    say ""
    say "  • Web page: http://$CAMERA_IP/  (settings and login are unchanged)"
    say "  • Previous firmware, if you ever need it: $(dirname "$CURRENT_APP_FILE")"
    say "  • Remove the maintenance SD card when you no longer need it: while it is in"
    say "    the camera, port 24 gives anyone on your network a root shell without a password."
}
main_update() {
    (( EUID != 0 )) || die "Run ./install.sh without sudo. The wizard asks for sudo only for packages and SD-card operations."
    mkdir -p "$WORK_DIR" "$SRC_DIR" "$BACKUP_DIR" "$BUILD_DIR" "$MOUNT_DIR" "$RUNTIME_DIR"
    trap cleanup EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    banner
    say "${BOLD}UPDATE MODE${RESET}: install the newest firmware on a camera that already runs it."
    say "No Wi-Fi setup, no backup stage; your settings are kept."

    section 1 5 "Camera"
    choose_update_camera
    mkdir -p "$WORK_DIR/cameras/$CAMERA_NAME"

    section 2 5 "Computer checks"
    install_dependencies
    ensure_wizard_assets
    update_sources

    section 3 5 "Maintenance access"
    ask_maintenance_access
    read_current_firmware

    section 4 5 "Build the new firmware"
    local base="$CURRENT_APP_FILE"
    if [[ -f "$LATEST_BACKUP_FILE" && -f "$(cat "$LATEST_BACKUP_FILE")/mtdblock7.bin" ]]; then
        base="$(cat "$LATEST_BACKUP_FILE")/mtdblock7.bin"
        info "Building from this camera's stock backup: $base"
    else
        info "No stock backup of this camera on this computer: building from its current firmware."
    fi
    BUILD_BASE_APP="$base" BUILD_NO_SD=1 build_firmware

    section 5 5 "Update & verify"
    update_flash
    update_reboot
}
main() {
    (( EUID != 0 )) || die "Run ./install.sh without sudo. The wizard asks for sudo only for packages and SD-card operations."
    mkdir -p "$WORK_DIR" "$SRC_DIR" "$BACKUP_DIR" "$BUILD_DIR" "$MOUNT_DIR" "$RUNTIME_DIR"
    trap cleanup EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    banner

    section 1 8 "Camera identification"
    confirm_model
    ask_camera_name

    section 2 8 "Computer checks"
    install_dependencies
    ensure_wizard_assets
    update_sources

    section 3 8 "Wi-Fi setup"
    ask_wifi

    section 4 8 "Prepare the microSD card"
    choose_sd_device
    format_sd

    section 5 8 "Back up the camera & save Wi-Fi"
    run_stage1_until_success

    section 6 8 "Build the cloud-free firmware"
    build_firmware
    section 7 8 "Flash & verify"
    stage2_physical_instructions
    ask_camera_ip
    flash_over_telnet

    section 8 8 "Final boot & RTSP check"
    final_boot
}

case "${1:-}" in
    --version|-V)
        echo "LSC 3215672.2 Decloud Wizard v$VERSION"
        echo "Toolkit:  $TOOLKIT_COMMIT"
        echo "Firmware: $FIRMWARE_COMMIT"
        echo "SmolRTSP: $SMOLRTSP_COMMIT"
        exit 0
        ;;
    --help|-h)
        cat <<EOF_HELP
LSC 3215672.2 Decloud Wizard v$VERSION

Run without arguments to start the interactive installer.

Options:
  --update         Update a camera that already runs this firmware: build the newest
                   version and flash it through the maintenance port (an SD card with
                   the maintenance shell in the camera: the first installation's card,
                   or a new one the wizard makes). Settings are kept.
  -h, --help       Show this help
  -V, --version    Show version
Supported labels: 3215672.2 / SI B26101 or C26101; 3215672.3 / SI D26228.
C26101 and D26228 are community-reported. The exact MTD layout is required.
Run the interactive installer without sudo.
EOF_HELP
        exit 0
        ;;
    --update)
        main_update
        exit 0
        ;;
esac

main "$@"
