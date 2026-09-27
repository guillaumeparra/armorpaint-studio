#!/bin/sh
# Mode batch (lot B7) dans ArmorPaint lui-même : --background sans fenêtre (patch headless), options studio de
# --export-textures (--export-mode, --export-naming, --export-color-space), rapport JSON (--report) et code de retour
# (patch batch-report) : 0 si tout a abouti, 1 sur erreur (fichier introuvable, console_error, erreur de script, export
# bloqué), 2 sur ligne de commande fausse (rien n'est lancé).
#
#   sh .tests/batch_app.sh [dossier de travail]
#   APP=<binaire ArmorPaint> pour un autre build (défaut : build Release de paint/).
# Demande : python3 avec PIL ; swiftc (Xcode) pour vérifier qu'aucune fenêtre n'est à l'écran, sinon ce contrôle est sauté.
set -eu
EXT=$(cd "$(dirname "$0")/.." && pwd)
APP=${APP:-$EXT/../../build/build/Release/ArmorPaint.app/Contents/MacOS/ArmorPaint}
M=$EXT/.tests/meshes
W=${1:-$(mktemp -d)}
mkdir -p "$W"
W=$(cd "$W" && pwd)
[ -x "$APP" ] || { echo "ArmorPaint not found: $APP" >&2; exit 1; }
FAIL=0
ok() { echo "ok   $1"; }
ko() { echo "FAIL $1"; FAIL=1; }
check() { if eval "$2"; then ok "$1"; else ko "$1"; fi; }
# Valeur d'un rapport : json <rapport> <expression python sur r>
json() { python3 -c "import json,sys; r=json.load(open(sys.argv[1])); print(eval(sys.argv[2]))" "$@"; }
# Condition python sur un rapport : jtrue <rapport> <expression sur r>
jtrue() { [ "$(json "$@" 2> /dev/null)" = True ]; }
# Lance ArmorPaint ; son code de retour dans $RC, sa sortie dans $W/<nom>.log. -ApplePersistenceIgnoreState en tête :
# la fin de la ligne reste celle du test.
run() {
	name=$1
	shift
	perl -e 'alarm 120; exec @ARGV' "$APP" -ApplePersistenceIgnoreState YES "$@" > "$W/$name.log" 2>&1 && RC=0 || RC=$?
}
# Fichiers d'un dossier (chemins complets, triés) = fichiers du premier export du rapport ?
same_files() {
	[ "$(find "$2" -type f | sort)" = "$(json "$1" "'\n'.join(sorted(r['exports'][0]['files'] + ([r['exports'][0]['manifest']] if r['exports'][0]['manifest'] else [])))")" ]
}
unset STUDIO_EXPORT_PRESETS STUDIO_EXPORT_COLOR_SPACE STUDIO_SG_NAMING STUDIO_PREFS

