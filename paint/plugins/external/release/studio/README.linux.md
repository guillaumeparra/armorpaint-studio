# ArmorPaint Studio @VERSION@ for Linux

[ArmorPaint](https://github.com/armory3d/armorpaint) with what a VFX and animation pipeline needs: OpenColorIO, a studio
texture export, ShotGrid, native UDIMs, and subzero for `.sbs`/`.sbsar` materials. Built from
[armorpaint-studio](https://github.com/guillaumeparra/armorpaint-studio) at `@SHA@`.

## Requirements

- Linux x86_64 with glibc @GLIBC@ or later: Rocky Linux 9 or RHEL 9, or newer. Built and tested on Rocky Linux 9.4.
- A GPU driver with Vulkan: the NVIDIA driver, or Mesa for AMD and Intel.
- An X11 session, or a Wayland session with Xwayland, which GNOME has by default. ArmorPaint draws through X11. Under
  Wayland it runs in Xwayland.
- System libraries. A Rocky workstation has them. Otherwise, from BaseOS and AppStream:
  `sudo dnf install vulkan-loader gtk3 alsa-lib libX11 libXi libXcursor libXrandr openssl-libs`.
- For the texture export window and subzero: Python 3.9 to 3.12. Rocky 9's `python3` works. `setup_python.sh` adds the
  rest.
- For subzero: Adobe Substance 3D Designer for Linux, installed and licensed. subzero drives its `sbsrender`.
- For ShotGrid: ShotGrid Desktop, signed in. Your pipeline configuration also needs the `tk-armorpaint` engine.

## Install

1. Extract the archive, then move the `ArmorPaint` folder anywhere: your home, `/opt`, or a shared drive.
2. Run `sh setup_python.sh`. It creates a Python in `~/.local/share/ArmorPaint/venv` with PySide6 @PYSIDE@,
   OpenColorIO @OCIO@ and numpy @NUMPY@, downloaded from PyPI. Then it adds `STUDIO_PYTHON` to
   `~/.local/share/ArmorPaint/studio.env`. It leaves the file alone if the variable is already set. It also lists the
   system libraries that Qt's windows need and that are missing, with the `dnf` command that installs them.
3. Start `ArmorPaint/ArmorPaint`.

Without step 2, painting, OCIO and the stock export work. The studio export window and subzero will not open, and
their message says why.

## Settings

`~/.local/share/ArmorPaint/studio.env` holds `VAR=value` lines. When `~/.ArmorPaint` exists, ArmorPaint uses that folder
instead. ArmorPaint reads the file at startup, also when launched from a desktop launcher. `studio.env.example` lists the
main variables:
- the OCIO configuration and working space;
- export presets;
- the ShotGrid Python;
- `sbsrender`.

Like ArmorPaint, the app keeps its preferences in its own folder, `ArmorPaint/data/` (`config.json`,
`studio_prefs.json`). Replacing the folder with a new version resets them. When the folder is read-only, as in an
install in `/opt` by root, they go to `~/.local/share/ArmorPaint/`.

## X11 and Wayland

ArmorPaint is an X11 app with a Vulkan renderer. It works the same in an X11 session and in a Wayland session through
Xwayland. The Qt windows (texture export, subzero) follow the session: X11, or native Wayland. Force one with
`QT_QPA_PLATFORM=xcb` or `QT_QPA_PLATFORM=wayland` in `studio.env`.

## Uninstall

1. Delete the `ArmorPaint` folder.
2. Delete `~/.local/share/ArmorPaint/`, which holds the Python, `studio.env` and the preferences of a read-only app.
3. Delete subzero's cache, `~/.cache/subzero`.

## Contents

| | |
|---|---|
| `ArmorPaint/` | the app: `ArmorPaint`, and `data/` with OpenColorIO @OCIO@ (`libstudio_ocio.so`) |
| `setup_python.sh`, `requirements.txt` | the Python of the Qt windows |
| `studio.env.example` | settings |
| `LICENSE.md` | zlib, like ArmorPaint; OpenColorIO, tinyexr and upstream licenses are in `ArmorPaint/data/` |

No Adobe file is included.

Features, the list of changes to ArmorPaint and the documentation are on the
[project page](https://github.com/guillaumeparra/armorpaint-studio).
