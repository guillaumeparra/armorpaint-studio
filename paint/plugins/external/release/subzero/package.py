# rez package of subzero (standalone). From the unpacked release folder: rez build --install (or --prefix <repo>).
# Python and its modules come from the studio's rez packages: add them to requires with the studio's names, for
# example "PySide6-6.5+", "opencolorio-2.2+" and "numpy" (color management is optional). Without them, bin/subzero
# takes $SUBZERO_PYTHON or $STUDIO_PYTHON.

name = "subzero"

version = "@VERSION@"

description = "subzero: .sbs/.sbsar materials rendered by sbsrender, exported to a folder (window and command line)"

authors = ["armorpaint-studio"]

tools = ["subzero"]

requires = ["python-3.9+<3.13"]

build_command = "python {root}/rezbuild.py {install}"


def commands():
    env.PATH.prepend("{root}/bin")  # noqa: F821 — rez
    env.PYTHONPATH.prepend("{root}/python")  # noqa: F821 — rez
