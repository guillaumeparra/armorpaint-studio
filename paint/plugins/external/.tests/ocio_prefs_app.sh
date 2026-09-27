#!/bin/sh
# Lot A5 dans ArmorPaint lui-même (--background) : préférences studio (studio_prefs.json), configuration OCIO et
# espace de travail des nouveaux projets pris des préférences, espace de travail d'un projet gardé dans son .arm (entrée
# studio_ocio.json) et figé à l'enregistrement, textures reconverties quand il change (pixels relus de l'app et
# comparés à PyOpenColorIO par ocio_import_check.py), choix du viewport gardé.
#
#   sh .tests/ocio_prefs_app.sh [dossier de travail]
#   APP=<binaire ArmorPaint> pour un autre build (défaut : build Release de paint/).
# Demande .ocio/venv (créé par ocio.sh) avec numpy, OpenEXR, tifffile et Pillow, comme ocio_import_app.sh.
set -eu
T=$(cd "$(dirname "$0")" && pwd)
EXT=$(dirname "$T")
APP=${APP:-$EXT/../../build/build/Release/ArmorPaint.app/Contents/MacOS/ArmorPaint}
PY=$EXT/.ocio/venv/bin/python
W=${1:-$(mktemp -d)}
mkdir -p "$W/cfg"
W=$(cd "$W" && pwd)
[ -x "$APP" ] || { echo "ArmorPaint not found: $APP" >&2; exit 1; }
"$PY" -c 'import PyOpenColorIO, numpy, OpenEXR, tifffile, PIL' 2> /dev/null ||
	{ echo "venv incomplet : lancer .tests/ocio.sh puis $EXT/.ocio/venv/bin/pip install numpy OpenEXR tifffile pillow" >&2; exit 1; }
FAIL=0
ok() { echo "ok   $1"; }
ko() { echo "FAIL $1"; FAIL=1; }
check() { if eval "$2"; then ok "$1"; else ko "$1"; fi; }
app() { perl -e 'alarm 120; exec @ARGV' "$APP" "$@" -ApplePersistenceIgnoreState YES; }
has() { grep -qxF "$2" "$1"; }
starts() { grep -qF "$2" "$1"; } # un morceau de ligne (la suite varie)
px() {
	out=$("$PY" "$T/ocio_import_check.py" "$@") && r=0 || r=$?
	echo "$out" | sed 's/^/     /'
	return $r
}
# prefs <JSON> : réécrit le fichier des préférences.
prefs() { printf '%s\n' "$1" > "$P"; }
# pj <expression Python sur d, le JSON du fichier des préférences> : vrai si elle l'est.
pj() { "$PY" -c 'import json, sys; d = json.load(open(sys.argv[1], encoding="utf-8")); sys.exit(0 if eval(sys.argv[2]) else 1)' "$P" "$1" 2> /dev/null; }
# entry <projet.arm> <expression Python sur e> : e est l'entrée studio_ocio.json du .arm (son texte JSON est rangé tel
# quel dans le paquet d'assets).
entry() {
	"$PY" -c '
import json, re, sys
m = re.search(rb"\{\n\t\"version\": 1,\n\t\"config\": [^\n]*,\n\t\"config_source\": .*?\n\}\n", open(sys.argv[1], "rb").read(), re.S)
e = json.loads(m.group(0)) if m else None
sys.exit(0 if e is not None and eval(sys.argv[2]) else 1)' "$1" "$2" 2> /dev/null
}
# Lignes minic. ws <étiquette> : espace de travail, origine, mode et nom du projet (la lecture du mode détecte d'abord
# un projet ouvert ou créé dans la frame) ; dmp <étiquette> <texture> <fichier> : pixels de la texture.
ws() {
	printf '\tstudio_ocio_project_working_space_mode();\n'
	printf '\tconsole_log(string("t: %s ws=[%%s] origin=[%%s] mode=%%d name=[%%s]", studio_ocio_working_space(), studio_ocio_working_space_origin(), studio_ocio_project_working_space_mode(), studio_ocio_project_working_space_name()));\n' "$1"
}
dmp() { printf '\tconsole_log(string("t: dump %s=%%d", studio_texture_dump("%s", "%s")));\n' "$1" "$2" "$3"; }
unset OCIO STUDIO_OCIO_WORKING_SPACE STUDIO_OCIO_LIB STUDIO_OCIO_IMPORT STUDIO_OCIO_VIEWPORT STUDIO_OCIO_DISPLAY STUDIO_OCIO_VIEW \
	STUDIO_OCIO_LOOK STUDIO_PREFS

