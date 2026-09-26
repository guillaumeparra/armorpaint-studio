#!/bin/sh
# UDIM dans ArmorPaint lui-même (--background), patches udim-import, fbx-split et udim-tile-limit (lot D0) :
#   1. import OBJ et FBX « Split By: UDIM Tile » : un objet par nœud et par tuile (body.1001...), triangle à cheval
#      coupé ; un nœud sans uv en tête du fichier est importé sans tuile, après les autres : pas de dépliage proposé
#      (il aurait écrasé les uv UDIM de toutes les tuiles) ; hors de l'atlas des calques, avec un message ;
#   2. fichier refusé (uv négatives, plus de 64 tuiles) : message, le projet garde son maillage ;
#   3. le même fichier ajouté : body.001.1001 (l'upstream renommait body.1001 en body.1003, une autre tuile) ;
#   4. 70 tuiles (import par objet de p.1001 ... p.1070) : message ; les 64 premières dans l'atlas, chacune dans sa case,
#      p.1065 à p.1070 hors de l'atlas ; peindre p.1065 ne touche aucune tuile (l'upstream peignait dans la case de la
#      1064), peindre p.1063 peint la 1063 ; export par tuile : 1001 à 1064 et un message par tuile écartée ; preset
#      studio bloqué par le préflight ;
#   5. conseil de résolution après un import UDIM (tuiles, grille, pixels par tuile, projet pour 2048 et 4096 px par
#      tuile) : dans la barre d'état si elle est libre ; sinon dans la console seule, l'avertissement (cap sans tuile) ou
#      l'erreur (70 tuiles) de l'import reste affiché.
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
import math, sys
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
script() {
	cat > "$W/$1.c"
	rc=0
	perl -e 'alarm 180; exec @ARGV' "$APP" --background --script "$W/$1.c" -ApplePersistenceIgnoreState YES > "$W/$1.log" 2>&1 || rc=$?
	[ $rc -eq 0 ] || ko "script $1 : ArmorPaint rc $rc (voir $W/$1.log)"
	grep '^t: ' "$W/$1.log" | sed 's/^t: //' > "$W/$1.out" || true
}
line() { sed -n "s/^$2 //p" "$W/$1.out"; }
names() { line "$1" "$2" | python3 -c "import json,sys; print(' '.join(o['name'] for o in json.load(sys.stdin)['objects']))"; }

# Conseil de résolution dans $W/$1.log pour $2 tuiles en grille $3 × $3, cohérent avec la résolution qu'il annonce (le
# texte exact est vérifié par export_test.c) ; l'affiche.
hint() {
	python3 - "$W/$1.log" "$2" "$3" <<'EOF'
import re, sys
log, tiles, stride = open(sys.argv[1]).read(), int(sys.argv[2]), int(sys.argv[3])
m = re.search(r"^UDIM: %d tiles, %dx%d grid in the (\d+) px layers: (\d+) px per tile\.(.*)$" % (tiles, stride, stride), log, re.M)
assert m, "no hint"
res, per, rest = int(m.group(1)), int(m.group(2)), m.group(3)
assert per == res // stride, (res, per)
for target in (2048, 4096):
    need = target * stride
    want = " For %d px per tile: " % target + ("project at %d px (" % need if need <= 16384 else "out of reach, %d px at most" % (16384 // stride))
    assert (want in rest) == (per < target), (want, rest)
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
check "OBJ : cap hors de l'atlas, signalé ; les tuiles dans l'atlas" '[ "$(line obj uv | grep -c " 0/")" = 3 ] && line obj uv | grep -q "^cap -32767 -32767 -32767 -32767 12/12$" && grep -q "UDIM: 1 objects without a tile, first \"cap\": the udim layers do not paint them." "$W/obj.log"'
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

echo "== 70 tuiles : plafond de l'atlas (64)"
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
check "message : 70 tuiles, 64 dans l'atlas, à partir de 1065" 'grep -q "UDIM: 70 tiles, the layers hold 64: the tiles from 1065 on are not painted nor exported." "$W/tiles1065.log"'
check "atlas : p.1001 ... p.1064 chacune dans sa case, p.1065 ... p.1070 hors de l'atlas" 'line tiles1065 uv | python3 -c "
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
script hint <<EOF
int main() {
	studio_import_mesh("$W/tiles4.obj", 2);
	console_log(string("t: status [%s]", studio_status_message()));
	return 0;
}
EOF
check "4 tuiles sans message de l'import : conseil dans la barre d'état" 'h=$(hint hint 4 2) && [ "$(line hint status)" = "[$h]" ]'
check "cap sans tuile : l'avertissement reste dans la barre d'état, le conseil dans la console" 'hint obj 3 2 > /dev/null && line obj status | grep -q "^\[UDIM: 1 objects without a tile, first \"cap\""'
check "70 tuiles : l'erreur reste dans la barre d'état, le conseil (64 tuiles) dans la console" 'hint tiles1065 64 8 > /dev/null && line tiles1065 status | grep -q "^\[UDIM: 70 tiles, the layers hold 64"'

[ $FAIL -eq 0 ] && echo "== OK" || echo "== FAIL"
exit $FAIL
