# rpicam-apps
This is a small suite of libcamera-based applications to drive the cameras on a Raspberry Pi platform.

>[!WARNING]
>These applications and libraries have been renamed from `libcamera-*` to `rpicam-*`. Symbolic links to allow users to keep using the old application names have now been removed.

Runtime Control (feature/rpicam-rt)
--------------------------------------

This fork adds a runtime control feature: a running `rpicam-vid` listens on a
Unix domain socket and accepts plain-text commands — no restart required. The
companion tools `rpicam-rt` (Qt GUI) and `rpicam-rt-cli` (terminal UI) allow
live adjustment of camera parameters. The Qt preview additionally supports
interactive ROI (region of interest) selection.

- Feature reference: [docs/rpicam_rt.md](docs/rpicam_rt.md)
- Interactive ROI selection: [docs/qt_preview_roi_selection.md](docs/qt_preview_roi_selection.md)
- Installation guide: [docs/INSTALL_RT_ROI.md](docs/INSTALL_RT_ROI.md)

Build
-----
For usage and build instructions, see the official Raspberry Pi documentation pages [here.](https://www.raspberrypi.com/documentation/computers/camera_software.html#building-libcamera-and-rpicam-apps)

For Developers
--------------

This project uses [pre-commit](https://pre-commit.com/) to run formatting and linting checks on each commit. To install:

```sh
pip install pre-commit
pre-commit install
pre-commit install --hook-type commit-msg
```

License
-------

The source code is made available under the simplified [BSD 2-Clause license](https://spdx.org/licenses/BSD-2-Clause.html).

Status
------

[![ToT libcamera build/run test](https://github.com/raspberrypi/rpicam-apps/actions/workflows/rpicam-test.yml/badge.svg)](https://github.com/raspberrypi/rpicam-apps/actions/workflows/rpicam-test.yml)
