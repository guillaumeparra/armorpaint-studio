#!/bin/sh
# Crée .shotgrid/venv, le python du sidecar ShotGrid avec PySide6 (fenêtres Qt de Toolkit), pour la démo
# (tools/armorpaint.sh --shotgrid-demo, .tests/demo_workfiles.sh). Propre à la machine, donc hors de git ; les sources
# de Toolkit, elles, sont versionnées dans .shotgrid/ (voir .shotgrid/README.md). Réseau requis la première fois.
set -e
EXT=$(cd "$(dirname "$0")/.." && pwd)
D=$EXT/.shotgrid
[ -d "$D/tk-core/python/sgtk" ] || { echo "$D/tk-core missing (see $D/README.md)" >&2; exit 1; }
if ! "$D/venv/bin/python" -c "import PySide6" 2> /dev/null; then
	rm -rf "$D/venv"
	python3 -m venv "$D/venv"
	"$D/venv/bin/pip" install -q PySide6==6.10.3
fi
echo "ShotGrid demo ready: $D"
