# ArmorPaint Studio

A studio fork of [ArmorPaint](https://github.com/armory3d/armorpaint), the open-source 3D PBR texture painter. It adds
what a VFX and animation pipeline needs: OpenColorIO, studio texture export, ShotGrid, native UDIMs, and Substance
materials (`.sbs`/`.sbsar`) rendered by Adobe Designer's `sbsrender`.

Tested on macOS (Apple Silicon). The Linux code is written but not validated yet, except subzero on Rocky Linux 9.4.

## Download

| | |
|---|---|
| **subzero** (standalone) | [Releases](https://github.com/guillaumeparra/armorpaint-studio/releases): `subzero-<version>.tar.gz` (folder with a launcher and a rez package) or `subzero-<version>-py3-none-any.whl` (pip). Does not need ArmorPaint. |
| **ArmorPaint Studio** | [Releases](https://github.com/guillaumeparra/armorpaint-studio/releases): `ArmorPaint-Studio-<version>-macos-arm64.zip`, for macOS 14 or later on Apple Silicon. Other systems: build from source ([below](#build)). |

### Install ArmorPaint Studio (macOS)

```sh
gh release download armorpaint-studio-v0.8.0 -R guillaumeparra/armorpaint-studio   # or from the Releases page
ditto -x -k ArmorPaint-Studio-0.8.0-macos-arm64.zip .
mv ArmorPaint-Studio-0.8.0-macos-arm64/ArmorPaint.app /Applications/
sh ArmorPaint-Studio-0.8.0-macos-arm64/setup_python.sh   # Python for the export window and subzero (from PyPI)
```

The app is not notarized by Apple. After a browser download, run
`xattr -dr com.apple.quarantine /Applications/ArmorPaint.app` once, or use System Settings > Privacy & Security > Open
Anyway. Settings and uninstall are covered in the [package README](../paint/plugins/external/release/studio/README.md),
which also ships in the zip.

### Install subzero

It needs Adobe Substance 3D Designer installed and licensed on the machine: subzero drives its `sbsrender`.

```sh
gh release download subzero-v0.1.0 -R guillaumeparra/armorpaint-studio   # or from the Releases page
tar xzf subzero-0.1.0.tar.gz

# with rez: nothing is installed (use your studio's package names)
rez env python PySide6 opencolorio numpy -- ./subzero-0.1.0/bin/subzero

# without rez: a Python of its own inside the folder (downloads from PyPI), then run
sh subzero-0.1.0/setup.sh
subzero-0.1.0/bin/subzero
```

pip, rez packages, the command line and all settings are covered in the
[subzero guide](../paint/plugins/external/release/subzero/README.md), which also ships in the package.

## Features

**Color management**: OpenColorIO 2.5, built in.
- The viewport goes through the OCIO display, view and look.
- Imported textures are read in their own color space and converted to the project's working space, such as ACEScg.
- Each exported texture is converted to the color space you pick. Checked against oiiotool, ociobakelut and Nuke.

**Texture export**
- A Qt export window replaces the stock dialog. It offers studio presets, a live preflight, file names from
  templates, and texture sets.
- Formats: PNG 8/16, TIFF 8/16/32, JPEG, EXR half/float (new EXR engine), plus computed channels and a JSON manifest.
- Batch mode: `--background` exports with no window, a JSON report and exit codes.

**ShotGrid**: the `tk-armorpaint` Toolkit engine.
- Workfiles and Publish: a copy of the project, the exported textures, `<UDIM>` sequences and a thumbnail.
- The context is saved in the `.arm` file. An offline mode queues publishes and replays them later, with an audit log.

**UDIM**
- OBJ and FBX import fixes, and one texture set per material and per tile.
- Export per texture set and per tile.

**subzero**: Substance materials.
- Open a `.sbs` or `.sbsar`, set its parameters in a panel styled like Painter's, and send it to ArmorPaint as a
  regular material. With Live on, every change updates it.
- EXR by default, an OCIO color space for each map, and a map viewer with exposure, gamma and saturation.
- Standalone, without ArmorPaint: the same window exports to a folder and has a library browser. A command line
  (`render`, `info`, `locate`) serves scripts.

**Setup**: a `studio.env` file configures the studio features at startup, even when ArmorPaint is launched from the
Finder or the Dock.

## Where things are

- **Plugins**: all studio code lives in [`paint/plugins/external/`](../paint/plugins/external/), which ArmorPaint
  compiles through its plugin hook. It holds:
  - the C modules (`studio_*.c`, `subzero.c`);
  - the Python and Qt windows (`assets/`);
  - the Toolkit engine (`tk-armorpaint/`);
  - the tests (`.tests/`) and the tools (`tools/`).
- **Core patches**: each change to an ArmorPaint file is a small patch, marked `STUDIO-PATCH(<id>)` in the code. Each
  is listed in [`PATCHES.md`](../paint/plugins/external/PATCHES.md) with its test and how to tell whether upstream
  has fixed it. There are two kinds:
  - bug fixes: importers (OBJ, FBX, TIFF LZW), deflate bounds, masks, Merge Group undo, derivative bake, Metal export,
    `.arm` paths, headless mode;
  - integration points: menu bar, preferences tab, project data, export window, OCIO viewport and import, UDIM core.

## Branches

- `upstream`: [armory3d/armorpaint](https://github.com/armory3d/armorpaint) as is. Nothing is ever committed to it.
- `studio` (default): `upstream` plus the patches and the plugins.

Upstream is merged in, never rebased. No pull request is sent upstream.

```sh
git fetch armory3d
git checkout upstream && git merge --ff-only armory3d/main
git checkout studio && git merge upstream      # a git hook then checks every patch (tools/check_patches.sh)
```

## Build

```sh
paint/plugins/external/tools/armorpaint.sh     # macOS: builds OCIO the first time, compiles, then launches
cd paint && ../base/make --compile             # compile only, once OCIO is built (tools/build_ocio.sh)
python3 paint/plugins/external/tools/release_studio.py --check   # macOS release zip, from a clean build
```

## Documentation

These documents are in French:
- [`paint/plugins/external/README.md`](../paint/plugins/external/README.md): every module, setting and test.
- [`docs/`](../paint/plugins/external/docs/): the artist guide, operations, and the ShotGrid acceptance test.
- [`ARMORPAINT_ANALYSE.md`](../ARMORPAINT_ANALYSE.md): an analysis of ArmorPaint.
- [`ARMORPAINT_LOTS.md`](../ARMORPAINT_LOTS.md): the roadmap and its status.
- [`ARMORPAINT_UDIM.md`](../ARMORPAINT_UDIM.md): the UDIM design.

License: zlib, like ArmorPaint ([`license.md`](../license.md)).
