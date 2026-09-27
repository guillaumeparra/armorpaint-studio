#!/bin/sh
# Jeux de textures (atlas), nom des fichiers et UDIM dans ArmorPaint, avec les maillages de .tests/udim_gen.py :
#   props.obj (crate, barrel, ball) : jeux changés par la fenêtre d'export (op atlases), export Visible un jeu de maps
#     par jeu, nommé par <atlas> ; calque limité à un jeu (refus d'un jeu vidé), jeux enregistrés avec le .arm ;
#   udim_cube.obj : par tuile UDIM (6 tuiles, <name>, <version>, <UDIM>), par matériau (body, cap : <object> ; Per
#     Object ne sépare que les objets qui ont un calque à eux, ici cap) ; par matériau et par tuile (lot D1) : un jeu
#     par matériau, fenêtre en mode Per UDIM Tile (section des jeux, note, résumé) et export par jeu et par tuile ;
#   un modèle de nom à variable inconnue : refusé, rien d'écrit.
#
#   sh .tests/sets_app.sh [dossier de travail]
#   APP=<binaire ArmorPaint>, PY=<python avec PySide6> (défaut : .shotgrid/venv).
set -eu
EXT=$(cd "$(dirname "$0")/.." && pwd)
APP=${APP:-$EXT/../../build/build/Release/ArmorPaint.app/Contents/MacOS/ArmorPaint}
PY=${PY:-$EXT/.shotgrid/venv/bin/python}
W=${1:-$(mktemp -d)}
mkdir -p "$W"
W=$(cd "$W" && pwd)
[ -x "$APP" ] || { echo "ArmorPaint not found: $APP" >&2; exit 1; }
export STUDIO_ENV= # lot E5 : aucun studio.env, pas même celui de l'utilisateur
"$PY" -c "import PySide6" 2>/dev/null || { echo "PySide6 not found for $PY (tools/setup_shotgrid.sh)" >&2; exit 1; }
FAIL=0
ok() { echo "ok   $1"; }
ko() { echo "FAIL $1"; FAIL=1; }
check() { if eval "$2"; then ok "$1"; else ko "$1"; fi; }
json() { "$PY" -c "import json,sys; r=json.load(open(sys.argv[1])); print(eval(sys.argv[2]))" "$@"; }

