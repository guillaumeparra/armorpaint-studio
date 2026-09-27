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
#   6. UDIM coupé (maillage sans tuile importé à la place) : plus aucune texture à tuiles ;
#   7. peinture (lot D2b) : trait à cheval sur 1001 et 1002, continu à la couture, undo et redo pas à pas, chaque pas
#      ne tient que les tuiles touchées ; de même pinceau aligné sur la caméra et caméra ortho ; en xray chaque pas tient
#      toutes les tuiles ; symétrie x : le miroir peint les tuiles d'en face ;
#   8. cartes UV, de triangles et de dilatation par tuile ; pipette (tuile sous le pinceau et sa couleur) ; remplissage
#      de face et d'îlot UV sur la tuile touchée seule, undo ; clone : source sur une autre tuile, rien de peint ;
#   9. bakes par tuile : Texcoord, Position, Object ID ; high poly (hauteur, dérivée, normales) avec le high confondu
#      ou incliné ; Curvature : un aperçu par tuile ;
#  10. 70 tuiles : le pinceau au-delà des 64 tenues ne peint rien ;
#  11. opérations de calques (lot D2c) : flatten par tuile ; Merge Down, undo, redo ; masques (et leur empilement
#      upstream), Apply Mask, undo ; groupe à masque de groupe, Merge Group ; surcharge de matériau d'un maillage dans
#      le flatten de sa tuile ; ColorID → masque sur toutes les tuiles ;
#  12. export (lot D2d) : par tuile, chaque fichier à la résolution des tuiles, identique au bit près à celui d'un
#      projet d'une seule tuile ; Visible, le composite en grille (une case par tuile, masques fusionnés par tuile, le
#      calque limité à un objet dans sa case seule) ; Selected et par objet, la tuile de l'objet ; Export d'un masque :
#      un fichier par tuile.
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
import math


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
open("single.obj", "w").write(quads("single", ((0, 0),)))  # La 1001 du ruban seule, sans tuile

STRIP = ((0, 0), (1, 0), (2, 0), (3, 0), (0, 1))


def mesh(objects):
    """Un objet par tuile : dans chaque objet, les v, vt, vn d'abord, puis ses faces (des v après des faces coupent
    l'objet). objects : (nom, z(u, v, tuile), n(x, y, u local, v local, tuile)) ; z en unités obj, n non normée ou None
    (normales de face calculées à l'import)."""
    out, nv = [], 0
    for name, z, n in objects:
        head, faces = ["o %s" % name], []
        for u, v in STRIP:
            tile = 1001 + u + 10 * v
            for t in (((u, v), (u + 1, v), (u, v + 1)), ((u + 1, v), (u + 1, v + 1), (u, v + 1))):
                for q in t:
                    x, y = 0.5 * (q[0] - 2), 0.5 * (q[1] - 1)
                    head += ["v %.6g %.6g %.6g" % (x, y, z(q[0] - u, q[1] - v, tile)), "vt %.6g %.6g" % q]
                    if n is not None:
                        a, b, c = n(x, y, q[0] - u, q[1] - v, tile)
                        d = (a * a + b * b + c * c) ** 0.5
                        head.append("vn %.6g %.6g %.6g" % (a / d, b / d, c / d))
                faces.append("f " + " ".join(("%d/%d/%d" if n is not None else "%d/%d") % ((nv + k + 1,) * (3 if n is not None else 2)) for k in range(3)))
                nv += 3
        out += head + faces
    return "\n".join(out) + "\n"


flat = lambda u, v, tile: 0.0
tilt = lambda u, v, tile: {1001: 0.15 * u, 1003: -0.15 * u, 1004: 0.15 * v}.get(tile, 0.0)
smooth = lambda x, y, u, v, tile: (0.4 * x, 0.4 * y, 1.0)  # Normales du low qui varient : cotangent_frame(n, n, uv) d'upstream
bend = {1001: 1.5, 1002: 0.0, 1003: 0.5, 1004: 0.0, 1011: 1.0}
curv = lambda x, y, u, v, tile: (bend[tile] * (u - 0.5), bend[tile] * (v - 0.5) * 0.5, 1.0)
open("curv.obj", "w").write(mesh((("curv", flat, curv),)))
open("lowhigh.obj", "w").write(mesh((("low", flat, smooth), ("high", tilt, None))))
open("lowflat.obj", "w").write(mesh((("low", flat, smooth), ("flat", flat, None))))


