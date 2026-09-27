#!/bin/sh
# Crée le python de la fenêtre subzero (chantier E) : PySide6, et PyOpenColorIO et numpy pour les espaces de couleur des
# cartes et le panneau Settings (lot E11) ; sans eux la fenêtre marche, couleur de base en srgb_tx et le reste tel que
# sbsrender l'écrit. PyOpenColorIO est à la version d'OCIO d'ArmorPaint (.ocio/CMakeLists.txt) : ocio://… y donne la
# même configuration. macOS et Linux (Rocky 9.4 : python3 3.9, ou python3.11 / python3.12 d'AppStream). Réseau requis.
#
#   sh tools/setup_subzero.sh [dossier du venv]    défaut : .subzero/venv, hors de git ; un dossier partagé du studio
#                                                  (/opt/studio/subzero…) convient
#   PYTHON=<interpréteur> pour en imposer un (3.9 à 3.12).
# Puis dans studio.env : STUDIO_PYTHON=<dossier>/bin/python (affiché à la fin).
set -e
EXT=$(cd "$(dirname "$0")/.." && pwd)
V=${1:-$EXT/.subzero/venv}
PYSIDE=6.10.3 # comme .shotgrid/venv (tools/setup_shotgrid.sh)
NUMPY=2.0.2   # dernière pour Python 3.9
OCIO=$(sed -n 's/^set(STUDIO_OCIO_TAG "v\([0-9.]*\)".*/\1/p' "$EXT/.ocio/CMakeLists.txt")
[ -n "$OCIO" ] || { echo "OCIO version not found in $EXT/.ocio/CMakeLists.txt" >&2; exit 1; }

# Interpréteur : $PYTHON, sinon le plus récent de 3.12 à 3.9.
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
	"$V/bin/pip" install -q "PySide6==$PYSIDE" "opencolorio==$OCIO" "numpy==$NUMPY"
fi

# Linux : bibliothèques système du greffon xcb de Qt 6 (les roues PySide6 ne les embarquent pas), toutes dans AppStream
# sur Rocky 9.4. Une session GNOME Wayland s'en passe (greffon wayland) ; X11 (Xorg, ssh -X, bureau à distance) les veut.
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

# Vérification : modules, et une fenêtre Qt hors écran.
QT_QPA_PLATFORM=offscreen "$V/bin/python" - << 'PY'
import sys
import numpy
import PyOpenColorIO as OCIO
from PySide6 import QtCore, QtWidgets
app = QtWidgets.QApplication([])
config = OCIO.Config.CreateFromFile("ocio://studio-config-latest")
print("Python %d.%d, PySide6 %s, OpenColorIO %s (%s), numpy %s" % (sys.version_info[0], sys.version_info[1], QtCore.qVersion(),
      OCIO.__version__, config.getName(), numpy.__version__))
PY
echo "subzero Python ready: add to studio.env"
echo "STUDIO_PYTHON='$V/bin/python'"
