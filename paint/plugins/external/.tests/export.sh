#!/bin/sh
# Banc des lots B2, B4 et B5 hors de l'app, sous ASan/UBSan :
#  - encodeurs PNG/TIFF (studio_image.c), relus par libpng et libtiff, et les patches sdefl-bound et
#    encode-image-overflow ; TIFF 8 et 16 bits relus aussi par l'importeur d'ArmorPaint (patch tiff-lzw) ;
#  - expressions, presets (dont les 5 livrés), nommage, préflight, SHA-256 (comparé à shasum) ;
#  - conseil de résolution après un import UDIM (studio_udim.c, lot D0).
#
# Usage : .tests/export.sh [dossier de travail]     FUZZ=<n> : expressions aléatoires (défaut 200000)
# Requiert libpng et libtiff (brew install libpng libtiff), et tiffinfo.

set -e
T=$(cd "$(dirname "$0")" && pwd)
EXT=$(dirname "$T")
BASE=$(cd "$EXT/../../../base" && pwd)
W=${1:-$(mktemp -d)}
mkdir -p "$W"
PREFIX=$(brew --prefix 2>/dev/null || echo /usr/local)

echo "== build ($W)"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
CFLAGS="-std=c11 -O1 -g $SAN -DWITH_COMPRESS -I$T/stubs -I$BASE/sources -I$BASE/sources/libs"
WARN="-Wall -Wextra -Wno-unused-parameter -Werror"
MODULES="studio_image studio_expr studio_preset studio_preflight studio_naming studio_json studio_sha256 studio_fs studio_udim"
OBJS=""
for m in $MODULES; do
	# shellcheck disable=SC2086
	cc $CFLAGS $WARN -c "$EXT/$m.c" -o "$W/$m.o"
	OBJS="$OBJS $W/$m.o"
done
# shellcheck disable=SC2086
cc $CFLAGS -w -c "$BASE/sources/iron_compress.c" -o "$W/iron_compress.o"
# shellcheck disable=SC2086
cc $CFLAGS -w -c "$T/stubs/stubs.c" -o "$W/stubs.o"
# iron_image.c (upstream) inclut stb_image_write : son écrivain JPEG décale un int signé au-delà de sa plage
# (stbiw__jpg_writeBits, bitBuf <<= 8), UB signalé sur toute image. Ce n'est pas ce qu'on teste ici : on garde ASan
# (le débordement d'encode-image-overflow) et on coupe le seul contrôle de décalage.
# shellcheck disable=SC2086
cc $CFLAGS -fno-sanitize=shift -w -DWITH_IMAGE_WRITE -c "$BASE/sources/iron_image.c" -o "$W/iron_image.o"
# shellcheck disable=SC2086
# Importeur TIFF d'ArmorPaint (patch tiff-lzw), relu contre nos TIFF.
# shellcheck disable=SC2086
cc $CFLAGS -w -c "$EXT/../io_tiff/io_tiff.c" -o "$W/io_tiff.o"
# shellcheck disable=SC2086
cc $CFLAGS -w -I"$PREFIX/include" "$T/image_test.c" "$W/stubs.o" "$W/studio_image.o" "$W/iron_compress.o" \
	"$W/iron_image.o" "$W/io_tiff.o" -L"$PREFIX/lib" -lpng -ltiff -o "$W/image_test"
# shellcheck disable=SC2086
cc $CFLAGS $WARN "$T/export_test.c" $OBJS "$W/iron_compress.o" "$W/stubs.o" -o "$W/export_test"

echo "== encodeurs PNG et TIFF (relus par libpng et libtiff)"
mkdir -p "$W/img"
"$W/image_test" "$W/img"
# tiffinfo -D décode tout : aucun avertissement libtiff attendu.
for f in "$W"/img/tif_1000x700_*.tif; do
	if tiffinfo -D "$f" 2>&1 >/dev/null | grep -q .; then
		echo "  FAIL tiffinfo: $f"
		tiffinfo -D "$f" 2>&1 >/dev/null | head -3
		exit 1
	fi
done
echo "  tiffinfo -D : aucun avertissement"

echo "== expressions, presets, nommage, préflight"
"$W/export_test" unit "$W/unit" "$EXT/assets"
"$W/export_test" fuzz "${FUZZ:-200000}"

echo "== SHA-256 contre shasum"
for f in "$W"/img/png_1000x700_c4_b16_m1.png "$W"/img/tif_17x33_c3_b8_m0_z5.tif "$W/export_test"; do
	a=$("$W/export_test" sha "$f" | cut -d' ' -f1)
	b=$(shasum -a 256 "$f" | cut -d' ' -f1)
	[ "$a" = "$b" ] || { echo "  FAIL sha256 $f: $a != $b"; exit 1; }
done
echo "  ok"
echo "== OK"