rm -rf "$W"/out_* "$W"/*.json "$W"/*.arm "$W"/*.log "$W"/*.c "$W/windows"
mkdir -p "$W/out_fresh" "$W/out_up" "$W/out_tile" "$W/out_object" "$W/out_cs" "$W/out_blocked" "$W/out_usage"

MAT='slot_material_t *m = script_material_create("mat");
	ui_node_t *rgb = script_material_create_node_at("RGB", -400.0, 0.0);
	ui_node_t *out = script_material_get_node("OUTPUT_MATERIAL_PBR");
	script_material_set_color(rgb, 0, 0, 0.8, 0.35, 0.1, 1.0);
	script_material_connect(rgb, 0, out, 0);
	script_material_update();
	script_material_set(m);
	script_fill_layer();'

# 0. Projets : cube UDIM (6 tuiles) ; props (3 objets), un calque limité à barrel.
cat > "$W/make.c" <<EOF
int main() {
	studio_import_mesh("$M/udim_cube.obj", 2);
	$MAT
	project_filepath_set("$W/cube_v002.arm");
	project_save(false);
	return 0;
}
EOF
run make --background --script "$W/make.c"
cat > "$W/make2.c" <<EOF
int main() {
	studio_import_mesh("$M/props.obj", 0);
	$MAT
	studio_layer_new();
	studio_layer_set_object(1);
	project_filepath_set("$W/props.arm");
	project_save(false);
	return 0;
}
EOF
run make2 --background --script "$W/make2.c"
check "projets de test enregistrés" '[ -f "$W/cube_v002.arm" ] && [ -f "$W/props.arm" ]'

echo "== sans fenêtre"
# 1. --background : aucune fenêtre à l'écran, pas d'icône dans le Dock (UIElement), le focus n'est pas pris.
cat > "$W/sleep.c" <<'EOF'
int main() {
	int j = studio_exec_run(string_split("sleep\n3", "\n"), NULL, 10000);
	return 0;
}
EOF
if command -v swiftc > /dev/null 2>&1; then
	cat > "$W/windows.swift" <<'EOF'
import CoreGraphics
// Fenêtres d'un processus : "onscreen=<n> total=<n>"
let pid = Int32(CommandLine.arguments[1])!
let all = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as! [[String: Any]]
let mine = all.filter { ($0[kCGWindowOwnerPID as String] as? Int32) == pid }
let on = mine.filter { ($0[kCGWindowIsOnscreen as String] as? Bool) == true }
print("onscreen=\(on.count) total=\(mine.count)")
EOF
	swiftc -O -o "$W/windows" "$W/windows.swift" > "$W/swiftc.log" 2>&1 || true
fi
perl -e 'alarm 60; exec @ARGV' "$APP" --background --script "$W/sleep.c" -ApplePersistenceIgnoreState YES > "$W/sleep.log" 2>&1 &
PID=$!
sleep 1.5
TYPE=$(lsappinfo list 2> /dev/null | grep "pid = $PID " | sed -n 's/.*type="\([A-Za-z]*\)".*/\1/p')
if [ -x "$W/windows" ]; then
	WINDOWS=$("$W/windows" $PID)
	check "aucune fenêtre à l'écran ($WINDOWS)" '[ "${WINDOWS%% *}" = "onscreen=0" ]'
else
	echo "skip swiftc absent : fenêtres non vérifiées"
fi
check "application sans icône dans le Dock (type $TYPE)" '[ "$TYPE" = UIElement ]'
wait $PID && RC=0 || RC=$?
check "le script a tourné sans fenêtre, code 0" '[ $RC -eq 0 ]'

echo "== exports"
# 2. Preset upstream : fichiers écrits par l'upstream listés dans le rapport, code 0.
T0=$(python3 -c 'import time; print(time.time())')
run up "$M/props.obj" --background --export-textures png generic "$W/out_up" --report "$W/up.json"
T1=$(python3 -c 'import time; print(time.time())')
echo "     export upstream : $(python3 -c "print('%.2f s' % ($T1 - $T0))")"
check "preset upstream : code 0, rapport ok, 5 PNG" '[ $RC -eq 0 ] && jtrue "$W/up.json" "r[\"status\"] == \"ok\" and r[\"exit_code\"] == 0" && [ $(ls "$W/out_up"/*.png | wc -l) -eq 5 ]'
check "preset upstream : fichiers du rapport = fichiers écrits" 'same_files "$W/up.json" "$W/out_up"'
check "preset upstream : export décrit (preset, mode, destination)" 'jtrue "$W/up.json" "[(e[\"preset\"], e[\"studio_preset\"], e[\"mode\"], e[\"destination\"], e[\"ok\"]) for e in r[\"exports\"]] == [(\"generic\", False, \"visible\", \"disk\", True)]"'
check "rapport : journal, arguments, durée" 'jtrue "$W/up.json" "r[\"log\"][-1][\"message\"] == \"Textures exported\" and \"--report\" in r[\"argv\"] and r[\"duration_s\"] > 0 and r[\"errors\"] == []"'

# 3. Preset studio, par tuile UDIM, modèle de nom : 9 maps × 6 tuiles, manifeste.
run tile "$W/cube_v002.arm" --background --export-textures exr16 basic "$W/out_tile" --export-mode per-tile \
	--export-naming "<name>_v<version>_<channel>.<UDIM>.<ext>" --report "$W/tile.json"
