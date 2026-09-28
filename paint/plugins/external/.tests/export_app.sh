#!/bin/sh
# Export studio (lots B2, B4, B5) dans ArmorPaint lui-même (--background) : presets livrés et presets utilisateur
# (STUDIO_EXPORT_PRESETS), ligne de commande (--export-textures) et scripts (studio_export_run/preflight), égalité des
# pixels avec l'export upstream, manifeste (SHA-256), préflight bloquant sans rien écrire ; bits et résolution par map
# (clé "resolution", réglages de l'export), preset donné en JSON (sidecar du projet), et rien d'écrit dans data/.
#
#   sh .tests/export_app.sh [dossier de travail]
#   APP=<binaire ArmorPaint> pour un autre build (défaut : build Release de paint/).
# Demande : python3 avec Pillow (comparaison des PNG) ; tiffinfo (libtiff) s'il est là.
set -eu
EXT=$(cd "$(dirname "$0")/.." && pwd)
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

rm -rf "$W/presets" "$W/out_"* "$W/ro"
mkdir -p "$W/presets" "$W/ro"
chmod 555 "$W/ro"
cat > "$W/presets/rawpng.json" <<'EOF'
{
	"version": 1,
	"defaults": { "format": "png", "bits": 8, "color_space": "raw" },
	"textures": [
		{ "name": "base", "channels": ["base_r", "base_g", "base_b", "1.0"] },
		{ "name": "rough", "channels": ["rough", "rough", "rough", "1.0"] }
	]
}
EOF
cat > "$W/presets/deep.json" <<'EOF'
{
	"version": 1,
	"naming": "<file>/<channel>.<ext>",
	"overwrite": "block",
	"defaults": { "format": "tiff", "bits": 16, "compression": "lzw", "color_space": "raw" },
	"textures": [
		{ "name": "base", "channels": ["base_r", "base_g", "base_b"], "color_space": "srgb" },
		{ "name": "orm", "channels": ["occ", "rough", "metal"], "format": "png", "bits": 16, "compression": "best" },
		{ "name": "mask", "channels": ["clamp(1 - metal * 2, 0, 1)"], "format": "tiff", "bits": 32, "compression": "deflate" },
		{ "name": "preview", "channels": ["base_r", "base_g", "base_b"], "format": "jpg", "quality": 85 }
	]
}
EOF
export STUDIO_EXPORT_PRESETS="$W/presets"

# 1. Projet : un matériau uni (couleur, rugosité, métal) sur le calque de remplissage, puis exports par script.
cat > "$W/run.c" <<EOF
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
	console_log(string("t: arnold=%d", studio_export_run("$W/out_arnold", "arnold", 0)));
	console_log(string("t: rawpng=%d", studio_export_run("$W/out_raw", "rawpng", 0)));
	console_log(string("t: deep=%d", studio_export_run("$W/out_deep", "deep", 0)));
	console_log(string("t: deep_again=%d", studio_export_run("$W/out_deep", "deep", 0)));
	console_log(string("t: ro=%s", studio_export_preflight("$W/ro", "arnold", 0)));
	console_log(string("t: dry=%s", studio_export_preflight("$W/out_dry", "karma", 0)));
	console_log(string("t: studio=%d generic=%d", studio_export_is_studio_preset("cycles"), studio_export_is_studio_preset("generic")));
	project_filepath_set("$W/p.arm");
	project_save(false);
	return 0;
}
EOF
"$APP" --background --script "$W/run.c" > "$W/run.log" 2>&1 || true
grep -E "^t: |studio: (export|[0-9]+ studio)|Texture export|Textures exported" "$W/run.log" | cut -c1-300
check "arnold par script" 'grep -q "t: arnold=1" "$W/run.log"'
check "7 textures arnold" '[ $(ls "$W/out_arnold"/untitled_*.exr | wc -l) -eq 7 ]'
check "manifeste arnold" '[ -f "$W/out_arnold/untitled.export.json" ]'
check "aucun fichier temporaire" '[ -z "$(find "$W"/out_* -name "*.studio-tmp")" ]'
check "rawpng par script" 'grep -q "t: rawpng=1" "$W/run.log"'
check "deep : dossiers, tiff, png16, jpg" '[ -f "$W/out_deep/untitled/base.tif" ] && [ -f "$W/out_deep/untitled/orm.png" ] && [ -f "$W/out_deep/untitled/mask.tif" ] && [ -f "$W/out_deep/untitled/preview.jpg" ]'
check "deep : écrasement bloqué" 'grep -q "t: deep_again=0" "$W/run.log" && grep -q "Texture export blocked (deep)" "$W/run.log"'
check "préflight : dossier en lecture seule" 'grep "t: ro=" "$W/run.log" | grep -q "\"code\": *\"not_writable\""'
check "préflight seul : rien d écrit" 'grep "t: dry=" "$W/run.log" | grep -q "\"ok\": *true" && [ ! -e "$W/out_dry" ]'
check "presets studio reconnus" 'grep -q "t: studio=1 generic=0" "$W/run.log"'
DATA=$(dirname "$APP")/../Resources/out/data
check "presets studio seulement lus : rien d écrit dans data/" '[ ! -e "$DATA/studio_presets_generated.txt" ] && [ -z "$(ls "$DATA/export_presets" | grep -E "arnold|basic|cycles|karma|unreal_engine|rawpng|deep")" ]'

