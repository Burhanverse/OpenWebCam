# OpenWebCam

Turn an Android phone into a USB webcam for Windows 11.

It shows up as **OpenWebCam** in Camera, Teams, Zoom, Chrome, Edge and other
webcam apps. It needs no drivers and no background
app, and it supports up to 4K at 30 fps.

```
Phone camera ── H.264 ── USB tethering ──► Windows virtual camera ──► any app
```

## Requirements

- Windows 11 (x64 or ARM64)
- Android 10+
- A USB cable that carries data

## Install

**Phone:** install [`OpenWebCam-android.apk`](https://github.com/maxcodl/OpenWebCam/releases/latest/download/OpenWebCam-android.apk).

**PC:** download [`OpenWebCam-windows-x64.zip`](https://github.com/maxcodl/OpenWebCam/releases/latest/download/OpenWebCam-windows-x64.zip)
([ARM64](https://github.com/maxcodl/OpenWebCam/releases/latest/download/OpenWebCam-windows-ARM64.zip)),
extract it, open **Terminal (Admin)** in that folder and run:

```powershell
.\OpenWebCamSetup.exe install
```

## Use

1. Plug in the phone and turn on **USB tethering**.
2. Open OpenWebCam on the phone and tap **Start streaming**.
3. Select **OpenWebCam** as the camera on the PC.

You can switch camera, resolution and orientation from the phone while it's
streaming.

## Commands

Run these from an admin terminal in the extracted folder.

| Command | What it does |
|---|---|
| `.\OpenWebCamSetup.exe install` | Installs or updates the camera. Run again after downloading a new version. |
| `.\OpenWebCamSetup.exe uninstall` | Removes the camera from Windows. |
| `.\OpenWebCamSetup.exe prefer-wifi` | Keeps PC internet on Wi-Fi/Ethernet instead of the phone. Rerun after using a new USB port. |
| `Get-Content C:\ProgramData\OpenWebCam\log.txt -Tail 50` | Shows recent camera log entries. |
| `adb install OpenWebCam-android.apk` | Installs the phone app from the PC (USB debugging on). |
| `adb uninstall com.max.androidwebcam` | Removes the phone app. |

## Troubleshooting

| Problem | Fix |
|---|---|
| Color bars instead of video | USB tethering is off or the phone isn't streaming. |
| PC internet slow while tethered | `.\OpenWebCamSetup.exe prefer-wifi` |
| Camera missing after an update | `.\OpenWebCamSetup.exe install`, then restart the PC. |
| Something else | Check the log (see Commands). |

## How it works

See [TECHNICAL.md](TECHNICAL.md) for the architecture, wire protocol and
build instructions.
