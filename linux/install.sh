#!/bin/sh
# OpenWebCam for Linux installer.
#
#   sudo ./install.sh              build (if needed) and install
#   sudo ./install.sh uninstall    remove everything
#
# Installs the receiver to /usr/local/bin, loads v4l2loopback as /dev/video10
# ("OpenWebCam"), starts the receiver whenever USB tethering comes up, and
# keeps internet traffic off the phone (NetworkManager).
set -eu

here=$(cd "$(dirname "$0")" && pwd)
build="$here/../build/linux"
bin=/usr/local/bin/openwebcam
unit=/etc/systemd/system/openwebcam@.service
rules=/etc/udev/rules.d/90-openwebcam.rules
nmconf=/etc/NetworkManager/conf.d/90-openwebcam.conf
modload=/etc/modules-load.d/openwebcam.conf
modopts=/etc/modprobe.d/openwebcam.conf

if [ "$(id -u)" != 0 ]; then
    echo "Run as root: sudo $0 ${1:-}"
    exit 1
fi

reload() {
    systemctl daemon-reload
    udevadm control --reload
    if systemctl is-active --quiet NetworkManager; then nmcli general reload conf || true; fi
}

# Runs the receiver for tethering interfaces that are already up.
start_existing() {
    for dev in /sys/class/net/*; do
        driver=$(basename "$(readlink "$dev/device/driver" 2>/dev/null)" 2>/dev/null || true)
        case "$driver" in
            rndis_host|cdc_ncm) systemctl start "openwebcam@$(basename "$dev").service" ;;
        esac
    done
}

do_install() {
    # Build (incremental) as the invoking user so the build folder isn't owned by root.
    as_user=""
    if [ -n "${SUDO_USER:-}" ]; then as_user="sudo -u $SUDO_USER"; fi
    $as_user cmake -S "$here" -B "$build"
    $as_user cmake --build "$build"

    if ! modinfo v4l2loopback >/dev/null 2>&1; then
        echo "v4l2loopback is not installed. Install it first, e.g.:"
        echo "  Debian/Ubuntu: sudo apt install v4l2loopback-dkms"
        echo "  Fedora:        sudo dnf install v4l2loopback"
        echo "  Arch:          sudo pacman -S v4l2loopback-dkms"
        exit 1
    fi

    install -m 755 "$build/openwebcam" "$bin"
    install -m 644 "$here/openwebcam@.service" "$unit"
    install -m 644 "$here/90-openwebcam.rules" "$rules"
    if [ -d /etc/NetworkManager/conf.d ]; then install -m 644 "$here/90-openwebcam.conf" "$nmconf"; fi
    echo v4l2loopback > "$modload"
    echo 'options v4l2loopback video_nr=10 card_label=OpenWebCam exclusive_caps=1' > "$modopts"

    if lsmod | grep -q '^v4l2loopback'; then
        if [ "$(cat /sys/devices/virtual/video4linux/video10/name 2>/dev/null)" != OpenWebCam ]; then
            echo "Note: v4l2loopback is already loaded with other options (OBS?)."
            echo "Reboot, or run: sudo modprobe -r v4l2loopback && sudo modprobe v4l2loopback"
        fi
    else
        modprobe v4l2loopback
    fi

    reload
    start_existing
    echo "Installed. Turn on USB tethering and pick \"OpenWebCam\" in any camera app."
}

do_uninstall() {
    systemctl stop 'openwebcam@*.service' 2>/dev/null || true
    rm -f "$bin" "$unit" "$rules" "$nmconf" "$modload" "$modopts"
    reload
    if [ "$(cat /sys/devices/virtual/video4linux/video10/name 2>/dev/null)" = OpenWebCam ]; then
        modprobe -r v4l2loopback 2>/dev/null || echo "v4l2loopback is in use; it unloads on reboot."
    fi
    echo "Uninstalled."
}

case "${1:-install}" in
    install) do_install ;;
    uninstall) do_uninstall ;;
    *) echo "Usage: sudo $0 [install|uninstall]"; exit 2 ;;
esac
