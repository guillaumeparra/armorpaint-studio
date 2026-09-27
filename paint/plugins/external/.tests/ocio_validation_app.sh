#!/bin/sh
# Lot A6 : validation de la chaîne couleur contre des outils indépendants (ocio_validation.py). Des mires (rampe sRGB,
# ColorChecker sRGB, HDR > 1, ColorChecker ACEScg) sont importées, peintes sur un calque (remplissage) et exportées
# dans quatre espaces, en calques 32, 16 et 8 bits ; les fichiers sont comparés à oiiotool --colorconvert. La LUT du
# viewport est comparée à oiiotool --ociodisplay (exact) et à une LUT ociobakelut. Nuke 17, s'il est installé, relit
# l'export ACEScg (aller-retour vers le sRGB de la mire) et refait la transformation d'affichage. Enfin, l'export
# upstream --export-textures exr16/exr32 est comparé au png (patch metal-target-format).
#
#   sh .tests/ocio_validation_app.sh [dossier de travail]
#   APP=<binaire ArmorPaint> pour un autre build ; NUKE=<binaire Nuke 17> (défaut : le plus récent de /Applications),
#   NUKE= (vide) pour s'en passer.
# Demande .ocio/venv (créé par ocio.sh) avec numpy, OpenEXR, Pillow et OpenImageIO (oiiotool) :
#   .ocio/venv/bin/pip install numpy OpenEXR Pillow OpenImageIO
set -eu
T=$(cd "$(dirname "$0")" && pwd)
EXT=$(dirname "$T")
APP=${APP:-$EXT/../../build/build/Release/ArmorPaint.app/Contents/MacOS/ArmorPaint}
PY=$EXT/.ocio/venv/bin/python
W=${1:-$(mktemp -d)}
mkdir -p "$W"
W=$(cd "$W" && pwd)
[ -x "$APP" ] || { echo "ArmorPaint not found: $APP" >&2; exit 1; }
export STUDIO_ENV= # lot E5 : aucun studio.env, pas même celui de l'utilisateur
"$PY" -c 'import PyOpenColorIO, numpy, OpenEXR, PIL' 2> /dev/null && [ -x "$EXT/.ocio/venv/bin/oiiotool" ] ||
	{ echo "venv incomplet : lancer .tests/ocio.sh puis $EXT/.ocio/venv/bin/pip install numpy OpenEXR Pillow OpenImageIO" >&2; exit 1; }
[ -n "${NUKE+x}" ] || NUKE=$(ls /Applications/Nuke17*/Nuke17*.app/Contents/MacOS/Nuke17* 2> /dev/null | tail -1 || true)
FAIL=0
ok() { echo "ok   $1"; }
ko() { echo "FAIL $1"; FAIL=1; }
check() { if eval "$2"; then ok "$1"; else ko "$1"; fi; }
app() { perl -e 'alarm 300; exec @ARGV' "$APP" "$@" -ApplePersistenceIgnoreState YES; }
# Commande de contrôle, sortie indentée ; rend son propre code (dans un pipe vers sed, le code serait celui de sed).
indented() { "$@" > "$W/.indented" 2>&1 && r=0 || r=$?; sed 's/^/     /' "$W/.indented"; return $r; }
unset OCIO STUDIO_OCIO_WORKING_SPACE STUDIO_OCIO_LIB STUDIO_OCIO_VIEWPORT STUDIO_OCIO_DISPLAY STUDIO_OCIO_VIEW STUDIO_OCIO_LOOK
unset STUDIO_OCIO_IMPORT STUDIO_EXPORT_COLOR_SPACE STUDIO_PREFS

rm -rf "$W"/out* "$W"/up_* "$W"/ref "$W"/nuke
"$PY" "$T/ocio_validation.py" make "$W"

# 1. Mires importées, peintes et exportées : calques 32, 16 et 8 bits (studio_project_set_bits), puis 32 bits en espace
#    de travail ACEScg (couleurs hors du gamut Rec.709)
for r in 32 16 8 32aces; do
	b=${r%aces}
	if [ "$r" = 32aces ]; then export STUDIO_OCIO_WORKING_SPACE=ACEScg; else unset STUDIO_OCIO_WORKING_SPACE; fi
	STUDIO_EXPORT_PRESETS="$W/presets" app "$W/plane.obj" --background --script "$W/a6_$r.c" > "$W/app$r.log" 2>&1 || true
	check "passe $r : 5 mires importées dans leur espace et exportées" \
		'grep -q "^t: set_bits=1 bits=$b" "$W/app$r.log" && [ $(grep -c "^t: export [a-z]*=1" "$W/app$r.log") -eq 5 ] &&
		 grep -q "^t: aces cs=\[ACEScg\]" "$W/app$r.log" && grep -q "^t: ramp cs=\[sRGB Encoded Rec.709 (sRGB)\]" "$W/app$r.log"'
done
unset STUDIO_OCIO_WORKING_SPACE
check "passe 32aces : espace de travail ACEScg" 'grep -q "working space \"ACEScg\" (\$STUDIO_OCIO_WORKING_SPACE)" "$W/app32aces.log"'


# 2. Nuke : aller-retour de l'export ACEScg et transformation d'affichage (facultatif)
if [ -n "$NUKE" ] && [ -x "$NUKE" ]; then
	perl -e 'alarm 300; exec @ARGV' "$NUKE" -t "$W/nuke_a6.py" > "$W/nuke.log" 2>&1 || true
	check "Nuke : aller-retour et affichage écrits ($(basename "$NUKE"))" '[ -f "$W/nuke/roundtrip_srgb.exr" ] && [ -f "$W/nuke/view_out.exr" ]'
else
	echo "skip Nuke absent (NUKE=<binaire Nuke 17> pour l'aller-retour)"
fi

# 3. Comparaisons (oiiotool, ociobakelut, Nuke) ; report.md garde les écarts mesurés
if indented "$PY" "$T/ocio_validation.py" check "$W"; then
	ok "exports, viewport et Nuke dans les tolérances ($W/report.md)"
else
	ko "écarts hors tolérance ($W/report.md)"
fi

# 4. Export upstream en EXR (patch metal-target-format) : mêmes valeurs que le png
for t in png exr16 exr32; do
	mkdir -p "$W/up_$t"
	app "$W/plane.obj" --background --export-textures "$t" generic "$W/up_$t" > "$W/up_$t.log" 2>&1 || true
done
if indented "$PY" "$T/ocio_validation.py" upstream "$W"; then
	ok "export upstream exr16/exr32 = png"
else
	ko "export upstream exr16/exr32 différent du png"
fi

[ $FAIL -eq 0 ] && echo "ocio validation app: OK ($W)" || { echo "ocio validation app: FAILED ($W)"; exit 1; }
