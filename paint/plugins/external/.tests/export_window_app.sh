#!/bin/sh
# Fenêtre d'export Qt (lot B6) dans ArmorPaint lancé avec son interface : un script ouvre la fenêtre, qui tourne en
# essai sans intervention (STUDIO_EXPORT_WINDOW_TEST, Qt offscreen) : choix, préflight, export, résultat en JSON.
# Vérifie l'état envoyé à la fenêtre, l'export et son manifeste (espace imposé), les fichiers décochés, le blocage sans
# rien écrire, et qu'une deuxième ouverture pendant la première ramène la fenêtre au lieu d'en lancer une autre. La
# liste groupée (tuiles UDIM, objets) est testée contre un faux ArmorPaint : export_window_ui.py.
#
#   sh .tests/export_window_app.sh [dossier de travail]
#   APP=<binaire ArmorPaint> pour un autre build (défaut : build Release de paint/).
#   PY=<python avec PySide6> (défaut : .shotgrid/venv, créé par tools/setup_shotgrid.sh).
set -eu
EXT=$(cd "$(dirname "$0")/.." && pwd)
APP=${APP:-$EXT/../../build/build/Release/ArmorPaint.app/Contents/MacOS/ArmorPaint}
PY=${PY:-$EXT/.shotgrid/venv/bin/python}
W=${1:-$(mktemp -d)}
mkdir -p "$W"
W=$(cd "$W" && pwd)
[ -x "$APP" ] || { echo "ArmorPaint not found: $APP" >&2; exit 1; }
"$PY" -c "import PySide6" 2>/dev/null || { echo "PySide6 not found for $PY (tools/setup_shotgrid.sh)" >&2; exit 1; }
FAIL=0
ok() { echo "ok   $1"; }
ko() { echo "FAIL $1"; FAIL=1; }
check() { if eval "$2"; then ok "$1"; else ko "$1"; fi; }

