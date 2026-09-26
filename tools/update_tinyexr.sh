#!/bin/sh
# Copie le sous-ensemble de tinyexr v3 (C11 pur) utilisé par studio_exr dans vendor/.tinyexr/.
#
# Usage : tools/update_tinyexr.sh <checkout de https://github.com/syoyo/tinyexr>
#
# amake compile tous les .c sous external/ mais ignore les dossiers qui commencent par un point : les sources
# vendorées ne sont donc compilées qu'à travers vendor/tinyexr.c, avec notre configuration (EXR_NO_ZSTD, threads).
# Le script aplatit include/ et src/ dans un seul dossier (les includes entre guillemets se résolvent depuis le
# dossier du fichier inclus, et amake ne permet pas d'ajouter -I depuis external/) et génère unity.inc.
# Après une mise à jour : rebuild, puis rejouer les tests (.tests/run.sh et PATCHES.md#studio-exr).

set -e
SRC=${1:?usage: $0 <tinyexr checkout>}
[ -f "$SRC/include/exr.h" ] || { echo "$SRC n'est pas un checkout tinyexr v3 (include/exr.h absent)"; exit 2; }
EXT_DIR=$(cd "$(dirname "$0")/.." && pwd)
DST="$EXT_DIR/vendor/.tinyexr"

# Hors périmètre : E/S stdio (on écrit nous-mêmes), freestanding, spectral, GPU (CUDA/Vulkan), ZSTD (extension
# tinyexr hors standard OpenEXR, qui demanderait de vendorer zstd).
EXCLUDE="exr_stdio.c exr_freestanding.c exr_spectral.c exr_gpu_cuda.c exr_vk_vulkan.c exr_zstd.c"

rm -rf "$DST"
mkdir -p "$DST"
cp "$SRC/LICENSE" "$SRC/NOTICE" "$SRC/include/exr.h" "$DST/"
cp "$SRC"/src/*.h "$DST/"
for f in "$SRC"/src/*.inc; do
	case $(basename "$f") in
	exr_gpu_* | exr_vk_*) ;;
	*) cp "$f" "$DST/" ;;
	esac
done

{
	echo "// Généré par tools/update_tinyexr.sh — ne pas éditer."
	for f in "$SRC"/src/*.c; do
		b=$(basename "$f")
		case " $EXCLUDE " in *" $b "*) continue ;; esac
		cp "$f" "$DST/"
		echo "#include \"$b\""
	done
} > "$DST/unity.inc"

commit=$(git -C "$SRC" rev-parse HEAD 2>/dev/null || echo inconnu)
date=$(git -C "$SRC" log -1 --format=%cs 2>/dev/null || echo inconnue)
echo "tinyexr $commit ($date)" > "$DST/VERSION"
cat "$DST/LICENSE" "$DST/NOTICE" > "$EXT_DIR/assets/tinyexr_license.txt"

# Correctifs locaux de tinyexr (vendor/patches/*.patch, marqueur STUDIO-VENDOR-PATCH dans le code). Un patch qui
# ne s'applique plus arrête la mise à jour : soit l'amont a corrigé le point (supprimer le .patch), soit le code a
# changé autour (refaire le patch). Ne jamais laisser une mise à jour repartir sans eux.
for p in "$EXT_DIR"/vendor/patches/*.patch; do
	[ -f "$p" ] || continue
	if patch -p1 -d "$DST" -s < "$p"; then
		echo "  patch $(basename "$p" .patch) appliqué"
	else
		echo "  ÉCHEC du patch $(basename "$p" .patch) — voir vendor/patches/README.md"
		exit 1
	fi
done

echo "vendor/.tinyexr mis à jour : $(cat "$DST/VERSION"), $(grep -c '^#include' "$DST/unity.inc") sources"
