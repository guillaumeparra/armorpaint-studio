#!/bin/sh
# Creates the Python of ArmorPaint Studio's Qt windows (texture export, subzero): a venv with the pinned versions of
# requirements.txt (PySide6, opencolorio, numpy), then points ArmorPaint to it with STUDIO_PYTHON in studio.env.
# Needs network access to PyPI. Nothing is installed outside the venv folder and studio.env.
#
#   sh setup_python.sh [venv folder]    default: venv/ in ArmorPaint's user folder
#                                       (~/Library/Application Support/ArmorPaint on macOS)
#   PYTHON=<interpreter> to choose one (3.9 to 3.12).
set -e
ROOT=$(cd "$(dirname "$0")" && pwd)

# ArmorPaint's user folder, where it reads studio.env (same rule as the app).
case $(uname -s) in
Darwin) USER_DIR="$HOME/Library/Application Support/ArmorPaint" ;;
*)
	if [ -d "$HOME/.ArmorPaint" ]; then
		USER_DIR="$HOME/.ArmorPaint"
	else
		USER_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/ArmorPaint"
	fi
	;;
esac
V=${1:-$USER_DIR/venv}

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
	[ -n "${PYTHON:-}" ] || { echo "no Python 3.9 to 3.12 found (python.org, Homebrew, or Rocky: sudo dnf install python3.12), or set PYTHON" >&2; exit 1; }
fi

if ! "$V/bin/python" -c "import PySide6, PyOpenColorIO, numpy" 2> /dev/null; then
	rm -rf "$V"
	mkdir -p "$(dirname "$V")"
	"$PYTHON" -m venv "$V"
	"$V/bin/python" -m pip install -q --upgrade pip
	"$V/bin/python" -m pip install -q -r "$ROOT/requirements.txt"
fi
V=$(cd "$V" && pwd)

# Linux: system libraries of the Qt 6 xcb plugin (PySide6 wheels do not ship them), all in AppStream on Rocky 9.4.
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
QT_QPA_PLATFORM=offscreen "$V/bin/python" - << 'PY'
import sys
import numpy
import PyOpenColorIO as OCIO
from PySide6 import QtCore, QtWidgets
app = QtWidgets.QApplication([])
config = OCIO.Config.CreateFromFile("ocio://studio-config-latest")
print("Python %d.%d, PySide6 %s, OpenColorIO %s (%s), numpy %s" % (
    sys.version_info[0], sys.version_info[1], QtCore.qVersion(), OCIO.__version__, config.getName(),
    numpy.__version__))
PY

# studio.env: STUDIO_PYTHON, unless the file already sets it (then only shown). '…' keeps the path as is.
ENV_FILE="$USER_DIR/studio.env"
case $V in
*"'"*) LINE="STUDIO_PYTHON=\"$(printf '%s' "$V/bin/python" | sed 's/[\\"$]/\\&/g')\"" ;;
*) LINE="STUDIO_PYTHON='$V/bin/python'" ;;
esac
if [ -f "$ENV_FILE" ] && grep -Eq '^[[:space:]]*(export[[:space:]]+)?STUDIO_PYTHON=' "$ENV_FILE"; then
	echo "$ENV_FILE already sets STUDIO_PYTHON, left as is; for this Python:"
	echo "  $LINE"
else
	mkdir -p "$USER_DIR"
	printf '%s\n' "$LINE" >> "$ENV_FILE"
	echo "added to $ENV_FILE:"
	echo "  $LINE"
fi
echo "ready: restart ArmorPaint if it is running"
