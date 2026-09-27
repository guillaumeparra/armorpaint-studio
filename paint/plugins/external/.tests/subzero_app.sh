#!/bin/sh
# Chantier E (subzero) dans ArmorPaint lancé avec son interface : un .sbsar importé comme un asset ouvre la fenêtre, qui
# tourne en essai sans intervention (SUBZERO_TEST, Qt offscreen) et envoie le matériau ; le projet est enregistré avec
# ce matériau sur deux calques de remplissage, rouvert, et le matériau réédité depuis son dossier (recette) : mise à jour
# des mêmes textures, sans nouveau matériau, calques de remplissage refaits ; puis passé d'EXR (défaut, lot E6) en PNG :
# nouvelles textures, anciennes retirées du projet et supprimées. La couleur de base est importée en srgb_tx. Si PY a
# PyOpenColorIO et numpy (lot E11) : couleur de base en ACEScg par SUBZERO_OCIO_COLOR dans studio.env, pixels comparés à
# PyOpenColorIO, espace qui suit le réglage sauf choix de l'artiste. La fenêtre seule (cas, règles, décalques, formats,
# panneau Settings…) est testée contre un faux ArmorPaint : subzero_window_ui.py.
#
#   sh .tests/subzero_app.sh [dossier de travail]
#   APP=<binaire ArmorPaint> (défaut : build Release de paint/), PY=<python avec PySide6, et PyOpenColorIO et numpy pour
#   E11> (défaut : .shotgrid/venv ; tools/setup_subzero.sh en crée un complet),
#   SUBZERO_SBSRENDER=<sbsrender> (défaut : détection dans /Applications ; passé à l'app par un studio.env du banc),
#   SUBZERO_SAMPLES comme subzero_check.py.
set -eu
EXT=$(cd "$(dirname "$0")/.." && pwd)
APP=${APP:-$EXT/../../build/build/Release/ArmorPaint.app/Contents/MacOS/ArmorPaint}
PY=${PY:-$EXT/.shotgrid/venv/bin/python}
W=${1:-$(mktemp -d)}
mkdir -p "$W"
W=$(cd "$W" && pwd)
SAMPLES=${SUBZERO_SAMPLES:-$HOME/Desktop/code/sbsar}
PLANKS=$SAMPLES/colorful_planks.sbsar
[ -x "$APP" ] || { echo "ArmorPaint not found: $APP" >&2; exit 1; }
"$PY" -c "import PySide6" 2>/dev/null || { echo "PySide6 not found for $PY (tools/setup_shotgrid.sh)" >&2; exit 1; }
[ -f "$PLANKS" ] || { echo "skip: $PLANKS not found"; exit 0; }
FAIL=0
ok() { echo "ok   $1"; }
ko() { echo "FAIL $1"; FAIL=1; }
check() { if eval "$2"; then ok "$1"; else ko "$1"; fi; }
json() { "$PY" -c "import json,sys; r=json.load(open(sys.argv[1])); print(eval(sys.argv[2]))" "$@"; }
# Ligne « t: <tag> <json> » du journal -> JSON.
tline() { sed -n "s/^.*t: $2 //p" "$1" | tail -1; }
exr_width() { PYTHONPATH="$EXT/assets" "$PY" -c "import subzero_exr, sys; print(subzero_exr.Image(sys.argv[1]).width)" "$1"; }

