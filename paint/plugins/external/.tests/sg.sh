#!/bin/sh
# Banc des lots C2 et B3 (ShotGrid), hors de l'app : lecteur JSON, noms des textures et superviseur sous ASan/UBSan,
# puis le sidecar Python.
# Usage : .tests/sg.sh [dossier de travail]
# Toolkit : STUDIO_TEST_TK_CORE (défaut : .shotgrid/tk-core du dépôt) et STUDIO_TEST_TK_APPS (défaut : .shotgrid/, les
# apps Workfiles et Publish). Python du sidecar : STUDIO_SG_PYTHON, défaut .shotgrid/venv s'il a PySide6
# (tools/setup_shotgrid.sh), sinon python3 : sans PySide6, les tests des fenêtres et du publish sont sautés.
# Un autre tk-core : git clone --depth 1 --branch <tag> https://github.com/shotgunsoftware/tk-core (pas le paquet PyPI
# « tk-core », qui n'est pas celui d'Autodesk).

set -e
T=$(cd "$(dirname "$0")" && pwd)
EXT=$(dirname "$T")
W=${1:-$(mktemp -d)}
mkdir -p "$W"
SG=$EXT/.shotgrid
: "${STUDIO_TEST_TK_CORE:=$SG/tk-core}" "${STUDIO_TEST_TK_APPS:=$SG}"
if [ -z "$STUDIO_SG_PYTHON" ]; then
	STUDIO_SG_PYTHON=python3
	"$SG/venv/bin/python" -c "import PySide6" 2> /dev/null && STUDIO_SG_PYTHON=$SG/venv/bin/python
fi
export STUDIO_TEST_TK_CORE STUDIO_TEST_TK_APPS
CFLAGS="-std=c11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"

echo "studio_json"
cc $CFLAGS "$T/json_test.c" "$EXT/studio_json.c" -o "$W/json_test"
"$W/json_test"

echo "studio_naming"
cc $CFLAGS "$T/naming_test.c" "$EXT/studio_naming.c" -o "$W/naming_test"
"$W/naming_test" "$W"

echo "studio_sg (supervisor)"
cc $CFLAGS "$T/sg_super_test.c" "$EXT/studio_sg.c" "$EXT/studio_json.c" "$EXT/studio_proc.c" -o "$W/sg_super_test"
"$W/sg_super_test" "$T" "$W"

if [ -d "$STUDIO_TEST_TK_CORE/python/sgtk" ]; then
	echo "studio_sg_bridge.py (sidecar, $STUDIO_SG_PYTHON)"
	# Sortie filtrée après coup : derrière un tube, le code de retour serait celui de grep, pas celui des tests.
	rc=0
	"$STUDIO_SG_PYTHON" -W error::ResourceWarning "$T/sg_test.py" > "$W/sg_test.out" 2>&1 || rc=$?
	grep -v "RuntimeWarning\|_pkgs_loaded\|^  import" "$W/sg_test.out" || true
	exit $rc
else
	echo "skip sidecar tests (no tk-core in $STUDIO_TEST_TK_CORE)"
fi