rm -rf "$W/out_"* "$W"/*.result.json "$W"/*.arm "$W"/*.export_settings.json
"$PY" "$EXT/.tests/udim_gen.py" "$W/meshes" > /dev/null
M=$W/meshes

# Lance un script dans ArmorPaint sans interface ; ses lignes « t: » dans $W/$1.out.
script() {
	cat > "$W/$1.c"
	perl -e 'alarm 120; exec @ARGV' "$APP" ${2:-} --background --script "$W/$1.c" -ApplePersistenceIgnoreState YES > "$W/$1.log" 2>&1 || true
	grep '^t: ' "$W/$1.log" | sed 's/^t: //' > "$W/$1.out" || true
}
line() { sed -n "s/^$2 //p" "$W/$1.out"; }

MAT='slot_material_t *m = script_material_create("mat");
	ui_node_t *rgb = script_material_create_node_at("RGB", -400.0, 0.0);
	ui_node_t *out = script_material_get_node("OUTPUT_MATERIAL_PBR");
	script_material_set_color(rgb, 0, 0, 0.8, 0.35, 0.1, 1.0);
	script_material_connect(rgb, 0, out, 0);
	script_material_update();
	script_material_set(m);
	script_fill_layer();'

# 1. props : trois objets, triés par nom à l'import ; projet enregistré.
script props <<EOF
int main() {
	studio_import_mesh("$M/props.obj", 0);
	$MAT
	console_log(string("t: atlases %s", studio_atlases()));
	project_filepath_set("$W/props.arm");
	project_save(false);
	return 0;
}
EOF
check "props : 3 objets triés par nom, un seul jeu" '[ "$(line props atlases)" = "{\"names\":[\"Atlas 1\"],\"objects\":[{\"name\":\"ball\",\"atlas\":0},{\"name\":\"barrel\",\"atlas\":0},{\"name\":\"crate\",\"atlas\":0}],\"layers\":[]}" ] && [ -f "$W/props.arm" ]'

# 2. Fenêtre : jeux props_a (ball, barrel) et props_b (crate), nom <name>_<atlas>_<channel>.<ext>, export Visible.
window() {
	name=$1
	printf '%s\n' "$2" | sed "s|}\$|, \"result\": \"$W/$name.result.json\"}|" > "$W/$name.test.json"
	printf 'int main() {\n\tstudio_export_window_show();\n\treturn 0;\n}\n' > "$W/$name.c"
	(STUDIO_PYTHON="$PY" STUDIO_EXPORT_WINDOW_TEST="$W/$name.test.json" QT_QPA_PLATFORM=offscreen \
		"$APP" "$3" --script "$W/$name.c" -ApplePersistenceIgnoreState YES > "$W/$name.log" 2>&1 & echo $! > "$W/app.pid")
	i=0
	while [ $i -lt 120 ] && [ ! -s "$W/$name.result.json" ]; do
		sleep 1
		i=$((i + 1))
	done
	sleep 1
	kill "$(cat "$W/app.pid")" 2>/dev/null || true
	wait 2>/dev/null || true
}
window sets '{"preset": "basic", "mode": 0, "padding": false, "dir": "'"$W/out_sets"'", "atlases": {"names": ["props_a", "props_b"], "objects": [0, 0, 1]}, "naming": "<name>_<atlas>_<channel>.<ext>"}' "$W/props.arm"
R=$W/sets.result.json
check "fenêtre : jeux changés par ArmorPaint, une ligne par objet" '[ -s "$R" ] && [ "$(json "$R" "r[\"atlases_reply\"][\"ok\"], r[\"sets\"][\"rows\"]")" = "(True, [['"'"'ball'"'"', '"'"'props_a'"'"'], ['"'"'barrel'"'"', '"'"'props_a'"'"'], ['"'"'crate'"'"', '"'"'props_b'"'"']])" ]'
check "export Visible : un jeu de 9 maps par jeu de textures, nommés par <atlas>" '[ "$(json "$R" "r[\"export\"][\"ok\"]")" = True ] && [ $(ls "$W/out_sets"/props_props_a_*.exr 2>/dev/null | wc -l) -eq 9 ] && [ $(ls "$W/out_sets"/props_props_b_*.exr 2>/dev/null | wc -l) -eq 9 ] && [ $(ls "$W/out_sets" | wc -l) -eq 19 ]'
check "manifeste : modèle de nom de l'export, variante = jeu" '"$PY" - "$W/out_sets/props.export.json" <<PY
import json, sys
m = json.load(open(sys.argv[1]))
assert m["preset"]["naming"] == "<name>_<atlas>_<channel>.<ext>", m["preset"]
assert sorted({f["variant"] for f in m["files"]}) == ["props_a", "props_b"], {f["variant"] for f in m["files"]}
PY'
check "fenêtre : exemple de nom sous le champ" 'grep -q "e.g. props_props_a_" "$R"'

# 3. Calque limité à un jeu : le jeu ne peut pas perdre tous ses objets ; renommage et nouvel ordre gardent le calque ;
#    enregistré avec le .arm.
script limit <<EOF
int main() {
	studio_import_mesh("$M/props.obj", 0);
	$MAT
	studio_set_atlases("{\"names\": [\"props_a\", \"props_b\"], \"objects\": [0, 0, 1]}");
	console_log(string("t: limit %d", studio_layer_set_atlas(1)));
	console_log(string("t: refuse %s", studio_set_atlases("{\"names\": [\"props_a\", \"props_b\"], \"objects\": [0, 0, 0]}")));
	console_log(string("t: bad [%s]", studio_set_atlases("{\"names\": [\"a\", \"a\"], \"objects\": [0, 0, 1]}")));
	studio_set_atlases("{\"names\": [\"x\", \"crates\", \"y\"], \"objects\": [2, 2, 1]}");
	console_log(string("t: renamed %s", studio_atlases()));
	project_filepath_set("$W/limit.arm");
	project_save(false);
	return 0;
}
EOF
script reload "$W/limit.arm" <<EOF
int main() {
	console_log(string("t: reload %s", studio_atlases()));
	return 0;
}
EOF
check "calque limité au jeu 1" '[ "$(line limit limit)" = 1 ]'
check "jeu vidé alors qu'un calque y est limité : refusé" 'line limit refuse | grep -q "limited to the set .props_b., which would have no object left"'
check "noms en double : refusés" 'line limit bad | grep -q "used twice"'
check "renommé, autre ordre : le calque reste sur son jeu" '[ "$(line limit renamed)" = "{\"names\":[\"x\",\"crates\",\"y\"],\"objects\":[{\"name\":\"ball\",\"atlas\":2},{\"name\":\"barrel\",\"atlas\":2},{\"name\":\"crate\",\"atlas\":1}],\"layers\":[{\"layer\":\"Layer 1\",\"atlas\":1}]}" ]'
check "jeux et calque limité relus du .arm" '[ "$(line reload reload)" = "$(line limit renamed)" ]'

# 4. Cube UDIM : par tuile avec <name>, <version> et <UDIM> ; par matériau, <object> = nom du matériau. Le projet est
#    rouvert pour l'export : l'enregistrement se fait à l'image suivante, et <name> vient du fichier ouvert.
script cube <<EOF
int main() {
	studio_import_mesh("$M/udim_cube.obj", 2);
	$MAT
	console_log(string("t: tiles %s", studio_atlases()));
	project_filepath_set("$W/cube_v002.arm");
	project_save(false);
	return 0;
}
EOF
script udim "$W/cube_v002.arm" <<EOF
int main() {
	studio_export_set_naming("<name>_v<version>_<channel>.<UDIM>.<ext>");
	console_log(string("t: udim %d", studio_export_run("$W/out_udim", "basic", 3)));
	studio_import_mesh("$M/udim_cube.obj", 1);
	$MAT
	console_log(string("t: materials %s", studio_atlases()));
	studio_layer_set_object(1);
	studio_export_set_naming("<name>_<object>_<channel>[.<UDIM>].<ext>");
	console_log(string("t: object %d", studio_export_run("$W/out_object", "basic", 2)));
	studio_export_set_naming("<name>_<shader>.<ext>");
	console_log(string("t: bad %d", studio_export_run("$W/out_bad", "basic", 0)));
	return 0;
}
EOF
check "cube par tuile : 6 objets cube.1001 ... cube.1013" '[ "$(line cube tiles | "$PY" -c "import json,sys; print(\" \".join(o[\"name\"] for o in json.load(sys.stdin)[\"objects\"]))")" = "cube.1001 cube.1002 cube.1003 cube.1011 cube.1012 cube.1013" ]'
check "export par tuile : 9 maps × 6 tuiles, <name> et <version> tirés de cube_v002" '[ "$(line udim udim)" = 1 ] && [ $(ls "$W/out_udim"/cube_v002_*.exr 2>/dev/null | wc -l) -eq 54 ] && for t in 1001 1002 1003 1011 1012 1013; do [ -f "$W/out_udim/cube_v002_base_color.$t.exr" ] || exit 1; done'
check "cube par matériau : body et cap" '[ "$(line udim materials | "$PY" -c "import json,sys; print(\" \".join(o[\"name\"] for o in json.load(sys.stdin)[\"objects\"]))")" = "body cap" ]'
check "export par objet : cap (calque à lui) seul, <object> = matériau, section UDIM retirée" '[ "$(line udim object)" = 1 ] && [ $(ls "$W/out_object"/cube_cap_*.exr 2>/dev/null | wc -l) -eq 9 ] && [ $(ls "$W/out_object"/*.exr | wc -l) -eq 9 ] && [ -f "$W/out_object/cube_cap_base_color.exr" ]'
check "variable inconnue <shader> : refusé, rien d'écrit" '[ "$(line udim bad)" = 0 ] && [ ! -e "$W/out_bad" ] && grep -q "unknown variable <shader>" "$W/udim.log"'

# 5. Fenêtre sur le cube UDIM : un seul objet (6 tuiles), pas de section Jeux de textures.
window cube '{"no_export": true, "dir": "'"$W/out_cube"'"}' "$W/cube_v002.arm"
R=$W/cube.result.json
check "fenêtre, cube UDIM : section des jeux masquée, 6 tuiles au projet" '[ -s "$R" ] && [ "$(json "$R" "r[\"sets\"][\"shown\"]")" = False ] && grep -q "6 UDIM tiles" "$R"'

# 6. Cube UDIM par matériau et par tuile (lot D1) : jeux body (1001, 1002, 1003, 1011) et cap (1012, 1013) ; fenêtre
#    en mode Per UDIM Tile : les deux jeux, la note du mode, le résumé jeux × tuiles ; export d'un jeu de maps par jeu,
#    pour ses tuiles seulement.
script cubemats <<EOF
int main() {
	studio_import_mesh("$M/udim_cube.obj", 3);
	$MAT
	console_log(string("t: sets %s", studio_atlases()));
	project_filepath_set("$W/cubemats.arm");
	project_save(false);
	return 0;
}
EOF
window cubemats '{"preset": "basic", "mode": 3, "padding": false, "dir": "'"$W/out_cubemats"'"}' "$W/cubemats.arm"
R=$W/cubemats.result.json
check "cube par matériau et par tuile : body.1001 ... cap.1013, un jeu par matériau" '[ "$(line cubemats sets | "$PY" -c "import json,sys; j=json.load(sys.stdin); print(\" \".join(\"%s:%s\" % (o[\"name\"], j[\"names\"][o[\"atlas\"]]) for o in j[\"objects\"]))")" = "body.1001:body body.1002:body body.1003:body body.1011:body cap.1012:cap cap.1013:cap" ]'
check "fenêtre par tuile : jeux body (4 tuiles) et cap (2), note du mode, 9 maps × 6 tuiles de jeux" '[ -s "$R" ] && [ "$(json "$R" "r[\"sets\"][\"shown\"], r[\"sets\"][\"rows\"]")" = "(True, [['"'"'body  (4 UDIM tiles)'"'"', '"'"'body'"'"'], ['"'"'cap  (2 UDIM tiles)'"'"', '"'"'cap'"'"']])" ] && json "$R" "r[\"sets\"][\"note\"]" | grep -q "^Per UDIM Tile: one set of maps per texture set and tile" && [ "$(json "$R" "r[\"composition\"]")" = "9 maps × 6 texture set tiles (2 sets, 6 UDIM tiles)" ]'
check "export par tuile : 9 maps de body sur ses 4 tuiles, de cap sur 1012 et 1013 seulement" '[ "$(json "$R" "r[\"export\"][\"ok\"]")" = True ] && [ $(ls "$W/out_cubemats" | grep -c "\.exr$") -eq 54 ] && [ $(ls "$W/out_cubemats" | grep -cE "_body_.*\.(1001|1002|1003|1011)\.exr$") -eq 36 ] && [ $(ls "$W/out_cubemats" | grep -cE "_cap_.*\.(1012|1013)\.exr$") -eq 18 ]'

[ $FAIL -eq 0 ] && echo "== OK" || echo "== FAIL"
exit $FAIL