rm -rf "$W/p.arm" "$W/p_subzero" "$W"/*.result.json "$W"/*.log "$W"/*.stx "$W/cache"
printf '{"version": 1}\n' > "$W/prefs.json"
# Environnement du banc (lot E5) : son propre studio.env, jamais celui de l'utilisateur, qui donne sbsrender quand
# SUBZERO_SBSRENDER est fourni (retiré de l'environnement : la lecture de $STUDIO_ENV est ainsi testée).
SBSRENDER=${SUBZERO_SBSRENDER:-}
unset SUBZERO_SBSRENDER SUBZERO_CACHE
{
	echo "# subzero_app.sh"
	[ -z "$SBSRENDER" ] || printf "export SUBZERO_SBSRENDER='%s'\n" "$SBSRENDER"
	echo 'SUBZERO_CACHE="${STUDIO_TEST_DIR}/cache"  # étendu à la lecture'
} > "$W/studio.env"
export STUDIO_ENV="$W/studio.env" STUDIO_TEST_DIR="$W" STUDIO_PREFS="$W/prefs.json" STUDIO_PYTHON="$PY" QT_QPA_PLATFORM=offscreen

# « Envoyer » -> matériau à jour dans ArmorPaint en 2,5 s au plus (critère du chantier E) : rendu 2048 des 6 sorties,
# puis import ou rechargement des textures.
timing() {
	check "envoi -> matériau à jour en moins de 2,5 s (rendu 2048, 6 sorties, puis ArmorPaint)" '[ "$(json "$R" "r[\"requests\"][0][\"render\"] + r[\"requests\"][0][\"armorpaint\"] < 2.5")" = True ]'
	echo "     rendu $(json "$R" "r['requests'][0]['render']") s, ArmorPaint $(json "$R" "r['requests'][0]['armorpaint']") s (charge de la machine $(sysctl -n vm.loadavg | cut -d' ' -f2))"
}

# 1. Projet vide avec un calque, enregistré.
cat > "$W/make.c" <<EOF
int main() {
	script_project_new();
	project_filepath_set("$W/p.arm");
	project_save(false);
	return 0;
}
EOF
perl -e 'alarm 120; exec @ARGV' "$APP" --background --script "$W/make.c" -ApplePersistenceIgnoreState YES > "$W/make.log" 2>&1 || true
[ -f "$W/p.arm" ] || { echo "project not saved, see $W/make.log" >&2; exit 1; }

# Lance ArmorPaint sur p.arm avec le script $2 (ouverture de la fenêtre, à la frame OPEN_AT : le démarrage d'ArmorPaint,
# textures et shaders, ralentit sbsrender d'une seconde environ), l'essai $3, puis $4 une fois la fenêtre fermée.
OPEN_AT=${OPEN_AT:-120}
run() {
	name=$1
	printf '%s\n' "$3" | sed "s|}\$|, \"result\": \"$W/$name.result.json\", \"screenshot\": \"$W/$name.png\"}|" > "$W/$name.test.json"
	cat > "$W/$name.c" <<EOF
int frame = 0;
int closed = 0;
void update() {
	frame++;
	if (frame == $OPEN_AT) {
		$2
		console_log(string("t: open %d", subzero_is_open()));
	}
	if (closed == 0 && frame > $OPEN_AT && !subzero_is_open()) {
		closed = frame;
		console_log(string("t: after %s", subzero_materials_json()));
		$4
	}
	if (closed > 0 && frame == closed + 10) {
		script_quit();
	}
}
int main() {
	console_log(string("t: before %s", subzero_materials_json()));
	script_notify_on_update(update);
	return 0;
}
EOF
	t0=$(date +%s)
	SUBZERO_TEST="$W/$name.test.json" perl -e 'alarm 180; exec @ARGV' "$APP" "$W/p.arm" --script "$W/$name.c" \
		-ApplePersistenceIgnoreState YES > "$W/$name.log" 2>&1 || true
	echo "     $name : $(($(date +%s) - t0)) s"
}

# 2. Import du .sbsar (aiguillé vers subzero), envoi à la résolution du projet ; le calque, et un second, deviennent
# des calques de remplissage de ce matériau ; projet enregistré.
DIR=$W/p_subzero/ColorfulPlanks
# Journal « t: cs » : source et espace de la couleur de base, puis de la normale.
CS='console_log(string("t: cs %s|%s|%s|%s", studio_texture_color_space_source("'"$DIR"'/ColorfulPlanks_basecolor.EXT"), studio_texture_color_space("'"$DIR"'/ColorfulPlanks_basecolor.EXT"), studio_texture_color_space_source("'"$DIR"'/ColorfulPlanks_normal.EXT"), studio_texture_color_space("'"$DIR"'/ColorfulPlanks_normal.EXT")));'
# Avec PyOpenColorIO (lot E11), la fenêtre donne aussi l'espace des cartes de donnée (rôle data) ; sans, ArmorPaint les
# reconnaît à leur nom.
if "$PY" -c "import PyOpenColorIO, numpy" 2> /dev/null; then HAS_OCIO=1 DATA_SRC=user; else HAS_OCIO=0 DATA_SRC=name; fi
run create "script_import_asset(\"$PLANKS\", 0);" '{"action": "create"}' "$(echo "$CS" | sed s/EXT/exr/g)"' console_log(string("t: fill %d", studio_layer_op("to_fill"))); studio_layer_new(); console_log(string("t: fill2 %d", studio_layer_op("to_fill"))); project_save(0);'
R=$W/create.result.json
L=$W/create.log
check "sbsrender trouvé au démarrage (log)" 'grep -q "subzero: sbsrender [0-9.]* (build [0-9]*) (" "$L"'
check "studio.env du banc lu au démarrage, variables étendues (log)" 'grep -q "studio: env $W/studio.env: " "$L" && grep -q "studio: env SUBZERO_CACHE=$W/cache ($W/studio.env)" "$L"'
if [ -n "$SBSRENDER" ]; then
	check "sbsrender donné par \$SUBZERO_SBSRENDER, depuis studio.env (log)" 'grep -q "subzero: sbsrender .*, from \$SUBZERO_SBSRENDER ($W/studio.env)" "$L"'
fi
check "import du .sbsar : fenêtre ouverte" '[ "$(tline "$L" open)" = 1 ] && grep -q "studio: subzero window: .*subzero_window.py" "$L"'
check "fenêtre : état (projet, résolution, fichier à ouvrir)" '[ -s "$R" ] && [ "$(json "$R" "r[\"state\"][\"project\"][\"file\"]")" = "$W/p.arm" ] && [ "$(json "$R" "r[\"state\"][\"open\"][\"file\"]")" = "$PLANKS" ] && [ "$(json "$R" "r[\"state\"][\"resolution\"][0]")" = 2048 ]'
check "create : matériau créé" '[ "$(json "$R" "[(q[\"action\"], q[\"reply\"][\"material\"], q[\"reply\"][\"created\"]) for q in r[\"requests\"]]")" = "[('"'"'create'"'"', '"'"'Colorful Planks'"'"', True)]" ]'
check "textures dans p_subzero/ColorfulPlanks, en EXR, en 2048, et la recette" '[ -f "$DIR/ColorfulPlanks_basecolor.exr" ] && [ -f "$DIR/subzero.json" ] && [ "$(exr_width "$DIR/ColorfulPlanks_normal.exr")" = 2048 ]'
A=$(tline "$L" after)
check "ArmorPaint : un matériau subzero choisi, 6 textures reliées à la sortie" '"$PY" - "$A" "$DIR" <<PY
import json, sys
a, d = json.loads(sys.argv[1]), sys.argv[2]
s = a["subzero"]
assert len(s) == 1 and s[0]["name"] == "Colorful Planks" and s[0]["dir"] == d and s[0]["current"], s
links = sorted((l["socket"], l["output"], l["file"]) for l in s[0]["links"])
assert [l[0] for l in links] == [0, 2, 3, 4, 5, 7] and all(l[1] == 0 for l in links), links
assert links[0][2] == "ColorfulPlanks_basecolor.exr" and links[4][2] == "ColorfulPlanks_normal.exr", links
PY'
check "deux calques de remplissage de ce matériau" '[ "$(tline "$L" fill)" = 1 ] && [ "$(tline "$L" fill2)" = 1 ]'
echo "     espaces : $(tline "$L" cs)"
check "couleur de base EXR importée en srgb_tx (choix de subzero), normale en donnée ($DATA_SRC)" '[ "$(tline "$L" cs | cut -d"|" -f1,3,4)" = "user|$DATA_SRC|Raw" ] && [ -n "$(tline "$L" cs | cut -d"|" -f2)" ]'
timing
MATS=$("$PY" -c "import json,sys; print(json.loads(sys.argv[1])['materials'])" "$A")

# 3. Projet rouvert, matériau réédité depuis son dossier : mise à jour, calque de remplissage refait.
run update "subzero_open(\"$DIR\");" '{"values": {"age": 0.9}, "action": "update"}' ''
R=$W/update.result.json
L=$W/update.log
B=$(tline "$L" before)
A=$(tline "$L" after)
check "rouvert : le matériau subzero est retrouvé par son dossier" '"$PY" -c "import json,sys; s=json.loads(sys.argv[1])[\"subzero\"]; assert len(s)==1 and s[0][\"dir\"]==sys.argv[2], s" "$B" "$DIR"'
check "fenêtre : ouverte sur la recette (dossier), réglages repris" '[ "$(json "$R" "r[\"state\"][\"open\"][\"dir\"]")" = "$DIR" ] && [ "$(json "$R" "r[\"material_name\"]")" = "Colorful Planks" ]'
check "update : même matériau, 6 textures rechargées" '[ "$(json "$R" "[(q[\"action\"], q[\"reply\"][\"created\"], q[\"reply\"][\"reloaded\"]) for q in r[\"requests\"]]")" = "[('"'"'update'"'"', False, 6)]" ]'
timing
check "pas de nouveau matériau" '[ "$("$PY" -c "import json,sys; print(json.loads(sys.argv[1])[\"materials\"])" "$A")" = "$MATS" ]'
check "les deux calques de remplissage refaits (log)" 'grep -q "subzero: material Colorful Planks refreshed, 2 fill layer(s)" "$L"'
check "recette : nouvelle valeur" '[ "$(json "$DIR/subzero.json" "r[\"values\"][\"age\"]")" = 0.9 ]'

# 4. Format changé (EXR -> PNG) : nouvelles textures, anciennes retirées du projet puis supprimées par la fenêtre.
run format "subzero_open(\"$DIR\");" '{"format": "png", "action": "update"}' "$(echo "$CS" | sed s/EXT/png/g)"' console_log(string("t: dump %d %d", studio_texture_dump("'"$DIR"'/ColorfulPlanks_basecolor.png", "'"$W"'/png.stx"), studio_texture_dump("'"$DIR"'/ColorfulPlanks_basecolor.exr", "'"$W"'/exr.stx"))); project_save(0);'
R=$W/format.result.json
L=$W/format.log
A=$(tline "$L" after)
check "update en PNG : même matériau, 6 anciennes textures retirées, aucune gardée" '[ "$(json "$R" "[(q[\"action\"], q[\"reply\"][\"created\"], q[\"reply\"][\"removed\"], q[\"reply\"][\"kept\"]) for q in r[\"requests\"]]")" = "[('"'"'update'"'"', False, 6, [])]" ]'
check "retrait journalisé pour chaque EXR (log)" '[ "$(grep -c "subzero: texture .*ColorfulPlanks_.*\.exr removed from the project (no longer rendered)" "$L")" = 6 ]'
check "dossier : les 6 PNG et la recette, EXR supprimés par la fenêtre" '[ "$(cd "$DIR" && ls | tr "\n" " ")" = "ColorfulPlanks_basecolor.png ColorfulPlanks_height.png ColorfulPlanks_metallic.png ColorfulPlanks_normal.png ColorfulPlanks_occlusion.png ColorfulPlanks_roughness.png subzero.json " ] && [ "$(json "$R" "len(r[\"requests\"][0][\"deleted\"])")" = 6 ]'
check "ArmorPaint : le matériau relié aux PNG" '"$PY" - "$A" <<PY
import json, sys
s = json.loads(sys.argv[1])["subzero"]
links = sorted((l["socket"], l["file"]) for l in s[0]["links"])
assert len(s) == 1 and [l[0] for l in links] == [0, 2, 3, 4, 5, 7] and all(l[1].endswith(".png") for l in links), links
PY'
check "pas de nouveau matériau, calques de remplissage refaits" '[ "$("$PY" -c "import json,sys; print(json.loads(sys.argv[1])[\"materials\"])" "$A")" = "$MATS" ] && grep -q "subzero: material Colorful Planks refreshed, 2 fill layer(s)" "$L"'
echo "     espaces : $(tline "$L" cs)"
check "couleur de base PNG en srgb_tx ; l'ancienne texture EXR n'est plus chargée" '[ "$(tline "$L" cs | cut -d"|" -f1)" = user ] && [ "$(tline "$L" dump)" = "1 0" ]'
check "recette : format png" '[ "$(json "$DIR/subzero.json" "r[\"format\"]")" = png ]'
check "aucune erreur de la fenêtre" '! grep -q "Traceback" "$W/create.log" "$W/update.log" "$W/format.log"'

# 5. Chaque format du menu (couleur et gris), et les fichiers que la fenêtre réécrit pour un décalque (EXR en B G R et
# opacité en Y ; PNG et TIFF sans alpha et en gris), importés par ArmorPaint : décodés à la bonne taille.
F=$W/formats
rm -rf "$F"
FORMATS=$(PYTHONPATH="$EXT/assets" "$PY" -c "import subzero_render; print(' '.join(subzero_render.FORMATS))")
for fmt in $FORMATS; do
	SUBZERO_CACHE="$W/cache" "$PY" "$EXT/assets/subzero_render.py" ${SBSRENDER:+--sbsrender "$SBSRENDER"} render "$PLANKS" \
		--out "$F/$fmt" --size 8 --output basecolor --output height --format "$fmt" > "$F.$fmt.json" 2>&1 || ko "rendu $fmt ($F.$fmt.json)"
done
PYTHONPATH="$EXT/assets" QT_QPA_PLATFORM=offscreen "$PY" - "$F" <<'PY' || ko "décalques préparés"
# Couleur de base rendue, alpha remplacé par un dégradé, puis séparée comme par la fenêtre.
import os, shutil, struct, sys
import subzero_exr
from PySide6 import QtGui
f = sys.argv[1]
app = QtGui.QGuiApplication([])
src, dst = os.path.join(f, "exr16", "basecolor.exr"), os.path.join(f, "decal", "decal.exr")
os.makedirs(os.path.dirname(dst))
with subzero_exr.Image(src) as im:
    w, h = im.width, im.height
    ramp = struct.pack("<%de" % w, *(x / (w - 1.0) for x in range(w)))
    lines = [ramp + b"".join(im.channel_bytes(im.line(y), c) for c in "BGR") for y in range(h)]
    subzero_exr.write(dst, w, h, [(c, subzero_exr.HALF) for c in "ABGR"], lines)
import subzero_material as sm
assert sm.split_alpha(dst, os.path.join(f, "decal", "decal_opacity.exr"))
for ext in ("png", "tif"):
    image = QtGui.QImage(os.path.join(f, ext, "basecolor." + ext)).convertToFormat(QtGui.QImage.Format_ARGB32)
    for x in range(image.width()):
        for y in range(image.height()):
            c = image.pixelColor(x, y)
            c.setAlpha(x)
            image.setPixelColor(x, y, c)
    base = os.path.join(f, "decal", "decal." + ext)
    image.save(base)
    assert sm.split_alpha(base, os.path.join(f, "decal", "decal_opacity." + ext))
PY
IMPORTS=$(cd "$F" && ls */*.* | grep -v "\.json$")
{
	echo 'int main() {'
	echo '	script_project_new();'
	for n in $IMPORTS; do printf '	script_import_asset("%s", 0);
' "$F/$n"; done
	for n in $IMPORTS; do printf '	console_log(string("t: dump %s %%d", studio_texture_dump("%s", "%s")));
' "$n" "$F/$n" "$F/$(echo "$n" | tr / _).stx"; done
	echo '	return 0;'
	echo '}'
} > "$W/formats.c"
perl -e 'alarm 120; exec @ARGV' "$APP" --background --script "$W/formats.c" -ApplePersistenceIgnoreState YES > "$W/formats.log" 2>&1 || true
BAD=$(for n in $IMPORTS; do
	stx=$F/$(echo "$n" | tr / _).stx
	[ "$(sed -n "s|^t: dump $n ||p" "$W/formats.log")" = 1 ] && [ "$("$PY" -c "import struct, sys; print(struct.unpack_from('<2i', open(sys.argv[1], 'rb').read(12), 4))" "$stx")" = "(256, 256)" ] || echo "$n"
done)
echo "     $(echo $IMPORTS | wc -w | tr -d ' ') fichiers : $(echo $IMPORTS)"
check "chaque format importé et décodé en 256 par ArmorPaint, décalques EXR (B G R, Y), PNG et TIFF compris" '[ -z "$BAD" ]'
[ -z "$BAD" ] || echo "     illisibles : $(echo $BAD)"

