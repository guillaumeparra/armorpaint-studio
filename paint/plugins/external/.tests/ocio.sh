#!/bin/sh
# Banc de test du lot A1, hors de l'app, sous ASan/UBSan : gestion couleur OCIO (studio_ocio.c et libstudio_ocio).
#
# Usage : .tests/ocio.sh [dossier de travail]
# Variables : OCIO_TEST_CONFIGS="<config> …" configurations de plus à comparer (par exemple celle du studio) ;
#             OCIO_TEST_ULPS=<n> tolérance de la comparaison (défaut 0 : bit à bit).
#
# Référence indépendante : PyOpenColorIO, la roue officielle d'OpenColorIO à la même version, installée dans
# .ocio/venv au premier lancement (réseau requis une fois). ocio_ref.py y écrit, par configuration, les listes, rôles,
# alias, file_rules et des transformations de pixels ; ocio_test.c doit tout retrouver.
#
# Deux variantes du harnais : « static », liée à .ocio/lib/libstudio_ocio.a comme l'app sur macOS, et « dlopen », le
# chargement dynamique de Linux (vérifications de symboles et d'ABI comprises, avec de fausses bibliothèques).

set -e
T=$(cd "$(dirname "$0")" && pwd)
EXT=$(dirname "$T")
B=$EXT/.ocio/build
W=${1:-$(mktemp -d)}
mkdir -p "$W"

case $(uname -s) in
Darwin) SO=libstudio_ocio.dylib; STATIC=$EXT/.ocio/lib/libstudio_ocio.a ;;
*) SO=libstudio_ocio.so; STATIC= ;;
esac
if [ ! -f "$B/$SO" ] || { [ -n "$STATIC" ] && [ ! -f "$STATIC" ]; }; then
	echo "== libstudio_ocio (tools/build_ocio.sh)"
	"$EXT/tools/build_ocio.sh"
fi

VER=$(sed -n 's/^set(STUDIO_OCIO_TAG "v\([^"]*\)".*/\1/p' "$EXT/.ocio/CMakeLists.txt")
PY=$EXT/.ocio/venv/bin/python
if ! "$PY" -c "import PyOpenColorIO as O, sys; sys.exit(O.GetVersion() != '$VER')" 2> /dev/null; then
	echo "== PyOpenColorIO $VER (.ocio/venv)"
	python3 -m venv "$EXT/.ocio/venv"
	"$EXT/.ocio/venv/bin/pip" install -q "opencolorio==$VER"
fi

echo "== symboles exportés"
grep '^_' "$EXT/.ocio/studio_ocio_lib.symbols" | sed 's/^_//' | sort > "$W/symbols.expected"
if [ "$(uname -s)" = Darwin ]; then
	nm -gU "$B/$SO" | awk '{print $3}' | sed 's/^_//' | sort > "$W/symbols.dylib"
	nm -g "$STATIC" 2> /dev/null | awk '$2 != "U" && NF == 3 {print $3}' | sed 's/^_//' | sort > "$W/symbols.static"
	cmp "$W/symbols.expected" "$W/symbols.static" || { echo "  FAIL libstudio_ocio.a"; diff "$W/symbols.expected" "$W/symbols.static"; exit 1; }
else
	nm -D --defined-only "$B/$SO" | awk '$2 == "T" {print $3}' | sed 's/@.*//' | sort > "$W/symbols.dylib"
fi
cmp "$W/symbols.expected" "$W/symbols.dylib" || { echo "  FAIL $SO"; diff "$W/symbols.expected" "$W/symbols.dylib"; exit 1; }
# Chaque fonction de l'en-tête est dans la liste, et inversement.
grep -o 'studio_ocio_lib_[a-z_]*(' "$EXT/studio_ocio_lib.h" | tr -d '(' | sort -u > "$W/symbols.header"
cmp "$W/symbols.expected" "$W/symbols.header" || { echo "  FAIL studio_ocio_lib.h"; diff "$W/symbols.expected" "$W/symbols.header"; exit 1; }
echo "  ok   $(wc -l < "$W/symbols.expected" | tr -d ' ') symboles studio_ocio_lib_*, rien d'autre"

echo "== build ($W)"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
CFLAGS="-std=c11 -O1 -g -Wall -Wno-unused-function $SAN -I$EXT"
# shellcheck disable=SC2086
if [ -n "$STATIC" ]; then
	cc $CFLAGS -DWITH_STUDIO_OCIO_STATIC "$T/ocio_test.c" "$EXT/studio_ocio.c" "$STATIC" -o "$W/ocio_static"
