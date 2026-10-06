# OpenWebCam

Turn an Android phone into a USB webcam for Windows 11 and Linux.

It shows up as **OpenWebCam** in Camera, Teams, Zoom, Chrome, Edge and other
webcam apps. It needs no drivers, and it supports up to 4K at 30 fps.

```
Phone camera ── H.264 ── USB tethering ──► virtual camera on the PC ──► any app
```

## Requirements

- Windows 11 (x64 or ARM64), or Linux with systemd
- Android 10+
- A USB cable that carries data

## Install

**Phone:** install [`OpenWebCam-android.apk`](https://github.com/maxcodl/OpenWebCam/releases/latest/download/OpenWebCam-android.apk).

**Windows:** download [`OpenWebCam-windows-x64.zip`](https://github.com/maxcodl/OpenWebCam/releases/latest/download/OpenWebCam-windows-x64.zip)
([ARM64](https://github.com/maxcodl/OpenWebCam/releases/latest/download/OpenWebCam-windows-ARM64.zip)),
extract it, open **Terminal (Admin)** in that folder and run:

```powershell
.\OpenWebCamSetup.exe install
```

**Linux:** install the build tools and v4l2loopback, then download
[`OpenWebCam-linux.tar.gz`](https://github.com/maxcodl/OpenWebCam/releases/latest/download/OpenWebCam-linux.tar.gz)
and run the installer:

```sh
# Debian / Ubuntu
sudo apt install cmake g++ pkg-config libavcodec-dev libswscale-dev v4l2loopback-dkms
# Fedora (FFmpeg from RPM Fusion)
sudo dnf install cmake gcc-c++ ffmpeg-devel v4l2loopback
# Arch
sudo pacman -S cmake gcc pkgconf ffmpeg v4l2loopback-dkms

tar -xzf OpenWebCam-linux.tar.gz
sudo sh linux/install.sh
```

## Use

1. Plug in the phone and turn on **USB tethering**.
2. Open OpenWebCam on the phone and tap **Start streaming**.
3. Select **OpenWebCam** as the camera on the PC.

You can switch camera, resolution and orientation from the phone while it's
streaming.

## Commands

### Windows

Run these from an admin terminal in the extracted folder.

| Command | What it does |
|---|---|
| `.\OpenWebCamSetup.exe install` | Installs or updates the camera. Run again after downloading a new version. |
| `.\OpenWebCamSetup.exe uninstall` | Removes the camera from Windows. |
| `.\OpenWebCamSetup.exe prefer-wifi` | Keeps PC internet on Wi-Fi/Ethernet instead of the phone. Rerun after using a new USB port. |
| `Get-Content C:\ProgramData\OpenWebCam\log.txt -Tail 50` | Shows recent camera log entries. |

### Linux

| Command | What it does |
|---|---|
| `sudo sh linux/install.sh` | Builds and installs, or updates. |
| `sudo sh linux/install.sh uninstall` | Removes everything. |
| `journalctl -u 'openwebcam@*' -n 50` | Shows recent log entries. |
| `openwebcam --size 1280x720` | Runs by hand with a different output size (stop the service first). |

The receiver starts by itself when USB tethering comes up and stops when the
cable is unplugged. Internet traffic stays on Wi-Fi/Ethernet if you use
NetworkManager.

### Phone

| Command | What it does |
|---|---|
| `adb install OpenWebCam-android.apk` | Installs the phone app from the PC (USB debugging on). |
| `adb uninstall com.max.androidwebcam` | Removes the phone app. |

## Troubleshooting

| Problem | Fix |
|---|---|
| Color bars instead of video | USB tethering is off or the phone isn't streaming. |
| Windows: PC internet slow while tethered | `.\OpenWebCamSetup.exe prefer-wifi` |
| Windows: camera missing after an update | `.\OpenWebCamSetup.exe install`, then restart the PC. |
| Linux: no `/dev/video10` | `sudo modprobe v4l2loopback`. With Secure Boot on, the DKMS module must be signed. |
| Linux: camera not listed in Chrome | v4l2loopback was loaded without `exclusive_caps=1` (e.g. by OBS). Reboot. |
| Something else | Check the log (see Commands). |

## How it works

See [TECHNICAL.md](TECHNICAL.md) for the architecture, wire protocol and
build instructions.
