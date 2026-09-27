#!/bin/sh
# UDIM dans ArmorPaint lui-même (--background), patches udim-import, fbx-split et udim-tile-limit (lot D0),
# udim-material-uv, udim-export-sets et udim-tile-name, udim-import et fbx-split par matériau et par tuile (lot D1) :
#   1. import OBJ et FBX « Split By: UDIM Tile » : un objet par nœud et par tuile (body.1001...), triangle à cheval
#      coupé ; un nœud sans uv en tête du fichier est importé sans tuile, après les autres : pas de dépliage proposé
#      (il aurait écrasé les uv UDIM de toutes les tuiles) ; les calques UDIM ne le peignent pas, avec un message ;
#   2. fichier refusé (uv négatives, plus de 64 tuiles) : message, le projet garde son maillage ;
#   3. le même fichier ajouté : body.001.1001 (l'upstream renommait body.1001 en body.1003, une autre tuile) ;
#   4. 70 tuiles (import par objet de p.1001 ... p.1070) : message ; les calques tiennent les 64 premières (lot D2),
#      chacune avec sa case dans les uv du maillage fusionné (le proxy du raytrace, lot D2f), p.1065 à p.1070 sans
#      case ; peindre p.1065 ne touche aucune tuile (l'upstream peignait dans la case de la 1064), peindre p.1063 peint
#      la 1063 ; export par tuile : 1001 à 1064 et un message par tuile écartée ; preset studio bloqué par le
#      préflight ;
#   5. conseil de résolution après un import UDIM (tuiles, résolution des textures de chaque tuile, mémoire d'un
#      calque) : dans la barre d'état si elle est libre ; sinon dans la console seule, l'avertissement (cap sans tuile)
#      ou l'erreur (70 tuiles) de l'import reste affiché.
#   6. uv matériau dans l'espace UDIM (patch udim-material-uv, lot D1) : un bruit procédural sur les uv, rempli sur cinq
#      tuiles pleines (1001 à 1004, 1011), est continu d'une tuile à sa voisine, à droite (1003 | 1004) et en haut
#      (1001 / 1011) ; témoins : tuiles non voisines, écart entre colonnes d'une même tuile ; un calque limité à la
#      tuile 1004 reçoit le même bruit que le calque de toutes les tuiles (l'upstream répétait le matériau par tuile, et
#      le calque d'une tuile avait ses propres uv).
#   7. découpage par matériau et par tuile (lot D1, OBJ et FBX) : un objet par matériau et par tuile (skin.1001...),
#      rassemblé d'un objet du fichier à l'autre (Maya répète usemtl dans chaque objet), triangle à cheval coupé ; les
#      positions des parts d'échelles différentes recalées (relues par un export OBJ du maillage : chaque sommet à la
#      position que son uv UDIM donne, surfaces uv conservées par matériau) ; un jeu de textures par matériau ; témoin :
#      le même fichier par objet et par tuile, sans jeu.
#   8. export par jeu de textures et par tuile (patch udim-export-sets, lot D1) : les maps de chaque jeu pour chacune de
#      ses tuiles seulement (skin 1001, 1002, 1011 ; cloth 1002), le jeu dans le nom ; un calque limité au jeu skin
#      (rouge) dans les fichiers de skin seuls, un calque limité au jeu cloth (bleu) dans cloth.1002 seul, même là où
#      leurs uv se chevauchent dans la 1002 ; un calque de toutes les tuiles dans tous, la même tuile pour les deux jeux
#      de la 1002 ; un calque limité à skin.1001 dans skin.1001 seul, et pas de fichier pour cloth, sans calque (comme
#      l'export Visible) ; preset studio avec <atlas> ; par objet, un calque limité à un jeu donne un fichier à chaque
#      objet du jeu, et l'export du calque sélectionné porte le nom du jeu (l'upstream lisait ces masques de jeu hors du
#      tableau des objets).
#   9. renommer une tuile (patch udim-tile-name, lot D1, comme l'onglet Meshes) : skin.1011 renommé torso donne
#      torso.1011 ; un suffixe tapé d'une autre tuile est ignoré, avec un message (cloth.1002 renommé robe.1005 donne
#      robe.1002) ; un nom vide laisse le nom ; un objet sans tuile n'en gagne pas (cap renommé cap.1005 reste cap) ;
#      projet sauvé puis rouvert : mêmes tuiles, mêmes jeux, le calque limité à torso.1011 exporté dans skin.1011 seul
#      (l'upstream renommait skin.1011 en torso, un objet sans tuile : la 1011 disparaissait de l'export).
#  10. canaux d'un calque dans un preset studio (layer('<nom>'), lot B4), lus par tuile comme la pile (lot D2d) — le
#      calque de toutes les tuiles seul (bruit) donne dans chaque tuile les pixels de la pile aplatie, et dans la 1004
#      ceux du banc 6 ; un calque limité à strip.1004 est dans la 1004 seule (vide ailleurs) ; grille entière
#      (Visible) : dans sa case du composite, égal à la pile ; par objet, strip.1004 comme la tuile 1004.
#
#   sh .tests/udim_app.sh [dossier de travail]
#   APP=<binaire ArmorPaint> pour un autre build (défaut : build Release de paint/).
# Demande : python3 avec Pillow.
set -eu
T=$(cd "$(dirname "$0")" && pwd)
EXT=$(dirname "$T")
APP=${APP:-$EXT/../../build/build/Release/ArmorPaint.app/Contents/MacOS/ArmorPaint}
W=${1:-$(mktemp -d)}
mkdir -p "$W"
W=$(cd "$W" && pwd)
[ -x "$APP" ] || { echo "ArmorPaint not found: $APP" >&2; exit 1; }
python3 -c "import PIL" 2>/dev/null || { echo "Pillow not found for python3" >&2; exit 1; }
FAIL=0
ok() { echo "ok   $1"; }
ko() { echo "FAIL $1"; FAIL=1; }
check() { if eval "$2"; then ok "$1"; else ko "$1"; fi; }

