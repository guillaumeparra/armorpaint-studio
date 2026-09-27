#!/bin/sh
# Banc du chantier E1 (subzero), hors de l'app : description des .sbsar et rendu par sbsrender, en Python seul
# (subzero_check.py, qui décrit les cas et les réglages SUBZERO_*). Sans Designer ni échantillons, les parties qui en
# dépendent sont sautées.
#
#   sh .tests/subzero.sh [dossier de travail]
#   SUBZERO_SBSRENDER=<sbsrender> (défaut : détection dans /Applications), SUBZERO_RENDER_ALL=1 : toute l'étagère.
set -eu
T=$(cd "$(dirname "$0")" && pwd)
W=${1:-$(mktemp -d)}
mkdir -p "$W"
W=$(cd "$W" && pwd)
rm -rf "$W/cache" "$W/render" "$W/shelf" "$W/wood_plain" "$W/planks" "$W/preset" "$W/preview" "$W/cooked"
SUBZERO_CACHE=$W/cache PYTHONDONTWRITEBYTECODE=1 exec python3 "$T/subzero_check.py" "$W"
