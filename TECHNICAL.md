# OpenWebCam: Technical Details

This document describes how OpenWebCam works internally: the Android sender, the
wire protocol, the Windows virtual camera, the installer, and the build. For
installation and everyday use, see [README.md](README.md).

## Contents

- [Overview](#overview)
- [Repository layout](#repository-layout)
- [Android app](#android-app)
- [Wire protocol](#wire-protocol)
- [Windows virtual camera](#windows-virtual-camera)
- [Linux receiver](#linux-receiver)
- [Installer](#installer)
- [Diagnostic tool](#diagnostic-tool)
- [Building](#building)
- [Design decisions](#design-decisions)
- [Limitations](#limitations)

## Overview

```
Android                                     Windows
-------                                     -------
Camera2 ──► MediaCodec (H.264, Surface in)  Camera app / browser / Teams / Zoom
                │                                    ▲  NV12, 30 fps
                ▼                                    │
         bounded packet queue               Windows Camera Frame Server
                │                                    ▲
                ▼                                    │ IMFMediaSource
       TCP server, port 5000  ◄── USB tethering ──  OpenWebCam.dll
                                  (RNDIS / NCM)     decode → rotate → scale → letterbox
```

The phone is the **server**. It listens on TCP port 5000 on every interface.
The PC is the **client**. When an app opens the camera, Windows loads
`OpenWebCam.dll` inside the Frame Server service. The DLL finds the phone at the
USB tethering adapter's gateway address and pulls the stream. When the app
closes the camera, the connection is dropped and nothing stays running on the PC.

USB tethering provides the network link, so no ADB, USB driver, or vendor tool
is needed. The phone becomes a DHCP server and IPv4 gateway on a small private
subnet, and Windows sees it as an ordinary RNDIS or NCM network adapter.

## Repository layout

```
app/                         Android app (Kotlin, Jetpack Compose)
  src/main/kotlin/com/max/androidwebcam/
    MainActivity.kt          UI: permission, status, tethering shortcut, settings
    WebcamService.kt         Foreground service: camera → encoder → TCP
    WebcamConfig.kt          Wire protocol constants, settings store, status flow
    Devices.kt               Camera/encoder capability catalog, tethering helpers
windows/
  CMakeLists.txt             Builds three targets (below)
  common/
    openwebcam.h             CLSID, friendly name, data directory
    net.h                    Phone discovery and wire protocol (client side)
    h264_decoder.h           H.264 → NV12 with the inbox Media Foundation decoder
  vcam/
    OpenWebCam.cpp           Virtual camera media source (COM DLL)
    convert.h                NV12 rotate, scale, letterbox
    OpenWebCam.def           DLL exports
  setup/
    setup.cpp                OpenWebCamSetup.exe: install / uninstall / prefer-wifi
    setup.manifest           Requests administrator rights
  probe/
    owc_probe.cpp            owc-probe.exe: command-line stream tester
linux/
  openwebcam.cpp           Receiver: phone → v4l2loopback device
  install.sh               Build + install / uninstall
  openwebcam@.service, 90-openwebcam.rules, 90-openwebcam.conf
                           systemd unit, udev trigger, NetworkManager metric
.github/workflows/build.yml  CI builds and automatic releases
```

## Android app

Package `com.max.androidwebcam`, `minSdk` 29 (Android 10), `targetSdk` 35.
The UI uses Jetpack Compose and Material 3. There are no third-party runtime
dependencies.

### Permissions

| Permission | Purpose |
|---|---|
| `CAMERA` | Capture video |
| `INTERNET` | Open the TCP server socket |
| `FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_CAMERA` | Keep streaming with the screen off or app in background |
| `POST_NOTIFICATIONS` | Show the ongoing "streaming" notification |

### Camera and size selection (`Devices.kt`)

`CameraCatalog.load` lists every camera and keeps only sizes that pass all of
these checks:

- Camera2 can output them to a `MediaCodec` surface.
- Width and height are even.
- The camera's minimum frame duration allows 30 fps.
- At least one H.264 encoder reports `areSizeAndRateSupported(w, h, 30)`.

Cameras with no usable size are hidden. Each camera gets a label such as
"Back camera" or "Front camera 2". The saved resolution is matched exactly when
possible. Otherwise the size with the closest pixel count is used.

`Tether.address()` reports the phone's IPv4 address on the first interface that
is up and named `rndis*`, `ncm*`, or `usb*`. The UI uses this only to show
whether tethering is on. `Tether.openSettings()` tries several vendor-specific
Settings activities in turn to open the tethering page.

### Streaming service (`WebcamService.kt`)

A foreground service of type `camera` runs the pipeline on four kinds of thread:

| Thread | Work |
|---|---|
| Camera (`HandlerThread`) | Opens the camera, creates the capture session, rebuilds the pipeline when settings change |
| Encoder drain | Pulls encoded buffers from `MediaCodec` and queues them |
| Accept | Accepts PC connections on port 5000 |
| Writer (one per client) | Writes queued packets to the socket |

**Capture.** Camera2 renders straight into the encoder's input `Surface`
(`COLOR_FormatSurface`), so frames never touch app memory. The request uses
`TEMPLATE_RECORD` with auto 3A. It also picks the AE target FPS range with an
upper bound of 30 and the highest lower bound, which keeps the frame rate
steady.

**Encoder settings.**

| Key | Value | Why |
|---|---|---|
| Codec | H.264 / AVC, chosen with `findEncoderForFormat` | Universally decodable on Windows |
| Bitrate | ~0.14 bits/pixel at 30 fps, clamped to 2–40 Mbit/s (720p ≈ 3.9, 1080p ≈ 8.7, 4K ≈ 35) | Good quality on a local link |
| Bitrate mode | VBR | |
| I-frame interval | 1 s | Quick recovery after drops or reconnects |
| B-frames | 0 | No reordering delay |
| `KEY_PREPEND_HEADER_TO_SYNC_FRAMES` | 1 | SPS/PPS with every keyframe |
| `KEY_LATENCY` | 1 | Output each frame immediately |
| `KEY_PRIORITY` | 0 (realtime) | |

The codec config (SPS + PPS) comes from `csd-0`/`csd-1` or from a
`BUFFER_FLAG_CODEC_CONFIG` buffer. It must already be in Annex-B form, with
`00 00 01` or `00 00 00 01` start codes, and is cached for new clients.

**Rotation.** The phone never rotates pixels. It works out the clockwise
rotation the PC must apply and sends it as metadata:

```
rotation = (sensorOrientation ± deviceDegrees) mod 360    (minus for front cameras)
```

In Auto mode, `deviceDegrees` comes from an `OrientationEventListener` with
hysteresis: the value changes only when the phone is more than 60° away from
the current orientation, so it does not flicker near 45°. The fixed modes
(Portrait, Landscape, Landscape flipped, Portrait upside down) use a constant.
Changing orientation sends a new INFO packet without restarting the encoder.
Changing camera or resolution rebuilds the whole pipeline.

**Back-pressure.** Each client has a 30-packet `ArrayBlockingQueue`, about one
second of video. The encoder never blocks on the network:

1. A newly connected client starts in "drop until keyframe" mode. It gets an
   INFO packet first, and a sync frame is requested.
2. When the first keyframe arrives, the cached SPS/PPS is queued, followed by
   the keyframe.
3. If the queue is full, it is cleared, a fresh INFO packet is queued, the
   client goes back to "drop until keyframe", and a new sync frame is
   requested.

As a result, a slow or stalled PC sees a short freeze and then a clean
recovery. Latency does not keep growing and the decoder never sees corrupted
frames.

The writer uses a 256 KiB `BufferedOutputStream` with `TCP_NODELAY`. It
flushes only when the queue is empty, so bursts are combined into larger
writes.

**One client at a time.** When a new connection arrives, the previous one is
closed and the newest connection is served.

**Status.** `StreamStatus` is a `MutableStateFlow<Status>` shared by the
service and UI in the same process. It reports whether streaming is running,
the connected PC address, a description such as "Back camera · 1920 × 1080
(1080p) · Auto", and the last error.

**Settings** (camera ID, width, height, orientation) are stored in
`SharedPreferences`. The UI saves them and sends `ACTION_APPLY` to the running
service, so changes take effect without stopping the stream.

### Release build

R8 minification and resource shrinking are enabled. The release signing key
comes from `keystore.properties` (local, gitignored) or `SIGNING_*`
environment variables (CI). Without either, the build falls back to the
debug key. `-PversionName`/`-PversionCode` override the version (CI sets them
from the release tag).

## Wire protocol

Version 2. The phone sends and the PC only reads. Integers are big-endian.

```
+-----------------+---------+----------------------+
| u32 length      | u8 type | payload (length - 1) |
+-----------------+---------+----------------------+
  length counts type + payload
```

| Type | Name | Payload |
|---|---|---|
| `1` | VIDEO | One H.264 access unit in Annex-B format. The first VIDEO packet after (re)sync is SPS+PPS, followed by an IDR. |
| `2` | INFO | `u16 width`, `u16 height`, `u16 rotation` (clockwise degrees: 0, 90, 180, 270) |

Rules:

- INFO is always the first packet on a connection. It is sent again whenever
  the size or rotation changes, and after any queue overflow.
- After a size change, the next VIDEO data begins with a keyframe.
- The client rejects lengths of 0 or above 8 MiB, INFO packets with odd or
  tiny dimensions, and rotations that are not multiples of 90.
- The client sets a 2 s receive timeout. At 30 fps, two seconds of silence
  means the link is gone.

## Windows virtual camera

`OpenWebCam.dll` is an in-process COM server. It is registered with
`MFCreateVirtualCamera` (Windows 11 build 22000 and later) and loaded by the
**Windows Camera Frame Server** service (`FrameServer`, running as
LocalService). Every app that uses the camera goes through the Frame Server,
so one source can serve several apps at once
(`MF_DEVICESTREAM_FRAMESERVER_SHARED`).

### COM objects

```
Activator (IMFActivate, CLSID {EAE9430F-FC57-419D-A8BE-9599849F761A})
  └─ ActivateObject ─► MediaSource (IMFMediaSourceEx, IMFGetService, IKsControl)
                           └─ MediaStream (IMFMediaStream2, IKsControl)
```

- **Activator.** The Frame Server creates this object by CLSID. It forwards
  all `IMFAttributes` calls to an internal attribute store and creates the
  media source on demand.
- **MediaSource.** A live source (`MFMEDIASOURCE_IS_LIVE`) with one video
  stream. `Pause` is not supported. `SetD3DManager` is accepted but ignored,
  because all samples are in system memory. `IKsControl` is present because
  virtual cameras must expose it, but every property, method, and event
  returns `ERROR_SET_NOT_FOUND`.
- **MediaStream.** Category `PINNAME_VIDEO_CAPTURE`, frame source type
  Color. It owns two worker threads while running.

All objects are built with WRL (`RuntimeClass`, `MakeAndInitialize`). The
stream holds a reference to the source, and `Shutdown` releases it to break
the reference cycle.

### Offered formats

Every format is NV12, 30 fps, progressive, square pixels, with fixed-size
samples:

```
1920x1080 (default)   3840x2160   2560x1440   1280x720   960x540
640x480               640x360     2160x3840   1080x1920  720x1280
```

The app chooses one of these sizes. The phone's resolution is independent,
and every frame is fitted to the size the app chose.

### Threads

**Receive thread** (`MediaStream::Receive`):

1. Discover the phone (see below). If it is not found, retry every 2 s and
   show color bars in the meantime.
2. Create an `H264Decoder` for the INFO size.
3. For each VIDEO packet, decode, then copy the result into a tightly packed
   NV12 buffer. Rotate if needed, scale while keeping the aspect ratio, and
   letterbox to the app's size.
4. Publish the frame as "newest", replacing any frame not yet delivered.
5. For each INFO packet, recreate the decoder if the size changed. Rotation
   changes apply to the next frame.
6. On disconnect, reconnect. If the decoder itself failed, wait 10 s first so
   the phone is not flooded with retries.

**Delivery thread** (`MediaStream::Run`): the Frame Server calls
`RequestSample(token)`, and the token is queued. The thread waits until there
is both a new frame and a pending token, then sends an `MEMediaSample` event
with the frame copied into an `MFCreate2DMediaBuffer`. If no frame has arrived
for 1 s, it sends color bars at 30 fps instead. Frames are never repeated, so
apps see the phone's real frame timing.

The only shared state between the two threads is a single "newest frame"
slot guarded by a mutex. Old frames are dropped rather than queued, which
keeps latency to about one frame.

### Phone discovery (`net.h`)

1. `GetAdaptersAddresses` lists IPv4 gateways of adapters that are up.
   Gateways of adapters whose description contains `NDIS` or `NCM` (USB
   tethering) are tried first, then all others.
2. For each candidate, open a non-blocking `connect` to port 5000 with a 1 s
   timeout.
3. A candidate counts as the phone only if the first packet it sends is a
   valid INFO packet. Other devices that happen to listen on port 5000 are
   rejected.

Because nothing is configured by hand, the phone's changing tethering subnet
is handled automatically.

### Decoding (`h264_decoder.h`)

The decoder is Microsoft's inbox H.264 decoder MFT (`CLSID_MSH264DecoderMFT`)
with `CODECAPI_AVLowLatencyMode` turned on. It runs in software mode, and the
caller allocates output samples.

- Input packets are dropped until the first IDR. If that IDR has no SPS of its
  own, the last SPS/PPS packet seen is fed in first. A small Annex-B scanner
  (`NalTypes`) detects these.
- `MF_E_TRANSFORM_STREAM_CHANGE` leads to renegotiating NV12 output and
  picking up the new stride.
- `CopyNV12` removes the decoder's row padding and height alignment. For
  example, 1080 is often coded as 1088. The output is a tightly packed
  `w × h × 1.5` buffer.

### Frame fitting (`convert.h`)

| Step | Implementation |
|---|---|
| Rotate | CPU loop over the Y plane and the interleaved UV plane, 90/180/270° clockwise |
| Fit | Largest even size with the source aspect ratio that fits the output |
| Scale | Inbox Video Processor MFT (`CLSID_VideoProcessorMFT`) in software mode. If it fails once, nearest-neighbor is used for the rest of the session. |
| Letterbox | Center on black (Y = 16, UV = 128, BT.601 limited range). Offsets are even so chroma stays aligned. |

Steps are skipped when they are not needed. A portrait phone feeding a 16:9
app gets black bars at the sides.

### No-signal pattern

75% SMPTE-style color bars in BT.601 limited range, with a white stripe moving
down 8 rows per frame. The moving stripe shows that the pipeline is alive but
not receiving video. A still image would mean something is frozen.

### Logging

`Log()` writes each message to `OutputDebugString` (visible in DebugView) and
appends it to `%ProgramData%\OpenWebCam\log.txt` with a timestamp and process
ID. The file is opened in shared mode, so it can be read while the camera is
in use. It records which process loaded the source and as which user, the
requested sizes, phone connect and disconnect events, size and rotation
changes, and decoder or scaler failures.

## Installer

`OpenWebCamSetup.exe` is a console program that requires administrator
rights through its manifest (`requireAdministrator`). It does not install a
service, scheduled task, or startup entry.

### `install`

1. Copy `OpenWebCam.dll` from next to the EXE to
   `%ProgramFiles%\OpenWebCam\`. If the Frame Server has the old DLL loaded,
   that file is renamed (`.old<tick>`) and set to be deleted at reboot, since
   a loaded DLL can be renamed but not overwritten.
2. Create `%ProgramData%\OpenWebCam` with a protected ACL: full control for
   SYSTEM and Administrators, and modify rights for LocalService,
   write-restricted tokens, and Users. The Frame Server can write the log and
   users can read or delete it.
3. Register the COM class under
   `HKLM\SOFTWARE\Classes\CLSID\{EAE9430F-...}\InprocServer32` with
   `ThreadingModel = Both`.
4. Call `MFCreateVirtualCamera(SoftwareCameraSource, Lifetime_System,
   Access_AllUsers, "OpenWebCam", CLSID)` and `Start`. With system lifetime,
   the camera persists across reboots and is visible to every user.
5. Run `prefer-wifi`.

### `prefer-wifi`

When USB tethering is turned on, the phone becomes a default gateway, and
Windows often ranks it above Wi-Fi. All internet traffic would then go
through the phone. This command finds RNDIS/NCM adapters and sets their IPv4
interface metric to 500, both immediately with `SetIpInterfaceEntry` and
persistently in `Tcpip\Parameters\Interfaces\{guid}\InterfaceMetric`. The
phone link stays usable for OpenWebCam, and normal traffic goes through
Wi-Fi or Ethernet.

Windows creates a new adapter for each USB port, so the command must be run
again after the phone is plugged into a different port.

### `uninstall`

Removes the virtual camera, deletes the COM registration, restores automatic
metrics on tethering adapters, and deletes the DLL (or schedules it for
deletion at reboot if it is in use). Logs are kept.

## Diagnostic tool

`owc-probe.exe [phone-ip]` is built by CI but not included in the user zip.
It uses the same discovery and decoder code as the camera, but runs as a
normal process, which makes it easier to debug than code running inside the
Frame Server. It:

- prints each discovery attempt,
- prints resolution, fps, Mbit/s, and average decode latency every 10 s,
- writes the 60th frame, before rotation, to `frame.bmp` (BT.601 → RGB).

## Linux receiver

Linux has no on-demand virtual camera API, so the Linux side is a small
userspace program, `openwebcam` (`linux/openwebcam.cpp`), that writes into a
**v4l2loopback** device. Apps read `/dev/video10` like any webcam.

```
phone :5000 ──► openwebcam ──► /dev/video10 (v4l2loopback, "OpenWebCam") ──► apps
                 discovery, H.264 decode (libavcodec),
                 scale (libswscale), rotate, letterbox → I420
```

It speaks the same wire protocol and follows the same rules as the Windows
camera:

- **Discovery:** gateways from `/proc/net/route`. Interfaces whose driver is
  `rndis_host` or `cdc_ncm` come first. `--interface` restricts the search to
  one interface. A host counts as the phone only if its first packet is a
  valid INFO.
- **Decoding:** FFmpeg's H.264 decoder with `AV_CODEC_FLAG_LOW_DELAY` and no
  frame threading, so there is no reorder delay. Packets before the first
  SPS/PPS + IDR are rejected by the decoder and skipped. The decoder is
  recreated when INFO reports a new size.
- **Fitting:** libswscale scales the frame, keeping the aspect ratio, before
  rotation, so rotation runs on the smaller image. A CPU loop rotates each
  plane, and the result is centered on black.
- **Output:** I420 at a fixed size, 1920x1080 by default (`--size` changes
  it). v4l2loopback has one format per device, so apps can't pick a size the
  way they can on Windows.
- **No signal:** color bars at 10 fps after one second without a frame.

### Lifecycle

| File | Installed to | Role |
|---|---|---|
| `90-openwebcam.rules` | `/etc/udev/rules.d/` | When an `rndis_host`/`cdc_ncm` interface appears, starts `openwebcam@<iface>.service` |
| `openwebcam@.service` | `/etc/systemd/system/` | Runs `openwebcam --interface <iface>`. `BindsTo` the interface, so it stops on unplug. `DynamicUser` with the `video` group. |
| `90-openwebcam.conf` | `/etc/NetworkManager/conf.d/` | Route metric 1000 on tethering interfaces, so Wi-Fi/Ethernet keep the internet traffic. Applies to every USB port, unlike Windows. |
| (generated) | `/etc/modprobe.d/openwebcam.conf` | `video_nr=10 card_label=OpenWebCam exclusive_caps=1` (Chrome lists only capture-only devices) |
| (generated) | `/etc/modules-load.d/openwebcam.conf` | Loads v4l2loopback at boot |

`linux/install.sh` builds with CMake as the invoking user, installs the files
above, loads the module, and starts the service for any tethering interface
already up. `uninstall` removes all of it. It unloads v4l2loopback only if the
module is the OpenWebCam instance.

## Building

### Windows

Requires Visual Studio 2022 with the C++ workload and a Windows 11 SDK, plus
CMake 3.21 or later.

```powershell
cmake -S windows -B build -A x64      # or -A ARM64
cmake --build build --config Release
```

Output in `build/Release/`: `OpenWebCam.dll`, `OpenWebCamSetup.exe`,
`owc-probe.exe`. The build uses C++17, `/W4 /permissive-`, and the static CRT,
so no Visual C++ redistributable is needed. It links only system libraries:
`mfplat`, `mfuuid`, `wmcodecdspuuid`, `mfsensorgroup`, `ws2_32`, `iphlpapi`,
`ole32`, `advapi32`, `runtimeobject`.

After rebuilding, run `OpenWebCamSetup.exe install` again. If the old DLL is
still loaded, restart the Frame Server (`net stop FrameServer` as admin) or
reboot.

### Android

Requires JDK 17.

```sh
./gradlew :app:assembleRelease
```

Output: `app/build/outputs/apk/release/app-release.apk`. Built with Android
Gradle Plugin 8.13, Kotlin 2.2, and the Compose BOM.

### Linux

Requires CMake 3.16+, a C++17 compiler, pkg-config, and the FFmpeg
development packages (libavcodec, libavutil, libswscale). Tested in CI with
FFmpeg 4.4 and 6.1.

```sh
cmake -S linux -B build/linux
cmake --build build/linux
```

### CI

`.github/workflows/build.yml` runs on pushes to `main` (except
documentation-only changes), pull requests, and manual runs:

- **version**: works out the next version from Conventional Commit messages
  since the last `v*` tag (`feat!`/`BREAKING CHANGE` → major, `feat` → minor,
  anything else → patch) and writes a grouped changelog.
- **windows**: matrix over x64 and ARM64. Produces
  `OpenWebCam-windows-<arch>.zip` (DLL + setup) and `owc-probe-<arch>.exe`.
- **linux**: builds on Ubuntu 22.04 and 24.04 and packages the `linux/`
  folder as `OpenWebCam-linux.tar.gz`. Users build on install because
  FFmpeg's ABI differs between distros.
- **android**: produces `OpenWebCam-android.apk`, signed with the release key
  from repository secrets (`KEYSTORE_BASE64`, `SIGNING_*`). It falls back to
  the debug key for pull requests and refuses to release without the key.
- **release** (pushes to `main` only): tags the commit and publishes a GitHub
  release with the changelog, both zips, the Linux tarball, and the APK.

## Design decisions

- **Frame Server virtual camera instead of a DirectShow filter or kernel
  driver.** It needs no driver signing, works in both UWP and Win32 apps, and
  loads only while an app has the camera open.
- **Phone as server, PC as client.** The PC finds the phone from its gateway
  address, so neither side needs configuration. The phone does not need to
  know the PC's address either.
- **USB tethering instead of ADB.** No developer mode, no `adb` binary, and no
  port forwarding. Any data-capable USB cable works.
- **H.264 instead of raw or MJPEG.** Hardware encoding on the phone and
  software decoding with the inbox decoder on the PC keep the bandwidth
  manageable, even at 4K.
- **Rotation as metadata.** The encoder never restarts when the phone is
  turned. Only the much cheaper rotation on the PC changes.
- **Drop, never queue.** Bounded queues on both ends and a "newest frame
  wins" slot keep latency low. Overload causes a short freeze, not a growing
  delay.
- **Only inbox components.** Only Media Foundation decoders and processors
  that ship with Windows are used, and there are no third-party libraries on
  either side.

## Limitations

- Windows needs Windows 11, since the virtual camera API does not exist on
  Windows 10. "N" editions need the Media Feature Pack.
- Linux needs v4l2loopback, an out-of-tree kernel module. With Secure Boot,
  the DKMS module must be signed. The output size is fixed per device.
- Linux discovery needs a gateway route through the phone. Setting
  `ipv4.never-default` on the tethering connection removes it (raise the
  route metric instead, as the installer does).
- The frame rate is fixed at 30 fps.
- Video only, no audio.
- Only one PC can stream at a time. The newest connection wins.
- USB only. Wi-Fi discovery is not implemented, although the protocol itself
  would work over any IPv4 link if the phone were the gateway.
- Decoding, rotation, and scaling run on the CPU in the Frame Server process.
- No camera controls (zoom, focus, exposure) are exposed to Windows.
