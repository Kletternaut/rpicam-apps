# rpicam-rt — Runtime Control Interface for rpicam-apps

## Overview

rpicam-rt is a runtime control interface for the rpicam-apps suite. A running
`rpicam-vid` instance listens on a Unix domain socket and accepts plain-text
commands that adjust camera parameters live — without stopping and restarting
the process. Two companion tools are provided: `rpicam-rt` (Qt GUI) and
`rpicam-rt-cli` (terminal UI).

Several camera controls — brightness, contrast, saturation, exposure
compensation, AWB mode, AWB gains, ROI/digital zoom, HDR mode, shutter and
framerate — are normally only applied at startup. rpicam-rt removes that
limitation, which is particularly valuable for iterative tuning (e.g. manual
focus, exposure, or framing) where the result must be visible in the live
stream while adjusting.

## History and naming

The feature was originally proposed as
[PR #917](https://github.com/raspberrypi/rpicam-apps/pull/917)
("rpicam-vid: add Unix Domain Socket runtime control and companion tools")
against the upstream `raspberrypi/rpicam-apps` repository. The PR received no
maintainer feedback and was closed on 2026-08-14 after about three months.

For a camera tool suite, live runtime control is arguably not a luxury but a
necessity: many workflows — manual focus, exposure tuning, framing — simply
cannot be done well without a live image preview, and a stop/restart cycle
between every adjustment makes iterative work impractical. Given that, the
absence of any upstream reaction to a feature of this scope is worth noting:
the PR sat unreviewed for its entire lifetime, and no technical feedback
arrived even after an explicit request to the maintainers. It was closed
without a single review comment.

Since then the feature is developed under the name **rpicam-rt**:

- Tools renamed: `rpicam-ctrl` → `rpicam-rt`, `rpicam-ctrl-cli` → `rpicam-rt-cli`
- Meson option renamed: `enable_rpicam_ctrl` → `enable_rpicam_rt`
- Capability token renamed: `rpicam_ctrl:1` → `rpicam_rt:1`
- The socket and state file names intentionally keep the `rpicam-vid` naming
  (`/tmp/rpicam-vid{N}.sock`, `/tmp/rpicam-vid{N}.state`), because they are
  properties of the `rpicam-vid` process.

## Components

| Component | Location | Description |
|---|---|---|
| `ControlSocket` | `apps/control_socket.hpp` | Self-contained header-only server; no dependency on rpicam-vid-specific code |
| `rpicam-rt` | `utils/rpicam_rt/` | Qt6/Qt5 graphical control panel |
| `rpicam-rt-cli` | `utils/rpicam-rt-cli` | Python 3 terminal UI (stdlib `curses` only, no dependencies) |

## Build and install

```bash
meson configure build -Denable_rpicam_rt=enabled
ninja -C build
sudo meson install -C build
sudo ldconfig
```

The GUI requires Qt6 or Qt5 (Widgets + Network); the TUI requires only Python 3.
The runtime control socket itself is compiled into `rpicam-vid` whenever the
option is enabled.

For a complete step-by-step installation guide (distro package removal, full
dependency list, verification), see [INSTALL_RT_ROI.md](INSTALL_RT_ROI.md).

### The `ldconfig` step is mandatory

`meson install` places `librpicam_app.so.1` in `/usr/local/lib/aarch64-linux-gnu/`,
but the dynamic linker keeps using its cached search results until the cache is
refreshed. On systems that have the distro-packaged rpicam-apps installed
(Debian/Raspberry Pi OS: `rpicam-apps-core`, `librpicam-app1`), an outdated
cache makes `rpicam-vid` load the **old packaged library** from
`/lib/aarch64-linux-gnu/librpicam_app.so.1` instead of the freshly installed one.
The version mismatch typically crashes the program with a segfault
(`Speicherzugriffsfehler` / exit code 139) as soon as it starts.

Running `sudo ldconfig` after `meson install` refreshes the cache so the new
library is found first. If the segfault persists, verify which library is
loaded:

```bash
ldd $(which rpicam-vid) | grep rpicam
```

It must point to `/usr/local/lib/...`, not to `/lib/...` or `/usr/lib/...`.

## Capability detection

rpicam-apps reports build capabilities through `--version` so that external
control applications can detect feature availability at runtime:

```
rpicam-apps capabilites: egl:1 qt:1 drm:1 libav:1 rpicam_rt:1
```

| Token | Meaning |
|---|---|
| `rpicam_rt:1` | Runtime control socket support is compiled in (Meson option `enable_rpicam_rt`) |
| `roi_selection:1` | Interactive ROI selection is available (requires the Qt preview plugin, see below) |

### Runtime vs. build-time capabilities

The tokens `egl`, `qt`, `drm`, `libav` and `roi_selection` are **not**
compile-time constants. The preview and encoder backends are plugin libraries
(`qt-preview.so`, `egl-preview.so`, `drm-preview.so`, `libav-encoder.so`) that
rpicam-apps loads from the installation directory **when the program starts**.
Until the build has been installed (`sudo meson install -C build`), the plugins
do not exist in the install path and every runtime token reports `0`:

```
rpicam-apps capabilites: egl:0 qt:0 drm:0 libav:0 roi_selection:0 rpicam_rt:1
```

Only `rpicam_rt:1` is a true compile-time token: the control socket is linked
into `rpicam-vid` whenever `enable_rpicam_rt` is enabled, so it is reported
even before installation.

`roi_selection` is not a Meson option at all — the interactive ROI selection is
always compiled in, but it depends on the Qt preview plugin. Its token
therefore follows the Qt plugin and reports `1` only when `qt:1` is present.

After installation the plugins are found and the tokens report their real
availability, e.g.:

```
rpicam-apps capabilites: egl:1 qt:1 drm:1 libav:1 roi_selection:1 rpicam_rt:1
```

## Control socket

### Socket paths

The socket path is derived automatically from the `--camera` index, so two
`rpicam-vid` instances can run simultaneously with independent sockets:

| `--camera` | Socket path |
|---|---|
| `0` (default) | `/tmp/rpicam-vid0.sock` |
| `1` | `/tmp/rpicam-vid1.sock` |

### Wire format

Commands are plain text, newline-terminated, in `key:value` form:

```
brightness:0.3
contrast:1.5
roi:0.25,0.25,0.5,0.5
awb:daylight
```

Multiple commands can be sent in a single write. The socket is non-blocking
(`SOCK_NONBLOCK`, `accept4`) and is polled once per frame in the capture loop,
so it never stalls the stream. The resulting `ControlList` is passed to the
existing `SetControls()` API.

Send commands from the shell:

```bash
echo "brightness:-0.3"           | nc -U /tmp/rpicam-vid0.sock
echo "awb:daylight"              | nc -U /tmp/rpicam-vid0.sock
echo "roi:0.25,0.25,0.5,0.5"     | nc -U /tmp/rpicam-vid0.sock
printf "contrast:1.4\nev:-1.0\n" | nc -U /tmp/rpicam-vid0.sock
```

### Supported commands

| Command | Value | Range / options |
|---|---|---|
| `brightness:<v>` | float | −1.0 … +1.0 |
| `ev:<v>` | float | −3.0 … +3.0 |
| `contrast:<v>` | float | 0.0 … 3.0 |
| `saturation:<v>` | float | 0.0 … 3.0 |
| `sharpness:<v>` | float | 0.0 … 16.0 |
| `gain:<v>` | float | 1.0 … 16.0 — sets analogue gain mode to manual automatically |
| `awb:<mode>` | string | `auto` `incandescent` `tungsten` `fluorescent` `indoor` `daylight` `cloudy` |
| `awbgains:<r>,<b>` | float,float | manual colour gains — disables auto AWB |
| `roi:<x>,<y>,<w>,<h>` | float×4 | relative 0.0–1.0; full frame = `0,0,1,1` |
| `metering:<mode>` | string | `centre` `spot` `average` `custom` |
| `exposure:<mode>` | string | `normal` `sport` |
| `denoise:<mode>` | string | `auto` `off` `cdn_off` `cdn_fast` `cdn_hq` |
| `hdr:<mode>` | string | `off` `auto` `sensor` `single-exp` |
| `shutter:<µs>` | integer | exposure time in microseconds; `0` = auto |
| `framerate:<fps>` | integer | target frame rate; `0` = auto (sensor maximum) |

### Caps protocol (server → client)

When a client connects, `rpicam-vid` sends a single line describing the active
sensor mode:

```
caps:maxfps=40,hasaf=0
```

| Field | Meaning |
|---|---|
| `maxfps` | Maximum frame rate of the active mode (integer fps) |
| `hasaf` | `1` if the camera supports autofocus hardware, `0` otherwise |

Clients parse this line and clamp the framerate slider maximum accordingly.

## Camera control semantics

- **AWB:** `awb:<mode>` sends `AwbEnable=true` alongside the mode; `awbgains:<r>,<b>` sends `AwbEnable=false`. This prevents libcamera from retaining stale colour-gain state when switching between manual and auto AWB.
- **EV:** `ev:<v>` explicitly sends `AeEnable=true` so the compensation is applied regardless of the auto-exposure pipeline state.
- **HDR:** on imx477 only `off` and `single-exp` are functional at runtime; `auto` and `sensor` both map to single-exposure HDR.
- **Shutter/framerate:** `shutter:0` restores auto exposure; `framerate:0` removes the frame-rate cap. Setting both non-zero enters fully manual exposure. The maximum useful shutter duration is `1 / framerate` seconds.
- **Shutter auto-resend:** when the framerate is changed and shutter is not on auto, the new shutter µs (recalculated from the updated one-frame period) is sent automatically, so the camera always matches the displayed value.

## ROI / digital zoom

The `roi:` command applies a hardware crop on the ISP (`ScalerCrop` /
`ScalerCrops`), not a software crop — full sensor resolution is retained
within the selected area. The implementation uses a runtime
`SupportsScalerCrops()` check on `RPiCamApp`, which is more reliable than
platform detection: sensors like the imx477 on Pi 5 use the legacy
`ScalerCrop` path even though the platform is PISP.

## State persistence

Both tools maintain independent state per camera index and persist it to
`/tmp/rpicam-vid{N}.state` (JSON):

- **No state bleeding** when switching cameras at runtime — each camera remembers its own last settings.
- **GUI/TUI interoperability** — both tools share the same state file format, so switching between `rpicam-rt` and `rpicam-rt-cli` preserves the last values.
- **Correct display after reconnect** — on reconnect the tool re-reads the state file; if it is absent (new `rpicam-vid` session), defaults are shown and sent.

Example state file:

```json
{
  "brightness": 0, "ev": 0, "contrast": 100, "saturation": 100,
  "zoom": 10, "gain": 10, "sharpness": 10,
  "awbGainR": 150, "awbGainB": 120,
  "shutter": 0, "framerateIdx": 0,
  "awbIdx": 0, "meteringIdx": 0, "exposureIdx": 0,
  "denoiseIdx": 0, "hdrIdx": 0
}
```

### Session isolation

Every new `rpicam-vid` session starts from hardware defaults: the companion
`.state` file is deleted both when the socket is created (before `bind()`) and
in the `ControlSocket` destructor (alongside the `.sock` file). `rpicam_vid`
also catches `SIGTERM` (in addition to `SIGINT`) and routes it through the
normal event-loop exit path, so `.sock` and `.state` are cleaned up on
`systemctl stop`, `pkill`, or `kill`. `SIGKILL` cannot be caught; the
constructor-time cleanup of stale files serves as the fallback.

## Multi-camera support

```bash
rpicam-vid --camera 0 -o /dev/null &   # /tmp/rpicam-vid0.sock
rpicam-vid --camera 1 -o /dev/null &   # /tmp/rpicam-vid1.sock
```

Both `rpicam-rt` and `rpicam-rt-cli` can switch between cameras at runtime
without restarting. If camera 0's socket is absent at startup but camera 1's
is present, camera 1 is selected automatically.

## Tools

### `rpicam-rt` — Qt GUI

A Qt6/Qt5 control panel with sliders for all continuous parameters and
dropdowns for mode selections:

- Sliders: brightness, EV, shutter (logarithmic scale), framerate (1 fps integer steps), contrast, saturation, gain, sharpness, AWB gains R/B, zoom
- Dropdowns: AWB, metering, exposure, denoise, HDR
- Camera selector (0 / 1) with auto-select of camera 1
- Per-mode fps cap from the `caps:` message; shutter auto-resend on framerate change
- Autofocus (AF) controls are implemented in code but hidden in the UI — untested due to missing hardware (no AF-capable lens available); not production-ready
- Keyboard shortcuts: `R` reset, `Q` quit, `C` switch camera
- Connects automatically on startup and reconnects if `rpicam-vid` is restarted

### `rpicam-rt-cli` — Terminal UI

ASCII slider interface in pure Python 3 (stdlib `curses`), same controls in
the same order as the GUI:

```bash
./utils/rpicam-rt-cli        # camera 0
./utils/rpicam-rt-cli 1      # camera 1
```

| Key | Action |
|---|---|
| `↑` / `↓` | Move between parameters |
| `←` / `→` | Decrease / increase value (or switch camera on the Camera row) |
| `C` | Cycle to next camera |
| `R` | Reset all parameters to defaults |
| `Q` | Quit |

## Extensibility

`ControlSocket` is implemented as a self-contained header with no dependency
on `rpicam-vid`-specific code. Integrating it into any other rpicam app
(`rpicam-still`, `rpicam-jpeg`, `rpicam-raw`, …) requires only three steps in
that app's event loop: construct the socket with the desired path, call
`AcceptConnections()` and `ReadControls()` once per frame, and pass the
returned `ControlList` to `SetControls()`. The command vocabulary and wire
format are identical regardless of which app hosts the socket.

## Compatibility guarantees

- **No breaking changes** — all existing CLI options continue to work identically.
- **Fully additive** — no existing code path is modified; the socket is purely additional logic layered on top of the existing `SetControls()` API.
- **Graceful degradation** — if `bind()` fails, a warning is logged and `rpicam-vid` continues normally without socket support.
- **Locale-safe** — float parsing uses `std::locale::classic()` explicitly.
- **Non-blocking** — `SOCK_NONBLOCK` and `accept4()` throughout; the capture loop is never stalled.

## Demo video

A demo video showing `rpicam-rt-cli` (terminal UI) and `rpicam-rt` (Qt GUI)
controlling live `rpicam-vid` streams in real time:

https://github.com/user-attachments/assets/2eb58603-3aa1-4cb2-ab8e-183538f75c8d
