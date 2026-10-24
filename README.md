<img src="icon/opencam.svg" width="96" alt="openCam icon">

# openCam

Use an Android phone as a webcam for every Windows app (Zoom, Teams, Chrome, OBS…), over USB or Wi-Fi.
No resolution cap, no watermark. Both sides show the same camera controls and stay in sync.

<p align="center">
  <img src="docs/pc-connect.png" width="49%" alt="openCam on Windows finding a phone on the network">
  <img src="docs/pc-settings.png" width="49%" alt="openCam settings panel with start-with-Windows and updates">
</p>

## Features

- **Every Windows app sees it.** A real Windows 11 virtual camera named *openCam*. No OBS plugin needed.
- **USB or Wi-Fi.** USB via `adb forward`. On Wi-Fi the phone and PC find each other, or pair with a 5-digit code.
- **Up to 4K, 30/60 fps.** Hardware H.264 encode on the phone. No watermark, no time limit.
- **Full camera control from either side.** Zoom, flip, torch, focus, exposure, stabilisation, Pro (ISO, shutter, white balance), Portrait blur.
- **Battery friendly.** The phone camera runs only while something is watching.
- **Hands-off.** Starts with Windows in the tray, reconnects by itself, and updates itself from GitHub Releases.

```
android/   Kotlin app: Camera2 → hardware H.264 → TCP server (foreground service)
windows/   openCam.exe (tray app: connect, decode, UI) + opencam_vcam.dll (virtual camera)
PROTOCOL.md  wire format between the two
```

## Requirements

- Windows 11 (the virtual camera API, `MFCreateVirtualCamera`, is Windows 11 only).
- Android 11 or newer.

## Download

Grab `openCam.exe`, `opencam_vcam.dll` and `openCam.apk` from the
[latest release](https://github.com/RDK456/openCam/releases/latest).

## Build

```bash
cd android && ./gradlew assembleRelease      # → android/app/build/outputs/apk/release/app-release.apk
windows\build.bat                             # → windows\out\openCam.exe + opencam_vcam.dll
```

## Install and first run

1. Install `openCam.apk` on the phone (or `adb install -r` it).
2. Run `openCam.exe` once. Keep `opencam_vcam.dll` next to it.
   - A UAC prompt appears **once**. It copies the camera DLL to `C:\Program Files\openCam` and registers it,
     because the Windows camera service needs to load it.
   - Windows Firewall asks about network access the first time. Allow **private networks**: Wi-Fi discovery needs it.
   - openCam adds itself to *Start with Windows*. Turn this off in the tray menu or the settings drawer.
3. Open openCam on the phone and grant camera + notification permission.

From then on it's hands-off. The PC app starts with Windows and sits in the tray. The phone app keeps running
as a notification after you leave it, until you tap **Quit**, leave Wi-Fi range or join a different network.
Any Windows app can now pick the camera named **openCam**.

### Wired (USB)

Turn on *Developer options → USB debugging* on the phone, plug it in and accept the "Allow USB debugging" prompt.
openCam runs `adb forward` itself. `adb.exe` must be next to `openCam.exe`, on `PATH`, or in the default
Android SDK location. USB is tried first whenever both are available.

### Wireless

Same Wi-Fi network as the PC. The phone announces itself, the PC also scans its network every few seconds, and they
connect on their own. The PC remembers the last phone and tries it first. If that doesn't work:

- **PC:** pick the phone from *Phones on this network* (or *Scan again*), or type the phone's **pairing code** (5 digits,
  shown on the phone while no PC is connected) or its IP address.
- **Phone:** tap **Search** under *PCs on this network* and tap your PC.

The one-time UAC step also adds a Windows Firewall rule for inbound UDP port 47101 only, so phone beacons and searches get through.
Networks with client isolation (some guest/public Wi-Fi) block devices from seeing each other; use USB there.

## Controls (phone and PC)

Zoom presets (plus pinch on the phone, mouse wheel on the PC), front/back flip, torch, AF/manual focus,
exposure compensation, stabilisation, **Pro** (ISO, shutter, focus distance, white balance, aperture),
**Portrait** (background blur), resolution up to 4K, 30/60 fps, bitrate, mirror and rotate-180 on the PC output,
and **LIVE** (pause/resume the feed). Controls the phone's camera lacks are greyed out.

The phone camera runs only while someone is watching: the phone preview, the PC window, or a Windows app
using the camera. Otherwise it stays off to save battery.

## Updates

Both apps check [GitHub Releases](https://github.com/RDK456/openCam/releases) every 6 hours, or right away with
**Check now** under *Updates* in the settings (gear) panel, which also shows the current version and status. A delta against the
installed version is downloaded when the release has one; otherwise the full file is downloaded. Either way it is
checked against the release's SHA-256 manifest before use.

- **PC:** installs on its own, but only once no app is using the camera and the window is closed; then it restarts
  in the tray. **Restart now** installs it at once. A UAC prompt appears only when the camera DLL changed.
- **Phone:** shows an "openCam X is ready" notification and an **Install** button in settings. Tap either and confirm
  Android's install prompt (the first time, allow *Install unknown apps* for openCam).

Publish a release with `.\release.ps1 1.2.3`: it builds both apps, makes deltas from the previous release, writes the
manifest, tags, pushes, and creates the GitHub release.

## Known limits

- **Aperture:** almost every phone has a fixed aperture, so this control is usually greyed out.
- **Portrait:** uses the phone maker's bokeh extension (Samsung, Pixel and some others). The phone shows no preview
  while it is on, because Android gives that mode a single output.
- If another app (e.g. WhatsApp video) takes the phone camera, openCam pauses and resumes when it is released.
- Uninstall the camera: run `openCam.exe --uninstall` from an elevated prompt.

## Check

`openCam.exe --selftest` exits 0 when the JSON parsing used for the phone link and the delta encode/apply round
trips work.
