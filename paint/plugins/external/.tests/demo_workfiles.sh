#!/bin/sh
# Démo de Workfiles (tk-multi-workfiles2 dans le moteur tk-armorpaint, studio_workfiles.c) sur le site ShotGrid simulé
# (sg_fixture.py, scénario "demo") : une production "Demo" avec des Assets et des Shots, une vraie config Toolkit et sa
# structure de dossiers sous <dossier>/storage. Au premier lancement, quelques versions sont créées par ArmorPaint
# lui-même (--background) aux chemins que donne Toolkit (next_version), puis l'app s'ouvre sur File Open.
# Ensuite : Ctrl+Alt+O (Cmd+Alt+O) File Open, Ctrl+Alt+S (Cmd+Alt+S) File Save.
#
# Usage : STUDIO_TEST_TK_CORE=<clone de tk-core> STUDIO_TEST_TK_APPS=<dossier des clones> \
#         STUDIO_SG_PYTHON=<python avec PySide6> .tests/demo_workfiles.sh [dossier de travail]
#   STUDIO_TEST_TK_APPS contient tk-multi-workfiles2, tk-framework-shotgunutils et tk-framework-qtwidgets (clones git
#   de github.com/shotgunsoftware, sur un tag) ; STUDIO_SG_PYTHON est l'interpréteur du sidecar, qui affiche les
#   fenêtres Qt (python3 -m venv <v> && <v>/bin/pip install PySide6).
#   APP=<binaire ArmorPaint> pour un autre build (défaut : build Release de paint/).
# Le dossier de travail est conservé : les versions sauvées pendant la démo y restent (rm -rf pour repartir de zéro).

set -e
T=$(cd "$(dirname "$0")" && pwd)
EXT=$(dirname "$T")
W=${1:-${TMPDIR:-/tmp}/studio_workfiles_demo}
APP=${APP:-$EXT/../../build/build/Release/ArmorPaint.app/Contents/MacOS/ArmorPaint}
if [ -z "$STUDIO_TEST_TK_CORE" ] || [ ! -d "$STUDIO_TEST_TK_CORE/python/sgtk" ]; then
	echo "STUDIO_TEST_TK_CORE must point to a tk-core checkout (see sg.sh)" >&2
	exit 1
fi
for app in tk-multi-workfiles2 tk-framework-shotgunutils tk-framework-qtwidgets; do
	[ -d "$STUDIO_TEST_TK_APPS/$app" ] || { echo "STUDIO_TEST_TK_APPS must contain $app (see the header)" >&2; exit 1; }
done
"${STUDIO_SG_PYTHON:=python3}" -c "import PySide6" 2>/dev/null || {
	echo "STUDIO_SG_PYTHON ($STUDIO_SG_PYTHON) has no PySide6 (see the header)" >&2
	exit 1
}
[ -x "$APP" ] || { echo "ArmorPaint not found: $APP" >&2; exit 1; }
mkdir -p "$W"
W=$(cd "$W" && pwd)

export STUDIO_SG_BRIDGE="$T/sg_fixture.py"
export STUDIO_TEST_WORKDIR="$W"
export STUDIO_TEST_SCENARIO=demo
export STUDIO_TEST_TK_CORE STUDIO_TEST_TK_APPS STUDIO_SG_PYTHON

# Une version : nouvelle scène, un matériau uni sur le calque, sauvée au chemin de next_version (une sauvegarde par
# lancement : export_arm_run_project part à la frame suivante).
seed() { # tâche nom r g b
	cat > "$W/seed.c" <<EOF
int main() {
	int j = studio_sg_run("next_version", "{\"task_id\":$1,\"name\":\"$2\"}", 0);
	if (studio_sg_ok(j) == 0) {
		console_log(string("seed failed: %s", studio_sg_result(j)));
		return 1;
	}
	char *path = studio_sg_get(j, "/data/path");
	script_project_new();
	slot_material_t *m = script_material_create("$2");
	ui_node_t *rgb = script_material_create_node_at("RGB", -400.0, 0.0);
	ui_node_t *out = script_material_get_node("OUTPUT_MATERIAL_PBR");
	script_material_set_color(rgb, 0, 0, $3, $4, $5, 1.0);
	script_material_connect(rgb, 0, out, 0);
	script_material_update();
	script_material_set(m);
	script_fill_layer();
	project_filepath_set(path);
	project_save(false);
	console_log(string("seed: %s", path));
	return 0;
}
EOF
	"$APP" --background --script "$W/seed.c" -ApplePersistenceIgnoreState YES 2>&1 | grep "seed" || true
}

if [ ! -f "$W/.seeded" ]; then
	echo "seeding versions in $W/storage (ArmorPaint --background)"
	seed 1 robot 0.85 0.2 0.1   # robot (Prop) / Texture : v001 rouge
	seed 1 robot 0.1 0.35 0.9   # v002 bleu
	seed 5 drone 0.2 0.75 0.3   # drone (Prop) / Texture : v001 vert
	seed 8 lookdev 0.95 0.8 0.1 # sq010_0020 / Lookdev : v001 jaune
	rm -f "$W/seed.c"
	touch "$W/.seeded"
fi
echo "storage:"
(cd "$W/storage" && find . -name "*.arm" | sort)

STUDIO_SG_WORKFILES_ON_START=1 exec "$APP" -ApplePersistenceIgnoreState YES