# 2. Manifeste : empreintes et tailles des fichiers écrits.
check "manifeste : SHA-256 et tailles" 'python3 - "$W/out_arnold" <<PY
import hashlib, json, os, sys
d = sys.argv[1]
m = json.load(open(os.path.join(d, "untitled.export.json")))
assert m["schema"] == "studio-texture-export/1" and m["preset"]["name"] == "arnold", m["preset"]
assert len(m["files"]) == 7
for f in m["files"]:
    b = open(os.path.join(d, f["path"]), "rb").read()
    assert hashlib.sha256(b).hexdigest() == f["sha256"], f["path"]
    assert len(b) == f["size"], f["path"]
PY'
if command -v tiffinfo > /dev/null; then
	check "tiffinfo sans avertissement" '[ -z "$(tiffinfo -D "$W/out_deep/untitled/base.tif" "$W/out_deep/untitled/mask.tif" 2>&1 >/dev/null)" ]'
else
	echo "skip tiffinfo absent (libtiff-tools) : TIFF non relus par libtiff"
fi

# 3. Ligne de commande : --export-textures avec un preset studio, puis l'export upstream (generic) pour comparer.
mkdir -p "$W/out_cli" "$W/out_generic" "$W/out_raw2"
"$APP" "$W/p.arm" --background --export-textures exr16 karma "$W/out_cli" > "$W/cli.log" 2>&1 || true
check "--export-textures karma" '[ $(ls "$W/out_cli"/p_*.exr 2>/dev/null | wc -l) -eq 7 ] && [ -f "$W/out_cli/p.export.json" ]'
"$APP" "$W/p.arm" --background --export-textures png generic "$W/out_generic" > "$W/generic.log" 2>&1 || true
"$APP" "$W/p.arm" --background --export-textures png rawpng "$W/out_raw2" > "$W/raw2.log" 2>&1 || true
check "raw 8 bits = generic upstream (base, rough)" 'python3 - "$W" <<PY
import sys
from PIL import Image
w = sys.argv[1]
for c in ("base", "rough"):
    a = Image.open(f"{w}/out_generic/p_{c}.png").convert("RGBA").tobytes()
    b = Image.open(f"{w}/out_raw2/p_{c}.png").convert("RGBA").tobytes()
    assert a == b, c
PY'

# 4. Atomicité : l'écriture de la 2e texture échoue (un dossier occupe son fichier temporaire) : la 1re, déjà écrite
#    en temporaire, n'est pas mise en place, et les fichiers existants restent intacts.
touch -t 202001010000 "$W/out_raw/p_base.png"
mkdir -p "$W/out_raw/p_rough.png.studio-tmp"
cat > "$W/fail.c" <<EOF
int main() {
	console_log(string("t: fail=%d", studio_export_run("$W/out_raw", "rawpng", 0)));
	return 0;
}
EOF
"$APP" "$W/p.arm" --background --script "$W/fail.c" > "$W/fail.log" 2>&1 || true
check "échec en cours d écriture : rien remplacé" 'grep -q "t: fail=0" "$W/fail.log" && grep -q "Texture export failed" "$W/fail.log" && [ "$(date -r "$W/out_raw/p_base.png" +%Y)" = 2020 ] && [ ! -e "$W/out_raw/p_base.png.studio-tmp" ]'
rmdir "$W/out_raw/p_rough.png.studio-tmp"

