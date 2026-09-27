#!/bin/sh
# Projet UDIM au format atlas (lot D1, studio ebe635a5) : écrit strip.arm et, pour comparaison, son export par tuile
# (ref/). Banc de migration du lot D2 (udim_app.sh) : un build D2 ouvre ce projet et range chaque case dans sa tuile.
# Ne se relance qu'avec un build d'avant D2 (APP=...) ; gardé pour dire d'où viennent les fichiers.
#   strip.obj : 5 tuiles pleines, 1001 à 1004 et 1011 (grille 3 × 3 des calques), projet 256 px (85 px par case) ;
#               position monde (x, 0, z) = ((u - 2) / 2, 0, (v - 1) / 2)
#   Layer 1 : partagé, bruit sur les uv UDIM (couleur de base) ; son masque : noir, un trait du centre de 1002 à celui
#             de 1003 (il passe la couture), peint avec le bruit (un masque prend le rouge du matériau)
#   Layer 3 : limité à strip.1004 (calque d'une tuile, pleine résolution), couleur (0.25, 0.5, 0.75)
set -eu
D=$(cd "$(dirname "$0")" && pwd)
T=$(dirname "$(dirname "$D")")
APP=${APP:-$T/../../../build/build/Release/ArmorPaint.app/Contents/MacOS/ArmorPaint}
W=$(mktemp -d)
python3 - "$T" "$W" <<'EOF'
import sys
sys.path.insert(0, sys.argv[1])
from obj_udim import T  # noqa: E402


def tri(*uv):  # Position = uv recentré et réduit, pour tenir dans la vue par défaut (x de -1 à 1, z de -0,5 à 1,5)
    return T(*(((0.5 * (uv[k] - 2), 0.5 * (uv[k + 1] - 1), 0.0), (uv[k], uv[k + 1])) for k in (0, 2, 4)))


out, nv = ["o strip"], 0
for u, v in ((0, 0), (1, 0), (2, 0), (3, 0), (0, 1)):
    for t in (tri(u, v, u + 1, v, u, v + 1), tri(u + 1, v, u + 1, v + 1, u, v + 1)):
        for p, q in t:
            out += ["v %.6g %.6g %.6g" % p, "vt %.6g %.6g" % q]
        out.append("f " + " ".join("%d/%d" % (nv + k + 1, nv + k + 1) for k in range(3)))
        nv += 3
open(sys.argv[2] + "/strip.obj", "w").write("\n".join(out) + "\n")
EOF
cat > "$W/make.c" <<EOF
int main() {
	studio_project_res(256);
	studio_import_mesh("$W/strip.obj", 2);
	slot_material_t *m = script_material_create("noise");
	ui_node_t *uv = script_material_create_node_at("UVMAP", -800.0, 0.0);
	ui_node_t *noise = script_material_create_node_at("TEX_NOISE", -400.0, 0.0);
	ui_node_t *out = script_material_get_node("OUTPUT_MATERIAL_PBR");
	script_material_set_float(noise, 1, 1, 3.0);
	script_material_set_float(noise, 1, 2, 0.0);
	script_material_connect(uv, 0, noise, 0);
	script_material_connect(noise, 1, out, 0);
	script_material_update();
	script_material_set(m);
	script_fill_layer();
	console_log(string("t: mask %d", studio_layer_new_mask(0)));
	context_select_tool(TOOL_TYPE_BRUSH);
	context_t *ctx = script_get_context();
	ctx->brush_radius = 0.3;
	ctx->brush_opacity = 1.0;
	ctx->brush_hardness = 0.8;
	script_paint_world(-0.25, 0.0, -0.25);
	script_paint_world(0.25, 0.0, -0.25);
	script_paint_end();
	console_log(string("t: new %d", studio_layer_new()));
	console_log(string("t: set %d", studio_layer_set_object(4)));
	slot_material_t *flat = script_material_create("flat");
	ui_node_t *rgb = script_material_create_node_at("RGB", -400.0, 0.0);
	script_material_set_color(rgb, 0, 0, 0.25, 0.5, 0.75, 1.0);
	script_material_connect(rgb, 0, script_material_get_node("OUTPUT_MATERIAL_PBR"), 0);
	script_material_update();
	script_material_set(flat);
	script_fill_layer();
	project_filepath_set("$D/strip.arm");
	project_save(false);
	studio_export_run("$W/ref", "", 3);
	console_log(string("t: masks %s", studio_layer_masks()));
	return 0;
}
EOF
mkdir -p "$W/ref"
perl -e 'alarm 180; exec @ARGV' "$APP" --background --script "$W/make.c" -ApplePersistenceIgnoreState YES > "$W/make.log" 2>&1
grep '^t: ' "$W/make.log"
rm -rf "$D/ref" && mkdir -p "$D/ref" && cp "$W"/ref/*_base.*.png "$D/ref/"
ls -la "$D" "$D/ref"
