# subzero @VERSION@

subzero opens `.sbsar` and `.sbs` materials, shows their parameters in a panel laid out like Painter's Shader
Settings, renders them with `sbsrender` and exports the textures to a folder. The same tool runs inside ArmorPaint
Studio, where it sends the material to the project. This package is the standalone version, for any pipeline:

- a **window**: open a package (drop one on it, or pick it in the library browser), set parameters, presets, size
  and format, look at the maps in the viewer, then **Export**. With **Live** on, every change is exported again.
- a **command line** for scripts and render machines (no Qt needed): `subzero render`, `subzero info`,
  `subzero locate`, with JSON on stdout.

Adobe Substance 3D Designer is not included. subzero drives its `sbsrender` (and `sbscooker` for `.sbs` files),
which must be installed and licensed on the machine.

## Install

Requirements: macOS or Linux (tested on macOS 27 and Rocky Linux 9.4), Python 3.9 or newer (`setup.sh`: 3.9 to 3.12,
the versions its pinned wheels cover). The window needs PySide6. Color management is optional and needs opencolorio
and numpy.

Nothing is installed outside the folder you choose, and no admin rights are needed. Besides the exported materials,
subzero writes only its cache (`SUBZERO_CACHE`), the library browser's `browser.json` (`SUBZERO_CONFIG_DIR`), and a
studio.env when **Settings > Save** or **Locate sbsrender...** is used. subzero itself makes no network access.

**Folder (tarball)**. Unpack `subzero-@VERSION@.tar.gz` anywhere, then create its Python (network access to PyPI):

    sh subzero-@VERSION@/setup.sh            # venv/ inside the folder, pinned versions of requirements.txt
    subzero-@VERSION@/bin/subzero

`bin/subzero` uses `$SUBZERO_PYTHON`, else that venv, else `$STUDIO_PYTHON`, else `python3` (in a rez environment:
`$SUBZERO_PYTHON`, else the resolved Python). It can be linked into the PATH (`ln -s …/bin/subzero ~/bin/`). On
Linux, `share/applications/subzero.desktop` is a desktop entry: replace `@ROOT@` with the folder and copy it to
`~/.local/share/applications/`.

**pip**. Into any Python 3.9+ environment:

    pip install "subzero-@VERSION@-py3-none-any.whl[all]"    # window and color management
    pip install subzero-@VERSION@-py3-none-any.whl           # command line only, or a Python that has PySide6

Extras: `window` (PySide6), `color` (opencolorio, numpy), `all`. Then `subzero` or `python -m subzero`.

**rez**. To try it, nothing needs installing: from the unpacked folder, with your studio's package names,

    rez env python PySide6 opencolorio numpy -- ./bin/subzero

To get a `subzero` package that only you see:

    rez build --install       # copies the package into your local packages path (rez config local_packages_path)
    rez env subzero PySide6 opencolorio numpy -- subzero

`rez build` also leaves a `build/` folder next to `package.py`. Delete `<local packages path>/subzero/` to remove
the package. Publishing it for everyone (`rez release`, or `rez build --install --prefix <shared repository>`) is a
pipeline step: first add the studio's PySide6, opencolorio and numpy packages to `requires` in `package.py`.

## Window

    subzero [package.sbsar | package.sbs | folder of an exported material]

The first export asks for a folder, unless `SUBZERO_EXPORT_DIR` is set. Each material goes to its own subfolder
`<folder>/<Name>/`: the textures `<Name>_<channel>.<ext>` (basecolor, opacity, occlusion, roughness, metallic,
normal, height) and the recipe `subzero.json`: package, graph, preset, values, size, format, outputs, color spaces
and sbsrender version. Opening that folder again restores every setting; **Update** re-exports in place, **Duplicate**
exports to a new subfolder, **Show** opens the folder. A decal (base color with a useful alpha) is split into
base color and opacity maps. The same folders open in ArmorPaint Studio.

If `sbsrender` is not found, the window says so; **Locate sbsrender...** picks it and saves it to studio.env.

The window starts at 512 px (`SUBZERO_SIZE`), so each change renders in a fraction of a second; pick the final size
before exporting. Previews are rendered at the chosen size, so the viewer shows what will be exported.

**Viewer** (button **Viewer**, `SUBZERO_VIEWER=0` to start without it): a square panel as tall as the window, left of
the settings, showing the maps of the last render (the exported files once exported).