# 5. Bits et résolution par map : clé "resolution" du preset, réglages de l'export par-dessus, preset donné en JSON.
cat > "$W/presets/small.json" <<'EOF'
{
	"version": 1,
	"defaults": { "format": "png", "bits": 8, "color_space": "raw", "resolution": 256 },
	"textures": [
		{ "name": "base", "channels": ["base_r", "base_g", "base_b", "1.0"] },
		{ "name": "rough", "channels": ["rough", "rough", "rough", "1.0"], "resolution": 0 }
	]
}
EOF
cat > "$W/res.c" <<EOF
int main() {
	console_log(string("t: small=%d", studio_export_run("$W/out_small", "small", 0)));
	studio_export_set_preset("small", "", "");
	studio_export_set_overrides("{\"rough\": {\"bits\": 16, \"resolution\": 512}, \"base\": {\"resolution\": 0}}");
	console_log(string("t: over=%d", studio_export_run("$W/out_over", "small", 0)));
	studio_export_set_overrides("{\"base\": {\"bits\": 32}}");
	console_log(string("t: badbits=%d", studio_export_run("$W/out_badbits", "small", 0)));
	studio_export_set_overrides("");
	studio_export_set_preset("mine", "{\"textures\": [{\"name\": \"r\", \"channels\": [\"rough\"], \"format\": \"tiff\", \"bits\": 16, \"resolution\": 1024}]}", "$W/p.export_settings.json");
	console_log(string("t: inline=%d", studio_export_run("$W/out_inline", "mine", 0)));
	console_log(string("t: inline_studio=%d", studio_export_is_studio_preset("mine")));
	return 0;
}
EOF
"$APP" "$W/p.arm" --background --script "$W/res.c" > "$W/res.log" 2>&1 || true
grep -E "^t: |Texture export" "$W/res.log" | cut -c1-300
check "résolution du preset : base 256, rough à celle du projet" 'grep -q "t: small=1" "$W/res.log" && python3 - "$W" <<PY
import sys
from PIL import Image
w = sys.argv[1]
assert Image.open(f"{w}/out_small/p_base.png").size == (256, 256)
big = Image.open(f"{w}/out_raw2/p_base.png").size
assert Image.open(f"{w}/out_small/p_rough.png").size == big, big
PY'
check "réduction fidèle : matériau uni, mêmes valeurs à 256" 'python3 - "$W" <<PY
import sys
from PIL import Image
w = sys.argv[1]
a = Image.open(f"{w}/out_raw2/p_base.png").convert("RGBA").getpixel((10, 10))
b = Image.open(f"{w}/out_small/p_base.png").convert("RGBA").getpixel((10, 10))
assert all(abs(x - y) <= 1 for x, y in zip(a, b)), (a, b)
PY'
check "réglages de l export : rough 512 en 16 bits, base à la taille du projet" 'grep -q "t: over=1" "$W/res.log" && python3 - "$W" <<PY
import json, sys
from PIL import Image
w = sys.argv[1]
import struct
ihdr = struct.unpack(">IIB", open(f"{w}/out_over/p_rough.png", "rb").read(25)[16:25])
assert ihdr == (512, 512, 16), ihdr  # PIL lit le RGBA 16 bits en 8 bits : l en-tête PNG fait foi
assert Image.open(f"{w}/out_over/p_base.png").size == Image.open(f"{w}/out_raw2/p_base.png").size
m = json.load(open(f"{w}/out_over/p.export.json"))
files = {f["texture"]: f for f in m["files"]}
assert files["rough"]["resolution"] == [512, 512] and files["rough"]["bits"] == 16, files["rough"]
assert m["preset"]["overrides"] == {"rough": {"bits": 16, "resolution": 512}, "base": {"resolution": 0}}, m["preset"]
PY'
check "réglage refusé (32 bits en PNG) : rien d écrit" 'grep -q "t: badbits=0" "$W/res.log" && grep -q "32 bits not supported in png" "$W/res.log" && [ ! -e "$W/out_badbits" ]'
check "preset donné en JSON : exporté, source au manifeste" 'grep -q "t: inline=1" "$W/res.log" && grep -q "t: inline_studio=1" "$W/res.log" && python3 - "$W" <<PY
import json, sys
w = sys.argv[1]
m = json.load(open(f"{w}/out_inline/p.export.json"))
assert m["preset"]["name"] == "mine" and m["preset"]["path"].endswith("p.export_settings.json"), m["preset"]
assert m["files"][0]["resolution"] == [1024, 1024] and m["files"][0]["path"].endswith(".tif"), m["files"]
PY'

