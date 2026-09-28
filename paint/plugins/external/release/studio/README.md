# ArmorPaint Studio @VERSION@ for macOS

[ArmorPaint](https://github.com/armory3d/armorpaint) with what a VFX and animation pipeline needs: OpenColorIO, a studio
texture export, ShotGrid, native UDIMs, and subzero for `.sbs`/`.sbsar` materials. Built from
[armorpaint-studio](https://github.com/guillaumeparra/armorpaint-studio) at `@SHA@`.

## Requirements

- macOS 14 or later on Apple Silicon.
- For the texture export window and subzero: Python 3.9 to 3.12 (python.org or Homebrew). `setup_python.sh` adds the
  rest.
- For subzero: Adobe Substance 3D Designer, installed and licensed. subzero drives its `sbsrender`.
- For ShotGrid: ShotGrid Desktop, signed in. Your pipeline configuration also needs the `tk-armorpaint` engine.

## Install

1. Move `ArmorPaint.app` to `/Applications`, or anywhere else.
2. The app is not notarized by Apple. When it comes from a browser download, macOS blocks the first launch. Run
   `xattr -dr com.apple.quarantine /Applications/ArmorPaint.app` once, or open it, then go to System Settings > Privacy &
   Security > Open Anyway. `gh release download` does not need this.
3. Run `sh setup_python.sh`. It creates a Python in `~/Library/Application Support/ArmorPaint/venv` with PySide6
   @PYSIDE@, OpenColorIO @OCIO@ and numpy @NUMPY@, downloaded from PyPI. Then it adds `STUDIO_PYTHON` to
   `~/Library/Application Support/ArmorPaint/studio.env`. It leaves the file alone if the variable is already set.

Without step 3, painting, OCIO and the stock export work. The studio export window and subzero will not open, and
their message says why.

## Settings

`~/Library/Application Support/ArmorPaint/studio.env` holds `VAR=value` lines. ArmorPaint reads it at startup, also
when launched from the Finder or the Dock. `studio.env.example` lists the main variables:
- the OCIO configuration and working space;
- export presets;
- the ShotGrid Python;
- `sbsrender`.

Like ArmorPaint, the app keeps its preferences in its own folder, `ArmorPaint.app/Contents/Resources/out/data/`
(`config.json`, `studio_prefs.json`). Replacing the app with a new version resets them. When the app is read-only,
they go to `~/Library/Application Support/ArmorPaint/`.

## Uninstall

1. Delete `ArmorPaint.app`.
2. Delete `~/Library/Application Support/ArmorPaint/`, which holds the Python, `studio.env` and the preferences of a
   read-only app.
3. Delete subzero's cache, `~/Library/Caches/subzero`.

## Contents

| | |
|---|---|
| `ArmorPaint.app` | the app, arm64, ad hoc signature, OpenColorIO @OCIO@ built in |
| `setup_python.sh`, `requirements.txt` | the Python of the Qt windows |
| `studio.env.example` | settings |
| `LICENSE.md` | zlib, like ArmorPaint; OpenColorIO, tinyexr and upstream licenses are in `ArmorPaint.app/Contents/Resources/out/data/` |

No Adobe file is included.

Features, the list of changes to ArmorPaint and the documentation are on the
[project page](https://github.com/guillaumeparra/armorpaint-studio).
