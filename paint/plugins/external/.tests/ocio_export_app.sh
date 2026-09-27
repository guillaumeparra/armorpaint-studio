#!/bin/sh
# Lot A3 dans ArmorPaint lui-même (--background) : export studio avec des color_space OCIO. Les pixels exportés
# (EXR 32 et half, TIFF 16, PNG 8, 1 et 4 canaux, alias, espace de donnée) sont comparés à PyOpenColorIO
# (ocio_export_check.py), le manifeste et le préflight vérifiés, puis les cas bloquants : espace inconnu, OCIO
# indisponible, configuration sans espace de travail reconnaissable (STUDIO_OCIO_WORKING_SPACE).
#
#   sh .tests/ocio_export_app.sh [dossier de travail]
#   APP=<binaire ArmorPaint> pour un autre build (défaut : build Release de paint/).
# Demande .ocio/venv (créé par ocio.sh) avec numpy, OpenEXR, tifffile et Pillow :
#   .ocio/venv/bin/pip install numpy OpenEXR tifffile pillow
set -eu
T=$(cd "$(dirname "$0")" && pwd)
EXT=$(dirname "$T")
APP=${APP:-$EXT/../../build/build/Release/ArmorPaint.app/Contents/MacOS/ArmorPaint}
PY=$EXT/.ocio/venv/bin/python
W=${1:-$(mktemp -d)}
mkdir -p "$W"
W=$(cd "$W" && pwd)
[ -x "$APP" ] || { echo "ArmorPaint not found: $APP" >&2; exit 1; }
export STUDIO_ENV= # lot E5 : aucun studio.env, pas même celui de l'utilisateur
"$PY" -c 'import PyOpenColorIO, numpy, OpenEXR, tifffile, PIL' 2> /dev/null ||
	{ echo "venv incomplet : lancer .tests/ocio.sh puis $EXT/.ocio/venv/bin/pip install numpy OpenEXR tifffile pillow" >&2; exit 1; }
FAIL=0
ok() { echo "ok   $1"; }
ko() { echo "FAIL $1"; FAIL=1; }
check() { if eval "$2"; then ok "$1"; else ko "$1"; fi; }
# console_log tronque les longues lignes : le script minic cherche lui-même les codes dans le rapport JSON.
# codes <variable> <code>… : lignes minic « t: code <code>=<position ou -1> » ; code <log> <code> : présent.
codes() {
	v=$1
	shift
	for c in "$@"; do
		printf '\tconsole_log(string("t: code %s=%%d", string_index_of(%s, "\\"%s\\"")));\n' "$c" "$v" "$c"
	done
}
code() { grep -q "^t: code $2=[0-9]" "$1"; }
app() { perl -e 'alarm 120; exec @ARGV' "$APP" "$@" -ApplePersistenceIgnoreState YES; }
# Commande de contrôle, sortie indentée ; rend son propre code (dans un pipe vers sed, le code serait celui de sed).
indented() { "$@" > "$W/.indented" 2>&1 && r=0 || r=$?; sed 's/^/     /' "$W/.indented"; return $r; }
unset OCIO STUDIO_OCIO_WORKING_SPACE STUDIO_OCIO_LIB

