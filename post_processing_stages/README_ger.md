# TemplateMatchStage – Dokumentation

## Zweck

Die `TemplateMatchStage` führt ein Template Matching mit OpenCV auf einem Videostream durch.  
Das Template wird aus einem zweiten Stream (z.B. RTSP/H264) entnommen und kann optional skaliert werden.  
Das Matching-Ergebnis wird als Rahmen im Bild visualisiert.

---

## Konfigurierbare Parameter (`template_match.json`)

| Parameter           | Typ      | Beschreibung                                                                 |
|---------------------|----------|------------------------------------------------------------------------------|
| match_fps           | int      | Wie oft pro Sekunde das Matching durchgeführt wird (Standard: 5)              |
| active_methods      | [bool]   | Welche Matching-Methoden aktiv sind (nur relevant bei mehreren Methoden)      |
| template_width      | int      | Feste Breite des Templates (optional)                                        |
| template_height     | int      | Feste Höhe des Templates (optional)                                          |
| scale_min           | double   | (Nicht genutzt bei festem Scaling)                                           |
| scale_max           | double   | (Nicht genutzt bei festem Scaling)                                           |
| scale_step          | double   | (Nicht genutzt bei festem Scaling)                                           |
| fixed_scale         | double   | Fester Skalierungsfaktor für das Template (z.B. 0.1)                         |
| match_threshold     | double   | Schwellwert, ab dem ein Match als gültig gilt (z.B. 0.8)                     |
| debug               | bool     | Aktiviert Debug-Ausgaben im Terminal                                         |

**Beispiel:**
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

## Funktionsweise

1. **Streams initialisieren:**  
   - `stream0_`: Hauptbild (z.B. Video)
   - `stream1_`: Template-Bild (z.B. lores oder externer Stream)

2. **Template vorbereiten:**  
   - Optional auf feste Größe skalieren (`template_width`, `template_height`)
   - Dann auf festen Faktor (`fixed_scale`) skalieren

3. **Matching:**  
   - OpenCV `matchTemplate` mit Methode `cv::TM_CCOEFF_NORMED`
   - Score (`maxVal`) und Position (`maxLoc`) werden bestimmt

4. **Visualisierung:**  
   - Ist `maxVal >= match_threshold`, wird ein weißer Rahmen gezeichnet
   - Sonst ein schwarzer Rahmen (optional: Rahmen nur bei Match zeichnen)

5. **Debug:**  
   - Bei aktiviertem Debug werden Score, Skalierung und Position ausgegeben

---

## Hinweise

- Das Template Matching funktioniert am besten, wenn das Template und das Suchbild ähnliche Größen und Kontraste haben.
- Für wechselnde Objektgrößen kann eine dynamische Skalierung implementiert werden (aktuell deaktiviert).
- Die Performance und Erkennungsrate hängen stark von den Parametern und der Bildqualität ab.

---

## Erweiterungsideen

- Farbige Rahmen (z.B. weiss/grau/schwarz) je nach Score
- Dynamische Skalierung wieder aktivieren, falls nötig
- Unterstützung für mehrere Matching-Methoden
- Logging in Datei statt Konsole

## Startparameter Instanzen

## gstreamer
============
rpicam-vid --camera 1 -t 0 --qt-preview --info-text %focus --width 640 --height 480 --lores-height 300 --lores-width 400 --lores-par 1 --codec libav --libav-format mpegts --inline -o - | gst-launch-1.0 fdsrc fd=0 ! tsdemux ! h264parse ! rtph264pay config-interval=1 pt=96 ! udpsink host=192.168.0.30 port=8554

## rpicam-vid
=============
rpicam-vid --info-text FoM:%focus,-FPS:%fps,-Frame:%frame --qt-preview --height 990 --width 1330 --camera 0 -t 0 --preview 500,50,600,400 --lores-width 320 --lores-height 240 --lores-par 1 --post-process-file assets/template_match.json