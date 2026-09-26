#!/bin/sh
# Banc de test du lot B1, hors de l'app, sous ASan/UBSan : codec EXR studio (studio_exr_codec.c, tinyexr) et repli
# DWAA/DWAB vers le décodeur upstream (paint/plugins/io_exr/io_exr.c).
#
# Usage : .tests/run.sh [dossier de travail]     FUZZ=<n> : mutations par fichier (défaut 2000)
# Requiert les outils OpenEXR (brew install openexr) : exrmetrics, exrmaketiled, exrmultipart.
# Le dossier .tests/ commence par un point : amake ne le compile pas dans l'app.

set -e
T=$(cd "$(dirname "$0")" && pwd)
EXT=$(dirname "$T")
BASE=$(cd "$EXT/../../../base" && pwd)
W=${1:-$(mktemp -d)}
mkdir -p "$W"
X="$W/exr_test"

echo "== build ($W)"
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
CFLAGS="-std=c11 -O1 -g $SAN -DWITH_COMPRESS -Wno-unused-function -I$T/stubs -I$BASE/sources -I$BASE/sources/libs"
# io_exr.c lit l'en-tête EXR par cast de pointeur non aligné : UB signalé à chaque appel, sur tous les fichiers, y
# compris valides. C'est du code upstream que le repli DWA appelle exprès, et l'alignement n'est pas ce qu'on teste
# ici — on garde ASan (qui attrape le débordement de channels[4]) et on coupe le seul contrôle d'alignement.
# shellcheck disable=SC2086
cc $CFLAGS -fno-sanitize=alignment -c "$EXT/../io_exr/io_exr.c" -o "$W/io_exr.o"
# shellcheck disable=SC2086
cc $CFLAGS "$T/exr_test.c" "$T/stubs/stubs.c" "$EXT/studio_exr_codec.c" "$EXT/vendor/tinyexr.c" \
	"$W/io_exr.o" "$BASE/sources/iron_compress.c" -o "$X"

echo "== fichiers de test"
"$X" gen "$W"
for z in none rle zips piz pxr24 b44 b44a htj2k256 htj2k32; do
	exrmetrics --convert -z $z "$W/rgba_half.exr" -o "$W/rgba_$z.exr"
done
exrmetrics --convert -z pxr24 "$W/rgba_float.exr" -o "$W/rgba_float_pxr24.exr"
exrmaketiled -o "$W/rgba_half.exr" "$W/tiled_one.exr"
exrmaketiled -m "$W/rgba_half.exr" "$W/tiled_mip.exr"
# exrmultipart -combine laisse la table d'offsets de la 2e part à zéro (OpenEXR la reconstruit à la lecture,
# tinyexr — plus strict — la refuserait). exrmetrics --convert réécrit le fichier avec une vraie table.
exrmultipart -combine -i "$W/y_half.exr::luma" "$W/rgba_half.exr::color" -o "$W/multipart_raw.exr" > /dev/null
exrmetrics --convert -z zip "$W/multipart_raw.exr" -o "$W/multipart.exr" > /dev/null
for f in rgb y rgba; do
	for z in dwaa dwab; do
		exrmetrics --convert -z $z "$W/${f}_half.exr" -o "$W/${f}_$z.exr"
		exrmetrics --convert -z none "$W/${f}_$z.exr" -o "$W/${f}_${z}_ref.exr"
	done
done

echo "== décodage et encodage"
"$X" check "$W"

echo "== fuzz"
for f in rgba_half rgba_piz rgba_b44 rgba_htj2k32 tiled_mip multipart layers yc rgb_dwaa y_dwaa rgb_dwab; do
	"$X" fuzz "$W/$f.exr" "${FUZZ:-2000}" 1
done

echo "== débordement de pile upstream"
# io_exr_parse remplit channel_t channels[4] sans borne : ASan doit l'attraper sur many.exr (8 canaux), que
# studio_exr_decode lit correctement ci-dessus.
if "$X" upstream "$W/many.exr" > "$W/upstream.log" 2>&1; then
	echo "  FAIL io_exr_parse n'a pas débordé sur many.exr"
	exit 1
fi
grep -q "stack-buffer-overflow" "$W/upstream.log"
echo "  ok   io_exr_parse : stack-buffer-overflow sur many.exr (8 canaux), voir $W/upstream.log"
echo "== OK"
