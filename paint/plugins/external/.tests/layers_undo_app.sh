#!/bin/sh
# Undo et Redo des fusions de calques dans ArmorPaint lui-même (--background), patch merge-group-undo. Projet par défaut
# (cube, sans UDIM), 256 px ; chaque état relevé : la pile (« nom<parent »), les pixels base / nor / pack de chaque
# calque et masque, l'aplati (base, nor, pack).
#   1. le cas signalé : « Layer 1 » ; « Layer 2 » et « Layer 3 » dans « Group 4 » à masque de groupe. Merge Group donne
#      « Layer 1 | Group 4 » ; Undo rend l'état d'avant, Redo celui d'après, un 2e Undo celui d'avant ; l'Undo suivant
#      (le masque de groupe) reste juste ;
#   2. masques de calque dans le groupe (un par enfant) et masque de groupe : de même ;
#   3. emplacements d'undo (N = undo_steps) : la fusion d'un groupe de k calques et masques en tient k. Après N - k
#      Clear, les Undo rendent l'état d'avant la fusion ; après N - k + 1 Clear, la fusion n'est plus annulable : l'Undo
#      de trop ne fait rien et ne touche pas la pile ;
#   4. groupe plus grand que les emplacements (N calques et un masque) : fusion faite, historique vidé, Undo sans effet ;
#   5. Merge Down (non-régression, pas du patch) : Undo rend les pixels et le nombre de calques, Redo la fusion.
#
#   sh .tests/layers_undo_app.sh [dossier de travail]
#   APP=<binaire ArmorPaint> pour un autre build (défaut : build Release de paint/).
set -eu
T=$(cd "$(dirname "$0")" && pwd)
EXT=$(dirname "$T")
APP=${APP:-$EXT/../../build/build/Release/ArmorPaint.app/Contents/MacOS/ArmorPaint}
W=${1:-$(mktemp -d)}
mkdir -p "$W"
W=$(cd "$W" && pwd)
[ -x "$APP" ] || { echo "ArmorPaint not found: $APP" >&2; exit 1; }
export STUDIO_ENV= # lot E5 : aucun studio.env, pas même celui de l'utilisateur
FAIL=0
ok() { echo "ok   $1"; }
ko() { echo "FAIL $1"; FAIL=1; }
check() { if eval "$2"; then ok "$1"; else ko "$1"; fi; }

