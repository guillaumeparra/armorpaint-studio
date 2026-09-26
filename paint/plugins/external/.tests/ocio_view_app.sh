#!/bin/sh
# Lot A2 dans ArmorPaint lui-même (--background) : transformation d'affichage OCIO du viewport. Des couleurs passent
# par le vrai compositeur (studio_ocio_view_probe : shader compositor_pass, LUT sur le GPU) et sont comparées à
# PyOpenColorIO (ocio_view_check.py) ; puis le choix display/view/look, les erreurs, le retour au tonemap filmique
# d'ArmorPaint, les modes sans tonemap, le rechargement de configuration et les variables d'environnement.
#
#   sh .tests/ocio_view_app.sh [dossier de travail]
#   APP=<binaire ArmorPaint> pour un autre build (défaut : build Release de paint/).
# Demande .ocio/venv (créé par ocio.sh) avec numpy : .ocio/venv/bin/pip install numpy
set -eu
T=$(cd "$(dirname "$0")" && pwd)
EXT=$(dirname "$T")
APP=${APP:-$EXT/../../build/build/Release/ArmorPaint.app/Contents/MacOS/ArmorPaint}
PY=$EXT/.ocio/venv/bin/python
W=${1:-$(mktemp -d)}
mkdir -p "$W"
W=$(cd "$W" && pwd)
[ -x "$APP" ] || { echo "ArmorPaint not found: $APP" >&2; exit 1; }
"$PY" -c 'import PyOpenColorIO, numpy' 2> /dev/null ||
	{ echo "venv incomplet : lancer .tests/ocio.sh puis $EXT/.ocio/venv/bin/pip install numpy" >&2; exit 1; }
FAIL=0
ok() { echo "ok   $1"; }
ko() { echo "FAIL $1"; FAIL=1; }
check() { if eval "$2"; then ok "$1"; else ko "$1"; fi; }
has() { grep -qxF "$2" "$1"; }
app() { perl -e 'alarm 120; exec @ARGV' "$APP" "$@" -ApplePersistenceIgnoreState YES; }
unset OCIO STUDIO_OCIO_WORKING_SPACE STUDIO_OCIO_LIB STUDIO_OCIO_VIEWPORT STUDIO_OCIO_DISPLAY STUDIO_OCIO_VIEW STUDIO_OCIO_LOOK

rm -rf "$W/cfg"
mkdir -p "$W/cfg"
"$PY" "$T/ocio_ref.py" --make-config "$W/cfg"

# Couleurs sondées (0xAARRGGBB) : gris, blanc, noir, primaires saturées, couleurs de peau et de matériaux.
COLORS="0xff2e2e2e 0xff808080 0xffffffff 0xff000000 0xff0a0a0a 0xffff0000 0xff00ff00 0xff0000ff 0xffffff00 0xffc08060 0xff3060a0 0xff20c040"
# probes <étiquette> : lignes « p <étiquette> <couleur> r g b » (compositeur) et « a … » (studio_ocio_view_apply_rgb,
# la LUT sur le CPU, pour l'entrée stockée en half qu'a lue le compositeur).
probes() {
	for c in $COLORS; do
		printf '\tprobe("%s", %s);\n' "$1" "$c"
	done
}