rm -rf "$W/presets" "$W/out_"* "$W/cfg"
mkdir -p "$W/presets" "$W/cfg"
"$PY" "$T/ocio_ref.py" --make-config "$W/cfg"
# src : les valeurs d'ArmorPaint telles quelles, la référence de toutes les autres textures.
cat > "$W/presets/ocio.json" << 'EOF'
{
	"version": 1,
	"defaults": { "format": "exr", "bits": 32, "color_space": "ACEScg" },
	"textures": [
		{ "name": "src", "channels": ["base_r", "base_g", "base_b", "rough"], "color_space": "raw" },
		{ "name": "opac", "channels": ["opac"], "color_space": "raw" },
		{ "name": "acescg", "channels": ["base_r", "base_g", "base_b"] },
		{ "name": "acescct", "channels": ["base_r", "base_g", "base_b", "opac"], "color_space": "ACEScct" },
		{ "name": "half", "channels": ["base_r", "base_g", "base_b"], "bits": 16, "color_space": "lin_ap1_scene" },
		{ "name": "srgb16", "channels": ["base_r", "base_g", "base_b"], "format": "tiff", "bits": 16, "compression": "none", "color_space": "sRGB - Texture" },
		{ "name": "srgb8", "channels": ["base_r", "base_g", "base_b"], "format": "png", "bits": 8, "color_space": "srgb_tx" },
		{ "name": "grey", "channels": ["rough"] },
		{ "name": "rawdata", "channels": ["rough"], "color_space": "Raw" },
		{ "name": "legacy", "channels": ["base_r", "base_g", "base_b"], "color_space": "linear" },
		{ "name": "working", "channels": ["base_r", "base_g", "base_b"], "color_space": "Linear Rec.709 (sRGB)" },
		{ "name": "colordata", "channels": ["base_r", "base_g", "base_b"], "color_space": "Raw" },
		{ "name": "lin8", "channels": ["base_r", "base_g", "base_b"], "format": "png", "bits": 8 }
	]
}
EOF
cat > "$W/presets/bad.json" << 'EOF'
{
	"version": 1,
	"textures": [ { "name": "base", "channels": ["base_r", "base_g", "base_b"], "format": "exr", "bits": 32, "color_space": "ACEScg_typo" } ]
}
EOF
# mix : couleurs en linear, ACEScg et srgb, données en raw et Raw ; mixref : le même, couleurs en Linear P3-D65 (lot B6).
cat > "$W/presets/mix.json" << 'EOF'
{
	"version": 1,
	"defaults": { "format": "exr", "bits": 32 },
	"textures": [
		{ "name": "base", "channels": ["base_r", "base_g", "base_b"], "color_space": "linear" },
		{ "name": "cg", "channels": ["base_r", "base_g", "base_b", "opac"], "color_space": "ACEScg" },
		{ "name": "tex", "channels": ["base_r", "base_g", "base_b"], "format": "tiff", "bits": 16, "compression": "none", "color_space": "srgb" },
		{ "name": "rough", "channels": ["rough"], "color_space": "raw" },
		{ "name": "data", "channels": ["base_r", "base_g", "base_b"], "color_space": "Raw" }
	]
}
EOF
sed -e 's/"color_space": "linear"/"color_space": "Linear P3-D65"/' -e 's/"color_space": "ACEScg"/"color_space": "Linear P3-D65"/' \
	-e 's/"color_space": "srgb"/"color_space": "Linear P3-D65"/' "$W/presets/mix.json" > "$W/presets/mixref.json"
cat > "$W/presets/tst.json" << 'EOF'
{
	"version": 1,
	"textures": [ { "name": "base", "channels": ["base_r", "base_g", "base_b"], "format": "exr", "bits": 32, "color_space": "srgb_tex" } ]
}
EOF
export STUDIO_EXPORT_PRESETS="$W/presets"