rm -rf "$W/out_"* "$W/ro" "$W"/*.result.json
mkdir -p "$W/ro"
chmod 555 "$W/ro"

# 1. Projet : un matériau uni sur le calque de remplissage, enregistré.
cat > "$W/make.c" <<EOF
int main() {
	script_project_new();
	slot_material_t *m = script_material_create("mat");
	ui_node_t *rgb = script_material_create_node_at("RGB", -400.0, 0.0);
	ui_node_t *out = script_material_get_node("OUTPUT_MATERIAL_PBR");
	script_material_set_color(rgb, 0, 0, 0.8, 0.35, 0.1, 1.0);
	script_material_connect(rgb, 0, out, 0);
	script_material_update();
	script_material_set(m);
	script_fill_layer();
	project_filepath_set("$W/p.arm");
	project_save(false);
	return 0;
}
EOF
perl -e 'alarm 120; exec @ARGV' "$APP" --background --script "$W/make.c" -ApplePersistenceIgnoreState YES > "$W/make.log" 2>&1 || true
[ -f "$W/p.arm" ] || { echo "project not saved, see $W/make.log" >&2; exit 1; }

# Ouvre la fenêtre avec ces choix, attend son résultat (ou la fin du délai), puis arrête ArmorPaint.
# $1 nom de l'essai, $2 choix (JSON sans "result"), $3 script d'ouverture (défaut : une ouverture).
window() {
	name=$1
	printf '%s\n' "$2" | sed "s|}\$|, \"result\": \"$W/$name.result.json\"}|" > "$W/$name.test.json"
	cat > "$W/$name.c" <<EOF
int main() {
	${3:-studio_export_window_show();}
	return 0;
}
EOF
	(STUDIO_PYTHON="$PY" STUDIO_EXPORT_WINDOW_TEST="$W/$name.test.json" QT_QPA_PLATFORM=offscreen \
		"$APP" "$W/p.arm" --script "$W/$name.c" -ApplePersistenceIgnoreState YES > "$W/$name.log" 2>&1 & echo $! > "$W/app.pid")
	i=0
	while [ $i -lt 120 ] && [ ! -s "$W/$name.result.json" ]; do
		sleep 1
		i=$((i + 1))
	done
	sleep 1 # la fenêtre se ferme après avoir écrit son résultat
	kill "$(cat "$W/app.pid")" 2>/dev/null || true
	wait 2>/dev/null || true
}

json() { "$PY" -c "import json,sys; r=json.load(open(sys.argv[1])); print(eval(sys.argv[2]))" "$@"; }

# 2. Export karma, couleurs en ACEScg, par la fenêtre.
window ok '{"preset": "karma", "color_space": "ACEScg", "dir": "'"$W/out_ok"'", "mode": 0, "padding": false}'
R=$W/ok.result.json
check "fenêtre lancée, résultat écrit" '[ -s "$R" ]'
check "état : les 5 presets livrés, et eux seuls" '[ "$(json "$R" "sorted(p[\"name\"] for p in r[\"state\"][\"presets\"] if p[\"shipped\"])")" = "['"'"'arnold'"'"', '"'"'basic'"'"', '"'"'cycles'"'"', '"'"'karma'"'"', '"'"'unreal_engine'"'"']" ]'
check "état : projet et OCIO" '[ "$(json "$R" "r[\"state\"][\"project\"][\"name\"]")" = "p" ] && [ "$(json "$R" "r[\"state\"][\"ocio\"][\"ready\"]")" = "True" ] && [ "$(json "$R" "\"ACEScg\" in r[\"state\"][\"color_spaces\"]")" = "True" ]'
check "préflight : 7 fichiers, non bloquant" '[ "$(json "$R" "len(r[\"preflight\"][\"report\"][\"files\"])")" = 7 ] && [ "$(json "$R" "r[\"preflight\"][\"ok\"]")" = "True" ]'
check "export abouti, 7 EXR et le manifeste" '[ "$(json "$R" "r[\"export\"][\"ok\"]")" = "True" ] && [ $(ls "$W/out_ok"/p_*.exr 2>/dev/null | wc -l) -eq 7 ] && [ -f "$W/out_ok/p.export.json" ]'
check "manifeste : preset karma, couleurs en ACEScg, données brutes" '"$PY" - "$W/out_ok/p.export.json" <<PY
import json, sys
m = json.load(open(sys.argv[1]))
assert m["preset"]["name"] == "karma" and m["preset"]["color_space_override"] == "ACEScg", m["preset"]
spaces = {f["texture"]: f["color_space"] for f in m["files"]}
assert spaces["base_color"] == "ACEScg" and spaces["emission_color"] == "ACEScg", spaces
assert spaces["normal"] == "raw" and spaces["specular_roughness"] == "raw", spaces
PY'
check "fenêtre : les fichiers du rapport et le message de fin" '[ "$(json "$R" "len(r[\"files\"])")" = 7 ] && grep -q "Exported 7 files with Karma" "$R"'
check "aucun avertissement (profondeur : note ou rien), la fenêtre n'affiche aucun problème" '"$PY" - "$R" <<PY
import json, sys
r = json.load(open(sys.argv[1]))
issues = r["preflight"]["report"]["issues"]
assert all(i["level"] == "note" for i in issues), issues
assert not any(i["code"] == "bits_above_source" for i in issues), issues
assert r["issues"] == "", r["issues"]
PY'
TEX=$(json "$R" "' '.join(f['texture'] for f in r['preflight']['report']['files'])")

# 3. Fichiers décochés : ni vérifiés ni écrits, gardés dans le rapport.
window skip '{"preset": "karma", "dir": "'"$W/out_skip"'", "mode": 0, "padding": false, "skip": [["normal", "", ""], ["specular_roughness", "", ""]]}'
R=$W/skip.result.json
check "décochés : 5 EXR écrits, manifeste à 5 fichiers" '[ "$(json "$R" "r[\"export\"][\"ok\"]")" = "True" ] && [ $(ls "$W/out_skip"/p_*.exr 2>/dev/null | wc -l) -eq 5 ] && [ ! -e "$W/out_skip/p_normal.exr" ] && [ "$("$PY" -c "import json,sys; print(len(json.load(open(sys.argv[1]))[\"files\"]))" "$W/out_skip/p.export.json")" = 5 ]'
check "décochés : dans le rapport avec skip, message de fin à 5" '[ "$(json "$R" "sorted(f[\"texture\"] for f in r[\"export\"][\"report\"][\"files\"] if f[\"skip\"])")" = "['"'"'normal'"'"', '"'"'specular_roughness'"'"']" ] && grep -q "Exported 5 files with Karma" "$R"'

# 4. Rien de coché : bloqué, pas d'export.
SKIP_ALL=$(for t in $TEX; do printf '["%s", "", ""],' "$t"; done)
window none '{"preset": "karma", "dir": "'"$W/out_none"'", "mode": 0, "padding": false, "skip": ['"${SKIP_ALL%,}"']}'
R=$W/none.result.json
check "rien de coché : bloqué (no_selection), rien d'écrit" '[ -s "$R" ] && [ "$(json "$R" "[i[\"code\"] for i in r[\"preflight\"][\"report\"][\"issues\"] if i[\"level\"] == \"blocking\"]")" = "['"'"'no_selection'"'"']" ] && [ "$(json "$R" "\"export\" in r")" = "False" ] && [ ! -e "$W/out_none" ] && grep -q "No file selected" "$R"'

# 5. Dossier en lecture seule : bloqué au préflight, pas d'export, rien d'écrit.
window ro '{"preset": "arnold", "dir": "'"$W/ro/out"'", "mode": 0, "padding": false}'
R=$W/ro.result.json
check "lecture seule : préflight bloquant, pas d'export" '[ -s "$R" ] && [ "$(json "$R" "r[\"preflight\"][\"ok\"]")" = "False" ] && [ "$(json "$R" "\"export\" in r")" = "False" ] && grep -q "Blocked" "$R" && [ ! -e "$W/ro/out" ]'

# 6. Deux ouvertures de suite : la seconde ramène la fenêtre, un seul processus.
window twice '{"preset": "basic", "dir": "'"$W/out_twice"'", "mode": 0, "padding": true}' \
	'studio_export_window_show(); studio_export_window_show(); console_log(string("t: open=%d", studio_export_window_is_open()));'
R=$W/twice.result.json
check "seconde ouverture : même fenêtre, export basic avec marge" '[ -s "$R" ] && [ "$(json "$R" "r[\"export\"][\"ok\"]")" = "True" ] && [ $(ls "$W/out_twice"/p_*.exr 2>/dev/null | wc -l) -eq 9 ] && [ $(grep -c "studio: export window: .*studio_export_window.py" "$W/twice.log") -eq 1 ]'

chmod 755 "$W/ro"
[ $FAIL -eq 0 ] && echo "== OK" || echo "== FAIL"
exit $FAIL