cat > "$W/run.c" << EOF
void probe(char *label, int c) {
	console_log(string("p %s %d %s", label, c, studio_ocio_view_probe(c)));
	float r = ((c >> 16) & 255) / 255.0;
	float g = ((c >> 8) & 255) / 255.0;
	float b = (c & 255) / 255.0;
	console_log(string("a %s %d %s", label, c, studio_ocio_view_apply_rgb(r, g, b)));
}
void state(char *label) {
	console_log(string("t: %s enabled=%d active=%d", label, studio_ocio_view_enabled(), studio_ocio_view_active()));
	console_log(string("t: %s choice=%s|%s|%s", label, studio_ocio_view_display(), studio_ocio_view_view(), studio_ocio_view_look()));
	console_log(string("t: %s status=%s", label, studio_ocio_view_status()));
}
int main() {
	state("default");
$(probes aces)
	console_log(string("t: set untonemapped=%d", studio_ocio_view_set("sRGB - Display", "Un-tone-mapped", "")));
$(probes untonemapped)
	console_log(string("t: set look=%d", studio_ocio_view_set("", "", "ACES 1.3 Reference Gamut Compression")));
	state("look");
$(probes look)
	console_log(string("t: set p3=%d", studio_ocio_view_set("Display P3 - Display", "", "")));
	state("p3");
$(probes p3)
	console_log(string("t: set bad view=%d", studio_ocio_view_set("sRGB - Display", "nope", "")));
	console_log(string("t: set bad display=%d", studio_ocio_view_set("nope", "", "")));
	console_log(string("t: set bad look=%d", studio_ocio_view_set("sRGB - Display", "", "nope")));
	state("after bad");
	console_log(string("t: set default=%d", studio_ocio_view_set("", "", "")));
	console_log(string("t: off=%d", studio_ocio_view_set_enabled(0)));
	state("off");
$(probes filmic)
	console_log(string("t: on=%d", studio_ocio_view_set_enabled(1)));
	context_set_viewport_mode(1);
$(probes basecolor)
	context_set_viewport_mode(0);
	console_log(string("t: reload cg=%d", studio_ocio_reload("ocio://cg-config-latest")));
	state("cg");
$(probes cg)
	console_log(string("t: reload tst=%d", studio_ocio_reload("$W/cfg/config.ocio")));
	state("tst");
$(probes tst)
	console_log(string("t: reload default=%d", studio_ocio_reload("")));
	state("back");
	return 0;
}
EOF
app --background --script "$W/run.c" > "$W/run.log" 2>&1 || true
grep -E "^t: " "$W/run.log" | cut -c1-240
ACES="sRGB - Display|ACES 2.0 - SDR 100 nits (Rec.709)|"
check "actif par défaut, display et view par défaut" 'has "$W/run.log" "t: default enabled=1 active=1" && has "$W/run.log" "t: default choice=$ACES"'
check "statut : LUT 129^3" 'grep -q "^t: default status=OCIO viewport: .*LUT 129^3 in" "$W/run.log"'
check "look choisi, display et view par défaut" 'has "$W/run.log" "t: set look=1" && has "$W/run.log" "t: look choice=${ACES}ACES 1.3 Reference Gamut Compression"'
check "display seul : sa view par défaut" 'has "$W/run.log" "t: set p3=1" && has "$W/run.log" "t: p3 choice=Display P3 - Display|ACES 2.0 - SDR 100 nits (P3 D65)|"'
check "view, display ou look inconnus : refusés, choix inchangé" 'has "$W/run.log" "t: set bad view=0" && has "$W/run.log" "t: set bad display=0" && has "$W/run.log" "t: set bad look=0" && has "$W/run.log" "t: after bad choice=Display P3 - Display|ACES 2.0 - SDR 100 nits (P3 D65)|"'
check "désactivé : tonemap filmique" 'has "$W/run.log" "t: off=0" && has "$W/run.log" "t: off enabled=0 active=0" && grep -q "^t: off status=OCIO viewport off" "$W/run.log"'
check "rechargement : choix gardé s il existe" 'has "$W/run.log" "t: reload cg=1" && has "$W/run.log" "t: cg enabled=1 active=1" && has "$W/run.log" "t: cg choice=$ACES" && grep -q "^t: cg status=.*cg-config\|^t: cg status=OCIO viewport: " "$W/run.log"'
check "configuration sans espace de travail : inactif, filmique" 'has "$W/run.log" "t: reload tst=1" && has "$W/run.log" "t: tst enabled=1 active=0" && grep -q "^t: tst status=OCIO viewport unavailable: working space unknown" "$W/run.log"'
check "retour à la configuration par défaut" 'has "$W/run.log" "t: back enabled=1 active=1" && has "$W/run.log" "t: back choice=$ACES"'
echo "-- compositeur (GPU) contre PyOpenColorIO"
if grep -E "^[pa] " "$W/run.log" | "$PY" "$T/ocio_view_check.py" | sed 's/^/     /'; then
	ok "compositeur = PyOpenColorIO, aux tolérances de la LUT"
else
	ko "compositeur = PyOpenColorIO, aux tolérances de la LUT"
fi

# Variables d'environnement : désactivé au démarrage ; display et view choisis, view invalide remplacée.
cat > "$W/env.c" << 'EOF'
int main() {
	console_log(string("t: env enabled=%d active=%d", studio_ocio_view_enabled(), studio_ocio_view_active()));
	console_log(string("t: env choice=%s|%s|%s", studio_ocio_view_display(), studio_ocio_view_view(), studio_ocio_view_look()));
	return 0;
}
EOF
# Sous-shells : en sh, une affectation devant l'appel d'une fonction survit à l'appel.
(STUDIO_OCIO_VIEWPORT=0 app --background --script "$W/env.c" > "$W/env0.log" 2>&1 || true)
check "STUDIO_OCIO_VIEWPORT=0 : désactivé" 'has "$W/env0.log" "t: env enabled=0 active=0"'
(STUDIO_OCIO_DISPLAY="Display P3 - Display" STUDIO_OCIO_VIEW=Raw STUDIO_OCIO_LOOK="ACES 1.3 Reference Gamut Compression" \
	app --background --script "$W/env.c" > "$W/env1.log" 2>&1 || true)
check "STUDIO_OCIO_DISPLAY, _VIEW, _LOOK" 'has "$W/env1.log" "t: env enabled=1 active=1" && has "$W/env1.log" "t: env choice=Display P3 - Display|Raw|ACES 1.3 Reference Gamut Compression"'
(STUDIO_OCIO_DISPLAY="Display P3 - Display" STUDIO_OCIO_VIEW=nope app --background --script "$W/env.c" > "$W/env2.log" 2>&1 || true)
check "STUDIO_OCIO_VIEW inconnue : view par défaut du display, journalisé" 'has "$W/env2.log" "t: env choice=Display P3 - Display|ACES 2.0 - SDR 100 nits (P3 D65)|" && grep -q "unknown to the config" "$W/env2.log"'

[ $FAIL -eq 0 ] && echo "ocio view app: OK ($W)" || { echo "ocio view app: FAILED ($W)"; exit 1; }
