#!/bin/sh
# Lot A4 dans ArmorPaint lui-même (--background) : les textures importées sont converties vers l'espace de travail OCIO.
# Les pixels relus de l'app (studio_texture_dump) sont comparés à PyOpenColorIO (ocio_import_check.py) : PNG, TIFF et
# PSD (à plat, calques) en 8 bits, EXR float et half par la file_rule, .hdr déjà dans l'espace de travail, donnée
# reconnue à son nom. Puis le choix par texture (reconversion, retour à l'automatique, espace inconnu, choix fait avant
# l'import), la vignette du navigateur de fichiers, le .arm (rouvert, paquet d'assets sans les fichiers),
# STUDIO_OCIO_IMPORT=0, et le nœud Image Texture (STUDIO-PATCH(image-texture-color-space)).
#
#   sh .tests/ocio_import_app.sh [dossier de travail]
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
"$PY" -c 'import PyOpenColorIO, numpy, OpenEXR, tifffile, PIL' 2> /dev/null ||
	{ echo "venv incomplet : lancer .tests/ocio.sh puis $EXT/.ocio/venv/bin/pip install numpy OpenEXR tifffile pillow" >&2; exit 1; }
FAIL=0
ok() { echo "ok   $1"; }
ko() { echo "FAIL $1"; FAIL=1; }
check() { if eval "$2"; then ok "$1"; else ko "$1"; fi; }
app() { perl -e 'alarm 120; exec @ARGV' "$APP" "$@" -ApplePersistenceIgnoreState YES; }
# px <commande de ocio_import_check.py>… : sa ligne de résultat, indentée, et son statut.
px() {
	out=$("$PY" "$T/ocio_import_check.py" "$@") && r=0 || r=$?
	echo "$out" | sed 's/^/     /'
	return $r
}
has() { grep -qxF "$2" "$1"; }
# Lignes minic : tex <étiquette> <chemin> journalise espace, source et linéarité ; dmp <étiquette> <chemin> <fichier>
# écrit les pixels ; set <étiquette> <chemin> <espace> choisit l'espace.
tex() {
	printf '\tconsole_log(string("t: %s cs=[%%s] source=[%%s] linear=%%d", studio_texture_color_space("%s"), studio_texture_color_space_source("%s"), studio_texture_is_linear("%s")));\n' \
		"$1" "$2" "$2" "$2"
}
dmp() { printf '\tconsole_log(string("t: dump %s=%%d", studio_texture_dump("%s", "%s")));\n' "$1" "$2" "$3"; }
set_cs() { printf '\tconsole_log(string("t: %s=%%d", studio_texture_set_color_space("%s", "%s")));\n' "$1" "$2" "$3"; }
unset OCIO STUDIO_OCIO_WORKING_SPACE STUDIO_OCIO_LIB STUDIO_OCIO_IMPORT