check "par tuile : code 0, 54 EXR nommés par le modèle" '[ $RC -eq 0 ] && [ $(ls "$W/out_tile"/cube_v002_*.exr | wc -l) -eq 54 ] && for t in 1001 1002 1003 1011 1012 1013; do [ -f "$W/out_tile/cube_v002_base_color.$t.exr" ] || exit 1; done'
check "par tuile : rapport (mode per-tile, preset studio, manifeste) = fichiers écrits" 'jtrue "$W/tile.json" "[(e[\"mode\"], e[\"studio_preset\"], e[\"ok\"]) for e in r[\"exports\"]] == [(\"per-tile\", True, True)]" && same_files "$W/tile.json" "$W/out_tile"'

# 4. Par objet : seul barrel a un calque à lui.
run object "$W/props.arm" --background --export-textures exr16 basic "$W/out_object" --export-mode per-object \
	--export-naming "<object>_<channel>.<ext>" --report "$W/object.json"
check "par objet : code 0, 9 maps de barrel" '[ $RC -eq 0 ] && [ $(ls "$W/out_object"/barrel_*.exr | wc -l) -eq 9 ] && [ $(ls "$W/out_object"/*.exr | wc -l) -eq 9 ] && same_files "$W/object.json" "$W/out_object"'

# 5. Espace OCIO imposé aux textures de couleur.
run cs "$M/props.obj" --background --export-textures png arnold "$W/out_cs" --export-color-space ACEScg --report "$W/cs.json"
check "espace imposé : code 0, manifeste en ACEScg" '[ $RC -eq 0 ] && grep -q "\"color_space_override\": \"ACEScg\"" "$W/out_cs/untitled.export.json" && [ $(grep -c "\"color_space\": \"ACEScg\"" "$W/out_cs/untitled.export.json") -eq 2 ]'

echo "== code 1 : erreurs pendant le run"
run nomesh "$W/absent.obj" --background --export-textures png generic "$W/out_up" --report "$W/nomesh.json"
check "maillage introuvable : code 1, erreur dans le rapport" '[ $RC -eq 1 ] && jtrue "$W/nomesh.json" "r[\"status\"] == \"failed\" and r[\"exit_code\"] == 1 and \"Could not open file\" in r[\"errors\"][0]"'
run blocked "$M/props.obj" --background --export-textures png basic "$W/out_blocked" --export-naming "<object>/<channel>.<ext>" --report "$W/blocked.json"
check "export bloqué par le préflight : code 1, rien d'écrit, export en échec" '[ $RC -eq 1 ] && [ -z "$(ls "$W/out_blocked")" ] && jtrue "$W/blocked.json" "[(e[\"ok\"], e[\"files\"]) for e in r[\"exports\"]] == [(False, [])]"'
printf 'int main() {\n\tconsole_log("t: before");\n\tnope_function(1);\n\treturn 0;\n}\n' > "$W/unknown.c"
run unknown --background --script "$W/unknown.c" --report "$W/unknown.json"
check "fonction inconnue dans le script (format de minic_error) : code 1" '[ $RC -eq 1 ] && jtrue "$W/unknown.json" "r[\"errors\"] == [\"<script>:3: error: unknown function \x27nope_function\x27 (got \x27(\x27)\"]"'
printf 'int main() {\n\tconsole_error("t: the script says no");\n\treturn 0;\n}\n' > "$W/error.c"
run error --background --script "$W/error.c" --report "$W/error.json"
check "console_error dans le script : code 1, niveau error au journal" '[ $RC -eq 1 ] && jtrue "$W/error.json" "r[\"log\"] == [{\"level\": \"error\", \"message\": \"t: the script says no\"}]"'

echo "== script : sorties et rapport en cours"
cat > "$W/outputs.c" <<EOF
int main() {
	studio_batch_output("version", "12");
	studio_batch_output("version", "13");
	studio_batch_output("path", "a \"quoted\" path");
	int j = studio_exec_run(string_split("cat\n$W/outputs.json", "\n"), NULL, 5000);
	console_log(string("t: during %s", studio_exec_stdout(j)));
	console_log(string("t: code=%d", studio_batch_exit_code()));
	return 0;
}
EOF
run outputs --background --script "$W/outputs.c" --report "$W/outputs.json"
check "sorties de script dans le rapport (clé remplacée, texte échappé), code 0" '[ $RC -eq 0 ] && jtrue "$W/outputs.json" "r[\"outputs\"] == {\"version\": \"13\", \"path\": \"a \\\"quoted\\\" path\"}" && grep -q "^t: code=0" "$W/outputs.log"'
check "rapport écrit dès le démarrage : status running pendant le run" 'grep -q "\"status\": \"running\"" "$W/outputs.log" && [ ! -e "$W/outputs.json.tmp" ]'