# 1. Projet : une texture magique (couleur de base et rugosité) et du bruit (opacité), des milliers de valeurs.
cat > "$W/run.c" << EOF
int main() {
	script_project_new();
	slot_material_t *m = script_material_create("mat");
	ui_node_t *magic = script_material_create_node_at("TEX_MAGIC", -400.0, 0.0);
	ui_node_t *noise = script_material_create_node_at("TEX_NOISE", -400.0, 300.0);
	ui_node_t *out = script_material_get_node("OUTPUT_MATERIAL_PBR");
	script_material_connect(magic, 0, out, 0);
	script_material_connect(noise, 0, out, 1);
	script_material_connect(magic, 1, out, 3);
	script_material_update();
	script_material_set(m);
	script_fill_layer();
	char *pre = studio_export_preflight("$W/out_ocio", "ocio", 0);
$(codes pre ocio_data_as_color ocio_color_as_data exr_not_linear linear_8bit)
	console_log(string("t: srgb8_8bit=%d", string_index_of(pre, "srgb8: scene-linear")));
	console_log(string("t: ocio=%d", studio_export_run("$W/out_ocio", "ocio", 0)));
	char *bad = studio_export_preflight("$W/out_bad", "bad", 0);
$(codes bad ocio_unknown_colorspace)
	console_log(string("t: bad=%d", studio_export_run("$W/out_bad", "bad", 0)));
	console_log(string("t: working=%s", studio_ocio_working_space()));
	project_filepath_set("$W/p.arm");
	project_save(false);
	return 0;
}
EOF
app --background --script "$W/run.c" > "$W/run.log" 2>&1 || true
grep -E "^t: (ocio|bad|working)=|^studio: OCIO|Texture export" "$W/run.log" | cut -c1-300
check "export OCIO par script" 'grep -q "^t: ocio=1" "$W/run.log"'
check "espace de travail : Linear Rec.709 (sRGB)" 'grep -qx "t: working=Linear Rec.709 (sRGB)" "$W/run.log"'
check "préflight : donnée vers couleur (grey)" 'code "$W/run.log" ocio_data_as_color'
check "préflight : couleur écrite en donnée (colordata)" 'code "$W/run.log" ocio_color_as_data'
check "préflight : EXR non linéaire (ACEScct)" 'code "$W/run.log" exr_not_linear'
check "préflight : linéaire en 8 bits (lin8)" 'code "$W/run.log" linear_8bit'
check "préflight : pas d avertissement pour srgb_tx en 8 bits" 'grep -q "^t: srgb8_8bit=-1" "$W/run.log"'
check "espace inconnu : bloquant, rien d écrit" 'code "$W/run.log" ocio_unknown_colorspace && grep -q "^t: bad=0" "$W/run.log" && [ ! -e "$W/out_bad" ]'
echo "-- pixels et manifeste (PyOpenColorIO)"
if indented "$PY" "$T/ocio_export_check.py" "$W/out_ocio" untitled; then
	ok "pixels et manifeste = PyOpenColorIO"
else
	ko "pixels et manifeste = PyOpenColorIO"
fi

# Sous-shells : en sh, une affectation devant l'appel d'une fonction survit à l'appel.
# 2. OCIO indisponible ($OCIO invalide) : l'app démarre, l'export OCIO est bloqué ; un preset sans OCIO passe.
cat > "$W/unavail.c" << EOF
int main() {
	char *pre = studio_export_preflight("$W/out_unavail", "ocio", 0);
$(codes pre ocio_unavailable)
	console_log(string("t: run=%d", studio_export_run("$W/out_unavail", "ocio", 0)));
	console_log(string("t: arnold=%d", studio_export_run("$W/out_arnold", "arnold", 0)));
	return 0;
}
EOF
(OCIO=$W/missing.ocio app "$W/p.arm" --background --script "$W/unavail.c" > "$W/unavail.log" 2>&1 || true)
check "OCIO indisponible : bloquant, rien d écrit" 'code "$W/unavail.log" ocio_unavailable && grep -q "^t: run=0" "$W/unavail.log" && [ ! -e "$W/out_unavail" ]'
check "OCIO indisponible : preset sans OCIO exporté" 'grep -q "^t: arnold=1" "$W/unavail.log" && grep -q "\"ocio\": null" "$W/out_arnold/p.export.json"'

# 3. Configuration sans rôles d'échange : pas d'espace de travail reconnu, bloquant ; STUDIO_OCIO_WORKING_SPACE le donne.
cat > "$W/tst.c" << EOF
int main() {
	char *pre = studio_export_preflight("$W/out_tst", "tst", 0);
$(codes pre ocio_no_working_space)
	console_log(string("t: run=%d", studio_export_run("$W/out_tst", "tst", 0)));
	return 0;
}
EOF
(OCIO=$W/cfg/config.ocio app "$W/p.arm" --background --script "$W/tst.c" > "$W/tst.log" 2>&1 || true)
check "sans espace de travail : bloquant, rien d écrit" 'code "$W/tst.log" ocio_no_working_space && grep -q "set STUDIO_OCIO_WORKING_SPACE" "$W/tst.log" && grep -q "^t: run=0" "$W/tst.log" && [ ! -e "$W/out_tst" ]'
(OCIO=$W/cfg/config.ocio STUDIO_OCIO_WORKING_SPACE=scene_linear app "$W/p.arm" --background --script "$W/tst.c" > "$W/tst2.log" 2>&1 || true)
check "STUDIO_OCIO_WORKING_SPACE=scene_linear : exporté" 'grep -q "^t: run=1" "$W/tst2.log" && "$PY" - "$W/out_tst/p.export.json" << PY
import json, sys
m = json.load(open(sys.argv[1]))
assert m["ocio"]["config"] == "studio_test" and m["ocio"]["working_space"] == "lin", m["ocio"]
assert m["files"][0]["ocio"]["from"] == "lin" and m["files"][0]["ocio"]["colorspace"] == "srgb_tex", m["files"][0]
PY'

