#!/bin/sh
# Jeu de textures par tuile (lot D2, patch udim-tiles) dans ArmorPaint lui-même (--background). Maillage : 5 tuiles
# pleines, 1001 à 1004 et 1011, projet 128 px ; position monde (x, 0, z) = ((u - 2) / 2, 0, (v - 1) / 2).
#   1. passage aux tuiles : chaque texture de calque (base, nor, pack) tenue par tuile, 128 px chacune ; le calque de
#      base, gris, gris sur toutes ; un nouveau calque à tuiles tout de suite ;
#   2. opérations par tuile : couleurs différentes par tuile (1001 rouge, 1002 vert, 1011 bleu) ; Clear vide toutes les
#      tuiles, Undo les rend chacune, Redo les revide ; l'emplacement d'undo ne tient que les tuiles sauvées ;
#      Duplicate copie chaque tuile, la copie est indépendante ; Delete puis Undo rend le calque et ses tuiles, sans
#      fuite (même nombre de contenus) ; changer la résolution du projet redimensionne toutes les tuiles des calques et
#      des emplacements d'undo, couleurs gardées ;
#   3. masque : blanc sur toutes les tuiles ; Invert, Undo, Redo par tuile ; un masque noir sur la 1002 seule ;
#   4. vue 3D : chaque objet dessiné avec sa tuile (gbuffer1 au centre de chaque tuile) : 1001 rouge, 1011 bleu, 1002
#      masquée (le gris du calque de base) ;
#   5. table des tuiles qui change (L17) : ajouter un maillage en 1012 ; les tuiles gardent leur peinture (1011 reste
#      bleue, dans la vue aussi) ; la 1012 reçoit le fond de chaque calque (texel (0, 0) d'une tuile gardée) ; un Clear
#      d'avant l'ajout : Redo revide les 6 tuiles, Undo les rend toutes, la 1012 comme avant le Redo (l'emplacement
#      d'undo, qui avait sauvé tout le calque, reçoit la tuile apparue depuis) ;
#   6. UDIM coupé (maillage sans tuile importé à la place) : plus aucune texture à tuiles.
#
#   sh .tests/udim_tiles_app.sh [dossier de travail]
#   APP=<binaire ArmorPaint> pour un autre build (défaut : build Release de paint/).
set -eu
T=$(cd "$(dirname "$0")" && pwd)
EXT=$(dirname "$T")
APP=${APP:-$EXT/../../build/build/Release/ArmorPaint.app/Contents/MacOS/ArmorPaint}
W=${1:-$(mktemp -d)}
mkdir -p "$W"
W=$(cd "$W" && pwd)
[ -x "$APP" ] || { echo "ArmorPaint not found: $APP" >&2; exit 1; }
FAIL=0
ok() { echo "ok   $1"; }
ko() { echo "FAIL $1"; FAIL=1; }
check() { if eval "$2"; then ok "$1"; else ko "$1"; fi; }