# Nouveau projet peint et enregistré dans le même script (patch script-project-new) : la remise à zéro des calques que
# project_new laisse à l'image suivante n'efface plus la peinture avant l'enregistrement.
cat > "$W/fresh.c" <<EOF
int main() {
	script_project_new();
	studio_import_mesh("$M/props.obj", 0);
	$MAT
	project_filepath_set("$W/fresh.arm");
	project_save(false);
	return 0;
}
EOF
run fresh --background --script "$W/fresh.c"
run fresh_export "$W/fresh.arm" --background --export-textures png generic "$W/out_fresh"
check "nouveau projet peint puis enregistré par le même script : peinture relue du .arm" '[ $RC -eq 0 ] && python3 -c "
import sys; from PIL import Image
c = Image.open(sys.argv[1]).convert(\"RGB\").getcolors(1 << 20)
sys.exit(not any(n > 1000 and rgb == (204, 89, 26) for n, rgb in c))" "$W/out_fresh/fresh_base.png"'

echo "== code 2 : ligne de commande fausse, rien n'est lancé"
usage() { # usage <nom> <message attendu> <arguments...>
	name=$1
	expect=$2
	shift 2
	run "$name" --report "$W/$name.json" "$@" # en tête : les options sans valeur finissent la ligne
	check "$name : code 2, « $expect »" '[ $RC -eq 2 ] && jtrue "$W/$name.json" "r[\"status\"] == \"failed\" and r[\"exit_code\"] == 2" && json "$W/$name.json" "r[\"errors\"]" | grep -qF -e "$expect" && [ -z "$(ls "$W/out_usage")" ] && ! grep -q "Textures exported" "$W/$name.log"'
}
usage preset "unknown preset nope" "$M/props.obj" --background --export-textures png nope "$W/out_usage"
usage type "expected png, jpg, exr16 or exr32" "$M/props.obj" --background --export-textures tif generic "$W/out_usage"
usage dir "is not a directory" "$M/props.obj" --background --export-textures png generic "$W/absent"
usage dirfile "is not a directory" "$M/props.obj" --background --export-textures png basic "$M/props.obj"
usage short "needs <type> <preset> <directory>" "$M/props.obj" --background --export-textures png generic
usage mode "expected visible, selected, per-object or per-tile" "$M/props.obj" --background --export-textures png basic "$W/out_usage" --export-mode diagonal
usage upnaming "need a studio preset" "$M/props.obj" --background --export-textures png generic "$W/out_usage" --export-naming "<file>_<channel>.<ext>"
usage variable "unknown variable <shader>" "$M/props.obj" --background --export-textures png basic "$W/out_usage" --export-naming "<shader>.<ext>"
usage space "not a color space of the OCIO configuration" "$M/props.obj" --background --export-textures png basic "$W/out_usage" --export-color-space "No Such Space"
usage alone "apply to --export-textures" "$M/props.obj" --background --export-mode per-object
# Un preset studio crée le dossier qui manque (l'upstream n'écrirait rien : code 2 plus haut).
run newdir --report "$W/newdir.json" "$M/props.obj" --background --export-textures png basic "$W/out_new/sub"
check "preset studio vers un dossier absent : créé, code 0" '[ $RC -eq 0 ] && [ -f "$W/out_new/sub/untitled_base_color.exr" ] && same_files "$W/newdir.json" "$W/out_new/sub"'
usage noscript "file not found" --background --script "$W/absent.c"
usage novalue "--export-mode needs a value" "$M/props.obj" --background --export-textures png basic "$W/out_usage" --export-mode
run noreport "$M/props.obj" --background --export-textures png generic "$W/out_usage" --report "$W/absent/r.json"
check "dossier du rapport absent : code 2, rien d'écrit" '[ $RC -eq 2 ] && [ -z "$(ls "$W/out_usage")" ] && grep -q "no such directory" "$W/noreport.log"'

[ $FAIL -eq 0 ] && echo "batch app: OK ($W)" || { echo "batch app: FAILED ($W)"; exit 1; }
