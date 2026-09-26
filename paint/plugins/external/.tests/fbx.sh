#!/bin/sh
# Banc du patch fbx-split, hors de l'app, sous ASan/UBSan : importeur FBX (paint/plugins/io_fbx/io_fbx.c, ufbx),
# lu par fbx_test.c comme import_mesh_run() le lit.
#
# Usage : .tests/fbx.sh [dossier de travail]     FUZZ=<n> : nombre de mutations (défaut 300, ~2 min)
#         STUDIO_ROOT=<armorpaint> pour tester une autre copie d'armorpaint, STUDIO_UPSTREAM=<ref> (défaut upstream)
#
# 1. same/ : fichiers sans UDIM ni bug connu (nœuds avec et sans uv, second jeu d'uv, couleurs, jusqu'à 64
#    matériaux) : splits par objet et par matériau identiques, octet pour octet, à l'upstream.
# 2. udim/ : cas UDIM (tuiles, coupes, refus, nœuds sans uv) et matériaux (plus de 64, uv mêlées) de fbx_udim.py ;
#    l'upstream doit montrer chaque bug corrigé.
# 3. fuzz  : mutations de same/ et udim/ (nombres extrêmes, jetons, octets), lues en split 0, 1 et 2 par l'arbre
#    courant et par l'upstream ; échec si l'arbre courant plante ou signale un UB (hors float-cast-overflow).
# Le dossier .tests/ commence par un point : amake ne le compile pas dans l'app.

set -e
T=$(cd "$(dirname "$0")" && pwd)
EXT=$(dirname "$T")
ROOT=${STUDIO_ROOT:-$(git -C "$EXT/../../.." rev-parse --show-toplevel)}
BASE="$ROOT/base/sources"
FBX="$ROOT/paint/plugins/io_fbx"
W=${1:-$(mktemp -d)}
UPSTREAM=${STUDIO_UPSTREAM:-upstream}
mkdir -p "$W/up"

echo "== build ($W)"
git -C "$ROOT" show "$UPSTREAM:paint/plugins/io_fbx/io_fbx.c" > "$W/up/io_fbx.c"
LIBS="$BASE/iron_array.c $BASE/iron_string.c $BASE/iron_math.c"
CFLAGS="-std=c11 -O1 -g -w -fno-omit-frame-pointer -I$BASE -I$FBX"
# ufbx sans UBSan : ce banc teste io_fbx.c, pas le lecteur. float-cast-overflow : le packing int16 des positions /
# normales / uvs (code upstream, inchangé) convertit sans borne ; sur arm64 la conversion sature.
# shellcheck disable=SC2086
cc $CFLAGS -fsanitize=address -c "$FBX/ufbx/ufbx.c" -o "$W/ufbx.o"
# shellcheck disable=SC2086
cc $CFLAGS -fsanitize=address,undefined -fno-sanitize=float-cast-overflow -DIO_FBX="\"$FBX/io_fbx.c\"" "$T/fbx_test.c" "$BASE/iron_udim.c" \
	$LIBS "$W/ufbx.o" -o "$W/fbx_test" -lm
# shellcheck disable=SC2086
cc $CFLAGS -fsanitize=address -DIO_FBX="\"$W/up/io_fbx.c\"" "$T/fbx_test.c" $LIBS "$W/ufbx.o" -o "$W/fbx_test_up" -lm
fail=0

echo "== same/ : splits par objet et par matériau identiques à l'upstream ($UPSTREAM)"
python3 "$T/fbx_udim.py" same "$W/same" "$W/fbx_test" "$W/fbx_test_up" || fail=1

echo "== udim/ : cas UDIM et matériaux (fbx-split, fbx_udim.py)"
python3 "$T/fbx_udim.py" "$W/udim" "$W/fbx_test" "$W/fbx_test_up" || fail=1

echo "== fuzz (${FUZZ:-300} mutations)"
python3 "$T/fbx_udim.py" fuzz "$W" "${FUZZ:-300}" "$W/fbx_test" "$W/fbx_test_up" | tee "$W/fuzz.txt"
head -1 "$W/fuzz.txt" | grep -q " 0 plantages" || { echo "  ❌ plantages : $W/fuzz_fail/"; fail=1; }
head -1 "$W/fuzz.txt" | grep -q "UB " && { echo "  ❌ UB signalé par l'arbre courant"; fail=1; }

[ $fail -eq 0 ] && echo "OK" || echo "ÉCHEC"
exit $fail
