#!/bin/sh
# Chantier E (subzero) dans ArmorPaint lancé avec son interface : un .sbsar importé comme un asset ouvre la fenêtre, qui
# tourne en essai sans intervention (SUBZERO_TEST, Qt offscreen) et envoie le matériau ; le projet est enregistré avec
# ce matériau sur deux calques de remplissage, rouvert, et le matériau réédité depuis son dossier (recette) : mise à jour
# des mêmes textures, sans nouveau matériau, calques de remplissage refaits. La fenêtre seule (cas, règles, décalque,
# 16 bits…) est testée contre un faux ArmorPaint : subzero_window_ui.py.
#
#   sh .tests/subzero_app.sh [dossier de travail]
#   APP=<binaire ArmorPaint> (défaut : build Release de paint/), PY=<python avec PySide6> (défaut : .shotgrid/venv),
#   SUBZERO_SBSRENDER=<sbsrender> (défaut : détection dans /Applications), SUBZERO_SAMPLES comme subzero_check.py.
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

rm -rf "$W/p.arm" "$W/p_subzero" "$W"/*.result.json "$W"/*.log "$W/cache"
printf '{"version": 1, "subzero.sbsrender": "%s"}\n' "${SUBZERO_SBSRENDER:-}" > "$W/prefs.json"
export STUDIO_PREFS="$W/prefs.json" SUBZERO_CACHE="$W/cache" STUDIO_PYTHON="$PY" QT_QPA_PLATFORM=offscreen

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
run create "script_import_asset(\"$PLANKS\", 0);" '{"action": "create"}' 'console_log(string("t: fill %d", studio_layer_op("to_fill"))); studio_layer_new(); console_log(string("t: fill2 %d", studio_layer_op("to_fill"))); project_save(0);'
R=$W/create.result.json
L=$W/create.log
check "sbsrender trouvé au démarrage (log)" 'grep -q "subzero: sbsrender [0-9.]* (build [0-9]*) (" "$L"'
check "import du .sbsar : fenêtre ouverte" '[ "$(tline "$L" open)" = 1 ] && grep -q "studio: subzero window: .*subzero_window.py" "$L"'
check "fenêtre : état (projet, résolution, fichier à ouvrir)" '[ -s "$R" ] && [ "$(json "$R" "r[\"state\"][\"project\"][\"file\"]")" = "$W/p.arm" ] && [ "$(json "$R" "r[\"state\"][\"open\"][\"file\"]")" = "$PLANKS" ] && [ "$(json "$R" "r[\"state\"][\"resolution\"][0]")" = 2048 ]'
check "create : matériau créé" '[ "$(json "$R" "[(q[\"action\"], q[\"reply\"][\"material\"], q[\"reply\"][\"created\"]) for q in r[\"requests\"]]")" = "[('"'"'create'"'"', '"'"'Colorful Planks'"'"', True)]" ]'
check "textures dans p_subzero/ColorfulPlanks, en 2048, et la recette" '[ -f "$DIR/ColorfulPlanks_basecolor.tga" ] && [ -f "$DIR/subzero.json" ] && [ "$("$PY" -c "import sys; h=open(sys.argv[1],\"rb\").read(18); print(h[12]|h[13]<<8)" "$DIR/ColorfulPlanks_normal.tga")" = 2048 ]'
A=$(tline "$L" after)
check "ArmorPaint : un matériau subzero choisi, 6 textures reliées à la sortie" '"$PY" - "$A" "$DIR" <<PY
import json, sys
a, d = json.loads(sys.argv[1]), sys.argv[2]
s = a["subzero"]
assert len(s) == 1 and s[0]["name"] == "Colorful Planks" and s[0]["dir"] == d and s[0]["current"], s
links = sorted((l["socket"], l["output"], l["file"]) for l in s[0]["links"])
assert [l[0] for l in links] == [0, 2, 3, 4, 5, 7] and all(l[1] == 0 for l in links), links
assert links[0][2] == "ColorfulPlanks_basecolor.tga" and links[4][2] == "ColorfulPlanks_normal.tga", links
PY'
check "deux calques de remplissage de ce matériau" '[ "$(tline "$L" fill)" = 1 ] && [ "$(tline "$L" fill2)" = 1 ]'
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
check "aucune erreur de la fenêtre" '! grep -q "Traceback" "$W/create.log" "$W/update.log"'

[ $FAIL -eq 0 ] && echo "== OK" || echo "== FAIL"
exit $FAIL
