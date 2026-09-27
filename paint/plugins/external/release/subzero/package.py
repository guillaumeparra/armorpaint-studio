# rez package of subzero (standalone). From the unpacked release folder, `rez build --install` copies it into your
# local packages path (`rez config local_packages_path`, ~/packages by default): only you see it. `rez release` or
# `--prefix <shared repo>` publish it for everyone: that is the TD's step.
# Python comes from the resolve. The window needs PySide6, color management opencolorio and numpy: request them with
# the studio's package names (rez env subzero PySide6 opencolorio numpy), or add them to requires below.

name = "subzero"

version = "@VERSION@"

description = "subzero: .sbs/.sbsar materials rendered by sbsrender, exported to a folder (window and command line)"

authors = ["armorpaint-studio"]

tools = ["subzero"]

requires = ["python-3.9+"]

build_command = "python {root}/rezbuild.py {install}"


def commands():
    env.PATH.prepend("{root}/bin")  # noqa: F821 — rez
    env.PYTHONPATH.prepend("{root}/python")  # noqa: F821 — rez
