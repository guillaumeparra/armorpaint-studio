#!/bin/sh
# Export studio (lots B2, B4, B5) dans ArmorPaint lui-même (--background) : presets livrés et presets utilisateur
# (STUDIO_EXPORT_PRESETS), ligne de commande (--export-textures) et scripts (studio_export_run/preflight), égalité des
# pixels avec l'export upstream, manifeste (SHA-256), préflight bloquant sans rien écrire.
#
#   sh .tests/export_app.sh [dossier de travail]
#   APP=<binaire ArmorPaint> pour un autre build (défaut : build Release de paint/).
# Demande : python3 avec Pillow (comparaison des PNG), tiffinfo (libtiff).
set -eu
EXT=$(cd "$(dirname "$0")/.." && pwd)
APP=${APP:-$EXT/../../build/build/Release/ArmorPaint.app/Contents/MacOS/ArmorPaint}
W=${1:-$(mktemp -d)}
mkdir -p "$W"
W=$(cd "$W" && pwd)
[ -x "$APP" ] || { echo "ArmorPaint not found: $APP" >&2; exit 1; }
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
check "presets studio installés" 'grep -q "t: studio=1 generic=0" "$W/run.log"'

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
check "tiffinfo sans avertissement" '[ -z "$(tiffinfo -D "$W/out_deep/untitled/base.tif" "$W/out_deep/untitled/mask.tif" 2>&1 >/dev/null)" ]'

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
check "échec en cours d écriture : rien remplacé" 'grep -q "t: fail=0" "$W/fail.log" && grep -q "Texture export failed" "$W/fail.log" && [ "$(stat -f %Sm -t %Y "$W/out_raw/p_base.png")" = 2020 ] && [ ! -e "$W/out_raw/p_base.png.studio-tmp" ]'
rmdir "$W/out_raw/p_rough.png.studio-tmp"

chmod 755 "$W/ro"
[ $FAIL -eq 0 ] && echo "export app: OK ($W)" || { echo "export app: FAILED ($W)"; exit 1; }