rm -rf "$W"/*.stx "$W"/*.arm "$W/packed" "$W/moved" "$W/out_"* "$W/presets"
"$PY" "$T/ocio_import_check.py" make "$W"
SRGB="sRGB Encoded Rec.709 (sRGB)"
LIN="Linear Rec.709 (sRGB)"
NAMES="ramp.png ramp_copy.png pre.png wood_normal.png browse.png move.png photo.tif flat.psd layered.psd plate.exr plate_half.exr sky.hdr"

# 1. Import : espace retenu et pixels de chaque format ; choix par texture ; enregistrement.
{
	echo 'int main() {'
	echo '	script_project_new();'
	echo '	console_log(string("t: enabled=%d", studio_ocio_import_enabled()));'
	set_cs "preset copy" "$W/ramp_copy.png" ACEScg
	set_cs "preset raw" "$W/pre.png" Raw
	# Le navigateur de fichiers charge l'image entière pour sa vignette, hors import : pas convertie.
	printf '\tdata_get_texture("%s");\n' "$W/browse.png"
	dmp browse_raw "$W/browse.png" "$W/s1_browse_raw.stx"
	for n in $NAMES; do printf '\tscript_import_asset("%s", 0);\n' "$W/$n"; done
	for n in $NAMES; do
		tex "$n" "$W/$n"
		dmp "$n" "$W/$n" "$W/s1_$n.stx"
	done
	tex layer layered.psd.top.png # calque d'un PSD : io_psd_import_layer, chemin relatif
	dmp layer layered.psd.top.png "$W/s1_layer.stx"
	set_cs set "$W/ramp.png" ACEScg
	tex ramp_set "$W/ramp.png"
	dmp ramp_set "$W/ramp.png" "$W/s1_ramp_set.stx"
	set_cs "set again" "$W/ramp.png" ACEScg
	set_cs "set nope" "$W/ramp.png" nope
	tex ramp_nope "$W/ramp.png"
	set_cs "move set" "$W/move.png" ACEScg
	dmp move_set "$W/move.png" "$W/s1_move_set.stx"
	set_cs "move auto" "$W/move.png" ""
	tex move_auto "$W/move.png"
	dmp move_auto "$W/move.png" "$W/s1_move_auto.stx"
	printf '\tproject_filepath_set("%s");\n\tproject_save(0);\n\treturn 0;\n}\n' "$W/p.arm"
} > "$W/s1.c"
app --background --script "$W/s1.c" > "$W/s1.log" 2>&1 || true
L=$W/s1.log
grep -E "^t: |^studio: texture|rror" "$L" | cut -c1-200
check "lot actif" 'has "$L" "t: enabled=1"'
check "choix avant import accepté" 'has "$L" "t: preset copy=1" && has "$L" "t: preset raw=1"'
check "PNG 8 bits : sRGB par défaut, converti" 'has "$L" "t: ramp.png cs=[$SRGB] source=[default] linear=0" && px convert "$W/s1_ramp.png.stx" "$W/ramp.png" "$SRGB"'
check "choix fait avant l import : ACEScg, converti" 'has "$L" "t: ramp_copy.png cs=[ACEScg] source=[user] linear=0" && px convert "$W/s1_ramp_copy.png.stx" "$W/ramp_copy.png" ACEScg'
check "choix fait avant l import : Raw, intact" 'has "$L" "t: pre.png cs=[Raw] source=[user] linear=0" && px same "$W/s1_pre.png.stx" "$W/pre.png"'
check "nom de donnée (normal) : Raw, intact" 'has "$L" "t: wood_normal.png cs=[Raw] source=[name] linear=0" && px same "$W/s1_wood_normal.png.stx" "$W/wood_normal.png"'
check "vignette du navigateur : pas convertie" 'has "$L" "t: dump browse_raw=1" && px same "$W/s1_browse_raw.stx" "$W/browse.png"'
check "vignette du navigateur puis import : décodée à nouveau, convertie" 'has "$L" "t: browse.png cs=[$SRGB] source=[default] linear=0" && px convert "$W/s1_browse.png.stx" "$W/browse.png" "$SRGB"'
check "TIFF 8 bits : sRGB, converti" 'has "$L" "t: photo.tif cs=[$SRGB] source=[default] linear=0" && px convert "$W/s1_photo.tif.stx" "$W/photo.tif" "$SRGB"'
check "PSD à plat : sRGB, converti" 'has "$L" "t: flat.psd cs=[$SRGB] source=[default] linear=0" && px convert "$W/s1_flat.psd.stx" "$W/flat.psd" "$SRGB"'
check "PSD à calques : premier calque converti" 'has "$L" "t: layered.psd cs=[$SRGB] source=[default] linear=0" && px convert "$W/s1_layered.psd.stx" "$W/layered.psd" "$SRGB"'
check "PSD à calques : autre calque, espace du fichier" 'has "$L" "t: layer cs=[$SRGB] source=[default] linear=0" && px convert "$W/s1_layer.stx" "$W/layered.psd.top.png" "$SRGB"'
check "EXR float : file_rule ACES2065-1, converti" 'has "$L" "t: plate.exr cs=[ACES2065-1] source=[rule] linear=1" && px convert "$W/s1_plate.exr.stx" "$W/plate.exr" ACES2065-1'
check "EXR half : file_rule ACES2065-1, converti" 'has "$L" "t: plate_half.exr cs=[ACES2065-1] source=[rule] linear=1" && px convert "$W/s1_plate_half.exr.stx" "$W/plate_half.exr" ACES2065-1'
check ".hdr : espace de travail, intact" 'has "$L" "t: sky.hdr cs=[$LIN] source=[default] linear=1" && px same "$W/s1_sky.hdr.stx" "$W/sky.hdr"'
check "choix ACEScg : texture reconvertie depuis son fichier" 'has "$L" "t: set=1" && has "$L" "t: ramp_set cs=[ACEScg] source=[user] linear=0" && px convert "$W/s1_ramp_set.stx" "$W/ramp.png" ACEScg'
check "même choix : rien à refaire ; espace inconnu refusé, choix gardé" 'has "$L" "t: set again=1" && has "$L" "t: set nope=0" && grep -q "unknown OCIO color space \"nope\"" "$L" && has "$L" "t: ramp_nope cs=[ACEScg] source=[user] linear=0"'
check "retour à l automatique" 'has "$L" "t: move set=1" && px convert "$W/s1_move_set.stx" "$W/move.png" ACEScg && has "$L" "t: move auto=1" && has "$L" "t: move_auto cs=[$SRGB] source=[default] linear=0" && px convert "$W/s1_move_auto.stx" "$W/move.png" "$SRGB"'
check ".arm : entrée studio_color_spaces.json" '[ -f "$W/p.arm" ] && grep -aqF "studio_color_spaces.json" "$W/p.arm" && grep -aqF "{\"file\": \"ramp.png\", \"color_space\": \"ACEScg\", \"source\": \"user\", \"float\": false, \"linear\": false}" "$W/p.arm" && grep -aqF "{\"file\": \"plate.exr\", \"color_space\": \"ACES2065-1\", \"source\": \"rule\", \"float\": true, \"linear\": true}" "$W/p.arm"'

# 2. Projet rouvert : chaque texture décodée comme elle l'a été ; puis enregistré avec ses assets dans le paquet.
{
	echo 'int main() {'
	for n in ramp.png ramp_copy.png pre.png wood_normal.png move.png plate.exr sky.hdr; do tex "$n" "$W/$n"; done
	dmp ramp "$W/ramp.png" "$W/s2_ramp.stx"
	dmp plate "$W/plate.exr" "$W/s2_plate.stx"
	echo '	console_log(string("t: pack=%d", studio_project_pack_assets(1)));'
	printf '\tproject_filepath_set("%s");\n\tproject_save(0);\n\treturn 0;\n}\n' "$W/packed/p.arm"
} > "$W/s2.c"
mkdir -p "$W/packed"
app "$W/p.arm" --background --script "$W/s2.c" > "$W/s2.log" 2>&1 || true
L=$W/s2.log
check "rouvert : choix de l utilisateur, décodé avec" 'has "$L" "t: ramp.png cs=[ACEScg] source=[user] linear=0" && has "$L" "t: ramp_copy.png cs=[ACEScg] source=[user] linear=0" && has "$L" "t: pre.png cs=[Raw] source=[user] linear=0" && px convert "$W/s2_ramp.stx" "$W/ramp.png" ACEScg'
check "rouvert : nom, défaut, file_rule" 'has "$L" "t: wood_normal.png cs=[Raw] source=[name] linear=0" && has "$L" "t: move.png cs=[$SRGB] source=[default] linear=0" && has "$L" "t: plate.exr cs=[ACES2065-1] source=[rule] linear=1" && has "$L" "t: sky.hdr cs=[$LIN] source=[default] linear=1" && px convert "$W/s2_plate.stx" "$W/plate.exr" ACES2065-1'
check "enregistré avec le paquet d assets" 'has "$L" "t: pack=0" && [ -f "$W/packed/p.arm" ] && grep -aqF "studio_color_spaces.json" "$W/packed/p.arm"'

# 3. Le .arm du paquet ailleurs, sans les fichiers : pixels du paquet (déjà convertis, pas une seconde fois), espace
# relu du .arm, changement refusé.
mkdir -p "$W/moved/sub"
cp "$W/packed/p.arm" "$W/moved/sub/p.arm" 2> /dev/null || true # manquant : l'étape 2 a déjà échoué, celle-ci aussi
{
	echo 'int main() {'
	tex ramp "$W/moved/ramp.png"
	tex photo "$W/moved/photo.tif"
	dmp ramp "$W/moved/ramp.png" "$W/s3_ramp.stx"
	dmp photo "$W/moved/photo.tif" "$W/s3_photo.stx"
	set_cs "set packed" "$W/moved/ramp.png" Raw
	tex ramp_after "$W/moved/ramp.png"
	echo '	return 0;'
	echo '}'
} > "$W/s3.c"
app "$W/moved/sub/p.arm" --background --script "$W/s3.c" > "$W/s3.log" 2>&1 || true
L=$W/s3.log
check "paquet : espace relu du .arm" 'has "$L" "t: ramp cs=[ACEScg] source=[user] linear=0" && has "$L" "t: photo cs=[$SRGB] source=[default] linear=0"'
check "paquet : pixels convertis une seule fois" 'px convert --opaque "$W/s3_ramp.stx" "$W/ramp.png" ACEScg && px convert "$W/s3_photo.stx" "$W/photo.tif" "$SRGB"'
check "paquet : changement d espace refusé sans le fichier" 'has "$L" "t: set packed=0" && grep -q "is not on disk" "$L" && has "$L" "t: ramp_after cs=[ACEScg] source=[user] linear=0"'

# 4. STUDIO_OCIO_IMPORT=0 : pas de conversion ni d'entrée dans le .arm ; celle d'un projet ouvert est gardée telle quelle.
{
	echo 'int main() {'
	echo '	script_project_new();'
	echo '	console_log(string("t: enabled=%d", studio_ocio_import_enabled()));'
	printf '\tscript_import_asset("%s", 0);\n' "$W/ramp.png"
	tex ramp "$W/ramp.png"
	dmp ramp "$W/ramp.png" "$W/s4_ramp.stx"
	set_cs "set off" "$W/ramp.png" ACEScg
	printf '\tproject_filepath_set("%s");\n\tproject_save(0);\n\treturn 0;\n}\n' "$W/off.arm"
} > "$W/s4.c"
(STUDIO_OCIO_IMPORT=0 app --background --script "$W/s4.c" > "$W/s4.log" 2>&1 || true)
L=$W/s4.log
check "STUDIO_OCIO_IMPORT=0 : désactivé, texture intacte" 'grep -q "studio: texture color spaces off" "$L" && has "$L" "t: enabled=0" && has "$L" "t: ramp cs=[] source=[] linear=0" && px same "$W/s4_ramp.stx" "$W/ramp.png"'
check "STUDIO_OCIO_IMPORT=0 : choix refusé, pas d entrée dans le .arm" 'has "$L" "t: set off=0" && [ -f "$W/off.arm" ] && ! grep -aqF "studio_color_spaces.json" "$W/off.arm"'
printf 'int main() {\n\tproject_filepath_set("%s");\n\tproject_save(0);\n\treturn 0;\n}\n' "$W/keep.arm" > "$W/s5.c"
(STUDIO_OCIO_IMPORT=0 app "$W/p.arm" --background --script "$W/s5.c" > "$W/s5.log" 2>&1 || true)
check "STUDIO_OCIO_IMPORT=0 : entrée d un projet ouvert gardée" '[ -f "$W/keep.arm" ] && grep -aqF "{\"file\": \"ramp.png\", \"color_space\": \"ACEScg\", \"source\": \"user\", \"float\": false, \"linear\": false}" "$W/keep.arm"'

# 5. Nœud Image Texture : un gris linéaire 0,5 (.hdr) en couleur de base et en rugosité, calque de remplissage 8 bits,
# export des valeurs d'ArmorPaint telles quelles. La couleur de base est gardée encodée gamma 2.2 : Linear doit donner
# 0,5^(1/2,2) = 186/255 ; sRGB en rugosité, 0,5^2,2 = 55 ou 56/255 ; Auto sur une texture linéaire, comme Linear.
mkdir -p "$W/presets"
cat > "$W/presets/node.json" << 'EOF'
{
	"version": 1,
	"defaults": { "format": "exr", "bits": 32, "color_space": "raw" },
	"textures": [
		{ "name": "base", "channels": ["base_r", "base_g", "base_b"] },
		{ "name": "rough", "channels": ["rough"] }
	]
}
EOF
export STUDIO_EXPORT_PRESETS="$W/presets"
# node <mode de la couleur de base> <sortie> : 0 Auto, 1 Linear (bouton 1 du nœud) ; la rugosité en sRGB.
node() {
	cat > "$W/node$1.c" << EOF
int main() {
	script_project_new();
	script_import_asset("$W/grey.hdr", 0);
	slot_material_t *m = script_material_create("mat");
	ui_node_t *a = script_material_create_node_at("TEX_IMAGE", -400.0, 0.0);
	ui_node_t *b = script_material_create_node_at("TEX_IMAGE", -400.0, 300.0);
	script_material_set_button(a, 0, 0.0);
	script_material_set_button(a, 1, $1.0);
	script_material_set_button(b, 0, 0.0);
	script_material_set_button(b, 1, 2.0);
	ui_node_t *out = script_material_get_node("OUTPUT_MATERIAL_PBR");
	script_material_connect(a, 0, out, 0);
	script_material_connect(b, 0, out, 3);
	script_material_update();
	script_material_set(m);
	script_fill_layer();
	console_log(string("t: export=%d", studio_export_run("$W/$2", "node", 0)));
	return 0;
}
EOF
	app --background --script "$W/node$1.c" > "$W/node$1.log" 2>&1 || true
}
node 1 out_linear
node 0 out_auto
check "nœud : Linear en couleur de base, encodé gamma 2.2" 'has "$W/node1.log" "t: export=1" && px exr "$W/out_linear/untitled_base.exr" 0.7254 0.7334'
check "nœud : sRGB en rugosité, décodé" 'px exr "$W/out_linear/untitled_rough.exr" 0.2156 0.2197'
check "nœud : Auto sur une texture linéaire, comme Linear" 'has "$W/node0.log" "t: export=1" && px exr "$W/out_auto/untitled_base.exr" 0.7254 0.7334'

[ $FAIL -eq 0 ] && echo "ocio import app: OK ($W)" || { echo "ocio import app: FAILED ($W)"; exit 1; }
