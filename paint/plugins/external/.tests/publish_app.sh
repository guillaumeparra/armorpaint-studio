#!/bin/sh
# Publish ShotGrid (lot C4) dans ArmorPaint lui-même, sur le site simulé (sg_fixture.py, scénario demo) : tk-multi-publish2
# et les hooks du moteur (tk-armorpaint/hooks/tk-multi-publish2) ; le hook du sidecar publish_app_hook.py relève ce qui
# est publié (le site simulé vit le temps d'un lancement d'ArmorPaint).
#   1. projet UDIM (tuiles 1001 et 1002) avec une texture et une envmap importées, enregistré par ArmorPaint au chemin de
#      next_version (robot.v001.arm), puis une texture rangée à côté de lui, dans le dossier de travail (decal.png) ;
#      réglages d'export dans le sidecar du projet : preset basic par tuile, deux maps ;
#   2. publish sans fenêtre (op publish, lancé en job par un script) : projet copié par ArmorPaint dans le dossier
#      publish ; textures exportées (noms du template) puis copiées, une séquence <UDIM> par texture, de type UDIM Image,
#      dépendante du projet ; vignettes du viewport ; projet ouvert ensuite à sa version suivante (v002) ;
#   3. la copie publiée : chemins des textures, de l'envmap et du maillage source relatifs à son dossier et normalisés
#      (STUDIO-PATCH(arm-relative-paths)), decal.png désignée dans le dossier de travail (STUDIO-PATCH(project-copy) :
#      work/ et publish/ sont à la même profondeur, seul un fichier rangé dans l'un des deux distingue leurs chemins) ;
#      rouverte, elle trouve toutes ses textures ; sans le dossier de travail, celles de src/ ; une simple copie du
#      fichier les perd (témoin) ;
#   4. fenêtre Publish (show_app "Publish...", Qt offscreen) pilotée par le hook comme par l'artiste : v002 publié,
#      projet ouvert en v003.
#
#   sh .tests/publish_app.sh [dossier de travail]
#   APP=<binaire ArmorPaint> pour un autre build (défaut : build Release de paint/).
#   PY=<python avec PySide6> (défaut : .shotgrid/venv, tools/setup_shotgrid.sh). Toolkit : .shotgrid/ du dépôt.
set -eu
T=$(cd "$(dirname "$0")" && pwd)
EXT=$(dirname "$T")
SG=$EXT/.shotgrid
APP=${APP:-$EXT/../../build/build/Release/ArmorPaint.app/Contents/MacOS/ArmorPaint}
PY=${PY:-$SG/venv/bin/python}
W=${1:-$(mktemp -d)}
mkdir -p "$W"
W=$(cd "$W" && pwd)
[ -x "$APP" ] || { echo "ArmorPaint not found: $APP" >&2; exit 1; }
"$PY" -c "import PySide6" 2>/dev/null || { echo "PySide6 not found for $PY (tools/setup_shotgrid.sh)" >&2; exit 1; }
FAIL=0
ok() { echo "ok   $1"; }
ko() { echo "FAIL $1"; FAIL=1; }
check() { if eval "$2"; then ok "$1"; else ko "$1"; fi; }

