#!/bin/sh
# Jeux de textures (atlas) quand l'onglet Meshes réordonne les objets (patch atlas-objects-order) : tri par nom,
# changement de parent, suppression. atlas_objects suit les objets, les masques de calque qui désignent un objet ou un
# jeu restent dessus ; un calque dont l'objet ou le jeu disparaît devient partagé (0).
#
#   sh .tests/atlas_order_app.sh [dossier de travail]
#   APP=<binaire ArmorPaint>.
set -eu
EXT=$(cd "$(dirname "$0")/.." && pwd)
APP=${APP:-$EXT/../../build/build/Release/ArmorPaint.app/Contents/MacOS/ArmorPaint}
W=${1:-$(mktemp -d)}
mkdir -p "$W"
W=$(cd "$W" && pwd)
[ -x "$APP" ] || { echo "ArmorPaint not found: $APP" >&2; exit 1; }
FAIL=0
check() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1"; echo "     attendu : $3"; echo "     obtenu  : $2"; FAIL=1; fi; }
rm -f "$W"/order.* "$W"/reload.*

# Lance un script dans ArmorPaint sans interface ; ses lignes « t: » dans $W/$1.out.
script() {
	cat > "$W/$1.c"
	perl -e 'alarm 120; exec @ARGV' "$APP" ${2:-} --background --script "$W/$1.c" -ApplePersistenceIgnoreState YES > "$W/$1.log" 2>&1 || true
	grep '^t: ' "$W/$1.log" | sed 's/^t: //' > "$W/$1.out" || true
}
line() { sed -n "s/^$2 //p" "$W/$1.out"; }

# props : ball, barrel, crate (triés par nom à l'import, barrel et crate enfants de ball), un jeu chacun (a, b, c).
# Layer 1 limité au jeu a, Layer 2 à l'objet barrel, Layer 3 au jeu c. Le .arm s'enregistre à l'image suivante : la
# dernière suppression se fait dans le second lancement, sur le projet relu.
script order <<EOF
void state(char *step) {
	console_log(string("t: %s %s", step, studio_atlases()));
	console_log(string("t: %s_masks %s", step, studio_layer_masks()));
}
int main() {
	studio_import_mesh("$EXT/.tests/meshes/props.obj", 0);
	studio_set_atlases("{\"names\": [\"a\", \"b\", \"c\"], \"objects\": [0, 1, 2]}");
	studio_layer_set_atlas(0);
	studio_layer_new();
	studio_layer_set_object(1);
	studio_layer_new();
	studio_layer_set_atlas(2);
	studio_mesh_set_parent("crate", "");
	studio_mesh_set_parent("barrel", "");
	state("start");
	script_object_set_name(script_get_object("ball"), "zball");
	studio_meshes_sort();
	state("sort");
	studio_mesh_set_parent("zball", "barrel");
	state("parent");
	script_object_remove(script_get_object("barrel"));
	state("delete");
	project_filepath_set("$W/order.arm");
	project_save(false);
	return 0;
}
EOF
script reload "$W/order.arm" <<EOF
void state(char *step) {
	console_log(string("t: %s %s", step, studio_atlases()));
	console_log(string("t: %s_masks %s", step, studio_layer_masks()));
}
int main() {
	state("reload");
	script_object_remove(script_get_object("crate"));
	state("single");
	return 0;
}
EOF

L='"layers":[{"layer":"Layer 1","atlas":0},{"layer":"Layer 3","atlas":2}]'
check "départ : un jeu par objet, calques sur a, barrel, c" "$(line order start)" \
	'{"names":["a","b","c"],"objects":[{"name":"ball","atlas":0},{"name":"barrel","atlas":1},{"name":"crate","atlas":2}],'"$L"'}'
check "départ : masques (jeux après les 3 objets)" "$(line order start_masks)" "Layer 1=4 Layer 2=2 Layer 3=6 filter=0"
check "tri par nom : chaque objet garde son jeu" "$(line order sort)" \
	'{"names":["a","b","c"],"objects":[{"name":"barrel","atlas":1},{"name":"crate","atlas":2},{"name":"zball","atlas":0}],'"$L"'}'
check "tri par nom : masques suivent objet et ordre des jeux (b, c, a)" "$(line order sort_masks)" "Layer 1=6 Layer 2=1 Layer 3=5 filter=0"
check "parent : zball sous barrel, jeux gardés" "$(line order parent)" \
	'{"names":["a","b","c"],"objects":[{"name":"barrel","atlas":1},{"name":"zball","atlas":0},{"name":"crate","atlas":2}],'"$L"'}'
check "parent : masques (b, a, c)" "$(line order parent_masks)" "Layer 1=5 Layer 2=1 Layer 3=6 filter=0"
check "suppression de barrel : son jeu retiré, pas de retour à un seul jeu" "$(line order delete)" \
	'{"names":["a","b","c"],"objects":[{"name":"zball","atlas":0},{"name":"crate","atlas":2}],'"$L"'}'
check "suppression : calque de barrel partagé, jeux (a, c)" "$(line order delete_masks)" "Layer 1=3 Layer 2=0 Layer 3=4 filter=0"
check "relu du .arm" "$(line reload reload)" "$(line order delete)"
check "relu du .arm : masques" "$(line reload reload_masks)" "$(line order delete_masks)"
check "un seul objet restant : un seul jeu utilisé, calques partagés" "$(line reload single)" \
	'{"names":["a","b","c"],"objects":[{"name":"zball","atlas":0}],"layers":[]}'
check "un seul objet restant : masques" "$(line reload single_masks)" "Layer 1=0 Layer 2=0 Layer 3=0 filter=0"

[ $FAIL -eq 0 ] && echo "== OK" || echo "== FAIL"
exit $FAIL