# 4. Lot B6 : espace imposé aux textures de couleur, par script puis par STUDIO_EXPORT_COLOR_SPACE (ligne de commande).
cat > "$W/ov.c" << EOF
int main() {
	console_log(string("t: set=%d", studio_export_set_color_space("Linear P3-D65")));
	console_log(string("t: cs=[%s]", studio_export_color_space()));
	char *pre = studio_export_preflight("$W/out_ov", "mix", 0);
	console_log(string("t: report=%d", string_index_of(pre, "\"color_space_override\": \"Linear P3-D65\"")));
	console_log(string("t: ov=%d", studio_export_run("$W/out_ov", "mix", 0)));
	console_log(string("t: set bad=%d", studio_export_set_color_space("nope")));
	char *bad = studio_export_preflight("$W/out_ovbad", "mix", 0);
$(codes bad ocio_unknown_colorspace)
	console_log(string("t: ovbad=%d", studio_export_run("$W/out_ovbad", "mix", 0)));
	console_log(string("t: reset=%d", studio_export_set_color_space("")));
	console_log(string("t: cs0=[%s]", studio_export_color_space()));
	char *ref = studio_export_preflight("$W/out_ref", "mixref", 0);
	console_log(string("t: ref report=%d", string_index_of(ref, "\"color_space_override\": null")));
	console_log(string("t: ref=%d", studio_export_run("$W/out_ref", "mixref", 0)));
	return 0;
}
EOF
app "$W/p.arm" --background --script "$W/ov.c" > "$W/ov.log" 2>&1 || true
grep -E "^t: " "$W/ov.log" | cut -c1-200
check "espace imposé par script : accepté, lu, dans le rapport" 'grep -q "^t: set=1" "$W/ov.log" && grep -q "^t: cs=\[Linear P3-D65\]" "$W/ov.log" && grep -q "^t: report=[0-9]" "$W/ov.log" && grep -q "^t: ov=1" "$W/ov.log"'
check "espace imposé inconnu : refusé, export bloqué, rien d écrit" 'grep -q "^t: set bad=0" "$W/ov.log" && code "$W/ov.log" ocio_unknown_colorspace && grep -q "^t: ovbad=0" "$W/ov.log" && [ ! -e "$W/out_ovbad" ]'
check "retour à l espace du preset" 'grep -q "^t: reset=1" "$W/ov.log" && grep -q "^t: cs0=\[\]" "$W/ov.log" && grep -q "^t: ref report=[0-9]" "$W/ov.log" && grep -q "^t: ref=1" "$W/ov.log"'
(STUDIO_EXPORT_COLOR_SPACE="Linear P3-D65" app "$W/p.arm" --background --export-textures exr32 mix "$W/out_cli" > "$W/cli.log" 2>&1 || true)
# Référence en ligne de commande : exr32 aplatit les calques en 32 bits, le script garde la profondeur du projet.
app "$W/p.arm" --background --export-textures exr32 mixref "$W/out_cliref" > "$W/cliref.log" 2>&1 || true
(STUDIO_EXPORT_COLOR_SPACE=nope app --background --script "$W/tst.c" > "$W/envbad.log" 2>&1 || true)
check "STUDIO_EXPORT_COLOR_SPACE inconnu : journalisé" 'grep -q "STUDIO_EXPORT_COLOR_SPACE .nope. is unknown" "$W/envbad.log"'
check "couleurs imposées = preset qui les déclare, données intactes, manifeste" '"$PY" "$T/ocio_override_check.py" "$W"'

[ $FAIL -eq 0 ] && echo "ocio export app: OK ($W)" || { echo "ocio export app: FAILED ($W)"; exit 1; }
