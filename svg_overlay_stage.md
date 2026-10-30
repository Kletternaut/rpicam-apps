# svg_overlay — konfigurierbares SVG-Logo als Post-Processing-Stage

Stand: 30.10.2026. Diese Stage blendet eine konfigurierbare SVG-Grafik (z. B. ein
Logo) dauerhaft in das Hauptbild von rpicam-apps ein. Position, Größe und
Transparenz kommen aus der JSON-Konfiguration; im Qt-Preview ist der
Logo-Bereich optional klickbar und öffnet eine URL.

## Features

- **Einmalige Rasterisierung:** Die SVG wird nur beim Start in `Configure()`
  gerastert (librsvg + cairo). Pro Frame läuft ausschließlich ein
  Alpha-Blend des vorberechneten Pixelbilds — keine SVG-Parsing-Kosten im
  Livestream.
- **Anker-Semantik ohne Überraschungen:** `x`/`y` sind 0..1, wobei
  `0` = bündig links/oben und `1` = bündig rechts/unten. Werte dazwischen
  skalieren linear über den verbleibenden Platz. Es gibt keine Kombination,
  die das Logo aus dem Bild schiebt — kein Start-Crash mehr.
- **Höhe folgt dem Seitenverhältnis:** `width` ist die echte Logo-Breite
  relativ zum Bild; die Höhe ergibt sich automatisch aus dem
  SVG-Seitenverhältnis. Ein `height`-Schlüssel wird ignoriert (Log-Hinweis).
- **Kantenglättung:** Die SVG wird bei Referenzauflösung gerastert und mit
  Flächenmittelung (Alpha-korrekt, gleicher Ansatz wie die Gamepad-Stage) auf
  die Zielgröße verkleinert. Dünne Konturen überleben das Verkleinern.
- **Klickbare URL (Qt-Preview):** Optional öffnet ein Linksklick auf den
  Logo-Bereich eine `http(s)`-URL im Browser. In aufgezeichneten Videos ist
  die URL naturgemäß nicht anklickbar.
- **Sauberer YUV420-Blend:** Luma je Pixel, Chroma je geteiltem 2×2-Block,
  mit Berücksichtigung des Stream-Farbraums (BT.601/709/2020, limited/full).

## Build

Die Stage wird als eigenes Modul `svg-overlay-postproc.so` gebaut. Sie ist
optional: Ohne `librsvg2-dev`/`libcairo2-dev` wird sie übersprungen und der
Rest von rpicam-apps baut normal weiter.

```sh
sudo apt install librsvg2-dev libcairo2-dev
meson setup build --reconfigure
ninja -C build
sudo meson install -C build
```

## Konfiguration

Beispiel (`assets/svg_overlay.json`):

```json
{
    "svg_overlay": {
        "svg_path": "/usr/local/share/rpi-camera-assets/sample_logo.svg",
        "x": 0.43,
        "y": 0.02,
        "width": 0.14,
        "opacity": 0.85,
        "url": "https://example.com"
    }
}
```

| Schlüssel | Bereich | Bedeutung |
|---|---|---|
| `svg_path` | Pfad | SVG-Datei (Pflicht) |
| `x`, `y` | 0..1 | Anker: 0 = bündig links/oben, 1 = bündig rechts/unten |
| `width` | 0..1 | echte Logo-Breite als Anteil der Bildbreite |
| `opacity` | 0..1 | globale Deckkraft, multipliziert mit dem SVG-Alpha |
| `url` | optional | `http(s)`-Ziel für den Klick im Qt-Preview |

Start:

```sh
rpicam-vid -t 0 --post-process-file /usr/local/share/rpi-camera-assets/svg_overlay.json
```

## Dateien

- `post_processing_stages/svg_overlay_stage.cpp` — Stage-Implementierung
- `post_processing_stages/meson.build` — Modul-Build (Abschnitt `svg-overlay-postproc`)
- `assets/svg_overlay.json` — Beispielkonfiguration
- `assets/sample_logo.svg` — Beispiel-Logo
- `preview/preview_plane_resample.hpp` — gefilterte Resampling-Stufen für den
  Logo-Bereich im Qt-Preview (Full-Frame bleibt auf dem schnellen
  Nearest-Neighbour-Pfad)
- `docs/svg-overlay-stage.md` — technische Dokumentation

## Hinweise

- Die klickbare Fläche entspricht dem konfigurierten Rechteck des Logos im
  Preview; die URL greift nur im Live-Preview (`--preview-backend qt`).
- Das SVG-Seitenverhältnis wird aus den intrinsischen Dimensionen gelesen;
  fehlen sie, wird quadratisch angenommen.