rm -rf "$W"/*.arm "$W"/*.stx "$W"/*.c "$W"/*.log "$W"/prefs.json* "$W/packed" "$W/moved"
"$PY" "$T/ocio_ref.py" --make-config "$W/cfg"
"$PY" "$T/ocio_import_check.py" make "$W"
P=$W/prefs.json
ACES=studio-config-v4.0.0_aces-v2.0_ocio-v2.5
LIN="Linear Rec.709 (sRGB)"
SRGB="sRGB Encoded Rec.709 (sRGB)"

echo "== 1. Fichier des préférences"
# 1a. --background sans STUDIO_PREFS : le fichier de l'artiste n'est ni lu ni écrit, les changements restent en mémoire.
BUNDLE=$(cd "$(dirname "$APP")/.." && pwd)
snapshot() { find "$BUNDLE" -name 'studio_prefs.json*' -exec shasum {} + 2> /dev/null | sort; }
before=$(snapshot)
cat > "$W/p1.c" << EOF
int main() {
	console_log(string("t: path=[%s]", studio_prefs_path()));
	console_log(string("t: set=%d", studio_ocio_set_config_preference("$W/cfg/config.ocio")));
	console_log(string("t: pref=[%s] origin=[%s]", studio_ocio_config_preference(), studio_ocio_config_origin()));
	studio_ocio_view_set_enabled(0);
	return 0;
}
EOF
app --background --script "$W/p1.c" > "$W/p1.log" 2>&1 || true
L=$W/p1.log
DEF=$(sed -n 's/^t: path=\[\(.*\)\]$/\1/p' "$L")
check "--background : fichier par défaut ignoré" 'has "$L" "studio: preferences $DEF ignored in --background (set STUDIO_PREFS to use a file)"'
check "--background : chemin propre, dans data/ de l app" 'case $DEF in "$BUNDLE"/*/data/studio_prefs.json) ! printf "%s" "$DEF" | grep -qE "//|/\./" ;; *) false ;; esac'
check "--background : réglage en mémoire, configuration rechargée" 'has "$L" "t: set=1" && has "$L" "t: pref=[$W/cfg/config.ocio] origin=[preferences]"'
check "--background : fichier de l artiste intact" '[ "$(snapshot)" = "$before" ]'

# 1a'. App en lecture seule (installée par l'IT) : préférences, journal d'audit et file des publishes vont dans le
# dossier de sauvegarde d'ArmorPaint. Sur une copie de l'app, pour ne pas toucher au build.
rm -rf "$W/ro" && mkdir -p "$W/ro"
cp -R "$(dirname "$BUNDLE")" "$W/ro/"
RO=$W/ro/$(basename "$(dirname "$BUNDLE")")
chmod -R a-w "$RO/Contents/Resources"
perl -e 'alarm 120; exec @ARGV' "$RO/Contents/MacOS/$(basename "$APP")" --background --script "$W/p1.c" \
	-ApplePersistenceIgnoreState YES > "$W/p1ro.log" 2>&1 || true
chmod -R u+w "$W/ro" && rm -rf "$W/ro"
L=$W/p1ro.log
RODEF=$(sed -n 's/^t: path=\[\(.*\)\]$/\1/p' "$L")
check "app en lecture seule : dossier de sauvegarde, dit au journal" 'case $RODEF in "$HOME/Library/Application Support/"*/studio_prefs.json) starts "$L" "studio: preferences: the app folder is read-only, studio files go to " && ! printf "%s" "$RODEF" | grep -qE "//|/\./" ;; *) false ;; esac'