fi
# La variante dlopen appelle aussi l'ABI directement (ref, errors) : liée à la même bibliothèque, que dlopen retrouve.
# shellcheck disable=SC2086
cc $CFLAGS "$T/ocio_test.c" "$EXT/studio_ocio.c" -L"$B" -lstudio_ocio -Wl,-rpath,"$B" -ldl -o "$W/ocio_dlopen"

# Fausses bibliothèques : toutes les fonctions, ABI 99 ; puis une fonction manquante.
FAKE_SYMS=$(sed 's/^_//' "$W/symbols.expected" | grep -v '^studio_ocio_lib_abi$')
{
	echo 'int studio_ocio_lib_abi(void) { return 99; }'
	for s in $FAKE_SYMS; do echo "void $s(void) {}"; done
} > "$W/fake_abi.c"
{
	echo 'int studio_ocio_lib_abi(void) { return 1; }'
	for s in $FAKE_SYMS; do [ "$s" = studio_ocio_lib_proc_cache_id ] || echo "void $s(void) {}"; done
} > "$W/fake_missing.c"
cc -shared -fPIC "$W/fake_abi.c" -o "$W/fake_abi.$SO"
cc -shared -fPIC "$W/fake_missing.c" -o "$W/fake_missing.$SO"

echo "== références (PyOpenColorIO)"
mkdir -p "$W/cfg"
"$PY" "$T/ocio_ref.py" --make-config "$W/cfg"
CONFIGS="$W/cfg/config.ocio ocio://studio-config-latest ocio://cg-config-latest $OCIO_TEST_CONFIGS"
i=0
for c in $CONFIGS; do
	i=$((i + 1))
	"$PY" "$T/ocio_ref.py" "$c" "$W/ref_$i.txt"
done
# Configurations invalides : fichier illisible, YAML qui n'en est pas, rôle vers un espace absent (échoue à validate).
printf 'not: [a, config\n' > "$W/garbage.ocio"
sed 's/scene_linear: lin/scene_linear: missing_space/' "$W/cfg/config.ocio" > "$W/cfg/invalid.ocio"

# Une affectation devant un appel de fonction survit à l'appel (sh POSIX) : chaque run pose OCIO, et celles qui posent
# d'autres variables sont dans un sous-shell.
unset STUDIO_OCIO_WORKING_SPACE STUDIO_OCIO_LIB
run() { # variante, mode, arguments…
	v=$1
	shift
	echo "-- $v $*"
	if [ "$v" = dlopen ]; then
		STUDIO_OCIO_LIB=${LIB-$B/$SO} "$W/ocio_$v" "$@"
	else
		"$W/ocio_$v" "$@"
	fi
}

for v in ${STATIC:+static} dlopen; do
	echo "== $v"
	i=0
	for c in $CONFIGS; do
		i=$((i + 1))
		OCIO=$c run $v ref "$W/ref_$i.txt"
	done
	OCIO= run $v errors
	OCIO= run $v threads
	OCIO= run $v reload "$W/cfg/config.ocio"
	OCIO= run $v working "Linear Rec.709 (sRGB)"
	(OCIO= STUDIO_OCIO_WORKING_SPACE=scene_linear run $v working ACEScg)
	OCIO=$W/cfg/config.ocio run $v working "" "set STUDIO_OCIO_WORKING_SPACE"
	(OCIO=$W/cfg/config.ocio STUDIO_OCIO_WORKING_SPACE=lin_srgb run $v working lin)
	(OCIO=$W/cfg/config.ocio STUDIO_OCIO_WORKING_SPACE=nope run $v working "" "STUDIO_OCIO_WORKING_SPACE: unknown colorspace: nope")
	OCIO=/nonexistent/config.ocio run $v unavailable "config /nonexistent/config.ocio (\$OCIO) rejected"
	OCIO=$W/garbage.ocio run $v unavailable "garbage.ocio (\$OCIO) rejected"
	OCIO=$W/cfg/invalid.ocio run $v unavailable "missing_space"
done

echo "== dlopen : bibliothèque absente ou incompatible"
if [ "$(uname -s)" = Darwin ]; then
	LIB= run dlopen unavailable "built without libstudio_ocio"
else
	(cd "$W" && LIB= run dlopen unavailable "cannot load ./$SO")
fi
LIB=/nonexistent/$SO run dlopen unavailable "cannot load /nonexistent/$SO"
LIB=$W/fake_abi.$SO run dlopen unavailable "has ABI 99, expected $(sed -n 's/^#define STUDIO_OCIO_LIB_ABI \([0-9]*\).*/\1/p' "$EXT/studio_ocio_lib.h")"
LIB=$W/fake_missing.$SO run dlopen unavailable "has no studio_ocio_lib_proc_cache_id"
echo "== OK"
