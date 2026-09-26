#!/bin/sh
# Banc du patch obj-global-indices, hors de l'app, sous ASan/UBSan : lecteur OBJ d'iron (base/sources/iron_obj.c).
#
# Usage : .tests/obj.sh [dossier de travail]     FUZZ=<n> : nombre de mutations (défaut 1000, ~3 min)
#
# 1. valid/  : l'arbre courant produit, part par part, octet pour octet, la même sortie que l'upstream (branche main).
# 2. shared/ : un OBJ dont les faces utilisent les v/vt/vn d'un objet précédent se lit comme sa version à données
#    séparées (lue par l'upstream).
# 3. bad/    : fichiers malformés, aucune erreur ASan/UBSan.
# 4. fuzz    : mutations de valid/ et shared/, lues par l'arbre courant et par l'upstream ; échec si l'arbre courant
#    plante. Les UB restants signalés (hors erreurs mémoire) sont listés : voir PATCHES.md#obj-global-indices.
# Le dossier .tests/ commence par un point : amake ne le compile pas dans l'app.

set -e
T=$(cd "$(dirname "$0")" && pwd)
EXT=$(dirname "$T")
ROOT=$(git -C "$EXT/../../.." rev-parse --show-toplevel)
BASE="$ROOT/base/sources"
W=${1:-$(mktemp -d)}
UPSTREAM=${STUDIO_UPSTREAM:-main}
mkdir -p "$W/up"

echo "== build ($W)"
git -C "$ROOT" show "$UPSTREAM:base/sources/iron_obj.c" > "$W/up/iron_obj.c"
LIBS="$BASE/iron_array.c $BASE/iron_string.c $BASE/iron_math.c"
CFLAGS="-std=c11 -O1 -g -w -fno-omit-frame-pointer -I$BASE"
# float-cast-overflow : le packing int16 des positions / normales / uvs (code upstream, inchangé) convertit sans
# borne ; sur arm64 la conversion sature. Ce banc teste les indices et les lectures, pas ce packing.
# shellcheck disable=SC2086
cc $CFLAGS -fsanitize=address,undefined -fno-sanitize=float-cast-overflow "$T/obj_test.c" "$BASE/iron_obj.c" $LIBS -o "$W/obj_test" -lm
# shellcheck disable=SC2086
cc $CFLAGS -fsanitize=address "$T/obj_test.c" "$W/up/iron_obj.c" $LIBS -o "$W/obj_test_up" -lm

echo "== corpus"
python3 "$T/obj_gen.py" corpus "$W"
fail=0

echo "== valid/ : identique à l'upstream ($UPSTREAM)"
for f in "$W"/valid/*.obj; do
	for s in o u; do
		"$W/obj_test_up" "$f" $s > "$W/up.txt" 2>&1
		"$W/obj_test" "$f" $s > "$W/new.txt" 2> "$W/err.txt" || true
		if ! cmp -s "$W/up.txt" "$W/new.txt" || [ -s "$W/err.txt" ]; then
			echo "  ❌ $(basename "$f") ($s)"; fail=1
		fi
	done
done
"$W/obj_test_up" "$W/valid/udim_grid.obj" o udim > "$W/up.txt" 2>&1
"$W/obj_test" "$W/valid/udim_grid.obj" o udim > "$W/new.txt" 2>&1
cmp -s "$W/up.txt" "$W/new.txt" || { echo "  ❌ udim_grid (udim)"; fail=1; }

echo "== shared/ : données d'objets précédents = version à données séparées"
for f in "$W"/shared/*.ref.obj; do
	g=${f%.ref.obj}.obj
	"$W/obj_test_up" "$f" o > "$W/up.txt" 2>&1
	"$W/obj_test" "$g" o > "$W/new.txt" 2> "$W/err.txt" || true
	if ! cmp -s "$W/up.txt" "$W/new.txt" || [ -s "$W/err.txt" ]; then
		echo "  ❌ $(basename "$g")"; fail=1
	else
		echo "  ✅ $(basename "$g")"
	fi
done

echo "== bad/ : pas d'erreur"
for f in "$W"/bad/*.obj; do
	for s in o u; do
		if ! "$W/obj_test" "$f" $s > /dev/null 2> "$W/err.txt" || [ -s "$W/err.txt" ]; then
			echo "  ❌ $(basename "$f") ($s)"; head -3 "$W/err.txt"; fail=1
		fi
	done
done

echo "== fuzz (${FUZZ:-1000} mutations)"
python3 "$T/obj_gen.py" fuzz "$W" "${FUZZ:-1000}" "$W/obj_test" "$W/obj_test_up" | tee "$W/fuzz.txt"
head -1 "$W/fuzz.txt" | grep -q " 0 plantages" || { echo "  ❌ plantages : $W/fuzz_fail/"; fail=1; }

[ $fail -eq 0 ] && echo "OK" || echo "ÉCHEC"
exit $fail
