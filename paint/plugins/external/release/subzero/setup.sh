#!/bin/sh
# Creates the Python of subzero: a venv with the pinned versions of requirements.txt (PySide6 for the window,
# opencolorio and numpy for color management). macOS and Linux (Rocky 9.4: python3 3.9, or python3.11 / python3.12
# from AppStream). Needs network access to PyPI.
#
#   sh setup.sh [venv folder]     default: venv/ next to this file, which bin/subzero uses; a shared studio folder
#                                 works too (then SUBZERO_PYTHON=<folder>/bin/python)
#   PYTHON=<interpreter> to choose one (3.9 to 3.12).
set -e
ROOT=$(cd "$(dirname "$0")" && pwd)
V=${1:-$ROOT/venv}

ok_python() { "$1" -c 'import sys; sys.exit(not ((3, 9) <= sys.version_info[:2] <= (3, 12)))' 2> /dev/null; }
if [ -n "${PYTHON:-}" ]; then
	ok_python "$PYTHON" || { echo "$PYTHON: Python 3.9 to 3.12 required" >&2; exit 1; }
else
	for p in python3.12 python3.11 python3.10 python3.9 python3; do
		if command -v "$p" > /dev/null && ok_python "$p"; then
			PYTHON=$p
			break
		fi
	done
	[ -n "${PYTHON:-}" ] || { echo "no Python 3.9 to 3.12 found (Rocky: sudo dnf install python3.12), or set PYTHON" >&2; exit 1; }
fi

if ! "$V/bin/python" -c "import PySide6, PyOpenColorIO, numpy" 2> /dev/null; then
	rm -rf "$V"
	"$PYTHON" -m venv "$V"
	"$V/bin/pip" install -q --upgrade pip
	"$V/bin/pip" install -q -r "$ROOT/requirements.txt"
fi

# Linux: system libraries of the Qt 6 xcb plugin (PySide6 wheels do not ship them), all in AppStream on Rocky 9.4.
# A GNOME Wayland session does without them (wayland plugin); X11 (Xorg, ssh -X, remote desktop) needs them.
if [ "$(uname -s)" = Linux ]; then
	MISSING=
	LDCONFIG=$(command -v ldconfig || echo /sbin/ldconfig)
	for pair in libxcb-cursor.so.0:xcb-util-cursor libxkbcommon-x11.so.0:libxkbcommon-x11 libxcb-icccm.so.4:xcb-util-wm \
		libxcb-image.so.0:xcb-util-image libxcb-keysyms.so.1:xcb-util-keysyms libxcb-render-util.so.0:xcb-util-renderutil \
		libEGL.so.1:libglvnd-egl libGL.so.1:libglvnd-glx libfontconfig.so.1:fontconfig libdbus-1.so.3:dbus-libs; do
		"$LDCONFIG" -p 2> /dev/null | grep -q "${pair%%:*}" || MISSING="$MISSING ${pair#*:}"
	done
	[ -z "$MISSING" ] || echo "missing system libraries for Qt: sudo dnf install$MISSING"
fi

# Check: modules, and a Qt window off screen.
QT_QPA_PLATFORM=offscreen PYTHONPATH="$ROOT/python" "$V/bin/python" - << 'PY'
import sys
import numpy
import PyOpenColorIO as OCIO
from PySide6 import QtCore, QtWidgets
import subzero
app = QtWidgets.QApplication([])
config = OCIO.Config.CreateFromFile("ocio://studio-config-latest")
print("subzero %s: Python %d.%d, PySide6 %s, OpenColorIO %s (%s), numpy %s" % (
    subzero.__version__, sys.version_info[0], sys.version_info[1], QtCore.qVersion(), OCIO.__version__,
    config.getName(), numpy.__version__))
PY
if [ "$V" = "$ROOT/venv" ]; then
	echo "ready: $ROOT/bin/subzero"
else
	echo "ready: SUBZERO_PYTHON='$V/bin/python' $ROOT/bin/subzero"
fi