# 1b. $STUDIO_PREFS absent : défauts ; chaque changement l'écrit.
cat > "$W/p2.c" << EOF
int main() {
	console_log(string("t: path=[%s]", studio_prefs_path()));
	console_log(string("t: set config=%d origin=[%s]", studio_ocio_set_config_preference("$W/cfg/config.ocio"), studio_ocio_config_origin()));
	console_log(string("t: set ws=%d", studio_ocio_set_working_space_preference("linear_alias")));
	console_log(string("t: set nope=%d", studio_ocio_set_working_space_preference("nope")));
$(ws ws)
	studio_ocio_view_set_enabled(0);
	return 0;
}
EOF
STUDIO_PREFS=$P app --background --script "$W/p2.c" > "$W/p2.log" 2>&1 || true
L=$W/p2.log
check "\$STUDIO_PREFS absent : défauts" 'has "$L" "studio: preferences $P (0)" && has "$L" "t: path=[$P]"'
check "configuration choisie : rechargée" 'has "$L" "t: set config=1 origin=[preferences]"'
check "espace de travail des nouveaux projets : alias accepté, nom inconnu refusé" 'has "$L" "t: set ws=1" && has "$L" "t: set nope=0" && starts "$L" "unknown OCIO color space \"nope\", working space preference not changed" && has "$L" "t: ws ws=[lin] origin=[preferences] mode=0 name=[]"'
check "fichier écrit" "pj \"d == {'version': 1, 'ocio.config': '$W/cfg/config.ocio', 'ocio.working_space': 'linear_alias', 'ocio.viewport': False}\""

# 1c. Relu au démarrage ; une clé inconnue (autre version du studio) est gardée telle quelle.
"$PY" -c 'import json, sys; d = json.load(open(sys.argv[1])); d["x.future"] = {"a": [1, 2.5], "b": "é"}; json.dump(d, open(sys.argv[1], "w", encoding="utf-8"), ensure_ascii=False)' "$P"
cat > "$W/p3.c" << EOF
int main() {
$(ws start)
	console_log(string("t: viewport=%d", studio_ocio_view_enabled()));
	console_log(string("t: set auto=%d", studio_ocio_set_working_space_preference("")));
$(ws auto)
	console_log(string("t: error=[%s]", studio_ocio_working_space_error()));
	return 0;
}
EOF
STUDIO_PREFS=$P app --background --script "$W/p3.c" > "$W/p3.log" 2>&1 || true
L=$W/p3.log
check "relu : configuration, espace de travail, viewport" 'has "$L" "studio: preferences $P (4)" && starts "$L" "studio: OCIO 2.5.2, config \"studio_test\" from $W/cfg/config.ocio (preferences)" && has "$L" "t: start ws=[lin] origin=[preferences] mode=0 name=[]" && has "$L" "t: viewport=0"'
check "automatique introuvable dans une configuration sans rôles d échange : erreur, où la corriger" 'has "$L" "t: set auto=1" && has "$L" "t: auto ws=[] origin=[] mode=0 name=[]" && starts "$L" "t: error=[no linear Rec.709 colorspace found in the config" && starts "$L" "choose a working space in Preferences > Color, or set STUDIO_OCIO_WORKING_SPACE]"'
check "clé retirée, clé inconnue gardée" "pj \"'ocio.working_space' not in d and d['x.future'] == {'a': [1, 2.5], 'b': 'é'} and d['ocio.viewport'] is False\""