rm -rf "$W"/*.log "$W"/*.out
cd "$W"
python3 - <<'EOF'
def quads(name, cells):
    out, nv = ["o %s" % name], 0
    for u, v in cells:
        for t in (((u, v), (u + 1, v), (u, v + 1)), ((u + 1, v), (u + 1, v + 1), (u, v + 1))):
            for q in t:
                out += ["v %.6g %.6g 0" % (0.5 * (q[0] - 2), 0.5 * (q[1] - 1)), "vt %.6g %.6g" % q]
            out.append("f " + " ".join("%d/%d" % (nv + k + 1, nv + k + 1) for k in range(3)))
            nv += 3
    return "\n".join(out) + "\n"


open("strip.obj", "w").write(quads("strip", ((0, 0), (1, 0), (2, 0), (3, 0), (0, 1))))
open("extra.obj", "w").write(quads("extra", ((1, 1),)))
EOF

# Lance un script dans ArmorPaint sans interface ; ses lignes « t: » dans $W/$1.out.
script() {
	cat > "$W/$1.c"
	rc=0
	perl -e 'alarm 180; exec @ARGV' "$APP" --background --script "$W/$1.c" -ApplePersistenceIgnoreState YES > "$W/$1.log" 2>&1 || rc=$?
	[ $rc -eq 0 ] || ko "script $1 : ArmorPaint rc $rc (voir $W/$1.log)"
	grep '^t: ' "$W/$1.log" | sed 's/^t: //' > "$W/$1.out" || true
}
line() { sed -n "s/^$2 //p" "$W/$1.out"; }
# Couleur dominante d'un pixel « r g b a » de la vue : red, green, blue ou grey
hue() {
	line "$1" "$2" | awk '{ if ($1 > 0.3 && $2 < 0.15 && $3 < 0.15) print "red"; else if ($2 > 0.3 && $1 < 0.15 && $3 < 0.15) print "green";
		else if ($3 > 0.3 && $1 < 0.15 && $2 < 0.15) print "blue"; else if ($1 > 0.1 && $1 < 0.4 && $2 > 0.1 && $2 < 0.4 && $3 > 0.1 && $3 < 0.4) print "grey";
		else print "?" }'
}

# Fonctions communes des scripts : couleurs d'un calque par tuile
COMMON='
void colors(char *what, int l) {
	console_log(string("t: %s %s | %s | %s | %s | %s", what, studio_udim_pixel(l, 0, 1001, 10, 10), studio_udim_pixel(l, 0, 1002, 10, 10),
		studio_udim_pixel(l, 0, 1003, 10, 10), studio_udim_pixel(l, 0, 1011, 10, 10), studio_udim_pixel(l, 0, 1012, 10, 10)));
}
void paint3(int l) {
	studio_udim_fill(l, 0, 1001, 1.0, 0.0, 0.0, 1.0);
	studio_udim_fill(l, 0, 1002, 0.0, 1.0, 0.0, 1.0);
	studio_udim_fill(l, 0, 1011, 0.0, 0.0, 1.0, 1.0);
}
void vp(char *what, float x, float z) {
	console_log(string("t: vp%s %s", what, studio_rt_pixel("gbuffer1", x, 0.0, z)));
}
'
R="1.000 0.000 0.000 1.000"
G="0.000 1.000 0.000 1.000"
B="0.000 0.000 1.000 1.000"
Z="0.000 0.000 0.000 0.000"
K="0.000 0.000 0.000 1.000"
H="1.000 1.000 1.000 1.000"
Y="0.498 0.498 0.498 1.000"
TILES="tiles=1001,1002,1003,1004,1011"
SLOT="base=[cur=1001 partial=1 $TILES 128x128] nor=[cur=1001 partial=1 $TILES 128x128] pack=[cur=1001 partial=1 $TILES 128x128]"

echo "== 1-2. passage aux tuiles, opérations de calque"
script ops <<EOF
$COMMON
int main() {
	studio_project_res(128);
	studio_import_mesh("$W/strip.obj", 2);
	studio_layer_op("sync");
	console_log(string("t: stats %s", studio_udim_stats()));
	console_log(string("t: l0 %s", studio_udim_layer(0)));
	colors("base", 0);
	console_log(string("t: new %d", studio_layer_new()));
	console_log(string("t: l1 %s", studio_udim_layer(1)));
	paint3(1);
	colors("painted", 1);
	studio_layer_select(1);
	console_log(string("t: clear %d", studio_layer_op("clear")));
	colors("cleared", 1);
	console_log(string("t: undo %d", studio_layer_op("undo")));
	colors("undone", 1);
	console_log(string("t: slot %s", studio_udim_layer(-1)));
	console_log(string("t: redo %d", studio_layer_op("redo")));
	colors("redone", 1);
	studio_layer_op("undo");
	console_log(string("t: duplicate %d", studio_layer_op("duplicate")));
	console_log(string("t: dupnames %s", studio_layer_masks()));
	colors("dup", 2);
	studio_udim_fill(2, 0, 1001, 1.0, 1.0, 1.0, 1.0);
	colors("dupwhite", 2);
	colors("orig", 1);
	console_log(string("t: before %s", studio_udim_stats()));
	studio_layer_select(1);
	console_log(string("t: delete %d", studio_layer_op("delete")));
	console_log(string("t: delnames %s", studio_layer_masks()));
	console_log(string("t: undel %d", studio_layer_op("undo")));
	console_log(string("t: undelnames %s", studio_layer_masks()));
	console_log(string("t: after %s", studio_udim_stats()));
	colors("restored", 1);
	console_log(string("t: res %d", studio_project_res(64)));
	console_log(string("t: l1res %s", studio_udim_layer(1)));
	console_log(string("t: slotres %s", studio_udim_layer(-1)));
	colors("resized", 1);
	return 0;
}
EOF
S5="$TILES 128x128"
check "table des tuiles, 3 calques de textures à tuiles" '[ "$(line ops stats)" = "tile=1001 table=1001,1002,1003,1004,1011 stores=3 partial=0 contents=15" ]'
check "calque de base : base, nor et pack par tuile, gris sur toutes" '[ "$(line ops l0)" = "base=[cur=1001 partial=0 $S5] nor=[cur=1001 partial=0 $S5] pack=[cur=1001 partial=0 $S5]" ] && [ "$(line ops base)" = "$Y | $Y | $Y | $Y | -" ]'
check "nouveau calque à tuiles tout de suite" '[ "$(line ops new)" = 1 ] && [ "$(line ops l1)" = "base=[cur=1001 partial=0 $S5] nor=[cur=1001 partial=0 $S5] pack=[cur=1001 partial=0 $S5]" ]'
check "une couleur par tuile" '[ "$(line ops painted)" = "$R | $G | $Z | $B | -" ]'
check "Clear vide toutes les tuiles" '[ "$(line ops clear)" = 1 ] && [ "$(line ops cleared)" = "$Z | $Z | $Z | $Z | -" ]'
check "Undo rend chaque tuile, Redo les revide" '[ "$(line ops undone)" = "$R | $G | $Z | $B | -" ] && [ "$(line ops redone)" = "$Z | $Z | $Z | $Z | -" ]'
check "emplacement d'undo : les tuiles sauvées" '[ "$(line ops slot)" = "$SLOT" ]'
check "Duplicate copie chaque tuile, copie indépendante" '[ "$(line ops duplicate)" = 1 ] && [ "$(line ops dup)" = "$R | $G | $Z | $B | -" ] && [ "$(line ops dupwhite)" = "$H | $G | $Z | $B | -" ] && [ "$(line ops orig)" = "$R | $G | $Z | $B | -" ]'
check "Delete puis Undo : calque et tuiles rendus, sans fuite" '[ "$(line ops delnames)" = "Layer 1=0 Layer 3=0 filter=0" ] && [ "$(line ops undel)" = 1 ] && [ "$(line ops restored)" = "$R | $G | $Z | $B | -" ] && [ "$(line ops before)" = "$(line ops after)" ]'
S64="$TILES 64x64"
check "résolution 64 px : toutes les tuiles des calques et des emplacements d'undo, couleurs gardées" '[ "$(line ops res)" = 1 ] && [ "$(line ops l1res)" = "base=[cur=1001 partial=0 $S64] nor=[cur=1001 partial=0 $S64] pack=[cur=1001 partial=0 $S64]" ] && line ops slotres | grep -q "^base=\[cur=1001 partial=1 $S64\]" && [ "$(line ops resized)" = "$R | $G | $Z | $B | -" ]'

echo "== 3-4. masque, vue 3D"
script mask <<EOF
$COMMON
int frame = 0;
void update() {
	frame++;
	if (frame == 4) {
		vp("1001", -0.75, -0.25);
		vp("1002", -0.25, -0.25);
		vp("1003", 0.25, -0.25);
		vp("1011", -0.75, 0.25);
		script_quit();
	}
}
int main() {
	studio_project_res(128);
	studio_import_mesh("$W/strip.obj", 2);
	studio_layer_op("sync");
	studio_layer_new();
	paint3(1);
	console_log(string("t: mask %d", studio_layer_new_mask(1)));
	console_log(string("t: names %s", studio_layer_masks()));
	console_log(string("t: m %s", studio_udim_layer(1)));
	colors("white", 1);
	studio_layer_select(1);
	console_log(string("t: invert %d", studio_layer_op("invert")));
	colors("inverted", 1);
	studio_layer_op("undo");
	colors("undone", 1);
	studio_layer_op("redo");
	colors("redone", 1);
	studio_layer_op("undo");
	studio_udim_fill(1, 0, 1002, 0.0, 0.0, 0.0, 1.0);
	colors("black1002", 1);
	script_notify_on_update(update);
	return 0;
}
EOF
check "masque à tuiles, blanc sur toutes" '[ "$(line mask mask)" = 1 ] && [ "$(line mask names)" = "Layer 1=0 Mask 3=0 Layer 2=0 filter=0" ] && [ "$(line mask m)" = "base=[cur=1001 partial=0 $S5] nor=[null] pack=[null]" ] && [ "$(line mask white)" = "$H | $H | $H | $H | -" ]'
check "Invert, Undo, Redo par tuile" '[ "$(line mask invert)" = 1 ] && [ "$(line mask inverted)" = "$Z | $Z | $Z | $Z | -" ] && [ "$(line mask undone)" = "$H | $H | $H | $H | -" ] && [ "$(line mask redone)" = "$Z | $Z | $Z | $Z | -" ]'
check "masque noir sur la 1002 seule" '[ "$(line mask black1002)" = "$H | $K | $H | $H | -" ]'
check "vue 3D : chaque objet avec sa tuile (1001 rouge, 1011 bleu, 1002 masquée, 1003 vide)" '[ "$(hue mask vp1001) $(hue mask vp1011) $(hue mask vp1002) $(hue mask vp1003)" = "red blue grey grey" ]'

echo "== 5-6. table des tuiles qui change, UDIM coupé"
script table <<EOF
$COMMON
int frame = 0;
void update() {
	frame++;
	if (frame == 4) {
		vp("1001", -0.75, -0.25);
		vp("1011", -0.75, 0.25);
		vp("1012", -0.25, 0.25);
		console_log(string("t: off %d", studio_import_mesh("$W/extra.obj", 0)));
	}
	if (frame == 8) {
		console_log(string("t: offstats %s", studio_udim_stats()));
		console_log(string("t: offl0 %s", studio_udim_layer(0)));
		script_quit();
	}
}
int main() {
	studio_project_res(128);
	studio_import_mesh("$W/strip.obj", 2);
	studio_layer_op("sync");
	studio_layer_new();
	paint3(1);
	studio_layer_select(1);
	studio_layer_op("clear");
	studio_layer_op("undo");
	console_log(string("t: append %d", studio_append_mesh("$W/extra.obj", 2)));
	studio_layer_op("sync");
	console_log(string("t: stats %s", studio_udim_stats()));
	colors("base", 0);
	colors("layer", 1);
	console_log(string("t: slot %s", studio_udim_layer(-1)));
	console_log(string("t: redo %d", studio_layer_op("redo")));
	colors("redone", 1);
	console_log(string("t: undo %d", studio_layer_op("undo")));
	colors("undone", 1);
	script_notify_on_update(update);
	return 0;
}
EOF
check "ajout d'un maillage en 1012 : table à 6 tuiles" '[ "$(line table append)" = 1 ] && [ "$(line table stats)" = "tile=1001 table=1001,1002,1003,1004,1011,1012 stores=9 partial=3 contents=51" ]'
check "tuiles gardées par numéro (L17), la 1012 avec le fond de chaque calque" '[ "$(line table base)" = "$Y | $Y | $Y | $Y | $Y" ] && [ "$(line table layer)" = "$R | $G | $Z | $B | $R" ]'
check "emplacement d'undo d'avant l'ajout : ses 5 tuiles ; Redo revide les 6, Undo rend les 6 (la 1012 d'avant le Redo)" '[ "$(line table slot)" = "$SLOT" ] && [ "$(line table redone)" = "$Z | $Z | $Z | $Z | $Z" ] && [ "$(line table undone)" = "$R | $G | $Z | $B | $R" ]'
check "vue 3D après l'ajout : 1001 rouge, 1011 toujours bleu, 1012 rouge (fond)" '[ "$(hue table vp1001) $(hue table vp1011) $(hue table vp1012)" = "red blue red" ]'
check "UDIM coupé : plus aucune texture à tuiles" '[ "$(line table off)" = 1 ] && [ "$(line table offstats)" = "tile=-1 table= stores=0 partial=0 contents=0" ] && [ "$(line table offl0)" = "base=[single 128x128] nor=[single 128x128] pack=[single 128x128]" ]'

[ $FAIL -eq 0 ] && echo "== OK" || echo "== FAIL"
exit $FAIL
