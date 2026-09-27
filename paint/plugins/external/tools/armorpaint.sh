#!/bin/sh
# Ouvre la version studio d'ArmorPaint (branche studio, plugins external/ compris), toujours à jour :
#   1. construit libstudio_ocio si elle manque (tools/build_ocio.sh, réseau requis la première fois) ;
#   2. recompile l'app (incrémental : ~3 s sans changement) ;
#   3. lance le build Release, détaché du terminal, sans la boîte « rouvrir les fenêtres ? » de macOS.
#
# Usage : tools/armorpaint.sh [arguments d'ArmorPaint : projet .arm, mesh…]
#         tools/armorpaint.sh --shotgrid-demo [dossier de travail]
#   --shotgrid-demo : sur le site ShotGrid simulé (production « Demo », .tests/demo_workfiles.sh), avec tk-core, les
#   apps Workfiles et Publish et le python PySide6 de .shotgrid/ (tools/setup_shotgrid.sh crée le venv s'il manque).
#   Sans lui, ShotGrid demande un vrai tk-core (STUDIO_SGTK_PATH ou le cache de ShotGrid Desktop) : sinon
#   toolkit_unavailable.
# La fenêtre d'export (File > Export > Textures) demande Python avec PySide6 : sans STUDIO_PYTHON, celui du venv de
# .shotgrid/ (créé par tools/setup_shotgrid.sh s'il manque). En studio, rez fournit STUDIO_PYTHON.
# Les variables d'environnement (OCIO, STUDIO_*) passent à l'app. Journal de l'app : $TMPDIR/armorpaint-studio.log
set -e
EXT=$(cd "$(dirname "$0")/.." && pwd)
PAINT=$(cd "$EXT/../.." && pwd)
APP=$PAINT/build/build/Release/ArmorPaint.app/Contents/MacOS/ArmorPaint
LOG=${TMPDIR:-/tmp}/armorpaint-studio.log

if [ "$(uname -s)" = Darwin ] && [ ! -f "$EXT/.ocio/lib/libstudio_ocio.a" ]; then
	echo "== libstudio_ocio (premier build, quelques minutes)"
	"$EXT/tools/build_ocio.sh"
fi

echo "== build"
if ! (cd "$PAINT" && ../base/make --compile) > "$LOG.build" 2>&1 || ! grep -q "BUILD SUCCEEDED" "$LOG.build"; then
	grep -E ": error:|error:|FAILED" "$LOG.build" | head -20
	echo "build échoué, journal complet : $LOG.build" >&2
	exit 1
fi

[ -x "$APP" ] || { echo "ArmorPaint introuvable : $APP" >&2; exit 1; }

SG=$EXT/.shotgrid
if [ -z "${STUDIO_PYTHON:-}" ]; then
	"$SG/venv/bin/python" -c "import PySide6" 2> /dev/null || "$EXT/tools/setup_shotgrid.sh"
	STUDIO_PYTHON=$SG/venv/bin/python
	export STUDIO_PYTHON
fi

if [ "$1" = --shotgrid-demo ]; then
	shift
	[ -d "$SG/tk-core/python/sgtk" ] && [ -d "$SG/tk-framework-qtwidgets" ] && "$SG/venv/bin/python" -c "import PySide6" 2> /dev/null ||
		"$EXT/tools/setup_shotgrid.sh"
	echo "== ArmorPaint, démo ShotGrid (journal : $LOG)"
	STUDIO_TEST_TK_CORE=$SG/tk-core STUDIO_TEST_TK_APPS=$SG STUDIO_SG_PYTHON=$SG/venv/bin/python APP=$APP \
		nohup "$EXT/.tests/demo_workfiles.sh" "$@" > "$LOG" 2>&1 &
	exit 0
fi
echo "== ArmorPaint (journal : $LOG)"
nohup "$APP" "$@" -ApplePersistenceIgnoreState YES > "$LOG" 2>&1 &