# 1d. $OCIO l'emporte : la préférence est gardée pour plus tard, rien n'est rechargé.
cat > "$W/p4.c" << 'EOF'
int main() {
	console_log(string("t: origin=[%s]", studio_ocio_config_origin()));
	console_log(string("t: set=%d", studio_ocio_set_config_preference("")));
	console_log(string("t: after=[%s] pref=[%s]", studio_ocio_config_origin(), studio_ocio_config_preference()));
	return 0;
}
EOF
(OCIO=ocio://cg-config-latest STUDIO_PREFS=$P app --background --script "$W/p4.c" > "$W/p4.log" 2>&1 || true)
L=$W/p4.log
check "\$OCIO avant la préférence" 'starts "$L" "studio: OCIO 2.5.2, config \"cg-config-v4.0.0_aces-v2.0_ocio-v2.5\" from ocio://cg-config-latest (\$OCIO)" && has "$L" "t: origin=[\$OCIO]"'
check "\$OCIO : préférence changée, gardée, rien rechargé" 'has "$L" "t: set=1" && starts "$L" "OCIO config preference kept for sessions without \$OCIO (ocio://cg-config-latest)" && has "$L" "t: after=[\$OCIO] pref=[]" && pj "\"ocio.config\" not in d"'

# 1e. Préférence invalide : erreur, pas de repli ; la retirer revient à la configuration intégrée.
prefs "{\"version\": 1, \"ocio.config\": \"$W/missing.ocio\"}"
cat > "$W/p5.c" << 'EOF'
int main() {
	console_log(string("t: status=[%s]", studio_ocio_status()));
	console_log(string("t: set=%d", studio_ocio_set_config_preference("")));
	console_log(string("t: after=[%s]", studio_ocio_status()));
	return 0;
}
EOF
STUDIO_PREFS=$P app --background --script "$W/p5.c" > "$W/p5.log" 2>&1 || true
L=$W/p5.log
check "préférence invalide : OCIO indisponible, sans repli" 'starts "$L" "t: status=[OCIO unavailable: config $W/missing.ocio (preferences) rejected"'
check "préférence retirée : configuration intégrée" 'has "$L" "t: set=1" && starts "$L" "t: after=[OCIO 2.5.2, config \"$ACES\" from ocio://studio-config-latest (built-in default)" && pj "\"ocio.config\" not in d"'

# 1f. Fichier illisible : journalisé, défauts ; mis de côté (.bak) à la première écriture.
printf 'garbage' > "$P"
printf 'int main() {\n\tstudio_ocio_view_set_enabled(1);\n\treturn 0;\n}\n' > "$W/p6.c"
STUDIO_PREFS=$P app --background --script "$W/p6.c" > "$W/p6.log" 2>&1 || true
L=$W/p6.log
check "fichier illisible : défauts, mis de côté" 'has "$L" "studio: preferences $P (0): $P is not a JSON object of preferences, defaults used" && has "$L" "studio: preferences: unreadable $P kept as $P.bak" && [ "$(cat "$P.bak")" = garbage ] && pj "d == {\"version\": 1, \"ocio.viewport\": True}"'

echo "== 2. Espace de travail du projet"
# 2a. Nouveau projet : la préférence ; l'enregistrement l'écrit dans le .arm et le fige.
prefs '{"version": 1, "ocio.working_space": "ACEScg"}'
# project_save() enregistre à la frame suivante : relu quelques frames plus tard.
cat > "$W/w1.c" << EOF
int frame = 0;
void update() {
	if (++frame == 3) {
$(ws saved)
		script_quit();
	}
}
int main() {
	script_project_new();
$(ws new)
	project_filepath_set("$W/named.arm");
	project_save(0);
	script_notify_on_update(update);
	return 0;
}
EOF
STUDIO_PREFS=$P app --background --script "$W/w1.c" > "$W/w1.log" 2>&1 || true
L=$W/w1.log
check "nouveau projet : préférence" 'has "$L" "t: new ws=[ACEScg] origin=[preferences] mode=0 name=[]"'
check "enregistré : figé" 'has "$L" "t: saved ws=[ACEScg] origin=[project] mode=2 name=[ACEScg]"'
check ".arm : entrée studio_ocio.json" "[ -f '$W/named.arm' ] && entry '$W/named.arm' \"e == {'version': 1, 'config': '$ACES', 'config_source': 'ocio://studio-config-latest', 'working_space': 'ACEScg', 'working_space_auto': False}\""

# 2b. Rouvert : l'espace du projet, quels que soient la préférence et l'environnement ; un nouveau projet les suit.
prefs '{"version": 1}'
cat > "$W/w2.c" << EOF
int main() {
$(ws opened)
	script_project_new();
$(ws new)
	return 0;
}
EOF
STUDIO_PREFS=$P app "$W/named.arm" --background --script "$W/w2.c" > "$W/w2.log" 2>&1 || true
(STUDIO_OCIO_WORKING_SPACE="Linear P3-D65" STUDIO_PREFS=$P app "$W/named.arm" --background --script "$W/w2.c" > "$W/w2env.log" 2>&1 || true)
check "rouvert : espace du projet" 'has "$W/w2.log" "t: opened ws=[ACEScg] origin=[project] mode=2 name=[ACEScg]" && has "$W/w2env.log" "t: opened ws=[ACEScg] origin=[project] mode=2 name=[ACEScg]"'
check "puis nouveau projet : automatique, ou \$STUDIO_OCIO_WORKING_SPACE" 'has "$W/w2.log" "t: new ws=[$LIN] origin=[automatic] mode=0 name=[]" && has "$W/w2env.log" "t: new ws=[Linear P3-D65] origin=[\$STUDIO_OCIO_WORKING_SPACE] mode=0 name=[]"'

# 2c. Projet sans entrée (enregistré sans OCIO, comme par ArmorPaint sans studio) : l'automatique, pas la préférence.
cat > "$W/w3.c" << EOF
int main() {
	script_project_new();
	project_filepath_set("$W/noentry.arm");
	project_save(0);
	return 0;
}
EOF
(OCIO=$W/missing.ocio STUDIO_PREFS=$P app --background --script "$W/w3.c" > "$W/w3.log" 2>&1 || true)
prefs '{"version": 1, "ocio.working_space": "ACEScg"}'
printf 'int main() {\n%s\n\treturn 0;\n}\n' "$(ws opened)" > "$W/w4.c"
STUDIO_PREFS=$P app "$W/noentry.arm" --background --script "$W/w4.c" > "$W/w4.log" 2>&1 || true
check "sans OCIO : pas d entrée écrite" '[ -f "$W/noentry.arm" ] && ! grep -aqF "studio_ocio.json" "$W/noentry.arm"'
check "projet sans entrée : automatique" 'has "$W/w4.log" "t: opened ws=[$LIN] origin=[project, automatic] mode=1 name=[]"'

# 2d. Projet d'une autre configuration, espace de travail qu'elle seule connaît : erreur signalée, puis choisi à nouveau.
cat > "$W/w5.c" << EOF
int frame = 0;
void update() {
	if (++frame == 3) {
$(ws saved)
		script_quit();
	}
}
int main() {
	script_project_new();
	project_filepath_set("$W/lin.arm");
	project_save(0);
	script_notify_on_update(update);
	return 0;
}
EOF
(OCIO=$W/cfg/config.ocio STUDIO_OCIO_WORKING_SPACE=lin STUDIO_PREFS=$P app --background --script "$W/w5.c" > "$W/w5.log" 2>&1 || true)
cat > "$W/w6.c" << EOF
int main() {
$(ws opened)
	console_log(string("t: bad mode=%d", studio_ocio_set_project_working_space(7, "")));
	console_log(string("t: bad name=%d", studio_ocio_set_project_working_space(2, "nope")));
	console_log(string("t: auto=%d", studio_ocio_set_project_working_space(1, "")));
$(ws auto)
	console_log(string("t: named=%d", studio_ocio_set_project_working_space(2, "lin_srgb")));
$(ws named)
	return 0;
}
EOF
STUDIO_PREFS=$P app "$W/lin.arm" --background --script "$W/w6.c" > "$W/w6.log" 2>&1 || true
L=$W/w6.log
check "\$STUDIO_OCIO_WORKING_SPACE figé à l enregistrement" 'has "$W/w5.log" "t: saved ws=[lin] origin=[project] mode=2 name=[lin]" && entry "$W/lin.arm" "e[\"config\"] == \"studio_test\" and e[\"working_space\"] == \"lin\" and not e[\"working_space_auto\"]"'
check "autre configuration : signalée" 'starts "$L" "this project was made with the OCIO config \"studio_test\", current is \"$ACES\""'
check "espace du projet inconnu : erreur, sans repli" 'starts "$L" "working space \"lin\" of the project:" && starts "$L" "(choose another one in the Color menu, Working Space)" && has "$L" "t: opened ws=[] origin=[] mode=2 name=[lin]"'
check "mode ou nom inconnus refusés" 'has "$L" "t: bad mode=0" && starts "$L" "unknown working space mode 7" && has "$L" "t: bad name=0" && starts "$L" "unknown OCIO color space \"nope\", working space of the project not changed"'
check "automatique, puis un alias" 'has "$L" "t: auto=1" && has "$L" "t: auto ws=[$LIN] origin=[project, automatic] mode=1 name=[]" && has "$L" "t: named=1" && has "$L" "t: named ws=[$LIN] origin=[project] mode=2 name=[lin_srgb]"'

echo "== 3. Textures reconverties"
prefs '{"version": 1}'
mkdir -p "$W/packed" "$W/moved/sub"
cat > "$W/r1.c" << EOF
int main() {
	script_project_new();
	script_import_asset("$W/ramp.png", 0);
	script_import_asset("$W/wood_normal.png", 0);
$(dmp auto "$W/ramp.png" "$W/r1_auto.stx")
$(dmp nor "$W/wood_normal.png" "$W/r1_nor.stx")
	console_log(string("t: acescg=%d", studio_ocio_set_project_working_space(2, "ACEScg")));
$(dmp acescg "$W/ramp.png" "$W/r1_acescg.stx")
$(dmp nor2 "$W/wood_normal.png" "$W/r1_nor2.stx")
	console_log(string("t: cs=[%s] source=[%s]", studio_texture_color_space("$W/ramp.png"), studio_texture_color_space_source("$W/ramp.png")));
	console_log(string("t: back=%d", studio_ocio_set_project_working_space(1, "")));
$(dmp back "$W/ramp.png" "$W/r1_back.stx")
	console_log(string("t: same=%d", studio_ocio_set_project_working_space(1, "")));
	console_log(string("t: config=%d", studio_ocio_set_config_preference("ocio://cg-config-latest")));
$(dmp cg "$W/ramp.png" "$W/r1_cg.stx")
	console_log(string("t: pack=%d", studio_project_pack_assets(1)));
	project_filepath_set("$W/packed/p.arm");
	project_save(0);
	return 0;
}
EOF
STUDIO_PREFS=$P app --background --script "$W/r1.c" > "$W/r1.log" 2>&1 || true
L=$W/r1.log
grep -E "^t: |converted to|asset pack" "$L" | cut -c1-200
check "import : converti vers l automatique" 'has "$L" "t: dump auto=1" && px convert "$W/r1_auto.stx" "$W/ramp.png" "$SRGB"'
check "espace du projet ACEScg : texture reconvertie depuis son fichier, même espace source" 'has "$L" "t: acescg=1" && has "$L" "studio: 1 texture(s) converted to the working space \"ACEScg\", layers kept as painted" && px convert --working=ACEScg "$W/r1_acescg.stx" "$W/ramp.png" "$SRGB" && has "$L" "t: cs=[$SRGB] source=[default]"'
check "donnée : pas rechargée" 'px same "$W/r1_nor.stx" "$W/wood_normal.png" && px same "$W/r1_nor2.stx" "$W/wood_normal.png"'
check "retour à l automatique ; même choix : rien à refaire" 'has "$L" "t: back=1" && px convert "$W/r1_back.stx" "$W/ramp.png" "$SRGB" && has "$L" "t: same=1" && [ "$(grep -c "converted to the working space \"$LIN\"" "$L")" -eq 2 ]'
check "configuration changée : textures reconverties" 'has "$L" "t: config=1" && px convert "$W/r1_cg.stx" "$W/ramp.png" "$SRGB" && pj "d[\"ocio.config\"] == \"ocio://cg-config-latest\""'
check "enregistré avec le paquet d assets" 'has "$L" "t: pack=0" && [ -f "$W/packed/p.arm" ] && entry "$W/packed/p.arm" "e[\"config_source\"] == \"ocio://cg-config-latest\" and e[\"working_space_auto\"]"'

# Le .arm ailleurs, sans les fichiers : les textures du paquet gardent leurs pixels, c'est signalé.
cp "$W/packed/p.arm" "$W/moved/sub/p.arm" 2> /dev/null || true
cat > "$W/r2.c" << EOF
int main() {
$(dmp before "$W/moved/ramp.png" "$W/r2_before.stx")
	console_log(string("t: acescg=%d", studio_ocio_set_project_working_space(2, "ACEScg")));
$(dmp after "$W/moved/ramp.png" "$W/r2_after.stx")
	return 0;
}
EOF
STUDIO_PREFS=$P app "$W/moved/sub/p.arm" --background --script "$W/r2.c" > "$W/r2.log" 2>&1 || true
L=$W/r2.log
check "paquet sans fichier : pixels gardés, signalé" 'has "$L" "t: acescg=1" && starts "$L" "studio: 1 texture(s) only in the asset pack of the project keep their pixels" && ! grep -q "converted to the working space" "$L" && px convert --opaque "$W/r2_before.stx" "$W/ramp.png" "$SRGB" && cmp -s "$W/r2_before.stx" "$W/r2_after.stx"'

echo "== 4. Viewport"
prefs '{"version": 1}'
printf 'int main() {\n\tconsole_log(string("t: set=%%d", studio_ocio_view_set("sRGB - Display", "Un-tone-mapped", "")));\n\treturn 0;\n}\n' > "$W/v1.c"
cat > "$W/v2.c" << 'EOF'
int main() {
	console_log(string("t: view=[%s] [%s] [%s] enabled=%d", studio_ocio_view_display(), studio_ocio_view_view(), studio_ocio_view_look(), studio_ocio_view_enabled()));
	return 0;
}
EOF
STUDIO_PREFS=$P app --background --script "$W/v1.c" > "$W/v1.log" 2>&1 || true
STUDIO_PREFS=$P app --background --script "$W/v2.c" > "$W/v2.log" 2>&1 || true
(STUDIO_OCIO_DISPLAY="Display P3 - Display" STUDIO_PREFS=$P app --background --script "$W/v2.c" > "$W/v3.log" 2>&1 || true)
"$PY" -c 'import json, sys; d = json.load(open(sys.argv[1])); d["ocio.viewport"] = False; json.dump(d, open(sys.argv[1], "w"))' "$P"
(STUDIO_OCIO_VIEWPORT=1 STUDIO_PREFS=$P app --background --script "$W/v2.c" > "$W/v4.log" 2>&1 || true)
STUDIO_PREFS=$P app --background --script "$W/v2.c" > "$W/v5.log" 2>&1 || true
check "choix du viewport écrit" 'has "$W/v1.log" "t: set=1" && pj "d[\"ocio.display\"] == \"sRGB - Display\" and d[\"ocio.view\"] == \"Un-tone-mapped\" and d[\"ocio.look\"] == \"\""'
check "relu au démarrage" 'has "$W/v2.log" "t: view=[sRGB - Display] [Un-tone-mapped] [] enabled=1"'
check "variables d environnement d abord, ensemble" 'has "$W/v3.log" "t: view=[Display P3 - Display] [ACES 2.0 - SDR 100 nits (P3 D65)] [] enabled=1"'
check "viewport éteint dans les préférences, allumé par la variable" 'has "$W/v5.log" "t: view=[sRGB - Display] [Un-tone-mapped] [] enabled=0" && has "$W/v4.log" "t: view=[sRGB - Display] [Un-tone-mapped] [] enabled=1"'
check "préférences inchangées par les lectures" 'pj "d[\"ocio.view\"] == \"Un-tone-mapped\" and d[\"ocio.viewport\"] is False"'

[ $FAIL -eq 0 ] && echo "ocio_prefs_app: tout passe" || echo "ocio_prefs_app: ÉCHECS"
exit $FAIL