rm -rf "$W"/*.log "$W"/*.out
cd "$W"

# script <nom> : lance le script C lu sur l'entrée, garde ses lignes « t: » dans <nom>.out
script() {
	{
		echo "$COMMON"
		cat
	} > "$W/$1.c"
	rc=0
	# -ApplePersistenceIgnoreState : sans lui, un plantage précédent laisse une fenêtre modale qui bloque le lancement
	perl -e 'alarm 120; exec @ARGV' "$APP" --background --script "$W/$1.c" -ApplePersistenceIgnoreState YES > "$W/$1.log" 2>&1 || rc=$?
	[ $rc -eq 0 ] || ko "script $1 : ArmorPaint rc $rc (voir $W/$1.log)"
	grep '^t: ' "$W/$1.log" | sed 's/^t: //' > "$W/$1.out" || true
}
line() { sed -n "s/^$2 //p" "$W/$1.out"; }
# same <script> <état> <état> : pile, pixels et aplati identiques
same() { [ -n "$(line "$1" "$2.tree")" ] && [ "$(line "$1" "$2.tree")" = "$(line "$1" "$3.tree")" ] && [ "$(line "$1" "$2.px")" = "$(line "$1" "$3.px")" ] && [ "$(line "$1" "$2.flat")" = "$(line "$1" "$3.flat")" ]; }

# Fonctions communes des scripts. state : l'état relevé ; group3 : le cas signalé, k = 3 (2 calques, 1 masque)
COMMON='
void state(char *what) {
	console_log(string("t: %s.tree %s", what, studio_layer_tree()));
	char *px = "";
	for (int i = 0; i < 12; i++) {
		px = string("%s%s/%s/%s; ", px, studio_udim_pixel(i, 0, -1, 10, 10), studio_udim_pixel(i, 1, -1, 10, 10), studio_udim_pixel(i, 2, -1, 10, 10));
	}
	console_log(string("t: %s.px %s", what, px));
	console_log(string("t: %s.flat %s/%s/%s", what, studio_udim_pixel(0, 9, -1, 10, 10), studio_udim_pixel(0, 10, -1, 10, 10), studio_udim_pixel(0, 11, -1, 10, 10)));
}
void colors(int i, float r, float g, float b, float a) {
	studio_udim_fill(i, 0, -1, r, g, b, a);
	studio_udim_fill(i, 1, -1, 0.5 + r / 4.0, 0.5 - g / 4.0, 1.0, 1.0);
	studio_udim_fill(i, 2, -1, 1.0, 0.2 + b / 2.0, r, 1.0);
}
void group3() {
	studio_project_res(256);
	colors(0, 0.8, 0.1, 0.1, 1.0);
	studio_layer_new();
	colors(1, 0.0, 1.0, 0.0, 1.0);
	studio_layer_new();
	colors(2, 0.0, 0.0, 1.0, 0.5);
	studio_layer_new_group();
	studio_layer_set_parent(1, 3);
	studio_layer_set_parent(2, 3);
	studio_layer_new_mask(1);
	studio_udim_fill(3, 0, -1, 0.5, 0.5, 0.5, 1.0);
}
'

# 1. Le cas signalé
script group <<'EOF'
int main() {
	group3();
	state("before");
	studio_layer_select(4);
	console_log(string("t: merge %d", studio_layer_op("merge_group")));
	state("merged");
	studio_layer_op("undo");
	state("undo");
	studio_layer_op("redo");
	state("redo");
	studio_layer_op("undo");
	state("undo2");
	studio_layer_op("undo");
	state("undo3");
	return 0;
}
EOF
check "groupe : pile de départ" '[ "$(line group before.tree)" = "Layer 1 | Layer 2<Group 4 | Layer 3<Group 4 | Mask 5<Group 4 | Group 4" ]'
check "Merge Group : un calque nommé comme le groupe" '[ "$(line group merge)" = 1 ] && [ "$(line group merged.tree)" = "Layer 1 | Group 4" ]'
check "Undo : pile, noms, pixels (base, nor, pack, masque) et aplati d'avant la fusion" 'same group undo before'
check "Redo : la fusion à l'identique" 'same group redo merged'
check "2e Undo : l'état d'avant la fusion" 'same group undo2 before'
check "Undo suivant (nouveau masque) : le masque de groupe retiré, le reste en place" '[ "$(line group undo3.tree)" = "Layer 1 | Layer 2<Group 4 | Layer 3<Group 4 | Group 4" ]'

# 2. Masques de calque dans le groupe
script masks <<'EOF'
int main() {
	studio_project_res(256);
	colors(0, 0.8, 0.1, 0.1, 1.0);
	studio_layer_new();
	colors(1, 0.0, 1.0, 0.0, 1.0);
	studio_layer_new();
	colors(2, 0.0, 0.0, 1.0, 1.0);
	studio_layer_new_group();
	studio_layer_set_parent(1, 3);
	studio_layer_set_parent(2, 3);
	studio_layer_select(1);
	studio_layer_new_mask(1);
	studio_udim_fill(1, 0, -1, 0.5, 0.5, 0.5, 1.0);
	studio_layer_select(3);
	studio_layer_new_mask(1);
	studio_udim_fill(3, 0, -1, 0.25, 0.25, 0.25, 1.0);
	studio_layer_select(5);
	studio_layer_new_mask(1);
	studio_udim_fill(5, 0, -1, 0.75, 0.75, 0.75, 1.0);
	state("before");
	studio_layer_select(6);
	console_log(string("t: merge %d", studio_layer_op("merge_group")));
	state("merged");
	studio_layer_op("undo");
	state("undo");
	studio_layer_op("redo");
	state("redo");
	studio_layer_op("undo");
	state("undo2");
	return 0;
}
EOF
check "masques : pile de départ" '[ "$(line masks before.tree)" = "Layer 1 | Mask 5<Layer 2 | Layer 2<Group 4 | Mask 6<Layer 3 | Layer 3<Group 4 | Mask 7<Group 4 | Group 4" ]'
check "masques : Merge Group" '[ "$(line masks merge)" = 1 ] && [ "$(line masks merged.tree)" = "Layer 1 | Group 4" ]'
check "masques : Undo rend les calques et leurs masques" 'same masks undo before'
check "masques : Redo" 'same masks redo merged'
check "masques : 2e Undo" 'same masks undo2 before'

# 3. Emplacements d'undo : N - k Clear après la fusion, puis N - k + 1
for extra in 0 1; do
	script budget$extra <<EOF
int main() {
	group3();
	config_t *c = script_get_config();
	int n = c->undo_steps;
	int clears = n - 3 + $extra;
	console_log(string("t: n %d", n));
	state("before");
	studio_layer_select(4);
	studio_layer_op("merge_group");
	state("merged");
	studio_layer_select(0);
	for (int i = 0; i < clears; i++) {
		studio_layer_op("clear");
	}
	state("cleared");
	for (int i = 0; i < clears; i++) {
		studio_layer_op("undo");
	}
	state("unclear");
	studio_layer_op("undo");
	state("undo");
	return 0;
}
EOF
done
N=$(line budget0 n)
check "emplacements : N = $N, au moins 3" '[ "${N:-0}" -ge 3 ]'
check "N - 3 Clear : Undo de chacun, calque rendu" '! same budget0 cleared merged && same budget0 unclear merged'
check "N - 3 Clear : la fusion s'annule encore, état d'avant" 'same budget0 undo before'
check "N - 2 Clear : Undo de chacun, calque rendu" '! same budget1 cleared merged && same budget1 unclear merged'
check "N - 2 Clear : la fusion n'est plus annulable, l'Undo de trop ne touche rien" 'same budget1 undo merged'

# 4. Groupe plus grand que les emplacements : N calques et un masque de groupe
script big <<'EOF'
int main() {
	studio_project_res(256);
	config_t *c = script_get_config();
	int n = c->undo_steps;
	int made = 0;
	colors(0, 0.8, 0.1, 0.1, 1.0);
	for (int i = 1; i <= n; i++) {
		made = made + studio_layer_new();
		colors(i, 0.1 * i, 1.0 - 0.1 * i, 0.5, 1.0);
	}
	studio_layer_new_group();
	for (int i = 1; i <= n; i++) {
		studio_layer_set_parent(i, n + 1);
	}
	studio_layer_new_mask(1);
	studio_udim_fill(n + 1, 0, -1, 0.5, 0.5, 0.5, 1.0);
	console_log(string("t: made %d %d", made, n));
	studio_layer_select(n + 2);
	console_log(string("t: merge %d", studio_layer_op("merge_group")));
	state("merged");
	studio_layer_op("undo");
	state("undo");
	return 0;
}
EOF
check "grand groupe : N calques créés" '[ "$(line big made | cut -d" " -f1)" = "$(line big made | cut -d" " -f2)" ]'
check "grand groupe : fusion faite, historique vidé (message)" '[ "$(line big merge)" = 1 ] && [ "$(line big merged.tree | sed "s/Group [0-9]*/G/")" = "Layer 1 | G" ] && grep -q "more layers and masks than undo steps, history cleared" "$W/big.log"'
check "grand groupe : Undo sans effet" 'same big undo merged'

# 5. Merge Down (non-régression)
script down <<'EOF'
int main() {
	studio_project_res(256);
	colors(0, 0.8, 0.1, 0.1, 1.0);
	studio_layer_new();
	colors(1, 0.0, 1.0, 0.0, 0.5);
	state("before");
	studio_layer_select(1);
	console_log(string("t: merge %d", studio_layer_op("merge_down")));
	state("merged");
	studio_layer_op("undo");
	state("undo");
	studio_layer_op("redo");
	state("redo");
	return 0;
}
EOF
count() { line down "$1.tree" | awk -F' [|] ' '{ print NF }'; }
check "Merge Down : un calque" '[ "$(line down merge)" = 1 ] && [ "$(count merged)" = 1 ]'
check "Merge Down : Undo rend les deux calques et leurs pixels" '[ "$(count undo)" = 2 ] && [ "$(line down undo.px)" = "$(line down before.px)" ] && [ "$(line down undo.flat)" = "$(line down before.flat)" ]'
check "Merge Down : Redo" 'same down redo merged'

[ $FAIL -eq 0 ] && echo "layers_undo_app : tout est bon" || echo "layers_undo_app : échecs, voir $W"
exit $FAIL
