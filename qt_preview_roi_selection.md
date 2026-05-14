# Interactive ROI Selection in Qt Preview

## Overview

The Qt preview window (`--qt-preview`) now supports interactive Region of Interest (ROI) selection via mouse. The selected region is applied as a hardware crop (`ScalerCrop` / `ScalerCrops`) to the camera ISP — this is a true hardware zoom, not a software crop, so full sensor resolution is retained within the selected area.

## Usage

| Action | Effect |
|---|---|
| Left-click + drag | Draw a selection rectangle |
| Release mouse button | Apply ROI — camera zooms into the selected area |
| Right-click | Reset to full frame |

- The selection rectangle is aspect-ratio locked to the preview window dimensions to avoid image distortion.
- While a ROI is active, accidental re-selection by left-click is blocked. Right-click first to reset.

## Terminal Output

When a ROI is applied, the equivalent `--roi` command-line parameter is printed to the terminal:

```
ROI selected: --roi 0.25,0.30,0.50,0.40
```

This allows you to copy the exact values for use in a subsequent `rpicam-vid` invocation without having to calculate them manually.

## Example

```bash
rpicam-vid --qt-preview -t 0 --width 1920 --height 1080
```

Draw a rectangle over the area of interest in the preview window. The camera will immediately zoom into that region. The terminal prints the corresponding `--roi` value.

To use the same ROI in a scripted or headless run:

```bash
rpicam-vid -t 10000 --width 1920 --height 1080 --roi 0.25,0.30,0.50,0.40 -o output.h264
```

## Implementation Notes

- Only supported with `--qt-preview`. EGL and DRM preview windows do not support this feature.
- Works on both Pi 4 (VC4, uses `ScalerCrop`) and Pi 5 (PISP, uses `ScalerCrops`).
- The selection callback runs in the Qt thread; `SetControls()` is called from there and is thread-safe.
- Selection rectangle is rendered as a 1 px hairline in `QColor(0, 180, 255, 200)`.