# 6. Canaux d'un calque (lot B4) : layer('<nom>') lit le calque lui-même, pas la pile aplatie. Un calque « AO bake »
#    opaque (0.25, 0.5, 0.75) sur le calque orange : ses canaux = la pile aplatie ; caché, il reste lu et la pile
#    redevient orange = layer_*('Layer 1'). Calque absent, homonyme, groupe : préflight bloquant, rien d'écrit.
cat > "$W/presets/layers.json" <<'EOF'
{
	"version": 1,
	"defaults": { "format": "png", "bits": 8, "color_space": "raw" },
	"textures": [
		{ "name": "base", "channels": ["base_r", "base_g", "base_b", "1.0"] },
		{ "name": "ao", "channels": ["layer_r('AO bake')", "layer_g('AO bake')", "layer_b('AO bake')", "layer_a('AO bake')"] },
		{ "name": "gray", "channels": ["layer(\"AO bake\")"] },
		{ "name": "under", "channels": ["layer_r('Layer 1')", "layer_g('Layer 1')", "layer_b('Layer 1')", "1.0"] },
		{ "name": "mix", "channels": ["clamp(layer_b('AO bake') - layer('AO bake'), 0, 1)"], "bits": 16 }
	]
}
EOF
for n in missing:Nope ambiguous:Layer\ 1 group:Grp; do
	printf '{ "version": 1, "textures": [ { "name": "m", "format": "png", "channels": ["layer('"'"'%s'"'"')"] } ] }\n' \
		"${n#*:}" > "$W/presets/l${n%%:*}.json"
