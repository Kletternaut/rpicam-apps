# SVG runtime controls in the preview

`rt_roi_overlay` is a post-processing stage that configures an interactive
overlay in the **Qt preview**. It uses transparent SVG controls with minus/plus
buttons and draggable sliders. Mode controls use the same buttons and a stepped
slider. It builds on `feature/rt-roi-preview2`; the appearance follows the
transparent overlay approach of `feature/gamepad_overlay_stage`.

A small button remains visible at the bottom right. Click it to hide or show the
controls without restarting the application. Hidden rows release their mouse
areas for ROI selection; the toggle button itself always excludes ROI input.
Hiding the controls does not reset camera settings.

The stage leaves camera buffers untouched: the controls appear in the local
preview, not in recordings, still images or the GStreamer output. The gamepad
stage is not required. If both stages are available, both may be enabled in the
same post-processing JSON.

## Build and start

Install the Qt SVG development module in addition to the existing build
dependencies. For Qt 5 use `libqt5svg5-dev`; for Qt 6 use `qt6-svg-dev`, matching
the Qt major version selected by Meson.

```sh
meson setup build --buildtype=release -Denable_qt=enabled -Denable_rpicam_rt=enabled
# For an existing build directory:
# meson setup --reconfigure build -Denable_qt=enabled -Denable_rpicam_rt=enabled
ninja -C build
sudo meson install -C build
sudo ldconfig

rpicam-vid --qt-preview --preview 0,0,1280,720 -t 0 \
  --post-process-file assets/rt_roi_overlay.json
```

The installed example is under `<prefix>/share/rpi-camera-assets/rt_roi_overlay.json`.
Copy it to a writable location to customize it. The configuration is read when
the application starts; restart after editing it. Other preview backends do not
have interactive mouse controls. Requesting this stage without a Qt preview
built with SVG support produces an explicit startup error. Qt previews without
this stage continue to work without the SVG development module.

## Configuration

```json
{
  "rt_roi_overlay": {
    "initially_visible": true,
    "toggle": { "x": 0.88, "y": 0.9, "width": 0.1, "height": 0.07, "opacity": 0.9 },
    "scale": 1.0,
    "opacity": 0.85,
    "background_opacity": 0.25,
    "foreground": "#ffffff",
    "accent": "#00b4ff",
    "background": "#101820",
    "controls": [
      {
        "parameter": "zoom",
        "label": "Zoom",
        "x": 0.02, "y": 0.03,
        "width": 0.36, "height": 0.128,
        "min": 1, "max": 5, "step": 0.1
      },
      {
        "parameter": "brightness",
        "x": 0.62, "y": 0.03,
        "width": 0.36, "height": 0.128
      }
    ]
  }
}
```

| Setting | Meaning |
| --- | --- |
| `initially_visible` | Whether controls start expanded (default `true`). The toggle button remains visible in either state. |
| `toggle.x`, `toggle.y`, `toggle.width`, `toggle.height` | Independent normalized position and size of the persistent toggle button. Global `scale` does not affect it. |
| `toggle.opacity` | Independent toggle-button opacity, from `0.1` to `1`. It remains accessible even when the control opacity is zero. |
| `controls` | Ordered list of visible controls. Omit it for brightness, EV, contrast, saturation and zoom; `[]` displays none. |
| `parameter` | One parameter name from the table below. A parameter may occur once. |
| `visible` | Optional, default `true`. `false` removes the control and its input area. |
| `label` | Optional displayed label. |
| `x`, `y` | Top-left position as a fraction of the preview width/height, in `[0,1]`. |
| `width`, `height` | Size as fractions of preview width/height. Defaults: `0.36`, `0.128`. |
| `scale` | Global size multiplier; leaves each top-left anchor in place. Every scaled control must fit inside the preview. |
| `opacity` | Control opacity: `0` hides the control rows and removes their input areas; `1` is fully opaque. The toggle button uses its own opacity. |
| `background_opacity` | Additional row-background opacity, multiplied by the overall opacity. `0` leaves the controls on a transparent background. |
| `foreground`, `accent`, `background` | Colours in `#rrggbb` format. SVG is generated from these validated colours. |
| `min`, `max`, `step` | Optional narrower display range and button/slider increment. Modes require integer limits and `step: 1`. |

Missing positions default to the left edge and vertically stacked rows. When
adding many controls, set their positions explicitly to make multiple columns.
Unknown parameters, invalid colours, nonfinite values, invalid ranges, duplicate
parameters and controls extending outside the preview are rejected at startup.
Avoid overlapping controls: the last configured row is drawn on top and receives
input first.

## Parameters

| Name | Default slider range / choices |
| --- | --- |
| `brightness` | -1 to 1 |
| `contrast`, `saturation` | 0 to 3 |
| `ev` | -3 to 3 stops |
| `sharpness` | 0 to 15.99 |
| `gain` | 0 to 16; 0 restores automatic gain |
| `shutter` | 0 to 100000 microseconds; 0 restores automatic exposure |
| `framerate` | 0 to 120 FPS; 0 restores automatic frame duration |
| `awb_red`, `awb_blue` | 0.1 to 8; editing either disables automatic white balance |
| `zoom` | 1 to 10; 1 restores the full sensor area |
| `roi_x`, `roi_y` | 0 to 0.99, normalized sensor coordinates |
| `roi_width`, `roi_height` | 0.01 to 1, normalized sensor dimensions |
| `awb` | auto, incandescent, tungsten, fluorescent, indoor, daylight, cloudy, custom, manual |
| `metering` | centre, spot, average, custom |
| `exposure` | normal, sport, long, custom |
| `denoise` | auto, off, cdn_off, cdn_fast, cdn_hq |
| `hdr` | off, single-exp |

Sensor capabilities may narrow these ranges. Unsupported controls are displayed
disabled. Values represent the requested settings; automatic shutter and gain
stay at zero instead of jumping with each measured exposure. The overlay reads
the same requested state that is updated by the runtime socket and ROI selection.
It does not use or overwrite the GUI/TUI state files and does not create another
socket connection. This preserves an existing `rpicam-rt` connection. The GUI/TUI
themselves do not gain a new server-to-client state synchronization protocol.

## Mouse and ROI interaction

- Left-click minus/plus changes one step. Click or drag the track to set a value.
- Click the persistent toggle to hide/show all rows. Its position, size and
  opacity are independent of the rows; it is drawn above overlapping rows.
- A press anywhere inside a control row belongs to that row until all mouse
  buttons are released. Transparent corners, labels, padding, disabled controls
  and right-clicks cannot start a selection or reset an ROI.
- A slider drag remains a slider drag when the pointer leaves the control or
  preview. An ROI drag that starts outside the controls remains an ROI drag when
  crossing a row; it cannot change a control.
- Outside control rows, left-drag selects an ROI, and right-click restores the
  full frame, as in the existing preview. Active crops from the overlay or socket
  also block accidental new selection until reset.
- Selection retains the preview aspect ratio and is clamped to the preview
  bounds at release.
- Rendering and input use the same normalized geometry, including after resizing
  the window. Zero-opacity and hidden control rows have no input areas; the
  persistent toggle always keeps its own input area.

## Verification

The portable model/configuration checks can run without a camera (Boost headers
are required for configuration tests). See `tests/README.rt-roi-overlay.md` for
commands and the hardware acceptance checklist. A successful host-side check
does not replace a Raspberry Pi build and a camera/Qt mouse-interaction test.
