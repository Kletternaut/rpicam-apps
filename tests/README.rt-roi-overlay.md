# RT ROI overlay regression tests

The standalone tests run without a Raspberry Pi, libcamera, Qt, or a display.
The configuration test additionally needs the Boost headers already required
by the main application. They cover control ranges, snapping, hit geometry,
transparent controls, configuration defaults and rejected input.

When Qt Widgets and Svg development packages are installed, CMake also builds
an offscreen Qt test of the actual SVG renderer/controller: +/- actions,
captured slider drags, disabled controls, and hide/show toggle behavior.
This target does not require libcamera. CMake explicitly reports skipped
targets when optional dependencies are missing.

```sh
cmake -S tests -B build-overlay-tests
cmake --build build-overlay-tests
ctest --test-dir build-overlay-tests --output-on-failure
```

For a nonstandard Boost installation, configure with
`-DBOOST_INCLUDE_DIR=/path/containing/boost`. On a multi-configuration generator,
add `--config Release` when building and `-C Release` when running CTest.

All three targets were compiled and passed with MSVC 19.44 and Qt 5.15.2 on
Windows. Set `RT_ROI_TEST_IMAGE` to a PNG path to save the Qt test's transparent
rendering sample. The full application build and camera checks were not run in
that environment. No automated target validates the full preview event routing
and camera lifecycle. Before deploying, build on the target Raspberry Pi and
verify:

1. Press a slider, drag outside its row and outside the preview, then release.
   The slider clamps at its endpoint and no ROI is selected.
2. Click row labels, transparent row interiors and +/- buttons. Only the
   intended control responds; no selection rectangle appears. Right-clicking
   anywhere within a row must not reset the ROI.
3. Start ROI selection outside all rows, drag across a row, and release. The
   ROI gesture keeps ownership; no slider or button changes. The selected
   rectangle stays inside the preview and preserves its aspect ratio.
4. While dragging a slider, press/release a second button and scroll. No ROI
   reset, ROI selection or second control action occurs. Release all buttons
   before starting the next gesture.
5. Resize the preview and verify drawing and clickable areas still align.
   Repeat with configured position, size, scale and transparent background.
   At `opacity: 0`, former row positions must be available for ROI selection.
   Hide/show the panel using its persistent toggle; the toggle must keep its
   own clickable area while hidden rows release theirs to ROI selection.
6. Change zoom through the overlay, then reset with a right-click outside it.
   Change ROI through the existing runtime client and verify the overlay's
   zoom/ROI values follow it. Test both main-only and main+lores preview on Pi 5.
7. Adjust numeric controls, mode choices, auto/manual gain and shutter, AWB
   gain pairs, and framerate. Confirm the existing runtime client remains
   connected and changing its settings updates the overlay.
8. Stop/restart the camera and close the window during a drag. Check for
   hangs, crashes, stale gestures or controls active while the camera stops.
9. Inspect a recording to confirm overlay graphics are absent from encoded
   frames. Check the explicit error on a preview backend without SVG support.