def cubes(path, center):
    """70 cubes p.1001 ... p.1070 (udim_app.sh) : le cube center à l'origine, les autres en couronne."""
    out, nv, n = [], 0, 0
    for t in range(1001, 1071):
        c = (0, 0, 0)
        if t != center:
            a = 2 * math.pi * n / 69
            c = (6 * math.cos(a), 6 * math.sin(a), 0)
            n += 1
        out.append("o p.%d" % t)
        out += ["v %g %g %g" % (c[0] + dx * 0.5, c[1] + dy * 0.5, c[2] + dz * 0.5) for dx in (-1, 1) for dy in (-1, 1) for dz in (-1, 1)]
        out += ["vt %g %g" % uv for uv in ((0.1, 0.1), (0.9, 0.1), (0.9, 0.9), (0.1, 0.9))]
        for f in ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)):
            out.append("f " + " ".join("%d/%d" % (nv + i + 1, (nv // 8) * 4 + k + 1) for k, i in enumerate(f)))
        nv += 8
    open(path, "w").write("\n".join(out) + "\n")


cubes("center1063.obj", 1063)
cubes("center1065.obj", 1065)


# Carte d'id 64 x 64 : moitié gauche rouge, moitié droite bleue
import struct, zlib
rows = b"".join(b"\x00" + b"".join(b"\xff\x00\x00" if x < 32 else b"\x00\x00\xff" for x in range(64)) for y in range(64))
chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
ihdr = struct.pack(">IIBBBBB", 64, 64, 8, 2, 0, 0, 0)
open("cid.png", "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
EOF

# Lance un script dans ArmorPaint sans interface ; ses lignes « t: » dans $W/$1.out. $2 : code de retour attendu (0).
script() {
	cat > "$W/$1.c"
	rc=0
	perl -e 'alarm 180; exec @ARGV' "$APP" --background --script "$W/$1.c" -ApplePersistenceIgnoreState YES > "$W/$1.log" 2>&1 || rc=$?
	[ $rc -eq "${2:-0}" ] || ko "script $1 : ArmorPaint rc $rc (voir $W/$1.log)"
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

# Matériau d'une couleur, pinceau dur (lot D2b)
PAINT='
void material(float r, float g, float b) {
	slot_material_t *m = script_material_create("flat");
	ui_node_t *rgb = script_material_create_node_at("RGB", -400.0, 0.0);
	script_material_set_color(rgb, 0, 0, r, g, b, 1.0);
	script_material_connect(rgb, 0, script_material_get_node("OUTPUT_MATERIAL_PBR"), 0);
	script_material_update();
	script_material_set(m);
}
void brush(float radius) {
	context_select_tool(TOOL_TYPE_BRUSH);
	context_t *ctx = script_get_context();
	ctx->brush_radius = radius;
	ctx->brush_opacity = 1.0;
	ctx->brush_hardness = 1.0;
}
void stroke(float x0, float x1, float z) {
	script_paint_world(x0, 0.0, z);
	script_paint_world((x0 + x1) * 0.5, 0.0, z);
	script_paint_world(x1, 0.0, z);
	script_paint_end();
}
'

echo "== 7. peinture par tuile : couture, undo par pas, caméra alignée, ortho, xray, symétrie"
for opt in none camera_align ortho xray; do
	script "paint_$opt" <<EOF
$PAINT
void seam(char *what) {
	console_log(string("t: %s %s | %s | %s | %s", what, studio_udim_pixel(1, 0, 1001, 127, 64), studio_udim_pixel(1, 0, 1002, 0, 64),
		studio_udim_pixel(1, 0, 1003, 64, 64), studio_udim_pixel(1, 0, 1011, 64, 64)));
}
int main() {
	studio_project_res(128);
	studio_import_mesh("$W/strip.obj", 2);
	studio_layer_new();
	studio_layer_select(1);
	material(1.0, 0.0, 0.0);
	brush(0.2);
	console_log(string("t: opt %d", studio_paint_option("$opt", 1)));
	stroke(-0.6, -0.4, -0.25);
	stroke(0.2, 0.3, -0.25);
	stroke(0.8, 0.85, 0.9);
	for (int k = 1; k <= 3; k++) {
		console_log(string("t: slot%d %s", k, studio_udim_layer(-k)));
	}
	seam("abc");
	studio_layer_op("undo");
	seam("undoC");
	studio_layer_op("undo");
	seam("undoB");
	studio_layer_op("undo");
	seam("undoA");
	studio_layer_op("redo");
	seam("redoA");
	studio_layer_op("redo");
	seam("redoB");
	return 0;
}
EOF
done
SA="base=[cur=1001 partial=1 tiles=1001,1002 128x128] nor=[cur=1001 partial=1 tiles=1001,1002 128x128] pack=[cur=1001 partial=1 tiles=1001,1002 128x128]"
SB="base=[cur=-1 partial=1 tiles=1003 1x1] nor=[cur=-1 partial=1 tiles=1003 1x1] pack=[cur=-1 partial=1 tiles=1003 1x1]"
SC="base=[cur=0 partial=1 tiles=0 128x128] nor=[cur=0 partial=1 tiles=0 128x128] pack=[cur=0 partial=1 tiles=0 128x128]"
SX="base=[cur=1001 partial=1 tiles=1001,1002,1003,1004,1011 128x128] nor=[cur=1001 partial=1 tiles=1001,1002,1003,1004,1011 128x128] pack=[cur=1001 partial=1 tiles=1001,1002,1003,1004,1011 128x128]"
for opt in none camera_align ortho xray; do
	check "$opt : trait à cheval sur 1001 et 1002, continu à la couture (texel 127 de 1001, 0 de 1002)" '[ "$(line paint_$opt opt)" = "$([ $opt = none ] && echo 0 || echo 1)" ] && [ "$(line paint_$opt abc)" = "$R | $R | $R | $Z" ]'
	check "$opt : undo et redo pas à pas, chaque tuile rendue" '[ "$(line paint_$opt undoC)" = "$R | $R | $R | $Z" ] && [ "$(line paint_$opt undoB)" = "$R | $R | $Z | $Z" ] && [ "$(line paint_$opt undoA)" = "$Z | $Z | $Z | $Z" ] && [ "$(line paint_$opt redoA)" = "$R | $R | $Z | $Z" ] && [ "$(line paint_$opt redoB)" = "$R | $R | $R | $Z" ]'
done
for opt in none camera_align ortho; do
	check "$opt : chaque pas ne tient que ses tuiles (1001+1002, 1003, rien pour un trait dans le vide)" '[ "$(line paint_$opt slot1)" = "$SA" ] && [ "$(line paint_$opt slot2)" = "$SB" ] && [ "$(line paint_$opt slot3)" = "$SC" ]'
done
check "xray : le pinceau traverse, chaque pas tient toutes les tuiles" '[ "$(line paint_xray slot1)" = "$SX" ] && [ "$(line paint_xray slot2)" = "$SX" ] && [ "$(line paint_xray slot3)" = "$SX" ]'
# Symétrie : peinte par la frame (render_path_paint_draw), le trait tenu ouvert une frame. Le segment de la frame, de
# part et d'autre de la couture 1001 | 1002 (x = -0,5), a son miroir en x sur la couture 1003 | 1004 (x = 0,5).
script paint_sym <<EOF
$PAINT
int frame = 0;
void seam(char *what) {
	console_log(string("t: %s %s | %s | %s | %s", what, studio_udim_pixel(1, 0, 1001, 127, 64), studio_udim_pixel(1, 0, 1002, 0, 64),
		studio_udim_pixel(1, 0, 1003, 127, 64), studio_udim_pixel(1, 0, 1004, 0, 64)));
}
void update() {
	frame++;
	if (frame == 3) {
		script_paint_world(-0.6, 0.0, -0.25);
		script_paint_world(-0.5, 0.0, -0.25);
		script_paint_world(-0.4, 0.0, -0.25);
	}
	if (frame == 4) {
		script_paint_end();
		seam("sym");
		console_log(string("t: slot %s", studio_udim_layer(-1)));
		studio_layer_op("undo");
		seam("undo");
		studio_layer_op("redo");
		seam("redo");
		script_quit();
	}
}
int main() {
	studio_project_res(128);
	studio_import_mesh("$W/strip.obj", 2);
	studio_layer_new();
	studio_layer_select(1);
	material(1.0, 0.0, 0.0);
	brush(0.2);
	console_log(string("t: opt %d", studio_paint_option("sym_x", 1)));
	script_notify_on_update(update);
	return 0;
}
EOF
S4="base=[cur=1001 partial=1 tiles=1001,1002,1003,1004 128x128] nor=[cur=1001 partial=1 tiles=1001,1002,1003,1004 128x128] pack=[cur=1001 partial=1 tiles=1001,1002,1003,1004 128x128]"
check "symétrie x : le miroir peint la couture 1003 | 1004, le pas tient les 4 tuiles, undo et redo" '[ "$(line paint_sym opt)" = 1 ] && [ "$(line paint_sym sym)" = "$R | $R | $R | $R" ] && [ "$(line paint_sym slot)" = "$S4" ] && [ "$(line paint_sym undo)" = "$Z | $Z | $Z | $Z" ] && [ "$(line paint_sym redo)" = "$R | $R | $R | $R" ]'

echo "== 8. pipettes, remplissages, cartes UV, clone"
script pick <<EOF
$PAINT
// 1003a : triangle (0, 0) (1, 0) (0, 1) de la 1003, 1003b : l'autre
void px(char *what) {
	console_log(string("t: %s %s | %s | %s | %s | %s | %s", what, studio_udim_pixel(1, 0, 1001, 64, 64), studio_udim_pixel(1, 0, 1002, 64, 64),
		studio_udim_pixel(1, 0, 1003, 26, 102), studio_udim_pixel(1, 0, 1003, 102, 26), studio_udim_pixel(1, 0, 1004, 64, 64),
		studio_udim_pixel(1, 0, 1011, 64, 64)));
}
void pick(float x, float z) {
	context_select_tool(TOOL_TYPE_PICKER);
	script_paint_world(x, 0.0, z);
	script_paint_end();
	console_log(string("t: pick%s %s", studio_udim_pick("cursor", x, 0.0, z), studio_udim_pick("picked", 0.0, 0.0, 0.0)));
}
void maps(char *what) {
	for (int c = 3; c <= 5; c++) {
		console_log(string("t: %s%d %s | %s | %s", what, c, studio_udim_pixel(1, c, 1001, 64, 64), studio_udim_pixel(1, c, 1003, 26, 102),
			studio_udim_pixel(1, c, 1003, 102, 26)));
	}
}
int main() {
	studio_project_res(128);
	studio_import_mesh("$W/strip.obj", 2);
	studio_layer_new();
	studio_layer_select(1);
	maps("map");
	studio_udim_fill(1, 0, 1001, 1.0, 0.0, 0.0, 1.0);
	studio_udim_fill(1, 0, 1002, 0.0, 1.0, 0.0, 1.0);
	studio_udim_fill(1, 0, 1003, 0.0, 0.0, 1.0, 1.0);
	studio_udim_fill(1, 0, 1004, 1.0, 1.0, 0.0, 1.0);
	studio_udim_fill(1, 0, 1011, 1.0, 0.0, 1.0, 1.0);
	pick(-0.75, -0.25);
	pick(-0.25, -0.25);
	pick(0.75, -0.25);
	pick(-0.75, 0.25);
	material(1.0, 1.0, 1.0);
	context_select_tool(TOOL_TYPE_FILL);
	studio_paint_option("fill_type", 1);
	script_paint_world(0.1, 0.0, -0.4);
	script_paint_end();
	px("face");
	console_log(string("t: faceslot %s", studio_udim_layer(-5))); // Emplacement 4 : les 4 pipettes avant
	studio_paint_option("fill_type", 3);
	console_log(string("t: islands %s", studio_udim_pick("islands", -0.25, 0.0, -0.25)));
	script_paint_world(-0.25, 0.0, -0.25);
	script_paint_end();
	px("island");
	console_log(string("t: islandslot %s", studio_udim_layer(-6)));
	studio_layer_op("undo");
	px("undoisland");
	studio_layer_op("undo");
	px("undoface");
	// Clone : la source sur une autre tuile n'est pas liée, rien n'est peint ; sur la même tuile, copiée
	studio_udim_fill(1, 0, 1002, 0.0, 1.0, 0.0, 1.0);
	brush(0.1);
	script_paint_world(-0.347, 0.0, -0.25);
	script_paint_end();
	context_select_tool(TOOL_TYPE_CLONE);
	console_log(string("t: src %d", studio_clone_source(0.1, 0.0, -0.25, -0.1, 0.0, -0.25)));
	script_paint_world(-0.1, 0.0, -0.25);
	script_paint_end();
	console_log(string("t: clone1003 %s", studio_udim_pixel(1, 0, 1002, 99, 71)));
	console_log(string("t: src %d", studio_clone_source(-0.347, 0.0, -0.25, -0.1, 0.0, -0.25)));
	script_paint_world(-0.1, 0.0, -0.25);
	script_paint_end();
	console_log(string("t: clone1002 %s", studio_udim_pixel(1, 0, 1002, 99, 71)));
	return 0;
}
EOF
W1="1.000 1.000 1.000 1.000"
YE="1.000 1.000 0.000 1.000"
MA="1.000 0.000 1.000 1.000"
check "carte UV de chaque tuile : le quadrilatère dans ses uvs locales (diagonale au centre, 1003 vide hors arêtes)" '[ "$(line pick map3)" = "$W1 | $Z | $Z" ]'
check "carte des triangles et de dilatation par tuile : les deux triangles de la 1003 couverts" 'line pick map4 | awk -F" [|] " "{ exit !(\$2 != \"0.000 0.000 0.000 1.000\" && \$3 != \"0.000 0.000 0.000 1.000\") }" && [ "$(line pick map5)" = "1.000 0.000 0.000 1.000 | 1.000 0.000 0.000 1.000 | 1.000 0.000 0.000 1.000" ]'
check "pipette : tuile sous le pinceau et sa couleur (1001 rouge, 1002 vert, 1004 jaune, 1011 magenta)" 'line pick pick1001 | grep -q "^tile=1001 rgb=255 0 0 " && line pick pick1002 | grep -q "^tile=1002 rgb=0 255 0 " && line pick pick1004 | grep -q "^tile=1004 rgb=255 255 0 " && line pick pick1011 | grep -q "^tile=1011 rgb=255 0 255 "'
check "remplissage de face : le triangle touché de la 1003 seul, l'undo ne tient que la 1003" '[ "$(line pick face)" = "$R | $G | $W1 | $B | $YE | $MA" ] && line pick faceslot | grep -q "^base=\[cur=-1 partial=1 tiles=1003 1x1\]"'
check "remplissage d'îlot UV : l'îlot de la 1002 seul, l'undo ne tient que la 1002" '[ "$(line pick islands)" = "tile=1002" ] && [ "$(line pick island)" = "$R | $W1 | $W1 | $B | $YE | $MA" ] && line pick islandslot | grep -q "^base=\[cur=-1 partial=1 tiles=1002 1x1\]"'
check "undo des remplissages, tuile par tuile" '[ "$(line pick undoisland)" = "$R | $G | $W1 | $B | $YE | $MA" ] && [ "$(line pick undoface)" = "$R | $G | $B | $B | $YE | $MA" ]'
check "clone : source sur une autre tuile, rien de peint ; sur la même tuile, copiée" '[ "$(line pick clone1003)" = "$G" ] && [ "$(line pick clone1002)" = "$W1" ]'

echo "== 9. bakes par tuile : nœud Bake Texture, Curvature, high poly"
BAKE='
int frame = 0;
int step = 0;
int node_id = -1;
int high = -1;
ui_node_t *bake = NULL;
void px(char *what) {
	console_log(string("t: %s %s | %s | %s | %s | %s", what, studio_udim_pixel(node_id, 6, 1001, 32, 96), studio_udim_pixel(node_id, 6, 1002, 32, 96),
		studio_udim_pixel(node_id, 6, 1003, 32, 96), studio_udim_pixel(node_id, 6, 1004, 32, 96), studio_udim_pixel(node_id, 6, 1011, 32, 96)));
}
void bake_node(char *name) {
	studio_layer_new();
	studio_layer_select(1);
	for (int i = 0; i < 20; i++) {
		if (string_equals(studio_object_rename(i, ""), name)) {
			high = i;
		}
	}
	slot_material_t *m = script_material_create("bake");
	bake = script_material_create_node_at("TEX_BAKE", -400.0, 0.0);
	script_material_connect(bake, 0, script_material_get_node("OUTPUT_MATERIAL_PBR"), 0);
	script_material_update();
	script_material_set(m);
}
// Un bake par image, depuis update : le nœud vide sa texture à l image suivante
int types[5] = {6, 5, 8, 0, 0};
char *names[5] = {"", "", "", "", ""};
int count = 0;
void update() {
	frame++;
	if (frame < 3 || string_equals(studio_bake_state(), "baking")) {
		return;
	}
	if (step > 0) {
		px(names[step - 1]);
	}
	if (step == count) {
		script_quit();
		return;
	}
	node_id = studio_bake(bake, types[step], high);
	step++;
}
'
script bake <<EOF
$BAKE
int main() {
	studio_project_res(128);
	studio_import_mesh("$W/strip.obj", 2);
	bake_node("");
	count = 3;
	names[0] = "texcoord";
	names[1] = "position";
	names[2] = "objectid";
	script_notify_on_update(update);
	return 0;
}
EOF
for mesh in lowhigh lowflat; do
	script "bake_$mesh" <<EOF
$BAKE
int main() {
	studio_project_res(128);
	studio_import_mesh("$W/$mesh.obj", 2);
	bake_node("$(echo $mesh | sed 's/^low//').1001");
	count = 3;
	types[0] = 1;
	types[1] = 3;
	types[2] = 4;
	names[0] = "normal";
	names[1] = "height";
	names[2] = "derivative";
	console_log(string("t: high %d", high));
	script_notify_on_update(update);
	return 0;
}
EOF
done
script curvature <<EOF
void px(char *what, int ch, int i) {
	console_log(string("t: %s %s | %s | %s | %s | %s", what, studio_udim_pixel(i, ch, 1001, 32, 96), studio_udim_pixel(i, ch, 1002, 32, 96),
		studio_udim_pixel(i, ch, 1003, 32, 96), studio_udim_pixel(i, ch, 1004, 32, 96), studio_udim_pixel(i, ch, 1011, 32, 96)));
}
int main() {
	studio_project_res(128);
	studio_import_mesh("$W/curv.obj", 2);
	studio_layer_new();
	studio_layer_select(1);
	slot_material_t *m = script_material_create("curv");
	ui_node_t *c = script_material_create_node_at("BAKE_CURVATURE", -400.0, 0.0);
	script_material_connect(c, 0, script_material_get_node("OUTPUT_MATERIAL_PBR"), 0);
	script_material_set(m);
	script_material_update();
	px("preview", 7, 0);
	context_select_tool(TOOL_TYPE_FILL);
	studio_paint_option("fill_type", 0);
	script_paint_world(-0.75, 0.0, -0.25);
	script_paint_end();
	px("layer", 0, 1);
	return 0;
}
EOF
# Composante n (1 à 4) du pixel de la tuile k (1 à 5) d'une ligne « r g b a | r g b a | ... »
comp() { line "$1" "$2" | awk -F' [|] ' -v k="$3" -v n="$4" '{ split($k, c, " "); print c[n] }'; }
gt() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a > b) }'; }
check "bake Texcoord : les uv locales de chaque tuile, le même texel partout" '[ "$(line bake texcoord)" = "0.255 0.753 0.000 1.000 | 0.255 0.753 0.000 1.000 | 0.255 0.753 0.000 1.000 | 0.255 0.753 0.000 1.000 | 0.255 0.753 0.000 1.000" ]'
check "bake Position : la position monde de chaque tuile (x croît de 1001 à 1004, z plus haut en 1011)" 'gt $(comp bake position 2 1) $(comp bake position 1 1) && gt $(comp bake position 3 1) $(comp bake position 2 1) && gt $(comp bake position 4 1) $(comp bake position 3 1) && gt $(comp bake position 5 3) $(comp bake position 1 3)'
check "bake Object ID : une couleur par objet, cinq différentes" '[ "$(line bake objectid | tr "|" "\n" | sort -u | wc -l | tr -d " ")" = 5 ]'
check "high poly confondu avec le low : hauteur 0 et dérivée 0,5 sur toutes les tuiles" '[ "$(line bake_lowflat high)" = 0 ] && [ "$(line bake_lowflat height)" = "$K | $K | $K | $K | $K" ] && [ "$(line bake_lowflat derivative)" = "0.502 0.502 0.000 1.000 | 0.502 0.502 0.000 1.000 | 0.502 0.502 0.000 1.000 | 0.502 0.502 0.000 1.000 | 0.502 0.502 0.000 1.000" ]'
check "high poly incliné (1001 et 1003 en u, 1004 en v) : hauteur sur ces tuiles seules" '[ "$(line bake_lowhigh high)" = 0 ] && [ "$(comp bake_lowhigh height 2 1) $(comp bake_lowhigh height 5 1)" = "0.000 0.000" ] && gt $(comp bake_lowhigh height 1 1) 0.2 && [ "$(comp bake_lowhigh height 1 1)" = "$(comp bake_lowhigh height 3 1)" ] && gt $(comp bake_lowhigh height 4 1) 0.2'
check "high poly incliné : la dérivée suit l axe de la pente, 0,5 sur les tuiles planes (patch bake-derivative)" '[ "$(comp bake_lowhigh derivative 1 1) $(comp bake_lowhigh derivative 3 1) $(comp bake_lowhigh derivative 4 2)" = "1.000 1.000 0.000" ] && [ "$(comp bake_lowhigh derivative 2 1) $(comp bake_lowhigh derivative 2 2) $(comp bake_lowhigh derivative 5 1) $(comp bake_lowhigh derivative 5 2)" = "0.502 0.502 0.502 0.502" ]'
check "high poly : normales identiques au high plat sur les tuiles planes, différentes sur les inclinées, ni NaN ni noir" '(for k in 2 5; do [ "$(line bake_lowhigh normal | awk -F" [|] " -v k=$k "{print \$k}")" = "$(line bake_lowflat normal | awk -F" [|] " -v k=$k "{print \$k}")" ] || exit 1; done; for k in 1 3 4; do [ "$(line bake_lowhigh normal | awk -F" [|] " -v k=$k "{print \$k}")" != "$(line bake_lowflat normal | awk -F" [|] " -v k=$k "{print \$k}")" ] || exit 1; done) && ! grep -qi nan "$W/bake_lowhigh.out" && gt $(comp bake_lowhigh normal 2 3) 0.9'
check "Curvature : un aperçu par tuile (1001 > 1011 > 1003 > 0, 1002 et 1004 planes à 0), le calque rempli le reprend" 'gt $(comp curvature preview 1 1) $(comp curvature preview 5 1) && gt $(comp curvature preview 5 1) $(comp curvature preview 3 1) && gt $(comp curvature preview 3 1) 0 && [ "$(comp curvature preview 2 1) $(comp curvature preview 4 1)" = "0.000 0.000" ] && [ "$(comp curvature layer 1 1) $(comp curvature layer 3 1) $(comp curvature layer 5 1)" = "$(comp curvature preview 1 1) $(comp curvature preview 3 1) $(comp curvature preview 5 1)" ]'

echo "== 10. 70 tuiles : le pinceau au-delà des 64 ne peint rien"
for c in 1063 1065; do
	script "cubes$c" 1 <<EOF # rc 1 : l'erreur « 70 tiles, the layers hold 64 »
$PAINT
int main() {
	studio_project_res(128);
	studio_import_mesh("$W/center$c.obj", 0);
	material(1.0, 0.0, 0.0);
	brush(0.5);
	stroke(0.0, 0.05, 0.0);
	char *reds = "";
	for (int t = 1001; t <= 1064; t++) {
		if (string_index_of(studio_udim_pixel(0, 0, t, 64, 64), "1.000 0.000 0.000") == 0) {
			reds = string("%s %d", reds, t);
		}
	}
	console_log(string("t: reds [%s]", reds));
	console_log(string("t: slot %s", studio_udim_layer(-1)));
	return 0;
}
EOF
done
check "cube 1063 au centre : peint dans sa tuile seule, l'undo ne tient qu'elle" '[ "$(line cubes1063 reds)" = "[ 1063]" ] && line cubes1063 slot | grep -q "^base=\[cur=-1 partial=1 tiles=1063 1x1\]"'
check "cube 1065 (hors des 64) au centre : rien de peint, rien de tenu" '[ "$(line cubes1065 reds)" = "[]" ] && line cubes1065 slot | grep -q "^base=\[cur=-*[01] partial=1 tiles=0 "'

echo "== 11. opérations de calques : flatten, fusion, masques, groupes, surcharge de matériau, ColorID"
# Couleurs d'un calque (canal 0) ou du flatten (canal 9) sur les 5 tuiles du ruban, au texel x
LAYERS='
void c5(char *what, int l, int ch, int x) {
	console_log(string("t: %s %s | %s | %s | %s | %s", what, studio_udim_pixel(l, ch, 1001, x, 10), studio_udim_pixel(l, ch, 1002, x, 10),
		studio_udim_pixel(l, ch, 1003, x, 10), studio_udim_pixel(l, ch, 1004, x, 10), studio_udim_pixel(l, ch, 1011, x, 10)));
}
void names() {
	console_log(string("t: names %s", studio_layer_masks()));
}
// Calque 1 : 1001 rouge, 1002 vert, 1011 bleu ; calque 2 : 1001 bleu, 1003 magenta
void two_layers() {
	studio_project_res(128);
	studio_import_mesh("'"$W"'/strip.obj", 2);
	studio_layer_new();
	studio_udim_fill(1, 0, 1001, 1.0, 0.0, 0.0, 1.0);
	studio_udim_fill(1, 0, 1002, 0.0, 1.0, 0.0, 1.0);
	studio_udim_fill(1, 0, 1011, 0.0, 0.0, 1.0, 1.0);
	studio_layer_new();
	studio_udim_fill(2, 0, 1001, 0.0, 0.0, 1.0, 1.0);
	studio_udim_fill(2, 0, 1003, 1.0, 0.0, 1.0, 1.0);
}
'
script merge <<EOF
$LAYERS
int main() {
	two_layers();
	c5("flat0", 0, 9, 10);
	studio_layer_select(2);
	console_log(string("t: merge %d", studio_layer_op("merge_down")));
	names();
	c5("merged", 1, 0, 10);
	c5("flat1", 0, 9, 10);
	studio_layer_op("undo");
	names();
	c5("undo1", 1, 0, 10);
	c5("undo2", 2, 0, 10);
	studio_layer_op("redo");
	c5("redo", 1, 0, 10);
	// Masques du calque fusionné : le premier noir sur la 1002 ; le second, blanc, par-dessus
	studio_layer_select(1);
	console_log(string("t: mask %d", studio_layer_new_mask(1)));
	studio_udim_fill(1, 0, 1002, 0.0, 0.0, 0.0, 1.0);
	c5("flatmask", 0, 9, 10);
	studio_layer_new_mask(1);
	studio_udim_fill(1, 0, 1011, 0.0, 0.0, 0.0, 1.0);
	names();
	c5("flatmask2", 0, 9, 10);
	studio_layer_select(1);
	console_log(string("t: apply %d", studio_layer_op("apply_mask")));
	names();
	c5("flatapply", 0, 9, 10);
	c5("applied", 2, 0, 10);
	studio_layer_op("undo");
	names();
	c5("unapplied", 3, 0, 10);
	return 0;
}
EOF
script group <<EOF
$LAYERS
int main() {
	two_layers();
	console_log(string("t: group %d %d %d", studio_layer_new_group(), studio_layer_set_parent(1, 3), studio_layer_set_parent(2, 3)));
	studio_layer_new_mask(1);
	studio_udim_fill(3, 0, 1011, 0.0, 0.0, 0.0, 1.0);
	names();
	c5("flatg", 0, 9, 10);
	studio_layer_select(4);
	console_log(string("t: mergeg %d", studio_layer_op("merge_group")));
	names();
	c5("merged", 1, 0, 10);
	c5("flatm", 0, 9, 10);
	return 0;
}
EOF
script override <<EOF
$LAYERS
int main() {
	studio_project_res(128);
	studio_import_mesh("$W/strip.obj", 2);
	studio_layer_new();
	studio_udim_fill(1, 0, 1001, 1.0, 0.0, 0.0, 1.0);
	studio_udim_fill(1, 0, 1002, 0.0, 1.0, 0.0, 1.0);
	studio_udim_fill(1, 0, 1011, 0.0, 0.0, 1.0, 1.0);
	c5("flat0", 0, 9, 10);
	script_material_create("ov");
	int o = -1;
	for (int i = 0; i < 8; i++) {
		if (string_equals(studio_object_rename(i, ""), "strip.1002")) {
			o = i;
		}
	}
	console_log(string("t: override %d", studio_mesh_override(o, 1)));
	c5("flatov", 0, 9, 10);
	studio_mesh_override(o, -1);
	c5("flatnoov", 0, 9, 10);
	studio_layer_select(1);
	int m = studio_colorid_mask("$W/cid.png", 0xff0000);
	console_log(string("t: cid %d", m));
	c5("cidleft", m, 0, 10);
	c5("cidright", m, 0, 100);
	return 0;
}
EOF
M="1.000 0.000 1.000 1.000"
FLAT="$B | $G | $M | $Y | $B"
MERGED="$B | $G | $M | $Z | $B"
check "flatten par tuile : le composite de chaque tuile (le gris du calque de base sur la 1004)" '[ "$(line merge flat0)" = "$FLAT" ]'
check "Merge Down : un calque de moins, chaque tuile fusionnée, flatten inchangé" '[ "$(line merge merge)" = 1 ] && [ "$(line merge merged)" = "$MERGED" ] && [ "$(line merge flat1)" = "$FLAT" ] && [ "$(line merge names | sed -n 1p)" = "Layer 1=0 Layer 2=0 filter=0" ]'
check "Merge Down, Undo : les deux calques et chacune de leurs tuiles rendus ; Redo : fusionnés" '[ "$(line merge names | sed -n 2p)" = "Layer 1=0 Layer 2=0 Layer 3=0 filter=0" ] && [ "$(line merge undo1)" = "$R | $G | $Z | $Z | $B" ] && [ "$(line merge undo2)" = "$B | $Z | $M | $Z | $Z" ] && [ "$(line merge redo)" = "$MERGED" ]'
check "masque noir sur la 1002 : le flatten n y montre que la base, les autres tuiles intactes" '[ "$(line merge mask)" = 1 ] && [ "$(line merge flatmask)" = "$B | $Y | $M | $Y | $B" ]'
check "deux masques : upstream, le dernier remplace les autres (fusion des masques en MIX)" '[ "$(line merge names | sed -n 3p)" = "Layer 1=0 Mask 3=0 Mask 4=0 Layer 2=0 filter=0" ] && [ "$(line merge flatmask2)" = "$FLAT" ]'
check "Apply Mask : le masque (noir sur 1002 et 1011) appliqué à chaque tuile du calque (alpha 0), puis retiré ; Undo le rend" '[ "$(line merge apply)" = 1 ] && [ "$(line merge names | sed -n 4p)" = "Layer 1=0 Mask 4=0 Layer 2=0 filter=0" ] && [ "$(line merge flatapply)" = "$B | $Y | $M | $Y | $Y" ] && [ "$(line merge applied)" = "$B | 0.000 1.000 0.000 0.000 | $M | $Z | 0.000 0.000 1.000 0.000" ] && [ "$(line merge names | sed -n 5p)" = "Layer 1=0 Mask 5=0 Mask 4=0 Layer 2=0 filter=0" ] && [ "$(line merge unapplied)" = "$MERGED" ]'
check "groupe à masque de groupe (noir sur la 1011) : flatten par tuile" '[ "$(line group group)" = "1 1 1" ] && [ "$(line group names | sed -n 1p)" = "Layer 1=0 Layer 2=0 Layer 3=0 Mask 5=0 Group 4=0 filter=0" ] && [ "$(line group flatg)" = "$B | $G | $M | $Y | $Y" ]'
check "Merge Group : un calque, chaque tuile fusionnée, le masque de groupe appliqué (1011 transparente), flatten inchangé" '[ "$(line group mergeg)" = 1 ] && [ "$(line group names | sed -n 2p)" = "Layer 1=0 Group 4=0 filter=0" ] && [ "$(line group merged)" = "$B | $G | $M | $Z | 0.000 0.000 1.000 0.000" ] && [ "$(line group flatm)" = "$(line group flatg)" ]'
check "surcharge de matériau du maillage de la 1002 : son flatten prend le matériau, les autres tuiles non ; retirée, le calque revient" '[ "$(line override override)" = 1 ] && [ "$(line override flatov)" = "$R | 0.800 0.800 0.800 1.000 | $Y | $Y | $B" ] && [ "$(line override flatnoov)" = "$(line override flat0)" ] && [ "$(line override flat0)" = "$R | $G | $Y | $Y | $B" ]'
check "ColorID → masque : la carte lue dans les uv de chaque tuile, blanc à gauche (rouge piqué), rien à droite, sur les 5 tuiles" '[ "$(line override cid)" = 1 ] && [ "$(line override cidleft)" = "$H | $H | $H | $H | $H" ] && [ "$(line override cidright)" = "$Z | $Z | $Z | $Z | $Z" ]'

echo "== 12. export : par tuile à la résolution des tuiles, Visible en grille, Selected, par objet"
# Bruit sur les uv (UV Map > Noise Texture > couleur de base), rempli dans le calque courant
NOISE='slot_material_t *m = script_material_create("noise");
	ui_node_t *uv = script_material_create_node_at("UVMAP", -800.0, 0.0);
	ui_node_t *noise = script_material_create_node_at("TEX_NOISE", -400.0, 0.0);
	ui_node_t *out = script_material_get_node("OUTPUT_MATERIAL_PBR");
	script_material_set_float(noise, 1, 1, 3.0);
	script_material_set_float(noise, 1, 2, 0.0);
	script_material_connect(uv, 0, noise, 0);
	script_material_connect(noise, 1, out, 0);
	script_material_update();
	script_material_set(m);'
mkdir -p "$W/out_noise" "$W/out_single" "$W/out_tiles" "$W/out_visible" "$W/out_selected" "$W/out_object" "$W/out_fresh" "$W/out_mask"
script export_fresh <<EOF
int main() {
	studio_project_res(128);
	studio_import_mesh("$W/strip.obj", 2);
	studio_export_run("$W/out_fresh", "", 0);
	return 0;
}
EOF
script export_noise <<EOF
int main() {
	studio_project_res(128);
	studio_import_mesh("$W/strip.obj", 2);
	$NOISE
	script_fill_layer();
	studio_export_run("$W/out_noise", "", 3);
	return 0;
}
EOF
script export_single <<EOF
int main() {
	studio_project_res(128);
	studio_import_mesh("$W/single.obj", 0);
	$NOISE
	script_fill_layer();
	studio_export_run("$W/out_single", "", 0);
	return 0;
}
EOF
# Calque 1 : 1001 rouge, 1002 vert, 1011 bleu, masqué par la carte d'id (blanc à gauche) ; calque 2, limité à
# strip.1004 : magenta sur la 1004
script export_cells <<EOF
int main() {
	studio_project_res(128);
	studio_import_mesh("$W/strip.obj", 2);
	studio_layer_new();
	studio_udim_fill(1, 0, 1001, 1.0, 0.0, 0.0, 1.0);
	studio_udim_fill(1, 0, 1002, 0.0, 1.0, 0.0, 1.0);
	studio_udim_fill(1, 0, 1011, 0.0, 0.0, 1.0, 1.0);
	studio_layer_select(1);
	console_log(string("t: cid %d", studio_colorid_mask("$W/cid.png", 0xff0000)));
	studio_layer_select(2);
	studio_layer_new();
	int o = -1;
	for (int i = 0; i < 8; i++) {
		if (string_equals(studio_object_rename(i, ""), "strip.1004")) {
			o = i;
		}
	}
	console_log(string("t: limit %d", studio_layer_set_object(o)));
	int l = 3;
	studio_udim_fill(l, 0, 1004, 1.0, 0.0, 1.0, 1.0);
	console_log(string("t: names %s", studio_layer_masks()));
	studio_export_run("$W/out_tiles", "", 3);
	studio_export_run("$W/out_visible", "", 0);
	studio_export_run("$W/out_object", "", 2);
	studio_layer_select(l);
	studio_export_run("$W/out_selected", "", 1);
	console_log(string("t: mask %d", studio_mask_export(1, "$W/out_mask", "cid")));
	return 0;
}
EOF
python3 - "$W" > "$W/exports.out" <<'EOF'
import glob, os, sys
from PIL import Image

w = sys.argv[1]
one = lambda pattern: (glob.glob(os.path.join(w, pattern)) + [None])[0]
maps = lambda d, tag: {os.path.basename(f).rsplit(tag, 1)[0].rsplit("_", 1)[-1]: f for f in glob.glob(os.path.join(w, d, "*" + tag))}
# Par tuile contre une seule tuile : les maps de la 1001 et du projet d'une tuile, pixel à pixel
tiles, single = maps("out_noise", ".1001.png"), maps("out_single", ".png")
same = sorted(k for k in tiles if k in single and Image.open(tiles[k]).tobytes() == Image.open(single[k]).tobytes())
print("noise %s | %s | %s" % (" ".join(same), " ".join(sorted(single)), Image.open(tiles["base"]).size if "base" in tiles else "-"))
print("noise_colors %d" % len(set(Image.open(tiles["base"]).convert("RGB").getdata())) if "base" in tiles else "noise_colors 0")
print("noise_1002 %d" % (Image.open(one("out_noise/*_base.1002.png")).tobytes() != Image.open(tiles["base"]).tobytes()))
hue = lambda p: {(255, 0, 0): "R", (0, 255, 0): "G", (0, 0, 255): "B", (255, 0, 255): "M", (0, 0, 0): "0"}.get(p[:3], "Y" if abs(p[0] - 127) <= 1 and p[0] == p[1] == p[2] else "?")
px = lambda f, x, y: hue(Image.open(f).convert("RGB").getpixel((x, y))) if f else "-"
# Par tuile : gauche (x 10) et droite (x 100) de chaque tuile
files = lambda d, tag: " ".join("%s%s" % (px(one("%s/*_base.%d.png" % (d, t)), 10, 64), px(one("%s/*_base.%d.png" % (d, t)), 100, 64)) for t in (1001, 1002, 1003, 1004, 1011))
print("tiles %s" % files("out_tiles", ""))
print("tiles_size %s" % " ".join("%dx%d" % Image.open(one("out_tiles/*_base.%d.png" % t)).size for t in (1001, 1011)))
# Visible : la grille 3 x 3, case s en (s % 3, s // 3), un quart et trois quarts de la case
vis = one("out_visible/*_base.png")
c = 128 / 3.0
print("visible %s" % " ".join("%s%s" % (px(vis, int((s % 3 + 0.25) * c), int((s // 3 + 0.5) * c)), px(vis, int((s % 3 + 0.75) * c), int((s // 3 + 0.5) * c))) for s in range(9)))
print("visible_files %d" % len(os.listdir(os.path.join(w, "out_visible"))))
fresh = one("out_fresh/*_base.png")
print("fresh %s" % " ".join(px(fresh, int((s % 3 + 0.5) * c), int((s // 3 + 0.5) * c)) for s in range(9)))
# Par objet : l'upstream n'exporte que les objets des calques limités (strip.1004) ; Selected : le calque limité seul
print("object %s %d" % (" ".join("%s%s" % (px(one("out_object/*strip.%d_base.png" % t), 10, 64), px(one("out_object/*strip.%d_base.png" % t), 100, 64)) for t in (1001, 1002, 1003, 1004, 1011)), len(os.listdir(os.path.join(w, "out_object")))))
# Export du masque : un fichier par tuile, blanc à gauche
mask = lambda t: one("out_mask/cid.%d.png" % t)
white = lambda f, x: "W" if f and Image.open(f).convert("RGB").getpixel((x, 64)) == (255, 255, 255) else "K" if f and Image.open(f).convert("RGB").getpixel((x, 64)) == (0, 0, 0) else "-"
print("mask %s %d %s" % (" ".join(white(mask(t), 10) + white(mask(t), 100) for t in (1001, 1002, 1003, 1004, 1011)), len(os.listdir(os.path.join(w, "out_mask"))), "%dx%d" % Image.open(mask(1011)).size if mask(1011) else "-"))
sel = one("out_selected/*_base.png")
print("selected %s %s %s" % (os.path.basename(sel).rsplit("_", 1)[0].split("_", 1)[-1] if sel else "-", px(sel, 10, 64) + px(sel, 100, 64), "%dx%d" % Image.open(sel).size if sel else "-"))
EOF
cat "$W/exports.out"
check "par tuile : les maps de la 1001 identiques au bit près à celles d'un projet d'une seule tuile, 128 px" 'line exports noise | awk -F" [|] " "{ exit !(\$1 == \$2 && \$1 != \"\" && \$3 == \"(128, 128)\") }" && [ $(line exports noise_colors) -gt 1000 ] && [ "$(line exports noise_1002)" = 1 ]'
check "par tuile : chaque tuile avec son calque, son masque (gauche seule) et le calque limité à la 1004" '[ "$(line export_cells cid) $(line export_cells limit)" = "1 1" ] && [ "$(line export_cells names)" = "Layer 1=0 Mask 3=0 Layer 2=0 Layer 4=5 filter=0" ] && [ "$(line exports tiles)" = "RY GY YY MM BY" ] && [ "$(line exports tiles_size)" = "128x128 128x128" ]'
check "Visible : la grille des tuiles (1001, 1002, 1003 ; 1004, 1011 ; cases vides), masque par tuile, calque limité dans sa case" '[ "$(line exports visible)" = "RY GY YY MM BY 00 00 00 00" ] && [ "$(line exports visible_files)" = 5 ]'
check "Visible juste après l'import (sans frame) : les calques passés aux tuiles d'abord, cases vides noires" '[ "$(line exports fresh)" = "Y Y Y Y Y 0 0 0 0" ]'
check "par objet : strip.1004 seul (l'objet du calque limité), sa tuile ; Selected : le calque limité seul, à la résolution des tuiles" '[ "$(line exports object)" = "-- -- -- MM -- 5" ] && line exports selected | grep -q "strip.1004 MM 128x128$"'

check "Export d'un masque : un fichier par tuile (<nom>.<tuile>.png), 128 px, blanc à gauche" '[ "$(line export_cells mask)" = 1 ] && [ "$(line exports mask)" = "WK WK WK WK WK 5 128x128" ]'
[ $FAIL -eq 0 ] && echo "== OK" || echo "== FAIL"
exit $FAIL
