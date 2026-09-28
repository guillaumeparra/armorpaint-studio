#!/bin/sh
# Lot A1 : construit libstudio_ocio (shim C d'OpenColorIO, sources dans .ocio/) dans .ocio/lib/ (ignoré par git).
#
# - macOS : libstudio_ocio.a, liée en statique dans ArmorPaint (.project/project.c). Une seule unité, fusionnée par
#   `ld -r` : le shim, OCIO et ses dépendances, où seuls les symboles studio_ocio_lib_* restent globaux. Rien ne peut
#   entrer en collision avec l'app. Relancer ensuite le build d'ArmorPaint (../base/make --compile depuis paint/).
# - partout : libstudio_ocio.dylib/.so dans le dossier de build, pour les bancs de test (.tests/ocio.sh) ; sous Linux,
#   ArmorPaint la charge par dlopen depuis data/ (copie dans assets/). Jamais dans .ocio/lib/ sur macOS : Xcode lie par
#   -lstudio_ocio et prendrait la dylib, que la validation des bibliothèques refuserait au lancement.
# - assets/opencolorio_licenses.txt : licences d'OCIO et des bibliothèques embarquées, livrées avec l'app.
#
# Usage : tools/build_ocio.sh [dossier de build]     (défaut : .ocio/build)
# Variables : OCIO_SOURCE_DIR=<clone d'OpenColorIO au bon tag> évite le téléchargement d'OCIO ;
# CMAKE_BUILD_PARALLEL_LEVEL=<n> compilations en parallèle (défaut : un par cœur ; ~1 Go de mémoire chacune).
# Premier build : 2 à 3 minutes (OCIO et ses dépendances depuis leurs sources). Réseau requis.

set -e
EXT=$(cd "$(dirname "$0")/.." && pwd)
B=${1:-$EXT/.ocio/build}
OUT=$EXT/.ocio/lib
mkdir -p "$B" "$OUT"

case $(uname -s) in
Darwin)
	SO=libstudio_ocio.dylib
	# Même plancher que le binaire d'ArmorPaint (minos de son LC_BUILD_VERSION).
	MINOS=14.0
	PLATFORM="-DCMAKE_OSX_DEPLOYMENT_TARGET=$MINOS -DCMAKE_OSX_ARCHITECTURES=$(uname -m)"
	;;
Linux) SO=libstudio_ocio.so; PLATFORM= ;;
*) echo "build_ocio.sh : plateforme non gérée ($(uname -s))" >&2; exit 1 ;;
esac

EXTRA=
if [ -n "$OCIO_SOURCE_DIR" ]; then
	EXTRA="-DFETCHCONTENT_SOURCE_DIR_OPENCOLORIO=$OCIO_SOURCE_DIR"
fi

# shellcheck disable=SC2086
cmake -S "$EXT/.ocio" -B "$B" -DCMAKE_BUILD_TYPE=Release $PLATFORM $EXTRA > "$B/configure.log" 2>&1 ||
	{ tail -30 "$B/configure.log" >&2; exit 1; }
# --parallel sans nombre donne `make -j` sans limite : sur une VM de 16 Go, le noyau tue cc1plus.
JOBS=${CMAKE_BUILD_PARALLEL_LEVEL:-$(getconf _NPROCESSORS_ONLN 2> /dev/null || echo 4)}
cmake --build "$B" --target studio_ocio --parallel "$JOBS" > "$B/build.log" 2>&1 || { tail -30 "$B/build.log" >&2; exit 1; }
rm -f "$OUT/$SO"

OCIO_BUILD=$B/_deps/opencolorio-build
if [ "$(uname -s)" = Darwin ]; then
	# Les archives d'OCIO et de ses dépendances, telles que CMake les a liées dans la dylib.
	ARCHIVES="$OCIO_BUILD/src/OpenColorIO/libOpenColorIO.a $(ls "$OCIO_BUILD"/ext/dist/lib/*.a)"
	# shellcheck disable=SC2086
	ld -r -arch "$(uname -m)" -platform_version macos $MINOS $MINOS \
		-exported_symbols_list "$EXT/.ocio/studio_ocio_lib.symbols" \
		"$B/CMakeFiles/studio_ocio.dir/studio_ocio_lib.cpp.o" $ARCHIVES -o "$B/studio_ocio_all.o"
	if nm -g "$B/studio_ocio_all.o" | grep -v ' U ' | grep -v ' _studio_ocio_lib_' | grep -q .; then
		echo "build_ocio.sh : studio_ocio_all.o exporte d'autres symboles que studio_ocio_lib_*" >&2
		exit 1
	fi
	rm -f "$OUT/libstudio_ocio.a"
	ar rcs "$OUT/libstudio_ocio.a" "$B/studio_ocio_all.o"
	echo "$OUT/libstudio_ocio.a ($(du -h "$OUT/libstudio_ocio.a" | cut -f1)), $B/$SO"
else
	cp "$B/$SO" "$EXT/assets/$SO"
	echo "$EXT/assets/$SO ($(du -h "$EXT/assets/$SO" | cut -f1))"
fi

# Licences : OCIO (BSD-3, et le code tiers qu'il embarque) et chaque dépendance liée.
L=$EXT/assets/opencolorio_licenses.txt
SRC=${OCIO_SOURCE_DIR:-$B/_deps/opencolorio-src}
{
	echo "ArmorPaint (studio build) embeds OpenColorIO $(sed -n 's/^set(STUDIO_OCIO_TAG "\([^"]*\)".*/\1/p' "$EXT/.ocio/CMakeLists.txt") and the libraries below."
	for f in "OpenColorIO:$SRC/LICENSE" "OpenColorIO third-party code:$SRC/THIRD-PARTY.md" \
		"Imath:$(ls "$OCIO_BUILD"/ext/build/Imath/src/*/LICENSE.md)" \
		"expat:$(ls "$OCIO_BUILD"/ext/build/libexpat/src/*/COPYING)" \
		"minizip-ng:$(ls "$OCIO_BUILD"/ext/build/libminizip-ng/src/*/LICENSE)" \
		"pystring:$(ls "$OCIO_BUILD"/ext/build/pystring/src/*/LICENSE)" \
		"yaml-cpp:$(ls "$OCIO_BUILD"/ext/build/yaml-cpp/src/*/LICENSE)" \
		"zlib:$(ls "$OCIO_BUILD"/ext/build/zlib/src/*/LICENSE)"; do
		printf '\n\n======== %s ========\n\n' "${f%%:*}"
		cat "${f#*:}"
	done
	S2N=$B/ext/build/sse2neon/sse2neon-src/LICENSE
	if [ -f "$S2N" ]; then
		printf '\n\n======== sse2neon ========\n\n'
		cat "$S2N"
	fi
} > "$L"
