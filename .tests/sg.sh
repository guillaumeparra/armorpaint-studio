#!/bin/sh
# Banc des lots C2 et B3 (ShotGrid), hors de l'app : lecteur JSON, noms des textures et superviseur sous ASan/UBSan,
# puis le sidecar Python.
# Usage : STUDIO_TEST_TK_CORE=<clone de tk-core> .tests/sg.sh [dossier de travail]
# Sans STUDIO_TEST_TK_CORE, seuls les tests qui n'ont pas besoin de Toolkit tournent (faux sidecar).
# tk-core : git clone --depth 1 --branch v0.24.2 https://github.com/shotgunsoftware/tk-core (pas le paquet PyPI
# « tk-core », qui n'est pas celui d'Autodesk).

set -e
T=$(cd "$(dirname "$0")" && pwd)
EXT=$(dirname "$T")
W=${1:-$(mktemp -d)}
mkdir -p "$W"
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

if [ -n "$STUDIO_TEST_TK_CORE" ]; then
	echo "studio_sg_bridge.py (sidecar)"
	python3 -W error::ResourceWarning "$T/sg_test.py" 2>&1 | grep -v "RuntimeWarning\|_pkgs_loaded\|^  import"
else
	echo "skip sidecar tests (STUDIO_TEST_TK_CORE not set)"
fi
