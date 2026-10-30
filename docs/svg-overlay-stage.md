# SVG overlay post-processing stage

`svg_overlay` rasterises a configured SVG once and blends it into the YUV420 main stream. It affects the processed image/video, not only the Qt preview.

## Build dependencies

The stage is built together with the other rsvg/cairo based stages. If the
development packages are missing, install them first:

```sh
sudo apt install librsvg2-dev libcairo2-dev
meson setup build --reconfigure
meson compile -C build
sudo meson install -C build
```

## Configuration

Example `svg_overlay` entry:

```json
{
    "svg_overlay": {
        "svg_path": "/usr/local/share/rpi-camera-assets/sample_logo.svg",
        "x": 0.43,
        "y": 0.02,
        "width": 0.14,
        "height": 0.14,
        "opacity": 0.85,
        "url": "https://example.com"
    }
}
```

- `svg_path`: path to the SVG file.
- `x`, `y`: anchor position (`0.0` to `1.0`). `0` keeps the logo flush at the left/top frame edge, `1` flush at the right/bottom edge; values in between scale linearly across the remaining space.
- `width`: actual logo width as a fraction of the frame width (`0.0` to `1.0`).
- The logo height follows automatically from `width` and the SVG's own aspect ratio, so the artwork is never letterboxed, clipped or moved off-frame. A configured `height` key is ignored.
- `opacity`: global opacity (`0.0` to `1.0`), multiplied by the SVG's per-pixel alpha.
- `url` (optional): `http://` or `https://` target. With `--preview-backend qt`, left-clicking the logo's configured rectangle opens the URL in the desktop browser. The URL is not an interactive link in encoded files; it is only clickable in the live preview.

Use the JSON with `rpicam-vid --post-process-config /usr/local/share/rpi-camera-assets/svg_overlay.json`. To combine it with `rt_roi_overlay`, place both top-level stage keys in the same JSON object. The clickable hit area uses the configured rectangle, including any transparent letterboxing within the SVG's aspect-ratio-preserving viewport.