- Keys **1** to **9** (keypad, or the top row by its position, AZERTY included) or a click on a thumbnail pick the
  map; **0** is 1:1, **F** fits, **+**/**-** zoom. Wheel zooms under the cursor, left or middle drag pans,
  double-click toggles fit and 1:1. The footer gives the file, size, depth and the values under the cursor.
- Three sliders above the image, as in Nuke's viewer: **exposure** (stops) and **saturation** apply to scene-linear
  values, **gamma** after the display transform. A click on a slider's name resets it.
- Color maps go through OCIO: file color space → scene_linear → display and view of the Settings panel. Data maps
  (roughness, normal, height…) are shown as they are. Without opencolorio and numpy: sRGB, and no sliders without
  numpy.

**Library browser** (button **Browser**): the `.sbsar` and `.sbs` packages of a folder with their thumbnails, as
Painter's shelf. One tab per folder of `SUBZERO_LIBRARY` (separated by `:`), then one per folder added with **+**
(kept in `browser.json` of `SUBZERO_CONFIG_DIR`; close the tab to remove it). Subfolders are listed, hidden ones are
skipped. Double-click (or drag onto the window) opens a package; the search field filters by name; right-click shows
the file or copies its path. Thumbnails come from the packages themselves (the graph icon) and are cached.

## Command line

    subzero render <package | material folder> [-o DIR] [--name NAME] [--graph URL] [--preset LABEL]
                   [--set ID=VALUE]... [--entry ID=IMAGE]... [--map OUTPUT=CHANNEL|none]...
                   [--size 2048] [--format exr16|exr32|png|tga|tif|jpg]
    subzero info <package> [--graph URL]      # graphs, parameters (id, type, default, range), presets, outputs
    subzero locate                             # sbsrender and sbscooker found, and their version
    subzero --version

`render` writes `<DIR>/<Name>/` like the window (default DIR: `$SUBZERO_EXPORT_DIR`, else the current folder).
Given a material folder, it renders it again in place with its recipe, which options override: for example
`subzero render ~/subzero/ColorfulPlanks --size 4096`. Textures of a previous render that the new one replaces
(another format, size or name) are deleted. Vectors and colors are written `--set wood_color=0.8,0.2,0.1`.

The result is JSON on stdout (`{"ok": true, "result": {"folder", "files": {channel: path}, "spaces", "size",
"format", "seconds", "warnings", ...}}`), messages go to stderr. Exit code 0 on success, 1 when rendering fails,
2 for a bad argument or setting.

## Settings

Environment variables, or lines of a studio.env file: `$STUDIO_ENV` (one or more files separated by `:`), else the
studio.env of ArmorPaint Studio's folder. A real environment variable wins. `share/studio.env.example` lists them.

| Variable | Default | |
|---|---|---|
| `SUBZERO_SBSRENDER` | detection | sbsrender (file, or Designer's folder); else ArmorPaint Studio's preference, else `/Applications` (macOS) or `/opt/Adobe` (Linux), then the PATH |
| `SUBZERO_EXPORT_DIR` | asked once | folder of the exported materials |
| `SUBZERO_EXPORT_TEMPLATE` | `{name}_{channel}` | texture names: `{name}` (material folder), `{channel}`, `{size}` (width) |
| `SUBZERO_FORMAT` | `exr16` | `exr16`, `exr32`, `png`, `tga`, `tif`, `jpg` |
| `SUBZERO_EXR_COMPRESSION` | `none` | `none`, `zip`, `piz`, `pxr24`, `b44` |
| `SUBZERO_SIZE` | `512` (window), `2048` (`render`) | default width, a power of two |
| `SUBZERO_NORMAL_FORMAT` | `opengl` | `opengl` or `directx` (green channel of the normal map) |
| `SUBZERO_LIVE` | `1` | export again after each change, once exported |
| `SUBZERO_ENGINE` | `mtl` on macOS | sbsrender engine (`sse2`: CPU, for machines without a GPU) |
| `SUBZERO_CACHE` | `~/.cache/subzero` | previews and cooked `.sbs` files (`~/Library/Caches/subzero` on macOS) |
| `SUBZERO_VIEWER` | `1` | viewer open when the window starts |
| `SUBZERO_LIBRARY` | none | folders of the library browser, separated by `:` (one tab each) |
| `SUBZERO_CONFIG_DIR` | per user | window settings (`browser.json`): `~/Library/Application Support/subzero` on macOS, `~/.config/subzero` on Linux |

**Color** (with opencolorio and numpy). `OCIO` is the configuration (default `ocio://studio-config-latest`). Each map
is converted from what sbsrender writes (sRGB-encoded base color, raw data) to its color space:
`SUBZERO_OCIO_COLOR` (color maps, default `srgb_tx`), `SUBZERO_OCIO_DATA` (data maps, default `Raw`), or per map
`SUBZERO_OCIO_BASECOLOR`, `SUBZERO_OCIO_ROUGHNESS`… Thumbnails and color swatches go through the display and view of
`STUDIO_OCIO_DISPLAY`, `STUDIO_OCIO_VIEW` and `STUDIO_OCIO_LOOK`. The window's **Settings** panel sets all of them
and can save them to studio.env.

## Notes

- Linux under X11 (Xorg, `ssh -X`, remote desktop) needs the system libraries of Qt's xcb plugin; `setup.sh` lists
  the missing ones (`sudo dnf install xcb-util-cursor xcb-util-wm …` on Rocky 9.4, all in AppStream). A Wayland
  session does without them.
- sbsrender writes TGA, EXR, PNG, TIFF and JPEG; previews in the window are TGA files in the cache, at the chosen
  size until the first export.
- `subzero render` never imports Qt, except to split a decal exported as PNG or TIFF; without PySide6 that step is
  skipped with a warning.
