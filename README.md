# OpenWebCam

Use your Android phone as a webcam on Windows 11 over USB.

The phone appears as a normal camera named **OpenWebCam** in every app,
including Camera, Teams, Zoom, Chrome, and Edge. There are no drivers to
install and nothing runs in the background on the PC. Windows loads the camera
only while an app is using it.

```
Phone  Camera → H.264 encoder ──── USB tethering ────►  PC  Virtual camera → any app
```

## Requirements

- Windows 11, x64 or ARM64. "N" editions also need the Media Feature Pack.
- Android 10 or later.
- A USB cable that carries data.

## Install

1. Download from [Releases](../../releases):
   - `OpenWebCam-android.apk` for the phone
   - `OpenWebCam-windows-x64.zip` for the PC, or `-ARM64` for ARM PCs
2. Install the APK on the phone.
3. Unzip the Windows package and run `OpenWebCamSetup.exe install`. It asks
   for administrator rights. After it finishes, you can delete the unzipped
   folder.

## Use

1. Connect the phone by USB and turn on **USB tethering**. The app has a
   shortcut to this setting.
2. Open OpenWebCam on the phone and tap **Start streaming**.
3. Select **OpenWebCam** as the camera in any app on the PC.

You can change the camera, resolution, and orientation in the app while it
is streaming.

## Troubleshooting

| Problem | Fix |
|---|---|
| Color bars instead of video | Turn on USB tethering and start streaming on the phone. |
| PC internet is slow while tethered | Run `OpenWebCamSetup.exe prefer-wifi`. |
| Camera missing after an update | Run `OpenWebCamSetup.exe install`, then restart the PC. |
| Anything else | Check `C:\ProgramData\OpenWebCam\log.txt`. |

## Uninstall

Run `OpenWebCamSetup.exe uninstall`, then remove the app from the phone.

## More

For details on the architecture, protocol, and build, see
[TECHNICAL.md](TECHNICAL.md).
