# rpicam-apps: RT/ROI Feature — Installation Guide

This document explains how to install `rpicam-apps` with the **runtime control
(rt) + interactive ROI selection** feature from the
[`feature/rt-roi`](https://github.com/Kletternaut/rpicam-apps/tree/feature/rt-roi)
branch of the `Kletternaut/rpicam-apps` fork, and how to verify the
installation.

## What the feature adds

- `rpicam-vid`: runtime control over a **Unix Domain Socket**. The socket is
  created automatically; its path is derived from the camera index
  (`/tmp/rpicam-vid0.sock` for camera 0, `/tmp/rpicam-vid1.sock` for
  camera 1).
- `rpicam-rt`: a Qt-based GUI control panel (`utils/rpicam_rt/`).
- `rpicam-rt-cli`: a command-line control client (`utils/rpicam-rt-cli`).
- Interactive **ROI (region of interest) selection** in the Qt preview
  (`--roi`).
- Version/capability reporting (`rpicam_rt:1`, `roi_selection`).

---

## 1. Check the installed version and capabilities

First, check whether `rpicam-apps` is already installed, and whether the
feature is present:

```bash
rpicam-vid --version
```

The output shows the build version and the compiled-in capabilities, for
example:

```
rpicam-apps build: v1.13.0 35dff05ed71c 19-08-2026 (12:18:57)
rpicam-apps capabilites: egl:1 qt:1 drm:1 libav:1 roi_selection:1 rpicam_rt:1
```

- `rpicam_rt:1` → the RT control panel is available.
- `roi_selection:1` → interactive ROI selection is available.

If `rpicam_rt:1` is missing, the feature is **not** yet available and you need
to build it yourself (continue with the steps below).

## 2. Distro package or self-built?

Determine whether your `rpicam-apps` comes from the distribution (installed via
`apt`) or was built locally:

```bash
dpkg -l | grep rpicam-apps
apt list --installed 2>/dev/null | grep rpicam-apps
```

- If a package is listed → it is a **distro** installation (continue with
  step 3).
- If nothing is listed → it was likely **built from source** (you can skip
  step 3).

## 3. Remove the distro package (only if applicable)

If `rpicam-apps` was installed from the distribution, remove it first:

```bash
sudo apt-get remove --purge rpicam-apps
```

## 4. Install build dependencies

Follow the official Raspberry Pi
[camera software documentation](https://www.raspberrypi.com/documentation/computers/camera_software.html#building-rpicam-apps)
and install the required build tools and libraries:

```bash
sudo apt install -y cmake libboost-program-options-dev libdrm-dev libexif-dev
sudo apt install -y meson ninja-build
```

If you want the `libav` encoder, the Qt preview window and OpenCV
post-processing, also install:

```bash
sudo apt install -y libcamera-dev libepoxy-dev libjpeg-dev libtiff5-dev libpng-dev
sudo apt install -y libavcodec-dev libavdevice-dev libavformat-dev libswresample-dev
sudo apt install -y libopencv-dev
sudo apt install -y qtbase5-dev libqt5core5a libqt5gui5 libqt5widgets5
```

## 5. Get the source with the RT/ROI feature

The `feature/rt-roi` branch of the fork is already fully patched. You can
clone it directly (recommended) or clone upstream and merge the branch into
it — both lead to the same result.

### Option A: clone the patched branch directly (recommended)

```bash
git clone -b feature/rt-roi https://github.com/Kletternaut/rpicam-apps.git
cd rpicam-apps
```

### Option B: clone upstream, then merge the feature branch

```bash
git clone https://github.com/raspberrypi/rpicam-apps.git
cd rpicam-apps

# Fetch and merge the RT/ROI feature branch from the fork
git fetch https://github.com/Kletternaut/rpicam-apps.git feature/rt-roi
git merge --ff-only FETCH_HEAD
```

The branch is based on the `v1.13.0` tag, so it bumps the project version
from `1.12.0` to `1.13.0`.

## 6. Configure the build

To see the options that were used in an existing `meson setup build ...`
command, inspect Meson's recorded command line:

```bash
cat build/meson-private/cmd_line.txt
```

Example output:

```
[options]
enable_libav = enabled
enable_drm = enabled
enable_egl = enabled
enable_qt = enabled
enable_opencv = enabled
enable_tflite = disabled
enable_hailo = enabled
enable_rpicam_rt = enabled
```

If this file does **not** exist, the `build` directory is not configured yet
(see "fresh setup" below).

### Fresh setup (no existing configuration)

```bash
meson setup build -Denable_libav=enabled -Denable_drm=enabled -Denable_egl=enabled -Denable_qt=enabled -Denable_opencv=disabled -Denable_tflite=disabled -Denable_hailo=disabled -Denable_rpicam_rt=enabled
```

### Already configured

Only add the `-Denable_rpicam_rt=enabled` option and reconfigure:

```bash
meson setup --reconfigure build -Denable_rpicam_rt=enabled
```

`-Denable_rpicam_rt=enabled` is the key option for this feature — it builds the
Qt runtime control panel (`rpicam-rt`). Adjust the other options to your needs.
Note: `roi_selection` is **not** a Meson option — it is reported automatically
as a capability when the Qt preview is available.

## 7. Compile

```bash
meson compile -C build
```

| Tip | On devices with 1 GB of memory or less, append `-j 1` to limit the build to a single process. |
| --- | ---------------------------------------------------------------------------------------------- |

## 8. Install

```bash
sudo meson install -C build
```

## 9. Update the library cache

```bash
sudo ldconfig
```

## 10. Verify the capabilities

After installing, confirm that the feature is active:

```bash
rpicam-hello --version
```

The output must contain `rpicam_rt:1` (and `roi_selection:1` when a Qt preview
is available):

```
rpicam-apps build: v1.13.0 35dff05ed71c 19-08-2026 (12:18:57)
rpicam-apps capabilites: egl:1 qt:1 drm:1 libav:1 roi_selection:1 rpicam_rt:1
```

## 11. Verify in piStudio (only for piStudio users)

This step is specific to the piStudio front-end — skip it if you don't use
piStudio.

1. Start piStudio.
2. Open **Help → System Information**.
3. Check that all `rpicam-apps` entries are listed as **Supported**.
4. The **Start** button **"rt"** should now be present.

---

## Optional: manual smoke test

```bash
# Start a stream with Qt preview (ROI selection available). The control
# socket is created automatically at /tmp/rpicam-vid0.sock (camera 0).
rpicam-vid -t 0 --qt-preview &

# Control it from the command line (auto-selects the active camera)
rpicam-rt-cli
```

The GUI control panel can also be launched directly via `rpicam-rt`.
