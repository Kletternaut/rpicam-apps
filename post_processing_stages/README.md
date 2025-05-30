## AcousticFocusStage – Documentation (English)

**Purpose:**  
The AcousticFocusStage is a post-processing stage for rpicam-apps that provides acoustic feedback based on the libcamera Focus Figure of Merit (FoM).  
This allows you to find the optimal focus point of a manual lens **without needing to look at or interpret the preview image**.

### Features

- Plays a sine tone via the Raspberry Pi’s audio output.
- The tone’s frequency is mapped to the current Focus FoM value (frequency rises or falls as FoM rises or falls).
- No visual contact with the preview is required.
- The tone is triggered once per second for a configurable duration.
- All parameters (frequency range, mapping type, duration, etc.) can be configured via JSON.
- **Note:** You must use an external USB sound card, HDMI audio, or another supported audio device for this stage to function.

### Dependencies

Install the following packages:

```sh
sudo apt update
sudo apt install sox libsox-fmt-all
```

### Build Instructions

1. Add `acoustic_focus_stage.cpp` to your `post_processing_stages` directory.
2. Add the stage to your `meson.build`:
   ```meson
   core_postproc_src = files([
       ...
       'acoustic_focus_stage.cpp',
   ])
   postproc_assets += files([
       ...
       assets_dir / 'acoustic_focus.json',
   ])
   ```
3. Rebuild and install:
   ```sh
   meson compile -C build
   sudo meson install -C build
   ```

### Configuration

Create a config file `acoustic_focus.json` (example):

```json
{
  "acoustic_focus": [
    {
      "stage": "acoustic_focus",
      "minFoM": 1,
      "maxFoM": 2000,
      "minFreq": 300,
      "maxFreq": 5000,
      "duration": 0.1,
      "mapping": "log",
      "description": "mapping values are log (logarithmic) or linear"
    }
  ]
}
```

- `minFoM`, `maxFoM`: Range of Figure of Merit values to map.
- `minFreq`, `maxFreq`: Frequency range for the output tone (Hz).
- `duration`: Tone duration in seconds.
- `mapping`: `"log"` for logarithmic mapping, `"linear"` for linear mapping.

### Usage

1. Start rpicam-vid with:
   ```sh
   rpicam-vid --post-process-config assets/acoustic_focus.json
   ```
2. Adjust focus on your manual lens. The tone’s pitch will rise or fall as the focus improves or worsens.

---

## AcousticFocusStage – Dokumentation (Deutsch)

**Zweck:**  
Die AcousticFocusStage ist eine Post-Processing-Stage für rpicam-apps, die akustisches Feedback auf Basis des libcamera Focus Figure of Merit (FoM) gibt.  
Damit findest du den optimalen Fokuspunkt einer manuellen Linse **ohne auf die Vorschau schauen oder diese interpretieren zu müssen**.

### Funktionen

- Gibt einen Sinuston über den Audio-Ausgang des Raspberry Pi aus.
- Die Tonhöhe wird aus dem aktuellen Focus FoM-Wert berechnet (steigt oder fällt mit dem FoM).
- Kein Sichtkontakt zur Vorschau erforderlich.
- Der Ton wird einmal pro Sekunde für eine konfigurierbare Dauer ausgegeben.
- Alle Parameter (Frequenzbereich, Mapping-Typ, Dauer usw.) sind per JSON konfigurierbar.
- **Hinweis:** Es muss eine Soundausgabe-Hardware vorhanden sein.

### Abhängigkeiten

Installiere folgende Pakete:

```sh
sudo apt update
sudo apt install sox libsox-fmt-all
```

### Kompilierung

1. Lege `acoustic_focus_stage.cpp` im Verzeichnis `post_processing_stages` ab.
2. Ergänze die Stage in deiner `meson.build`:
   ```meson
   core_postproc_src = files([
       ...
       'acoustic_focus_stage.cpp',
   ])
   postproc_assets += files([
       ...
       assets_dir / 'acoustic_focus.json',
   ])
   ```