rm -rf "$W"/out_* "$W"/*.log "$W"/*.out
cd "$W"
python3 - "$T" <<'EOF'
import json, math, sys
sys.path.insert(0, sys.argv[1])
from fbx_udim import write_fbx  # noqa: E402
from obj_udim import T, tri_uv  # noqa: E402


def write_obj(path, objs):
    """objs : [(nom, triangles, avec uv)], les indices v / vt globaux au fichier."""
    out, nv, nt = [], 0, 0
    for name, tris, uv in objs:
        out.append("o %s" % name)
        for t in tris:
            for p, q in t:
                out.append("v %.6g %.6g %.6g" % p)
                if uv:
                    out.append("vt %.6g %.6g" % q)
        for t in tris:
            out.append("f " + " ".join(("%d/%d" % (nv + k + 1, nt + k + 1)) if uv else str(nv + k + 1) for k in range(3)))
            nv += 3
            nt += 3 if uv else 0
    open(path, "w").write("\n".join(out) + "\n")


cap = [T(((i, 0, 5), (0, 0)), ((i + 1, 0, 5), (0, 0)), ((i, 1, 5), (0, 0))) for i in range(4)]
body = [tri_uv(0.2, 0.2, 0.8, 0.2, 0.2, 0.8), tri_uv(1.2, 0.2, 1.8, 0.2, 1.2, 0.8), tri_uv(0.2, 1.2, 0.8, 1.2, 0.2, 1.8, z=1.0),
        tri_uv(0.5, 0.3, 1.5, 0.3, 0.5, 0.7, z=2.0)]  # Le dernier à cheval sur 1001 et 1002
write_obj("udim.obj", [("cap", cap, False), ("body", body, True)])
write_fbx("udim.fbx", [("cap", cap, {"uv": False}), ("body", body, {})])
neg = [tri_uv(0.2, 0.2, 0.8, 0.2, 0.2, 0.8), tri_uv(-0.5, 0.2, -0.2, 0.2, -0.5, 0.8)]
write_obj("neg.obj", [("body", neg, True)])
write_fbx("neg.fbx", [("body", neg, {})])
write_obj("many.obj", [("many", [tri_uv(u + 0.2, v + 0.2, u + 0.8, v + 0.2, u + 0.2, v + 0.8) for v in range(7) for u in range(10)], True)])
write_obj("plain.obj", [("plain", cap, False)])
write_obj("tiles4.obj", [("body", [tri_uv(u + 0.2, v + 0.2, u + 0.8, v + 0.2, u + 0.2, v + 0.8, z=u + v) for v in range(2) for u in range(2)], True)])
# Cinq tuiles pleines, deux triangles chacune : 1001 à 1004 sur une ligne, 1011 au-dessus de 1001
strip = []
for u, v in ((0, 0), (1, 0), (2, 0), (3, 0), (0, 1)):
    strip += [tri_uv(u, v, u + 1, v, u, v + 1), tri_uv(u + 1, v, u + 1, v + 1, u, v + 1)]
write_obj("strip.obj", [("strip", strip, True)])
# Matériau × tuile : body et head reprennent skin et cloth, comme Maya ; head plus loin (z = 4), d'une autre échelle.
# skin.1001 rassemble un triangle de chacun.
mats = [("body", [("skin", tri_uv(0.2, 0.2, 0.8, 0.2, 0.2, 0.8)), ("skin", tri_uv(0.5, 0.3, 1.5, 0.3, 0.5, 0.7)),
                  ("cloth", tri_uv(1.2, 0.2, 1.8, 0.2, 1.2, 0.8))]),
        ("head", [("skin", tri_uv(0.3, 0.5, 0.7, 0.5, 0.3, 0.9, z=4.0)), ("skin", tri_uv(0.2, 1.2, 0.8, 1.2, 0.2, 1.8, z=4.0)),
                  ("cloth", tri_uv(1.3, 0.5, 1.7, 0.5, 1.3, 0.9, z=4.0))])]
out, nv = [], 0
for name, faces in mats:
    out.append("o %s" % name)
    for _, t in faces:
        for p, q in t:
            out += ["v %.6g %.6g %.6g" % p, "vt %.6g %.6g" % q]
    cur = None
    for m, t in faces:
        if m != cur:
            out.append("usemtl %s" % m)
            cur = m
        out.append("f " + " ".join("%d/%d" % (nv + k + 1, nv + k + 1) for k in range(3)))
        nv += 3
open("mats.obj", "w").write("\n".join(out) + "\n")
write_fbx("mats.fbx", [(name, [t for _, t in faces], {"mats": [m for m, _ in faces]}) for name, faces in mats])
area = lambda t: 0.5 * abs((t[1][1][0] - t[0][1][0]) * (t[2][1][1] - t[0][1][1]) - (t[2][1][0] - t[0][1][0]) * (t[1][1][1] - t[0][1][1]))
json.dump({m: sum(area(t) for _, faces in mats for n, t in faces if n == m) for m in ("skin", "cloth")}, open("mats.json", "w"))


def cubes(path, center):
    """70 cubes p.1001 ... p.1070, uv [0.1, 0.9] sur chaque face ; le cube center à l'origine, les autres en couronne."""
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


cubes("center1065.obj", 1065)
cubes("center1063.obj", 1063)
EOF

# Lance un script dans ArmorPaint sans interface ; ses lignes « t: » dans $W/$1.out.
# script <nom> [projet .arm ouvert avant le script] : le script lu sur l'entrée standard. Code de retour attendu :
# $RC_WANT (0 ; 1 quand le run signale des erreurs, import refusé ou tuiles écartées, lot B7).
RC_WANT=0
script() {
	cat > "$W/$1.c"
	rc=0
	perl -e 'alarm 180; exec @ARGV' "$APP" ${2:+"$2"} --background --script "$W/$1.c" -ApplePersistenceIgnoreState YES > "$W/$1.log" 2>&1 || rc=$?
	[ $rc -eq $RC_WANT ] || ko "script $1 : ArmorPaint rc $rc au lieu de $RC_WANT (voir $W/$1.log)"
	grep '^t: ' "$W/$1.log" | sed 's/^t: //' > "$W/$1.out" || true
}
line() { sed -n "s/^$2 //p" "$W/$1.out"; }
names() { line "$1" "$2" | python3 -c "import json,sys; print(' '.join(o['name'] for o in json.load(sys.stdin)['objects']))"; }

# Conseil de résolution dans $W/$1.log pour $2 tuiles, la mémoire cohérente avec la résolution qu'il annonce (le texte
# exact est vérifié par export_test.c) ; l'affiche.
hint() {
	python3 - "$W/$1.log" "$2" <<'EOF'
import re, sys
log, tiles = open(sys.argv[1]).read(), int(sys.argv[2])
m = re.search(r"^UDIM: %d tiles, each with its own (\d+) px textures: ([\d.]+) (MB|GB) per (8|16|32) bit layer\.$" % tiles, log, re.M)
assert m, "no hint"
res, size, unit, bits = int(m.group(1)), float(m.group(2)), m.group(3), int(m.group(4))
want = tiles * res * res * 4 * bits // 8 * 3 / (1e9 if unit == "GB" else 1e6)
assert abs(size - want) <= (0.05 if unit == "GB" else 0.5), (size, want)
print(m.group(0))
EOF
}

# Uv du maillage fusionné (celui qu'on peint) : une ligne « uv <objet> <u min> <u max> <v min> <v max> <hors atlas>/<n> »
# par objet peint, dans l'ordre de la fusion. minic lit les i16_array_t en int32 : deux valeurs par case.
DUMP='int i16_at(i16_array_t *a, int j) {
	int w = a->buffer[j / 2];
	int h = j % 2 == 0 ? w & 65535 : (w >> 16) & 65535;
	return h >= 32768 ? h - 65536 : h;
}

i16_array_t *tex_of(object_t *o) {
	mesh_object_t *m = o->ext;
	any_array_t *vas = m->data->vertex_arrays;
	for (int i = 0; i < vas->length; ++i) {
		vertex_array_t *va = vas->buffer[i];
		if (string_equals(va->attrib, "tex")) {
			return va->values;
		}
	}
	return NULL;
}

void dump_uv(object_t *o, i16_array_t *tex, int off) {
	int n = tex_of(o)->length;
	int u0 = 99999; int u1 = -99999; int v0 = 99999; int v1 = -99999; int out = 0;
	for (int j = 0; j < n; j += 2) {
		int u = i16_at(tex, off + j);
		int v = i16_at(tex, off + j + 1);
		if (u == -32767 && v == -32767) { out++; }
		if (u < u0) { u0 = u; }
		if (u > u1) { u1 = u; }
		if (v < v0) { v0 = v; }
		if (v > v1) { v1 = v; }
	}
	console_log(string("t: uv %s %d %d %d %d %d/%d", o->name, u0, u1, v0, v1, out, n / 2));
}

void dump() {
	mesh_object_t *mo = context_main_object();
	object_t *main = mo->base;
	any_array_t *ch = main->children;
	object_t *merged = NULL;
	for (int i = 0; i < ch->length; ++i) {
		object_t *c = ch->buffer[i];
		if (string_index_of(c->name, "_merged") >= 0) {
			merged = c;
		}
	}
	i16_array_t *tex = tex_of(merged);
	dump_uv(main, tex, 0);
	int off = tex_of(main)->length;
	for (int i = 0; i < ch->length; ++i) {
		object_t *c = ch->buffer[i];
		if (c != merged) {
			dump_uv(c, tex, off);
			off += tex_of(c)->length;
		}
	}
}'

RED='slot_material_t *m = script_material_create("red");
	ui_node_t *rgb = script_material_create_node_at("RGB", -400.0, 0.0);
	ui_node_t *out = script_material_get_node("OUTPUT_MATERIAL_PBR");
	script_material_set_color(rgb, 0, 0, 1.0, 0.0, 0.0, 1.0);
	script_material_connect(rgb, 0, out, 0);
	script_material_update();
	script_material_set(m);
	context_select_tool(TOOL_TYPE_BRUSH);
	context_t *ctx = script_get_context();
	ctx->brush_radius = 0.5;
	ctx->brush_opacity = 1.0;
	ctx->brush_hardness = 0.8;
	script_paint_world(0.0, 0.0, 0.0);
	script_paint_world(0.05, 0.0, 0.05);
	script_paint_end();'

echo "== import UDIM (OBJ, FBX), refus, ajout"
script plain <<EOF
int main() {
	studio_import_mesh("$W/plain.obj", 0);
	console_log(string("t: box [%s]", studio_dialog_title()));
	return 0;
}
EOF
check "témoin : maillage sans uv, dépliage proposé" '[ "$(line plain box)" = "[Unwrap Mesh]" ]'

# obj à tiles1063 : imports refusés, tuiles hors de l'atlas, erreurs voulues.
RC_WANT=1
script obj <<EOF
$DUMP

int main() {
	studio_import_mesh("$W/udim.obj", 2);
	console_log(string("t: box [%s]", studio_dialog_title()));
	console_log(string("t: status [%s]", studio_status_message()));
	console_log(string("t: atlases %s", studio_atlases()));
	dump();
	studio_import_mesh("$W/neg.obj", 2);
	console_log(string("t: neg %s", studio_atlases()));
	studio_import_mesh("$W/many.obj", 2);
	console_log(string("t: many %s", studio_atlases()));
	script_append_mesh("$W/udim.obj");
	console_log(string("t: append %s", studio_atlases()));
	return 0;
}
EOF
check "OBJ : body.1001 body.1002 body.1011, cap sans tuile" '[ "$(names obj atlases)" = "body.1001 body.1002 body.1011 cap" ]'
check "OBJ : un triangle coupé, cap signalé" 'grep -q "UDIM import: 4 objects in 3 tiles, 1 triangles cut along the tile borders." "$W/obj.log" && grep -q "UDIM import: 1 objects without uvs imported without a tile, first \"cap\"." "$W/obj.log"'
check "OBJ : cap sans uv en tête du fichier, pas de dépliage proposé" '[ "$(line obj box)" = "[]" ]'
check "OBJ : cap sans case dans le maillage fusionné, signalé ; les tuiles chacune dans la sienne" '[ "$(line obj uv | grep -c " 0/")" = 3 ] && line obj uv | grep -q "^cap -32767 -32767 -32767 -32767 12/12$" && grep -q "UDIM: 1 objects without a tile, first \"cap\": the udim layers do not paint them." "$W/obj.log"'
check "OBJ refusé (uv négatives) : message, maillage gardé" 'grep -q "UDIM import refused: uvs outside the tiles 1001-1990 in \"body\" (1 triangles, first uv -0.5 0.2)." "$W/obj.log" && [ "$(names obj neg)" = "$(names obj atlases)" ]'
check "OBJ refusé (70 tuiles) : message, maillage gardé" 'grep -q "UDIM import refused: 70 tiles, the layers hold 64 at most." "$W/obj.log" && [ "$(names obj many)" = "$(names obj atlases)" ]'
check "ajout du même fichier : la tuile reste en suffixe (body.001.1001)" '[ "$(names obj append)" = "body.1001 body.1002 body.1011 cap body.001.1001 body.001.1002 body.001.1011 cap.001" ]'

script fbx <<EOF
int main() {
	studio_import_mesh("$W/udim.fbx", 2);
	console_log(string("t: box [%s]", studio_dialog_title()));
	console_log(string("t: atlases %s", studio_atlases()));
	studio_import_mesh("$W/neg.fbx", 2);
	console_log(string("t: neg %s", studio_atlases()));
	return 0;
}
EOF
check "FBX : body.1001 body.1002 body.1011, cap sans tuile, un triangle coupé" '[ "$(names fbx atlases)" = "body.1001 body.1002 body.1011 cap" ] && grep -q "UDIM import: 4 objects in 3 tiles, 1 triangles cut along the tile borders." "$W/fbx.log"'
check "FBX : cap sans uv en tête du fichier, pas de dépliage proposé" '[ "$(line fbx box)" = "[]" ]'
check "FBX refusé (uv négatives) : son message reste, maillage gardé" 'grep -q "UDIM import refused: uvs outside the tiles 1001-1990 in \"body\" (1 triangles, first uv -0.5 0.2)." "$W/fbx.log" && ! grep -q "Could not import file" "$W/fbx.log" && [ "$(names fbx neg)" = "$(names fbx atlases)" ]'

echo "== 70 tuiles : plafond des calques (64)"
mkdir -p "$W/out_1065" "$W/out_1063"
script tiles1065 <<EOF
$DUMP

int main() {
	studio_import_mesh("$W/center1065.obj", 0);
	console_log(string("t: status [%s]", studio_status_message()));
	dump();
	$RED
	studio_export_run("$W/out_1065", "", 3);
	console_log(string("t: studio %d", studio_export_run("$W/out_studio", "basic", 3)));
	return 0;
}
EOF
script tiles1063 <<EOF
int main() {
	studio_import_mesh("$W/center1063.obj", 0);
	$RED
	studio_export_run("$W/out_1063", "", 3);
	return 0;
}
EOF
check "message : 70 tuiles, les calques en tiennent 64, à partir de 1065" 'grep -q "UDIM: 70 tiles, the layers hold 64: the tiles from 1065 on are not painted nor exported." "$W/tiles1065.log"'
check "maillage fusionné (proxy du raytrace) : p.1001 ... p.1064 chacune dans sa case, p.1065 ... p.1070 sans case" 'line tiles1065 uv | python3 -c "
import sys
rows = [l.split() for l in sys.stdin]
assert [r[0] for r in rows] == [\"p.%d\" % t for t in range(1001, 1071)], [r[0] for r in rows][:3]
step = 32767 / 8.0
for k, (name, u0, u1, v0, v1, out) in enumerate(rows):
    u0, u1, v0, v1 = map(int, (u0, u1, v0, v1))
    if k < 64:
        cx, cy = k % 8, k // 8
        assert out.startswith(\"0/\") and cx * step <= u0 and u1 <= (cx + 1) * step and cy * step <= v0 and v1 <= (cy + 1) * step, (name, u0, u1, v0, v1)
    else:
        n = out.split(\"/\")[1]
        assert out == n + \"/\" + n and u0 == u1 == v0 == v1 == -32767, (name, out)
"'
check "export par tuile : 1001 à 1064, 5 maps chacune ; 1065 à 1070 écartées avec un message" '[ $(ls "$W/out_1065" | grep -cE "\.(10[0-5][0-9]|106[0-4])\.png$") -eq 320 ] && [ $(ls "$W/out_1065" | wc -l) -eq 320 ] && [ $(grep -c "UDIM export: tile 10[67][0-9] not exported, it is past the 64 tiles of the layers." "$W/tiles1065.log") -eq 6 ]'
python3 - "$W/out_1065" "$W/out_1063" > "$W/red.out" <<'EOF'
import glob, sys
from PIL import Image
for d in sys.argv[1:]:
    red = []
    for f in sorted(glob.glob(d + "/*_base.*.png")):
        im = Image.open(f).convert("RGB")
        if any(r > 180 and g < 80 and b < 80 for r, g, b in im.getdata()):
            red.append(f.rsplit(".", 2)[1])
    print(d.rsplit("_", 1)[1], " ".join(red))
EOF
check "peindre p.1065 (sans case) : aucune tuile touchée (l'upstream peignait la 1064)" '[ "$(line red 1065)" = "" ] && grep -q "^1065" "$W/red.out"'
check "témoin : peindre p.1063 peint la 1063, elle seule" '[ "$(line red 1063)" = "1063" ]'
check "preset studio : bloqué par le préflight, rien d'écrit" '[ "$(line tiles1065 studio)" = 0 ] && [ ! -e "$W/out_studio" ] && grep -q "6 UDIM tiles past the 64 of the layers, from 1065 on: they are not painted" "$W/tiles1065.log"'

echo "== conseil de résolution après l'import"
RC_WANT=0
script hint <<EOF
int main() {
	studio_import_mesh("$W/tiles4.obj", 2);
	console_log(string("t: status [%s]", studio_status_message()));
	return 0;
}
EOF
check "4 tuiles sans message de l'import : conseil dans la barre d'état" 'h=$(hint hint 4) && [ "$(line hint status)" = "[$h]" ]'
check "cap sans tuile : l'avertissement reste dans la barre d'état, le conseil dans la console" 'hint obj 3 > /dev/null && line obj status | grep -q "^\[UDIM: 1 objects without a tile, first \"cap\""'
check "70 tuiles : l'erreur reste dans la barre d'état, le conseil (64 tuiles) dans la console" 'hint tiles1065 64 > /dev/null && line tiles1065 status | grep -q "^\[UDIM: 70 tiles, the layers hold 64"'

echo "== uv matériau dans l'espace UDIM"
# Bruit sur les uv (UV Map > Noise Texture > couleur de base), rempli dans le calque courant ; export par tuile.
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
mkdir -p "$W/out_noise" "$W/out_noise1004"
script noise <<EOF
int main() {
	studio_import_mesh("$W/strip.obj", 2);
	console_log(string("t: atlases %s", studio_atlases()));
	$NOISE
	script_fill_layer();
	studio_export_run("$W/out_noise", "", 3);
	return 0;
}
EOF
script noise1004 <<EOF
int main() {
	studio_import_mesh("$W/strip.obj", 2);
	console_log(string("t: set %d", studio_layer_set_object(3)));
	$NOISE
	script_fill_layer();
	studio_export_run("$W/out_noise1004", "", 3);
	return 0;
}
EOF
check "5 tuiles pleines : strip.1001 ... strip.1004, strip.1011" '[ "$(names noise atlases)" = "strip.1001 strip.1002 strip.1003 strip.1004 strip.1011" ] && [ "$(line noise1004 set)" = 1 ]'
# Écart moyen (0-255) entre deux lignes de pixels, prises K pixels en retrait des bords (le filtrage de l'export mêle le
# dernier texel d'une case à sa voisine) ; D : même distance à l'intérieur d'une tuile, référence de la pente du bruit.
python3 - "$W/out_noise" "$W/out_noise1004" > "$W/seams.out" <<'EOF'
import glob, sys
from PIL import Image


def load(d, tile):
    f = glob.glob("%s/*_base.%d.png" % (d, tile))
    return Image.open(f[0]).convert("RGB") if f else None


shared, single = sys.argv[1], sys.argv[2]
im = {t: load(shared, t) for t in (1001, 1002, 1003, 1004, 1011)}
w, h = im[1001].size
K, D = 6, 13
col = lambda i, x: [i.getpixel((x, y)) for y in range(h)]
row = lambda i, y: [i.getpixel((x, y)) for x in range(w)]
gap = lambda a, b: sum(abs(p - q) for pa, pb in zip(a, b) for p, q in zip(pa, pb)) / (3.0 * len(a))
# Droite de 1003 (u = 3) contre gauche de 1004 (u = 3), témoins : gauche de 1001 (u = 0), colonne D plus à gauche
edge = col(im[1003], w - 1 - K)
print("right %.2f %.2f %.2f" % (gap(edge, col(im[1004], K)), gap(edge, col(im[1003], w - 1 - K - D)), gap(edge, col(im[1001], K))))
# Haut de 1001 (v = 1) contre bas de 1011 (v = 1), témoins : bas de 1002 (v = 0, u + 1), ligne D plus bas
edge = row(im[1001], K)
print("up %.2f %.2f %.2f" % (gap(edge, row(im[1011], h - 1 - K)), gap(edge, row(im[1001], K + D)), gap(edge, row(im[1002], h - 1 - K))))
# Calque limité à 1004 contre le calque de toutes les tuiles, témoin : sa 1003
one = load(single, 1004)
full = lambda i: list(i.getdata())[::7]
print("single %.2f %.2f" % (gap(full(one), full(im[1004])), gap(full(one), full(im[1003]))))
EOF
cat "$W/seams.out"
# Voisines : pas plus d'écart que deux pixels à la même distance dans une tuile (+2 d'arrondi) ; non voisines : au
# moins 5 fois plus (le banc voit une coupure) et plus de 10.
seam() { line seams "$1" | awk '{ exit !($1 <= 2 * $2 + 2 && $3 >= 5 * $1 && $3 > 10) }'; }
same() { line seams "$1" | awk '{ exit !($1 < 3 && $2 > 10) }'; }
check "bruit continu de 1003 à 1004, témoin 1001 coupé" 'seam right'
check "bruit continu de 1001 à 1011 (en haut), témoin 1002 coupé" 'seam up'
check "calque limité à 1004 : le même bruit que le calque de toutes les tuiles, témoin 1003 différent" 'same single'

echo "== découpage par matériau et par tuile"
script mats <<EOF
int main() {
	studio_import_mesh("$W/mats.obj", 3);
	console_log(string("t: obj %s", studio_atlases()));
	script_export_mesh("$W/mats_obj_out.obj");
	studio_import_mesh("$W/mats.fbx", 3);
	console_log(string("t: fbx %s", studio_atlases()));
	script_export_mesh("$W/mats_fbx_out.obj");
	studio_import_mesh("$W/mats.obj", 2);
	console_log(string("t: objects %s", studio_atlases()));
	return 0;
}
EOF
# Objets et leur jeu de textures : « skin.1001:skin ... »
sets() { line "$1" "$2" | python3 -c "import json,sys; j=json.load(sys.stdin); print(' '.join('%s:%s' % (o['name'], j['names'][o['atlas']]) for o in j['objects']))"; }
# Maillage exporté : chaque sommet à la position que son uv UDIM donne (x = u, y = v, z de son objet : 0 ou 4), à
# l'arrondi int16 près ; surfaces uv par matériau conservées (rien de perdu ni de doublé). « ok » ou les écarts.
python3 - "$W/mats.json" "$W/mats_obj_out.obj" "$W/mats_fbx_out.obj" > "$W/matspos.out" <<'EOF'
import json, sys
want = json.load(open(sys.argv[1]))
for path in sys.argv[2:]:
    objs, v, vt, cur, errs = {}, [], [], None, []
    for l in open(path):
        w = l.split()
        if w and w[0] == "o":
            cur = objs.setdefault(w[1], [])
        elif w and w[0] == "v":
            v.append(tuple(map(float, w[1:4])))
        elif w and w[0] == "vt":
            vt.append(tuple(map(float, w[1:3])))
        elif w and w[0] == "f":
            cur.append([(v[int(x.split("/")[0]) - 1], vt[int(x.split("/")[1]) - 1]) for x in w[1:4]])
    area = {}
    for name, tris in objs.items():
        mat, tile = name.rsplit(".", 1)
        tu, tv = (int(tile) - 1001) % 10, (int(tile) - 1001) // 10
        for t in tris:
            g = [(tu + u, tv + uv_v) for _, (u, uv_v) in t]
            z = set(round(p[2], 2) for p, _ in t)
            if any(abs(p[0] - q[0]) > 2e-3 or abs(p[1] - q[1]) > 2e-3 for (p, _), q in zip(t, g)) or len(z) != 1 or not z <= {0.0, 4.0}:
                errs.append("%s: %s pour les uv %s" % (name, [p for p, _ in t], g))
            area[mat] = area.get(mat, 0.0) + 0.5 * abs((g[1][0] - g[0][0]) * (g[2][1] - g[0][1]) - (g[2][0] - g[0][0]) * (g[1][1] - g[0][1]))
    errs += ["%s: surface uv %.5f, attendu %.5f" % (m, area.get(m, 0.0), a) for m, a in want.items() if abs(area.get(m, 0.0) - a) > 1e-4]
    print(path.rsplit("_", 2)[1], " ; ".join(errs[:3]) if errs else "ok")
EOF
cat "$W/matspos.out"
check "OBJ : cloth.1002 skin.1001 skin.1002 skin.1011, un jeu par matériau" '[ "$(sets mats obj)" = "cloth.1002:cloth skin.1001:skin skin.1002:skin skin.1011:skin" ]'
check "OBJ : 4 objets dans 3 tuiles, un triangle coupé" 'grep -q "UDIM import: 4 objects in 3 tiles, 1 triangles cut along the tile borders." "$W/mats.log"'
check "OBJ : positions recalées, surfaces conservées (export du maillage)" '[ "$(line matspos obj)" = ok ]'
check "FBX : les mêmes objets, un jeu par matériau" '[ "$(sets mats fbx)" = "cloth.1002:cloth skin.1001:skin skin.1002:skin skin.1011:skin" ]'
check "FBX : positions, surfaces (export du maillage)" '[ "$(line matspos fbx)" = ok ]'
check "témoin par objet et par tuile : body.1001 ... head.1011, sans jeu" '[ "$(sets mats objects)" = "body.1001:Atlas 1 body.1002:Atlas 1 head.1001:Atlas 1 head.1002:Atlas 1 head.1011:Atlas 1" ]'

echo "== export par jeu de textures et par tuile"
# Couleur unie (r, g, b) remplie dans le calque courant.
fill() {
	echo "slot_material_t *m$1$2$3 = script_material_create(\"fill\");
	ui_node_t *rgb$1$2$3 = script_material_create_node_at(\"RGB\", -400.0, 0.0);
	script_material_set_color(rgb$1$2$3, 0, 0, $1.0, $2.0, $3.0, 1.0);
	script_material_connect(rgb$1$2$3, 0, script_material_get_node(\"OUTPUT_MATERIAL_PBR\"), 0);
	script_material_update();
	script_material_set(m$1$2$3);
	script_fill_layer();"
}
mkdir -p "$W/out_sets" "$W/out_sets_object" "$W/out_sets_selected" "$W/out_sets_studio" "$W/out_shared" "$W/out_tile"
script setexp <<EOF
int main() {
	studio_import_mesh("$W/mats.obj", 3);
	char *j = studio_atlases();
	console_log(string("t: atlases %s", j));
	int skin = string_index_of(j, "{\"names\":[\"skin\"") == 0 ? 0 : 1;
	console_log(string("t: limit %d", studio_layer_set_atlas(skin)));
	$(fill 1 0 0)
	console_log(string("t: new %d", studio_layer_new()));
	console_log(string("t: cloth %d", studio_layer_set_atlas(1 - skin)));
	$(fill 0 0 1)
	studio_export_run("$W/out_sets", "", 3);
	studio_export_run("$W/out_sets_object", "", 2);
	studio_export_run("$W/out_sets_selected", "", 1);
	studio_export_set_naming("<atlas>_<channel>.<UDIM>.<ext>");
	console_log(string("t: studio %d", studio_export_run("$W/out_sets_studio", "basic", 3)));
	return 0;
}
EOF
script shared <<EOF
int main() {
	studio_import_mesh("$W/mats.obj", 3);
	$(fill 1 0 0)
	studio_export_run("$W/out_shared", "", 3);
	return 0;
}
EOF
script tilelayer <<EOF
int main() {
	studio_import_mesh("$W/mats.obj", 3);
	console_log(string("t: limit %d", studio_layer_set_object(1)));
	$(fill 1 0 0)
	studio_export_run("$W/out_tile", "", 3);
	return 0;
}
EOF
# Par dossier : « files » les jeux·tuiles des maps de base et le nombre de fichiers ; par fichier, « probe » la couleur
# du point témoin (uv dans la tuile, dans un triangle du jeu, hors de l'autre jeu : red, blue ou none), « reds » et
# « blues » 1 s'il a au moins un pixel rouge, bleu ; « same » 1 si cloth.1002 et skin.1002 sont identiques.
python3 - "$W" > "$W/setfiles.out" <<'EOF'
import glob, os, re, sys
from PIL import Image

w = sys.argv[1]
probe = {"skin.1001": (0.4, 0.4), "skin.1002": (0.1, 0.35), "skin.1011": (0.4, 0.4), "cloth.1002": (0.4, 0.4)}
red = lambda p: p[0] > 200 and p[1] < 60 and p[2] < 60
blue = lambda p: p[2] > 200 and p[0] < 60 and p[1] < 60
for d, pat in (("sets", r"_(skin|cloth)_base\.(\d{4})\.png$"), ("shared", r"_(skin|cloth)_base\.(\d{4})\.png$"),
               ("tile", r"_(skin|cloth)_base\.(\d{4})\.png$"), ("sets_object", r"_(skin|cloth)\.(\d{4})_base\.png$")):
    files = sorted(os.listdir("%s/out_%s" % (w, d)))
    found = {}
    for f in files:
        m = re.search(pat, f)
        if m:
            found["%s.%s" % m.groups()] = "%s/out_%s/%s" % (w, d, f)
    print("%s files %s %d" % (d, " ".join(sorted(found)), len(files)))
    probes, reds, blues = [], [], []
    for k in sorted(found):
        im = Image.open(found[k]).convert("RGB")
        u, v = probe.get(k, (0.5, 0.5))  # un fichier en trop : son centre
        at = im.getpixel((int(u * im.width), int((1 - v) * im.height)))
        probes.append("%s:%s" % (k, "red" if red(at) else "blue" if blue(at) else "none"))
        reds.append("%s:%d" % (k, any(red(p) for p in im.getdata())))
        blues.append("%s:%d" % (k, any(blue(p) for p in im.getdata())))
    print("%s probe %s" % (d, " ".join(probes)))
    print("%s reds %s" % (d, " ".join(reds)))
    print("%s blues %s" % (d, " ".join(blues)))
    if "cloth.1002" in found and "skin.1002" in found:
        same = list(Image.open(found["cloth.1002"]).getdata()) == list(Image.open(found["skin.1002"]).getdata())
        print("%s same %d" % (d, same))
print("selected %s" % " ".join(sorted(os.listdir(w + "/out_sets_selected"))))
print("studio %s" % " ".join(sorted(f for f in os.listdir(w + "/out_sets_studio") if "base_color" in f)))
print("studio_count %d" % len([f for f in os.listdir(w + "/out_sets_studio") if f.endswith(".exr")]))
EOF
cat "$W/setfiles.out"
check "jeux : les maps de skin sur 1001, 1002, 1011, de cloth sur 1002 seulement (4 × 5 fichiers)" '[ "$(line setfiles "sets files")" = "cloth.1002 skin.1001 skin.1002 skin.1011 20" ] && [ "$(line setexp limit) $(line setexp new) $(line setexp cloth)" = "1 1 1" ]'
check "calque de skin (rouge) dans skin seul, de cloth (bleu) dans cloth.1002 seul, uv qui se chevauchent dans la 1002" '[ "$(line setfiles "sets probe")" = "cloth.1002:blue skin.1001:red skin.1002:red skin.1011:red" ] && [ "$(line setfiles "sets reds")" = "cloth.1002:0 skin.1001:1 skin.1002:1 skin.1011:1" ] && [ "$(line setfiles "sets blues")" = "cloth.1002:1 skin.1001:0 skin.1002:0 skin.1011:0" ]'
check "calque partagé : dans les quatre, la même case pour cloth.1002 et skin.1002" '[ "$(line setfiles "shared files")" = "cloth.1002 skin.1001 skin.1002 skin.1011 20" ] && [ "$(line setfiles "shared probe")" = "cloth.1002:red skin.1001:red skin.1002:red skin.1011:red" ] && [ "$(line setfiles "shared same")" = 1 ]'
check "calque limité à skin.1001 : dans skin.1001 seul ; cloth sans calque, sans fichier" '[ "$(line setfiles "tile files")" = "skin.1001 skin.1002 skin.1011 15" ] && [ "$(line setfiles "tile probe")" = "skin.1001:red skin.1002:none skin.1011:none" ] && [ "$(line setfiles "tile reds")" = "skin.1001:1 skin.1002:0 skin.1011:0" ] && [ "$(line tilelayer limit)" = 1 ]'
check "preset studio, <atlas> et <UDIM> : skin_base_color.1001 ... cloth_base_color.1002, 9 maps × 4" '[ "$(line setexp studio)" = 1 ] && [ "$(line setfiles studio)" = "cloth_base_color.1002.exr skin_base_color.1001.exr skin_base_color.1002.exr skin_base_color.1011.exr" ] && [ "$(line setfiles studio_count)" = 36 ]'
check "par objet : un fichier par objet des jeux des calques, chacun avec le calque de son jeu" '[ "$(line setfiles "sets_object files")" = "cloth.1002 skin.1001 skin.1002 skin.1011 20" ] && [ "$(line setfiles "sets_object probe")" = "cloth.1002:blue skin.1001:red skin.1002:red skin.1011:red" ] && [ "$(line setfiles "sets_object reds")" = "cloth.1002:0 skin.1001:1 skin.1002:1 skin.1011:1" ]'
check "calque sélectionné limité au jeu cloth : nommé cloth, 5 maps" '[ "$(line setfiles selected | tr " " "\n" | grep -c "_cloth_")" -eq 5 ] && [ "$(line setfiles selected | wc -w)" -eq 5 ]'

echo "== renommer une tuile"
mkdir -p "$W/out_renamed"
script rename <<EOF
int main() {
	studio_import_mesh("$W/udim.obj", 2);
	console_log(string("t: cap %s", studio_object_rename(3, "cap.1005")));
	console_log(string("t: cap_status %s", studio_status_message()));
	studio_import_mesh("$W/mats.obj", 3);
	console_log(string("t: limit %d", studio_layer_set_object(3)));
	$(fill 1 0 0)
	console_log(string("t: torso %s", studio_object_rename(3, "torso")));
	console_log(string("t: robe %s", studio_object_rename(0, "robe.1005")));
	console_log(string("t: robe_status %s", studio_status_message()));
	console_log(string("t: empty %s", studio_object_rename(1, "")));
	console_log(string("t: same %s", studio_object_rename(2, "skin2.1002")));
	console_log(string("t: out [%s]", studio_object_rename(4, "x")));
	project_filepath_set("$W/renamed.arm");
	project_save(false);
	return 0;
}
EOF
script reopened "$W/renamed.arm" <<EOF
int main() {
	console_log(string("t: atlases %s", studio_atlases()));
	studio_export_run("$W/out_renamed", "", 3);
	return 0;
}
EOF
# Les jeux·tuiles des maps de base exportées, le nombre de fichiers, et par fichier 1 s'il a au moins un pixel rouge.
python3 - "$W/out_renamed" > "$W/renamedfiles.out" <<'EOF'
import os, re, sys
from PIL import Image

d = sys.argv[1]
files = sorted(os.listdir(d))
found = {}
for f in files:
    m = re.search(r"_(skin|cloth)_base\.(\d{4})\.png$", f)
    if m:
        found["%s.%s" % m.groups()] = os.path.join(d, f)
print("files %s %d" % (" ".join(sorted(found)), len(files)))
red = lambda p: p[0] > 200 and p[1] < 60 and p[2] < 60
print("reds %s" % " ".join("%s:%d" % (k, any(red(p) for p in Image.open(found[k]).convert("RGB").getdata())) for k in sorted(found)))
EOF
cat "$W/renamedfiles.out"
check "renommer : skin.1011 en torso donne torso.1011, un nom vide laisse skin.1001, skin2.1002 gardé" '[ "$(line rename torso) $(line rename empty) $(line rename same) $(line rename out) $(line rename limit)" = "torso.1011 skin.1001 skin2.1002 [] 1" ]'
check "suffixe tapé d'une autre tuile ignoré, avec un message ; un objet sans tuile n'en gagne pas" '[ "$(line rename robe) $(line rename cap)" = "robe.1002 cap" ] && line rename robe_status | grep -q "^robe.1005: the UDIM tile of an object comes from its import, a rename keeps it$" && line rename cap_status | grep -q "^cap.1005: the UDIM tile"'
check "projet rouvert : les noms, leurs jeux et leurs tuiles gardés" '[ "$(line reopened atlases | python3 -c "import json,sys; j=json.load(sys.stdin); print(\" \".join(\"%s:%s\" % (o[\"name\"], j[\"names\"][o[\"atlas\"]]) for o in j[\"objects\"]))")" = "robe.1002:cloth skin.1001:skin skin2.1002:skin torso.1011:skin" ]'
check "export par tuile après réouverture : la 1011 toujours là, le calque de torso.1011 dans skin.1011 seul (cloth sans calque, sans fichier)" '[ "$(line renamedfiles files)" = "skin.1001 skin.1002 skin.1011 15" ] && [ "$(line renamedfiles reds)" = "skin.1001:0 skin.1002:0 skin.1011:1" ]'

echo "== canaux d'un calque (layer) par tuile"
# Calque de toutes les tuiles : le bruit ; calque limité à strip.1004 : (0.25, 0.5, 0.75) opaque, au-dessus.
mkdir -p "$W/upresets"
cat > "$W/upresets/ulayers.json" <<'EOF'
{
	"version": 1,
	"defaults": { "format": "png", "bits": 8, "color_space": "raw" },
	"textures": [
		{ "name": "base", "channels": ["base_r", "base_g", "base_b"] },
		{ "name": "shared", "channels": ["layer_r('Layer 1')", "layer_g('Layer 1')", "layer_b('Layer 1')", "layer_a('Layer 1')"] },
		{ "name": "tile", "channels": ["layer_r('T1004')", "layer_g('T1004')", "layer_b('T1004')", "layer_a('T1004')"] }
	]
}
EOF
export STUDIO_EXPORT_PRESETS="$W/upresets"
script ulayers <<EOF
int main() {
	studio_import_mesh("$W/strip.obj", 2);
	$NOISE
	script_fill_layer();
	console_log(string("t: new %d", studio_layer_new()));
	console_log(string("t: set %d", studio_layer_set_object(3)));
	slot_material_t *flat = script_material_create("flat");
	ui_node_t *rgb = script_material_create_node_at("RGB", -400.0, 0.0);
	script_material_set_color(rgb, 0, 0, 0.25, 0.5, 0.75, 1.0);
	script_material_connect(rgb, 0, script_material_get_node("OUTPUT_MATERIAL_PBR"), 0);
	script_material_update();
	script_material_set(flat);
	script_fill_layer();
	console_log(string("t: rename %d", studio_layer_rename("T1004")));
	console_log(string("t: tiles %d", studio_export_run("$W/out_ltiles", "ulayers", 3)));
	console_log(string("t: visible %d", studio_export_run("$W/out_lvisible", "ulayers", 0)));
	console_log(string("t: object %d", studio_export_run("$W/out_lobject", "ulayers", 2)));
	return 0;
}
EOF
unset STUDIO_EXPORT_PRESETS
python3 - "$W" > "$W/ulayerscheck.out" <<'EOF'
import glob, os, sys
from PIL import Image
w = sys.argv[1]


def img(pattern):
    f = glob.glob(os.path.join(w, pattern))
    assert len(f) == 1, (pattern, f)
    return Image.open(f[0])


rgb = lambda i: i.convert("RGB").tobytes()


def over_black(layer, stack):
    """Écarts entre la pile aplatie et un calque RGBA brut composé sur du noir (la pile d'un calque seul) : pixels
    opaques différents, écart maximal sur les autres (bords des cases UDIM, alpha partiel), part de ces derniers."""
    lp, sp = list(layer.getdata()), list(stack.convert("RGB").getdata())
    diff, worst, partial = 0, 0, 0
    for (r, g, b, a), q in zip(lp, sp):
        if a == 255:
            diff += (r, g, b) != q
        elif a > 0:
            partial += 1
            worst = max(worst, max(abs(c * a / 255 - d) for c, d in zip((r, g, b), q)))
    return diff, worst, partial / len(lp)


ok = []
# Par tuile : la pile aplatie = le calque partagé composé sur du noir, partout sauf dans la 1004 où le calque de la
# tuile la couvre ; les canaux d'un calque sont ses valeurs brutes, alpha à part.
for t in (1001, 1002, 1003, 1004, 1011):
    base, shared, tile = img("out_ltiles/*_base.%d.png" % t), img("out_ltiles/*_shared.%d.png" % t), img("out_ltiles/*_tile.%d.png" % t)
    if t == 1004:
        a = tile.split()[3].histogram()
        assert a[255] > 0 and a[0] + a[255] == tile.width * tile.height, "alpha 1004"
        assert rgb(tile) == rgb(base), "tile 1004 != stack"
        d, worst, part = over_black(shared, img("out_noise/*_base.1004.png"))
        assert d == 0 and worst <= 1.5 and part < 0.01, ("shared 1004 != noise bench", d, worst, part)
        assert rgb(shared) != rgb(base)
    else:
        d, worst, part = over_black(shared, base)
        assert d == 0 and worst <= 1.5 and part < 0.01, ("shared %d != stack" % t, d, worst, part)
        assert tile.getextrema() == ((0, 0),) * 4, "tile layer in %d" % t
ok.append("tiles")
# Grille entière : le calque de la tuile dans sa case (1/9 de la grille 3 x 3), égal à la pile là où il est opaque ;
# ailleurs la pile = le calque partagé (bords des cases filtrés tolérés).
base, shared, tile = img("out_lvisible/*_base.png"), img("out_lvisible/*_shared.png"), img("out_lvisible/*_tile.png")
bp, sp, tp = list(base.convert("RGB").getdata()), list(shared.getdata()), list(tile.getdata())
n = len(tp)
opaque = [i for i in range(n) if tp[i][3] == 255]
empty = [i for i in range(n) if tp[i][3] == 0]
assert abs(len(opaque) / n - 1 / 9) < 0.01, len(opaque) / n
assert sum(tp[i][:3] != bp[i] for i in opaque) < 0.002 * n, "visible: tile != stack"
assert sum(sp[i][:3] != bp[i] for i in empty if sp[i][3] == 255) < 0.002 * n, "visible: shared != stack"
ok.append("visible")
# Par objet : l'upstream n'exporte que strip.1004 (le seul objet d'un calque limité), découpé comme la tuile 1004.
files = sorted(os.path.basename(f) for f in glob.glob(os.path.join(w, "out_lobject", "*.png")))
assert len(files) == 3 and all("strip.1004" in f for f in files), files
for c in ("base", "shared", "tile"):
    a = [f for f in files if "_%s" % c in f]
    assert len(a) == 1, (c, files)
    assert Image.open(os.path.join(w, "out_lobject", a[0])).tobytes() == img("out_ltiles/*_%s.1004.png" % c).tobytes(), c
ok.append("object")
print("ok " + " ".join(ok))
EOF
cat "$W/ulayerscheck.out"
check "calque ajouté, limité à strip.1004, renommé ; trois exports" '[ "$(line ulayers new) $(line ulayers set) $(line ulayers rename) $(line ulayers tiles) $(line ulayers visible) $(line ulayers object)" = "1 1 1 1 1 1" ]'
check "layer() par tuile : calque de toutes les tuiles lu par tuile, calque de la tuile dans la sienne seule" 'grep -q "^ok tiles" "$W/ulayerscheck.out"'
check "layer() grille entière : le calque de la tuile dans sa case du composite" 'grep -q "^ok tiles visible" "$W/ulayerscheck.out"'
check "layer() par objet : strip.1004 comme la tuile 1004" 'grep -q "^ok tiles visible object" "$W/ulayerscheck.out"'

[ $FAIL -eq 0 ] && echo "== OK" || echo "== FAIL"
exit $FAIL
