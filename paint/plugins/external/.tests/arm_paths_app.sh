#!/bin/sh
# Chemins des fichiers d'un projet (STUDIO-PATCH(arm-relative-paths)), dans ArmorPaint sans interface. Un projet avec
# maillage source, texture et envmap pris dans src/ :
#   1. enregistré deux fois dans la même session, dans a/ puis dans b/deep/ (Save As) : chaque fichier a des chemins
#      relatifs à son dossier vers src/, et l'envmap reste absolue en mémoire. Upstream : après le premier
#      enregistrement, maillage et envmap gardaient en mémoire leur chemin relatif à a/, faux depuis b/deep/ (et pour
#      Reimport Mesh) ;
#   2. celui de a/ rouvert et enregistré dans c/ : ses chemins vont à src/ sans passer par a/ (normalisés) ; a/
#      renommé, celui de c/ rouvert retrouve texture et envmap. Upstream : ../a/../src/wood.png, perdu avec a/.
#
#   sh .tests/arm_paths_app.sh [dossier de travail]
#   APP=<binaire ArmorPaint> pour un autre build (défaut : build Release de paint/).
set -eu
T=$(cd "$(dirname "$0")" && pwd)
EXT=$(dirname "$T")
APP=${APP:-$EXT/../../build/build/Release/ArmorPaint.app/Contents/MacOS/ArmorPaint}
PY=${PY:-python3}
W=${1:-$(mktemp -d)}
mkdir -p "$W"
W=$(cd "$W" && pwd)
[ -x "$APP" ] || { echo "ArmorPaint not found: $APP" >&2; exit 1; }
export STUDIO_ENV= # lot E5 : aucun studio.env, pas même celui de l'utilisateur
FAIL=0
ok() { echo "ok   $1"; }
ko() { echo "FAIL $1"; FAIL=1; }
check() { if eval "$2"; then ok "$1"; else ko "$1"; fi; }

rm -rf "$W/src" "$W/a" "$W/a.away" "$W/b" "$W/c" "$W"/*.log "$W"/*.c
mkdir -p "$W/src" "$W/a" "$W/b/deep" "$W/c"
"$PY" - "$W/src" <<'EOF'
import os, struct, sys, zlib
d = sys.argv[1]
open(os.path.join(d, "robot.obj"), "w").write("o robot\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                                              "vt 0.1 0.1\nvt 0.9 0.1\nvt 0.9 0.9\nvt 0.1 0.9\nf 1/1 2/2 3/3 4/4\n")
raw = b"".join(b"\0" + bytes((140, 90, 40, 255)) * 4 for _ in range(4))
ch = lambda t, x: struct.pack(">I", len(x)) + t + x + struct.pack(">I", zlib.crc32(t + x))
open(os.path.join(d, "wood.png"), "wb").write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 6, 0, 0, 0))
                                             + ch(b"IDAT", zlib.compress(raw)) + ch(b"IEND", b""))
open(os.path.join(d, "sky.hdr"), "wb").write(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 8 +X 16\n" + bytes((128, 128, 160, 129)) * 128)
EOF

# ArmorPaint sans interface, sur le projet $2 s'il est donné ; script $1.c (entrée standard), ses lignes « t: » dans
# $W/$1.log.
background() {
	cat > "$W/$1.c"
	perl -e 'alarm 300; exec @ARGV' "$APP" ${2:+"$2"} --background --script "$W/$1.c" -ApplePersistenceIgnoreState YES \
		> "$W/$1.log" 2>&1 || ko "$1 : ArmorPaint rc $? (voir $W/$1.log)"
}
line() { sed -n "s/^t: $2 //p" "$W/$1.log"; }
norm() { "$PY" -c 'import os, sys; print(os.path.normpath(sys.argv[1]))' "$1"; }
# check_paths <nom> <.arm> : ses chemins désignent le maillage, la texture et l'envmap de src/, relatifs à son dossier
# et normalisés (arm_paths.py).
check_paths() {
	if "$PY" "$T/arm_paths.py" "$2" "$W/src/robot.obj" "$W/src/wood.png" "$W/src/sky.hdr" > "$W/check.out" 2>&1; then
		ok "$1"
	else
		ko "$1"
		sed 's/^/     /' "$W/check.out" | tail -4
	fi
}

echo "== 1. enregistré dans a/ puis dans b/deep/ pendant la même session"
# Un enregistrement se fait à l'image suivante (project_save) : le second attend quelques images.
background saves <<EOF
int frame = 0;
void update() {
	frame++;
	if (frame == 5) {
		project_filepath_set("$W/b/deep/q.arm");
		project_save(false);
	}
	if (frame == 10) {
		project_t *p = script_get_project();
		console_log(string("t: envmap %s", p->envmap));
		script_quit();
	}
}
int main() {
	script_project_new();
	studio_import_mesh("$W/src/robot.obj", 0);
	script_import_asset("$W/src/wood.png", 0);
	script_import_asset("$W/src/sky.hdr", 1);
	project_filepath_set("$W/a/p.arm");
	project_save(false);
	script_notify_on_update(update);
	return 0;
}
EOF
check_paths "a/p.arm : maillage, texture et envmap relatifs vers src/" "$W/a/p.arm"
check_paths "b/deep/q.arm, enregistré ensuite : relatifs à son propre dossier" "$W/b/deep/q.arm"
check "envmap toujours absolue en mémoire après les enregistrements" '[ "$(line saves envmap)" = "$W/src/sky.hdr" ]'

echo "== 2. a/p.arm rouvert, enregistré dans c/, puis a/ renommé"
background resave "$W/a/p.arm" <<EOF
int frame = 0;
void update() {
	if (++frame == 5) {
		script_quit();
	}
}
int main() {
	project_filepath_set("$W/c/r.arm");
	project_save(false);
	script_notify_on_update(update);
	return 0;
}
EOF
check_paths "c/r.arm : relatifs vers src/, sans passer par a/" "$W/c/r.arm"
mv "$W/a" "$W/a.away"
background reopen "$W/c/r.arm" <<'EOF'
int main() {
	project_t *p = script_get_project();
	console_log(string("t: envmap %s", p->envmap));
	return 0;
}
EOF
mv "$W/a.away" "$W/a"
check "c/r.arm rouvert sans a/ : texture et envmap trouvées" \
	'[ "$(norm "$(line reopen envmap)")" = "$W/src/sky.hdr" ] && ! grep -q "Could not locate texture" "$W/reopen.log"'

[ $FAIL -eq 0 ] && echo "arm_paths_app: tout passe" || echo "arm_paths_app: ÉCHECS (journaux dans $W)"
exit $FAIL
