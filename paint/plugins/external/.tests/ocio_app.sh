#!/bin/sh
# Lot A1 dans ArmorPaint lui-même (--background --script) : OCIO lié dans l'app, configuration choisie au démarrage
# (défaut intégré, $OCIO, $OCIO invalide), API de scripting, rechargement. Les valeurs attendues viennent de
# PyOpenColorIO (.ocio/venv, créé par ocio.sh).
#
#   sh .tests/ocio_app.sh [dossier de travail]
#   APP=<binaire ArmorPaint> pour un autre build (défaut : build Release de paint/).
set -eu
T=$(cd "$(dirname "$0")" && pwd)
EXT=$(dirname "$T")
APP=${APP:-$EXT/../../build/build/Release/ArmorPaint.app/Contents/MacOS/ArmorPaint}
PY=$EXT/.ocio/venv/bin/python
W=${1:-$(mktemp -d)}
mkdir -p "$W/cfg"
W=$(cd "$W" && pwd)
[ -x "$APP" ] || { echo "ArmorPaint not found: $APP" >&2; exit 1; }
[ -x "$PY" ] || { echo "PyOpenColorIO absent : lancer d'abord .tests/ocio.sh" >&2; exit 1; }
FAIL=0

"$PY" "$T/ocio_ref.py" --make-config "$W/cfg"
cat > "$W/ocio.c" << EOF
int main() {
	console_log(string("status=%s", studio_ocio_status()));
	console_log(string("rgb=%s", studio_ocio_transform_rgb("scene_linear", "color_picking", 0.18, 0.5, 1.0)));
	console_log(string("resolve=%s", studio_ocio_resolve("scene_linear")));
	console_log(string("is_data=%d", studio_ocio_is_data(studio_ocio_resolve("data"))));
	console_log(string("from_path=%s", studio_ocio_colorspace_from_path("robot_nor.exr")));
	console_log(string("reload=%d %s", studio_ocio_reload("$W/cfg/config.ocio"), studio_ocio_status()));
	console_log(string("reload_rgb=%s", studio_ocio_transform_rgb("scene_linear", "color_picking", 0.18, 0.5, 1.0)));
	console_log(string("reload_default=%d %s", studio_ocio_reload(""), studio_ocio_status()));
	return 0;
}
EOF

# expected <config> : "r g b" de scene_linear -> color_picking, comme studio_ocio_transform_rgb.
expected() {
	"$PY" -c '
import sys, PyOpenColorIO as O
c = O.Config.CreateFromFile(sys.argv[1])
p = c.getProcessor("scene_linear", "color_picking").getDefaultCPUProcessor()
print(" ".join("%.9g" % v for v in p.applyRGB([0.18, 0.5, 1.0])))' "$1"
}
DEF=$(expected ocio://studio-config-latest)
TST=$(expected "$W/cfg/config.ocio")

# run <nom> <attendu>… : lance ArmorPaint (environnement déjà posé), exige rc 0 et chaque ligne attendue dans la sortie,
# entière ; terminée par « … », son début seulement.
run() {
	name=$1
	shift
	out=$(perl -e 'alarm 60; exec @ARGV' "$APP" --background --script "$W/ocio.c" -ApplePersistenceIgnoreState YES 2>&1) &&
		rc=0 || rc=$?
	ok=1
	[ $rc -eq 0 ] || ok=0
	for want in "$@"; do
		case $want in
		*…) printf '%s\n' "$out" | grep -qF -- "${want%…}" ;; # préfixe (la suite de la ligne varie)
		*) printf '%s\n' "$out" | grep -qxF -- "$want" ;;
		esac || { ok=0; echo "     manque : $want"; }
	done
	if [ $ok -eq 1 ]; then
		echo "ok   $name"
	else
		echo "FAIL $name (rc $rc)"
		printf '%s\n' "$out" | grep '^studio: OCIO\|^status=\|^rgb=\|^reload' | sed 's/^/     /'
		FAIL=1
	fi
}

echo "== OCIO dans l'app"
unset OCIO
run "défaut intégré (ACES studio)" \
	"studio: OCIO 2.5.2, config \"studio-config-v4.0.0_aces-v2.0_ocio-v2.5\" from ocio://studio-config-latest (built-in default)…" \
	"rgb=$DEF" "resolve=ACEScg" "is_data=1" \
	"reload=1 OCIO 2.5.2, config \"studio_test\" from $W/cfg/config.ocio (explicit)…" "reload_rgb=$TST" \
	"reload_default=1 OCIO 2.5.2, config \"studio-config-v4.0.0_aces-v2.0_ocio-v2.5\" from ocio://studio-config-latest (built-in default)…"
OCIO=$W/cfg/config.ocio run "\$OCIO de test" \
	"studio: OCIO 2.5.2, config \"studio_test\" from $W/cfg/config.ocio (\$OCIO)…" \
	"rgb=$TST" "resolve=lin" "is_data=1" "from_path=raw" \
	"reload_default=1 OCIO 2.5.2, config \"studio_test\" from $W/cfg/config.ocio (\$OCIO)…"
OCIO=$W/missing.ocio run "\$OCIO invalide : pas de repli, l'app démarre" \
	"studio: OCIO unavailable: config $W/missing.ocio (\$OCIO) rejected…" \
	"rgb=" "resolve=" "is_data=-1" \
	"reload=1 OCIO 2.5.2, config \"studio_test\"…" "reload_default=0 OCIO unavailable: config $W/missing.ocio (\$OCIO) rejected…"

[ $FAIL -eq 0 ] && echo "ocio_app: tout passe" || echo "ocio_app: ÉCHECS"
exit $FAIL