3. Baue und installiere neu:
   ```sh
   meson compile -C build
   sudo meson install -C build
   ```

### Konfiguration

Beispiel für `acoustic_focus.json`:

```json
{
  "acoustic_focus": [
    {
      "stage": "acoustic_focus",
      "minFoM": 1,
      "maxFoM": 2000,
      "minFreq": 400,
      "maxFreq": 2000,
      "duration": 0.1,
      "mapping": "log", // oder "linear"
      "description": "mapping values are log (logarithmic) or linear"
    }
  ]
}
```

- `minFoM`, `maxFoM`: Bereich der Figure of Merit-Werte.
- `minFreq`, `maxFreq`: Frequenzbereich für den Ton (Hz).
- `duration`: Tondauer in Sekunden.
- `mapping`: `"log"` für logarithmisch, `"linear"` für linear.

### Verwendung

1. Starte rpicam-vid mit:
   ```sh
   rpicam-vid --post-process-config assets/acoustic_focus.json
   ```
2. Drehe am Fokusring deiner manuellen Linse. Die Tonhöhe steigt oder fällt, je nach Fokusqualität.

---

**Hinweis:**  
Die Stage ist ein reines Hilfsmittel für das manuelle Fokussieren und benötigt keinen Blickkontakt zum Monitor!
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
| rtsp_port         | int     | UDP port for external template stream (default: 8554)                       |
| debug_level       | int     | Debug output level (see below)                                              |

**Example:**
```json
{
    "template_match": {
        "match_fps": 2,
        "active_methods": [true, false, false],
        "scale_min": 0.1,
        "scale_max": 1.00,
        "scale_step": 0.1,
        "rescale_threshold": 0.75,
        "match_threshold": 0.8,
        "auto_scale": true,
        "fixed_scale": 0.1,
        "debug_level": 0,
        "rtsp_port": 8555
    }
}
```

---

## Debug Levels

| Level | Output                                                                                   |
|-------|-----------------------------------------------------------------------------------------|
| 0     | No debug output, no debug image                                                         |
| 1     | Prints center position and size of rectangle, saves debug image                         |
| 2     | Like 1, plus prints minVal and maxVal for each method                                   |
| 3+    | Like 2, plus additional test/debug output (e.g. "Matching loop, debug_level_: ...")     |

---

## How It Works

1. **Integration:**  
   - The stage is registered as a post-processing module and runs automatically after frame capture in rpicam-vid.
   - It receives the main camera stream and (optionally) a secondary stream for the template.

2. **Template Preparation:**  
   - The template can be resized to a fixed width/height and then scaled by a fixed factor.

3. **Auto-Scaling (Optional):**  
   - If `auto_scale` is enabled, the stage automatically searches for the best matching scale of the template within a specified range (`scale_min`, `scale_max`, `scale_step`).
   - For each scale and enabled method, the best match score and position are determined.
   - If a sufficiently good match is found for several consecutive frames, the auto-scaling "locks" to the detected scale for faster processing.
   - If the match quality drops below the threshold, auto-scaling is unlocked and the search resumes.
   - The current auto-scaling status and locked scale are shown in the debug output (level 3).

4. **Matching:**  
   - OpenCV's `matchTemplate` is used with the enabled methods (`cv::TM_CCOEFF_NORMED`, `cv::TM_SQDIFF_NORMED`, `cv::TM_CCORR_NORMED`).
   - For each enabled method, the best match score and position are determined.

5. **Visualization:**  
   - For each enabled method, a rectangle is drawn at the detected position:
     - White (4px): `cv::TM_CCOEFF_NORMED`
     - Gray (2px): `cv::TM_SQDIFF_NORMED`
     - Black (1px): `cv::TM_CCORR_NORMED`
   - All rectangles are drawn at the same position, only thickness and grayscale value differ.

6. **Debug:**  
   - Controlled via `debug_level` (see table above).
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


