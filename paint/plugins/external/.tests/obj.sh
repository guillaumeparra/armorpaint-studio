#!/bin/sh
# Banc des patches obj-global-indices et obj-float-exponent, hors de l'app, sous ASan/UBSan : lecteur OBJ d'iron
# (base/sources/iron_obj.c).
#
# Usage : .tests/obj.sh [dossier de travail]     FUZZ=<n> : nombre de mutations (défaut 1000, ~3 min)
#         STUDIO_ROOT=<armorpaint> pour tester une autre copie d'armorpaint
#
# 1. valid/  : l'arbre courant produit, part par part, octet pour octet, la même sortie que l'upstream (branche main).
# 2. shared/ : un OBJ dont les faces utilisent les v/vt/vn d'un objet précédent se lit comme sa version à données
#    séparées (lue par l'upstream).
# 3. bad/    : fichiers malformés, aucune erreur ASan/UBSan.
# 4. float   : read_float() sur des jetons à exposant (1.5e-3, -0.5e-2, 1E+2, 1e-45, 1e999...) comparé à strtof ;
#    sans exposant, 20 000 jetons décodés à l'identique de l'upstream (obj_float_test.c).
# 5. exp/    : un OBJ écrit en notation scientifique se lit comme sa version décimale (lue par l'upstream).
# 6. fuzz    : mutations de valid/, shared/ et exp/, lues par l'arbre courant et par l'upstream ; échec si l'arbre
#    courant plante ou signale un UB (hors float-cast-overflow, voir plus bas).
# Le dossier .tests/ commence par un point : amake ne le compile pas dans l'app.

set -e
T=$(cd "$(dirname "$0")" && pwd)
EXT=$(dirname "$T")
ROOT=${STUDIO_ROOT:-$(git -C "$EXT/../../.." rev-parse --show-toplevel)}
BASE="$ROOT/base/sources"
W=${1:-$(mktemp -d)}
UPSTREAM=${STUDIO_UPSTREAM:-upstream}
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
# shellcheck disable=SC2086
cc $CFLAGS -fsanitize=address,undefined "$T/obj_float_test.c" $LIBS -o "$W/obj_float_test" -lm
# shellcheck disable=SC2086
cc $CFLAGS -I"$W/up" -fsanitize=address "$T/obj_float_test.c" $LIBS -o "$W/obj_float_test_up" -lm

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

echo "== float : read_float() avec exposant = strtof (à 1 ulp), sans exposant = upstream"
"$W/obj_float_test" 2> "$W/err.txt" || fail=1
[ -s "$W/err.txt" ] && { head -3 "$W/err.txt"; fail=1; }
"$W/obj_float_test" plain > "$W/new.txt" 2> "$W/err.txt" || true
"$W/obj_float_test_up" plain > "$W/up.txt" 2>&1 || true
if cmp -s "$W/up.txt" "$W/new.txt" && [ ! -s "$W/err.txt" ]; then
	echo "  ✅ $(wc -l < "$W/new.txt" | tr -d ' ') jetons sans exposant identiques à l'upstream"
else
	echo "  ❌ jetons sans exposant : $W/new.txt / $W/up.txt"; fail=1
fi

echo "== exp/ : notation scientifique = version décimale"
for f in "$W"/exp/*.ref.obj; do
	g=${f%.ref.obj}.obj
	for s in o u; do
		"$W/obj_test_up" "$f" $s > "$W/up.txt" 2>&1
		"$W/obj_test" "$g" $s > "$W/new.txt" 2> "$W/err.txt" || true
		if ! cmp -s "$W/up.txt" "$W/new.txt" || [ -s "$W/err.txt" ]; then
			echo "  ❌ $(basename "$g") ($s)"; fail=1
		else
			echo "  ✅ $(basename "$g") ($s)"
		fi
	done
done

echo "== fuzz (${FUZZ:-1000} mutations)"
python3 "$T/obj_gen.py" fuzz "$W" "${FUZZ:-1000}" "$W/obj_test" "$W/obj_test_up" | tee "$W/fuzz.txt"
head -1 "$W/fuzz.txt" | grep -q " 0 plantages" || { echo "  ❌ plantages : $W/fuzz_fail/"; fail=1; }
head -1 "$W/fuzz.txt" | grep -q "UB " && { echo "  ❌ UB signalé par l'arbre courant"; fail=1; }

[ $fail -eq 0 ] && echo "OK" || echo "ÉCHEC"
exit $fail