# 6. Lot E11 : SUBZERO_OCIO_COLOR=ACEScg dans le studio.env du banc, lu au démarrage d'ArmorPaint ; retour en EXR 16f.
# La fenêtre convertit la couleur de base (seule) en ACEScg, ArmorPaint l'importe dans cet espace : ses pixels dans
# l'espace de travail sont ceux de PyOpenColorIO. Puis le réglage retiré (7) : la texture, restée dans l'espace que
# subzero lui avait donné, le suit, et la couleur dans ArmorPaint est la même qu'en ACEScg ; remis (8) après un choix de
# l'artiste dans ArmorPaint : ce choix est gardé.
if [ $HAS_OCIO = 0 ]; then
	echo "skip couleur (E11) : PyOpenColorIO ou numpy absent de $PY (tools/setup_subzero.sh)"
else
	SRGB="sRGB Encoded Rec.709 (sRGB)"
	BASE=$DIR/ColorfulPlanks_basecolor.exr
	ENV0=$(cat "$W/studio.env")
	# Espaces (t: cs), pixels de la couleur de base relus d'ArmorPaint dans $W/<étape>.stx, projet enregistré.
	cs_dump() {
		echo "$CS" | sed s/EXT/exr/g
		printf ' console_log(string("t: dump %%d", studio_texture_dump("%s", "%s/%s.stx"))); project_save(0);' "$BASE" "$W" "$1"
	}
	# Lien de la couleur de base de la requête : espace, espace précédent.
	base_link() { json "$1" "[(l.get('color_space'), l.get('color_space_was')) for l in r['requests'][0]['args']['links'] if l['socket'] == 0]"; }
	# Pixels d'ArmorPaint = PyOpenColorIO, par le banc du lot A4 (OpenEXR, numpy).
	CPY=$PY
	"$CPY" -c "import OpenEXR" 2> /dev/null || CPY=$EXT/.ocio/venv/bin/python
	px() {
		out=$("$CPY" "$EXT/.tests/ocio_import_check.py" convert "$@") && r=0 || r=$?
		echo "$out" | sed 's/^/     /'
		return $r
	}

	printf '%s\nSUBZERO_OCIO_COLOR=ACEScg\n' "$ENV0" > "$W/studio.env"
	run ocio "subzero_open(\"$DIR\");" '{"format": "exr16", "action": "update"}' "$(cs_dump ocio)"
	R=$W/ocio.result.json
	L=$W/ocio.log
	check "E11 : SUBZERO_OCIO_COLOR lu dans studio.env au démarrage (log)" 'grep -q "studio: env SUBZERO_OCIO_COLOR=ACEScg ($W/studio.env)" "$L"'
	check "E11 : fenêtre avec la configuration d'ArmorPaint, réglage venu du fichier" '[ "$(json "$R" "(r[\"color\"][\"ready\"], r[\"color\"][\"origins\"].get(\"SUBZERO_OCIO_COLOR\"))")" = "(True, '"'"'$W/studio.env'"'"')" ]'
	check "E11 : couleur de base seule convertie, liée en ACEScg (avant : $SRGB, recette de l'étape 4)" '[ "$(json "$R" "[x[\"converted\"] for x in r[\"renders\"] if x[\"kind\"] == \"final\"][-1]")" = "['"'"'base'"'"']" ] && [ "$(base_link "$R")" = "[('"'"'ACEScg'"'"', '"'"'$SRGB'"'"')]" ]'
	echo "     espaces : $(tline "$L" cs)"
	check "E11 : ArmorPaint importe la couleur de base en ACEScg, la normale en Raw (choix de subzero)" '[ "$(tline "$L" cs)" = "user|ACEScg|user|Raw" ] && [ "$(tline "$L" dump)" = 1 ]'
	check "E11 : pixels dans l'espace de travail = PyOpenColorIO ACEScg" 'px "$W/ocio.stx" "$BASE" ACEScg'
	check "E11 : recette en ACEScg" '[ "$(json "$DIR/subzero.json" "r[\"color\"][\"spaces\"][\"base\"]")" = ACEScg ]'

	printf '%s\n' "$ENV0" > "$W/studio.env"
	run follow "subzero_open(\"$DIR\");" '{"action": "update"}' "$(cs_dump follow)"
	R=$W/follow.result.json
	L=$W/follow.log
	check "E11 réglage retiré : couleur de base liée en $SRGB, depuis ACEScg" '[ "$(base_link "$R")" = "[('"'"'$SRGB'"'"', '"'"'ACEScg'"'"')]" ]'
	check "E11 réglage retiré : la texture suit, pixels relus du fichier réécrit" 'grep -q "subzero: $BASE now in $SRGB (was ACEScg)" "$L" && [ "$(tline "$L" cs)" = "user|$SRGB|user|Raw" ] && px "$W/follow.stx" "$BASE" "$SRGB"'
	# Deux rendus de sbsrender : jusqu'à 5 % des pixels changent de plus de 1 % d'un rendu à l'autre (Metal, pas
	# déterministe), d'où une comparaison statistique ; une couleur mal lue (ACEScg pris pour du sRGB ou du linéaire)
	# en écarte plus de 99 %, avec un écart moyen de 1,6e-2 au moins.
	check "E11 : même couleur dans ArmorPaint, livrée en ACEScg ou en sRGB" '"$PY" - "$W/ocio.stx" "$W/follow.stx" <<PY