# storage/ et le cache de chemins de Toolkit qui le décrit (shotgun_home/) repartent ensemble de zéro.
rm -rf "$W/storage" "$W/shotgun_home" "$W/src" "$W/naive" "$W"/*.log "$W"/*.json "$W"/*.c
mkdir -p "$W/src" "$W/naive"
export STUDIO_SG_BRIDGE="$T/sg_fixture.py" STUDIO_SG_PYTHON="$PY" STUDIO_TEST_WORKDIR="$W" STUDIO_TEST_SCENARIO=demo \
	STUDIO_TEST_TK_CORE="$SG/tk-core" STUDIO_TEST_TK_APPS="$SG" STUDIO_SG_HOOK="$T/publish_app_hook.py" \
	QT_QPA_PLATFORM=offscreen

"$PY" - "$W/src" <<'EOF'
import os, struct, sys, zlib
d = sys.argv[1]
# Maillage UDIM : un quad dans chacune des tuiles 1001 et 1002.
out = ["o robot"]
for k in range(2):
    out += ["v %g 0 0" % (1.2 * k), "v %g 0 0" % (1.2 * k + 1), "v %g 1 0" % (1.2 * k + 1), "v %g 1 0" % (1.2 * k)]
    out += ["vt %g 0.1" % (k + 0.1), "vt %g 0.1" % (k + 0.9), "vt %g 0.9" % (k + 0.9), "vt %g 0.9" % (k + 0.1)]
out += ["f %d/%d %d/%d %d/%d %d/%d" % (b + 1, b + 1, b + 2, b + 2, b + 3, b + 3, b + 4, b + 4) for b in (0, 4)]
open(os.path.join(d, "robot.obj"), "w").write("\n".join(out) + "\n")
# Texture (PNG 4x4) et envmap (Radiance HDR 16x8, sans compression)
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

# ArmorPaint avec son interface sur le projet $2 ; un script lance la requête $3 (args $4, dans une chaîne C) en job,
# attend sa fin et quitte. Relevé du hook dans $W/$1.json.
publish() {
	cat > "$W/$1.c" <<EOF
int job = 0;
void poll() {
	if (job != 0 && studio_sg_state(job) == 3) {
		console_log(string("t: job ok=%d %s", studio_sg_ok(job), studio_sg_get(job, "/error/message")));
		studio_sg_release(job);
		job = 0;
		script_quit();
	}
}
int main() {
	job = studio_sg_start("$3", "$4", 600000);
	script_notify_on_update(poll);
	return 0;
}
EOF
	STUDIO_TEST_PUBLISH_RESULT="$W/$1.json" perl -e 'alarm 600; exec @ARGV' "$APP" "$2" --script "$W/$1.c" \
		-ApplePersistenceIgnoreState YES > "$W/$1.log" 2>&1 || ko "$1 : ArmorPaint rc $? (voir $W/$1.log)"
}

# check_py <nom> <relevé> : assertions Python (entrée standard) sur le relevé $W/<relevé>.json du hook, avec r (le
# JSON), pub (PublishedFile par nom) et les dossiers W, WORK, PUB.
check_py() {
	if "$PY" - "$W/$2.json" "$W" "${WORK:-}" "${PUB:-}" > "$W/check.out" 2>&1 <<EOF
import json, os, sys
r = json.load(open(sys.argv[1]))
W, WORK, PUB = sys.argv[2:5]
pub = {f["name"]: f for f in r.get("published", [])}
$(cat)
EOF
	then ok "$1"; else
		ko "$1"
		tail -3 "$W/check.out" | sed 's/^/     /'
	fi
}

echo "== 1. projet UDIM enregistré au chemin de next_version"
background seed <<EOF
int main() {
	int j = studio_sg_run("next_version", "{\"task_id\":1,\"name\":\"robot\"}", 0);
	if (studio_sg_ok(j) == 0) {
		console_log(string("t: failed %s", studio_sg_result(j)));
		return 1;
	}
	char *path = studio_sg_get(j, "/data/path");
	script_project_new();
	studio_import_mesh("$W/src/robot.obj", 2);
	script_import_asset("$W/src/wood.png", 0);
	script_import_asset("$W/src/sky.hdr", 1);
	slot_material_t *m = script_material_create("paint");
	ui_node_t *rgb = script_material_create_node_at("RGB", -400.0, 0.0);
	ui_node_t *out = script_material_get_node("OUTPUT_MATERIAL_PBR");
	script_material_set_color(rgb, 0, 0, 0.8, 0.3, 0.1, 1.0);
	script_material_connect(rgb, 0, out, 0);
	script_material_update();
	script_material_set(m);
	script_fill_layer();
	project_filepath_set(path);
	project_save(false);
	console_log(string("t: seed %s", path));
	return 0;
}
EOF
V1=$(line seed seed)
[ -n "$V1" ] && [ -f "$V1" ] || { echo "seed failed, see $W/seed.log" >&2; exit 1; }
WORK=$(dirname "$V1")
PUB=$(dirname "$(dirname "$WORK")")/publish/armorpaint
ok "projet enregistré : ${V1#"$W"/}"
cp "$W/src/wood.png" "$WORK/decal.png"
background decal "$V1" <<EOF
int frame = 0;
void update() {
	if (++frame == 5) {
		script_quit();
	}
}
int main() {
	script_import_asset("$WORK/decal.png", 0);
	project_save(false);
	script_notify_on_update(update);
	return 0;
}
EOF
check "texture du dossier de travail importée, projet réenregistré" '"$PY" "$T/arm_paths.py" "$V1" | grep -qx decal.png'
cat > "$WORK/robot.export_settings.json" <<EOF
{"export": {"preset": "basic", "dir": "$W/unused", "mode": 3, "padding": false,
 "presets": {"basic": {"disabled": ["opacity", "occlusion", "roughness", "metallic", "height", "emission", "subsurface"]}}}}
EOF

echo "== 2. publish sans fenêtre (op publish)"
publish pub1 "$V1" publish '{\"task_id\":1,\"description\":\"first pass\"}'
check "job abouti" '[ "$(line pub1 job)" = "ok=1 " ]'
check_py "3 publishes : le projet et une séquence <UDIM> par map, finalisés, sans erreur" pub1 <<'EOF'
res = r["result"]
assert sorted(p["item"] for p in res["publishes"]) == ["robot.v001.arm", "robot_base_color.<UDIM>.exr", "robot_normal.<UDIM>.exr"], res
assert res["finalized"] and not [m for m in res["messages"] if m["level"] == "error"], res["messages"]
EOF
check_py "projet : copie écrite par ArmorPaint dans publish/, enregistrée v1 au type ArmorPaint Project" pub1 <<'EOF'
p = pub["robot.arm"]
assert os.path.isfile(os.path.join(PUB, "robot.v001.arm"))
assert (p["path"]["local_path"], p["version_number"], p["type_code"]) == (os.path.join(PUB, "robot.v001.arm"), 1, "ArmorPaint Project"), p
EOF
check "textures : tuiles exportées dans work/ (noms du template) puis copiées dans publish/" \
	'(for t in base_color normal; do for u in 1001 1002; do [ -f "$WORK/textures/v001/robot_$t.$u.exr" ] && [ -f "$PUB/textures/v001/robot_$t.$u.exr" ] || exit 1; done; done)'
check_py "textures : une séquence <UDIM> v1 de type UDIM Image, dépendante du projet" pub1 <<'EOF'
for name in ("robot_base_color.<UDIM>.exr", "robot_normal.<UDIM>.exr"):
    f = pub[name]
    assert f["path"]["local_path"] == os.path.join(PUB, "textures", "v001", name), f["path"]
    assert (f["version_number"], f["type_code"], f["depends_on"]) == (1, "UDIM Image", [pub["robot.arm"]["id"]]), f
EOF
check_py "description et tâche sur chaque publish" pub1 <<'EOF'
assert {(f["description"], f["task"]["id"]) for f in pub.values()} == {("first pass", 1)}, pub
EOF
check_py "vignettes : une par publish, PNG du viewport (1280 px au plus)" pub1 <<'EOF'
t = r["thumbnails"]
assert len(t) == 3 and all(x["exists"] and x["size"] and 0 < max(x["size"]) <= 1280 for x in t), t
EOF
check "vignette capturée par ArmorPaint" 'grep -q "studio: publish: thumbnail" "$W/pub1.log"'
check "projet ouvert ensuite en v002 (enregistré par ArmorPaint)" \
	'[ -f "$WORK/robot.v002.arm" ] && grep -q "studio: workfiles: save $WORK/robot.v002.arm" "$W/pub1.log"'

echo "== 3. la copie publiée retrouve ses fichiers, même sans le dossier de travail"
NAME="copie publiée : chemins relatifs à publish/ et normalisés, vers src/ et vers decal.png du dossier de travail"
if "$PY" "$T/arm_paths.py" "$PUB/robot.v001.arm" "$W/src/robot.obj" "$W/src/wood.png" "$W/src/sky.hdr" \
	"$WORK/decal.png" > "$W/check.out" 2>&1; then ok "$NAME"; else
	ko "$NAME"
	tail -3 "$W/check.out" | sed 's/^/     /'
fi
PATHS='int main() {
	project_t *p = script_get_project();
	for (int i = 0; i < p->assets->length; ++i) {
		console_log(string("t: asset %s", p->assets->buffer[i]));
	}
	console_log(string("t: envmap %s", p->envmap));
	return 0;
}'
printf '%s\n' "$PATHS" | background copy "$PUB/robot.v001.arm"
check "copie publiée rouverte : toutes ses textures trouvées" \
	'[ -n "$(line copy asset)" ] && ! grep -q "Could not locate texture" "$W/copy.log"'
mv "$WORK" "$WORK.away" # le dossier de travail de l'artiste, absent chez les autres
printf '%s\n' "$PATHS" | background away "$PUB/robot.v001.arm"
mv "$WORK.away" "$WORK"
check "rouverte sans le dossier de travail : texture et envmap trouvées dans src/, seule decal.png manque" \
	'[ "$(norm "$(line away envmap)")" = "$W/src/sky.hdr" ] && [ "$(grep -c "Could not locate texture" "$W/away.log")" -eq 1 ] \
	&& grep -q "Could not locate texture .*/decal.png" "$W/away.log"'