done
cat > "$W/layers.c" <<EOF
int main() {
	script_project_new();
	slot_material_t *m = script_material_create("mat");
	ui_node_t *rgb = script_material_create_node_at("RGB", -400.0, 0.0);
	script_material_set_color(rgb, 0, 0, 0.8, 0.35, 0.1, 1.0);
	script_material_connect(rgb, 0, script_material_get_node("OUTPUT_MATERIAL_PBR"), 0);
	script_material_update();
	script_material_set(m);
	script_fill_layer();
	console_log(string("t: new=%d", studio_layer_new()));
	slot_material_t *ao = script_material_create("ao");
	ui_node_t *grey = script_material_create_node_at("RGB", -400.0, 0.0);
	script_material_set_color(grey, 0, 0, 0.25, 0.5, 0.75, 1.0);
	script_material_connect(grey, 0, script_material_get_node("OUTPUT_MATERIAL_PBR"), 0);
	script_material_update();
	script_material_set(ao);
	script_fill_layer();
	console_log(string("t: rename=%d", studio_layer_rename("AO bake")));
	console_log(string("t: shown=%d", studio_export_run("$W/out_layers", "layers", 0)));
	console_log(string("t: hide=%d", studio_layer_set_visible(0)));
	console_log(string("t: hidden=%d", studio_export_run("$W/out_hidden", "layers", 0)));
	console_log(string("t: hidden_pf=%s", studio_export_preflight("$W/out_hidden", "layers", 0)));
	console_log(string("t: missing=%d", studio_export_run("$W/out_lmissing", "lmissing", 0)));
	console_log(string("t: missing_pf=%s", studio_export_preflight("$W/out_lmissing", "lmissing", 0)));
	studio_layer_rename("Layer 1");
	console_log(string("t: ambiguous_pf=%s", studio_export_preflight("$W/out_lambiguous", "lambiguous", 0)));
	studio_layer_rename("AO bake");
	console_log(string("t: group=%d", studio_layer_new_group()));
	studio_layer_rename("Grp");
	console_log(string("t: group_pf=%s", studio_export_preflight("$W/out_lgroup", "lgroup", 0)));
	studio_export_set_preset("bad", "{\"textures\": [{\"name\": \"b\", \"channels\": [\"layer(AO bake)\"]}]}", "$W/l.export_settings.json");
	console_log(string("t: bad=%d", studio_export_run("$W/out_lbad", "bad", 0)));
	return 0;
}
EOF
"$APP" --background --script "$W/layers.c" > "$W/layers.log" 2>&1 || true
grep -E "^t: |Texture export|layer" "$W/layers.log" | cut -c1-300
check "calque ajouté, renommé, caché" 'grep -q "t: new=1" "$W/layers.log" && grep -q "t: rename=1" "$W/layers.log" && grep -q "t: hide=1" "$W/layers.log"'
check "canaux du calque du dessus = pile aplatie" 'grep -q "t: shown=1" "$W/layers.log" && python3 - "$W/out_layers" <<PY
import sys
from PIL import Image
d = sys.argv[1]
base = Image.open(f"{d}/untitled_base.png").convert("RGBA")
ao = Image.open(f"{d}/untitled_ao.png")
assert ao.mode == "RGBA", ao.mode
assert [c.tobytes() for c in base.split()[:3]] == [c.tobytes() for c in ao.split()[:3]]
alpha = ao.split()[3].histogram()
assert alpha[255] > 0 and alpha[0] + alpha[255] == ao.width * ao.height, "alpha"  # opaque sur les îlots UV
assert ao.getpixel((10, 10)) == (64, 128, 191, 255), ao.getpixel((10, 10))  # 0.25, 0.5, 0.75 tels quels
gray = Image.open(f"{d}/untitled_gray.png")
assert gray.mode == "L" and gray.tobytes() == ao.split()[0].tobytes(), gray.mode
under = Image.open(f"{d}/untitled_under.png").convert("RGB").getpixel((10, 10))
assert under[0] > under[1] > under[2], under  # orange, sous le calque du dessus
PY'
check "calque caché : toujours lu, la pile redevient orange" 'grep -q "t: hidden=1" "$W/layers.log" && ! grep "t: hidden_pf=" "$W/layers.log" | grep -q hidden_layers && grep "t: missing_pf=" "$W/layers.log" | grep -q hidden_layers && python3 - "$W" <<PY
import sys
from PIL import Image
w = sys.argv[1]
a = Image.open(f"{w}/out_layers/untitled_ao.png").tobytes()
assert Image.open(f"{w}/out_hidden/untitled_ao.png").tobytes() == a
base = Image.open(f"{w}/out_hidden/untitled_base.png").convert("RGB").tobytes()
assert base == Image.open(f"{w}/out_hidden/untitled_under.png").convert("RGB").tobytes()
assert base == Image.open(f"{w}/out_layers/untitled_under.png").convert("RGB").tobytes()
PY'
check "expression sur des canaux de calque (16 bits)" 'python3 - "$W/out_layers" <<PY
import struct, sys
from PIL import Image
d = sys.argv[1]
ihdr = struct.unpack(">IIBB", open(f"{d}/untitled_mix.png", "rb").read(26)[16:26])
assert ihdr[2:] == (16, 0), ihdr  # gris 16 bits
r, g, b, _ = Image.open(f"{d}/untitled_ao.png").getpixel((10, 10))
v = Image.open(f"{d}/untitled_mix.png").getpixel((10, 10)) / 65535
assert abs(v - (b - r) / 255) < 2 / 255, (v, r, b)
PY'
check "calque absent : bloquant, rien d écrit" 'grep -q "t: missing=0" "$W/layers.log" && grep "t: missing_pf=" "$W/layers.log" | grep -q "\"code\": *\"layer_missing\"" && [ ! -e "$W/out_lmissing" ]'
check "calques homonymes : bloquant" 'grep "t: ambiguous_pf=" "$W/layers.log" | grep -q "\"code\": *\"layer_ambiguous\"" && [ ! -e "$W/out_lambiguous" ]'
check "groupe de calques : bloquant" 'grep -q "t: group=1" "$W/layers.log" && grep "t: group_pf=" "$W/layers.log" | grep -q "\"code\": *\"layer_group\""'
check "nom sans guillemets : preset refusé" 'grep -q "t: bad=0" "$W/layers.log" && grep -q "takes a layer name in quotes" "$W/layers.log" && [ ! -e "$W/out_lbad" ]'

chmod 755 "$W/ro"
[ $FAIL -eq 0 ] && echo "export app: OK ($W)" || { echo "export app: FAILED ($W)"; exit 1; }