import sys
import numpy as np
def stx(p):
    b = open(p, "rb").read()
    w, h, f = np.frombuffer(b, "<i4", 3, 4)
    assert f == 1, (p, f)  # GPU_TEXTURE_FORMAT_RGBA64 : EXR half
    return np.frombuffer(b, np.float16, offset=16).reshape(h, w, 4).astype(np.float64)
a, b = stx(sys.argv[1]), stx(sys.argv[2])
d = np.abs(a[..., :3] - b[..., :3])
off = (d / np.maximum(np.abs(b[..., :3]), 1e-3)).max(-1) > 1e-2
print("     écart moyen %.2e, pixels à plus de 1 %% : %.2f %%" % (d.mean(), off.mean() * 100))
assert d.mean() < 1e-3 and off.mean() < 0.2
PY'

	printf '%s\nSUBZERO_OCIO_COLOR=ACEScg\n' "$ENV0" > "$W/studio.env"
	run artist "studio_texture_set_color_space(\"$BASE\", \"Raw\"); subzero_open(\"$DIR\");" '{"action": "update"}' "$(cs_dump artist)"
	R=$W/artist.result.json
	L=$W/artist.log
	check "E11 choix de l'artiste : lien ACEScg depuis $SRGB, texture gardée en Raw" '[ "$(base_link "$R")" = "[('"'"'ACEScg'"'"', '"'"'$SRGB'"'"')]" ] && [ "$(tline "$L" cs)" = "user|Raw|user|Raw" ] && ! grep -q "now in" "$L"'
	check "E11 : aucune erreur de la fenêtre" '! grep -q "Traceback" "$W/ocio.log" "$W/follow.log" "$W/artist.log"'
	printf '%s\n' "$ENV0" > "$W/studio.env"
fi

[ $FAIL -eq 0 ] && echo "== OK" || echo "== FAIL"
exit $FAIL