cp "$V1" "$W/naive/robot.v001.arm"
printf '%s\n' "$PATHS" | background naive "$W/naive/robot.v001.arm"
check "témoin : une simple copie du .arm ne trouve plus sa texture" 'grep -q "Could not locate texture" "$W/naive.log"'

echo "== 4. fenêtre Publish (show_app, pilotée par le hook)"
publish pub2 "$WORK/robot.v002.arm" show_app '{\"command\":\"Publish...\",\"task_id\":1}'
check "job abouti" '[ "$(line pub2 job)" = "ok=1 " ]'
check_py "fenêtre : Publish Complete, projet v002 et ses deux séquences publiés" pub2 <<'EOF'
d = r["dialog"]
assert d["status"] == "Publish Complete", d
assert sorted(d["published_items"]) == ["robot.v002.arm", "robot_base_color.<UDIM>.exr", "robot_normal.<UDIM>.exr"], d
assert sorted((n, f["version_number"]) for n, f in pub.items()) == [
    ("robot.arm", 2), ("robot_base_color.<UDIM>.exr", 2), ("robot_normal.<UDIM>.exr", 2)], pub
EOF
check_py "fenêtre : copies v002 dans publish/, vignettes envoyées" pub2 <<'EOF'
assert os.path.isfile(os.path.join(PUB, "robot.v002.arm")) and os.path.isfile(os.path.join(PUB, "textures", "v002", "robot_normal.1002.exr"))
assert len(r["thumbnails"]) == 3 and all(t["exists"] for t in r["thumbnails"]), r["thumbnails"]
EOF
check "fenêtre : projet ouvert ensuite en v003" '[ -f "$WORK/robot.v003.arm" ]'

[ $FAIL -eq 0 ] && echo "publish_app: tout passe" || echo "publish_app: ÉCHECS (journaux dans $W)"
exit $FAIL
