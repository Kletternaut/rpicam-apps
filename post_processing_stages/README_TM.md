## TemplateMatchStage – Documentation

## Overview

**TemplateMatchStage** is a post-processing stage for the [rpicam-vid](https://github.com/raspberrypi/rpicam-apps) application.  
It integrates seamlessly into the rpicam-vid pipeline and allows you to perform template matching on the camera stream in real time.  
The stage uses OpenCV to find a template (from a second stream or file) in the main video stream and visualizes the result as a rectangle overlay.

---

## How to Use

- This stage is loaded via the `--post-process-file` option of `rpicam-vid`.
- Configuration is done through a JSON file (see below).
- The stage can be combined with other post-processing stages in the rpicam-vid pipeline.

---

## Configuration Parameters (`template_match.json`)

| Parameter         | Type    | Description                                                                 |
|-------------------|---------|-----------------------------------------------------------------------------|
| match_fps         | int     | How many times per second matching is performed (default: 5)                |
| active_methods    | [bool]  | Which matching methods are enabled (e.g. `[true, false, false]`)            |
| template_width    | int     | Fixed width of the template (optional)                                      |
| template_height   | int     | Fixed height of the template (optional)                                     |
| scale_min         | double  | (Not used with fixed scaling)                                               |
| scale_max         | double  | (Not used with fixed scaling)                                               |
| scale_step        | double  | (Not used with fixed scaling)                                               |
| fixed_scale       | double  | Fixed scaling factor for the template (e.g., 0.1)                           |
| match_threshold   | double  | Threshold above which a match is considered valid (e.g., 0.8)               |
| rescale_threshold | double  | Threshold for rescaling (optional, for dynamic scaling)                     |
| debug             | bool    | Enables debug output in the terminal                                        |

**Example:**
```json
{
    "template_match": {
        "match_fps": 10,
        "active_methods": [true, false, false],
        "fixed_scale": 0.1,
        "match_threshold": 0.8,
        "debug": true
    }
}
```

---

## How It Works

1. **Integration:**  
   - The stage is registered as a post-processing module and runs automatically after frame capture in rpicam-vid.
   - It receives the main camera stream and (optionally) a secondary stream for the template.

2. **Template Preparation:**  
   - The template can be resized to a fixed width/height and then scaled by a fixed factor.

3. **Matching:**  
   - OpenCV's `matchTemplate` is used with the enabled methods (`cv::TM_CCOEFF_NORMED`, `cv::TM_SQDIFF_NORMED`, `cv::TM_CCORR_NORMED`).
   - For each enabled method, the best match score and position are determined.

4. **Visualization:**  
   - For each enabled method, a rectangle is drawn at the detected position:
     - White (4px): `cv::TM_CCOEFF_NORMED`
     - Gray (2px): `cv::TM_SQDIFF_NORMED`
     - Black (1px): `cv::TM_CCORR_NORMED`
   - All rectangles are drawn at the same position, only thickness and grayscale value differ.

5. **Debug:**  
   - If debug is enabled, score, scaling, and position are printed to the terminal.
   - Optionally, a debug image with overlays is saved.

---

## Notes

- Template matching works best when the template and search image have similar sizes and contrast.
- For objects with changing size, dynamic scaling can be implemented (currently disabled).
- Performance and detection quality depend on parameters and image quality.
- Only enabled methods in `active_methods` are used and visualized.

---

## Extension Ideas

- Add colored rectangles (e.g., white/gray/black) depending on score or method.
- Re-enable dynamic scaling if needed.
- Support for more matching methods.
- Logging to file instead of console.

---

## Example Usage

### GStreamer (for external template stream)
```sh
rpicam-vid --camera 1 -t 0 --qt-preview --info-text %focus --width 640 --height 480 --lores-height 300 --lores-width 400 --lores-par 1 --codec libav --libav-format mpegts --inline -o - | \
gst-launch-1.0 fdsrc fd=0 ! tsdemux ! h264parse ! rtph264pay config-interval=1 pt=96 ! udpsink host=192.168.0.30 port=8554
```

### rpicam-vid with Template Matching
```sh
rpicam-vid --info-text FoM:%focus,-FPS:%fps,-Frame:%frame --qt-preview --height 990 --width 1330 --camera 0 -t 0 --preview 500,50,600,400 --lores-width 320 --lores-height 240 --lores-par 1 --post-process-file assets/template_match.json
```

---

## Parameter Overview Table

| Method Index | OpenCV Constant         | Rectangle Color (Gray) | Thickness (px) | Description                |
|--------------|------------------------|------------------------|----------------|----------------------------|
| 0            | TM_CCOEFF_NORMED       | White (255)            | 4              | Default, robust matching   |
| 1            | TM_SQDIFF_NORMED       | Gray (127)             | 2              | Sensitive to differences   |
| 2            | TM_CCORR_NORMED        | Black (0)              | 1              | Correlation-based matching |

---

## Troubleshooting

- **No rectangle visible:**  
  - Check if the template is smaller than the main image and not empty.
  - Ensure at least one method is enabled in `active_methods`.
  - Make sure `match_fps` is not set too low.
- **Performance issues:**  
  - Lower `match_fps` or reduce template size.
- **No match found:**  
  - Adjust `match_threshold` or try different matching methods.

---


