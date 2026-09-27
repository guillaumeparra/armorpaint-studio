#!/bin/sh
# Contexte ShotGrid et mode dégradé (lot C5) dans ArmorPaint lui-même, sur le site simulé (sg_fixture.py, scénario demo)
# au réseau coupé à la demande (STUDIO_TEST_OFFLINE_FLAG : le site est injoignable tant que ce fichier existe) :
#   1. contexte gardé dans le .arm : posé comme après une fenêtre ShotGrid (list_commands), enregistré avec le projet,
#      relu à l'ouverture et montré dans le titre ; un nouveau projet le garde, un .arm sans contexte le retire ;
#   2. ShotGrid injoignable pendant File Open : ArmorPaint passe hors ligne (titre), refuse aussitôt les commandes
#      ShotGrid suivantes et exporte aux noms d'ArmorPaint ; le site revenu, retrouvé par l'essai programmé alors
#      qu'ArmorPaint est au repos, titre et noms des templates reviennent ;
#   3. ShotGrid perdu en plein publish (STUDIO_TEST_OFFLINE_AT), fenêtre d'ArmorPaint ouverte : le projet est
#      enregistré, les textures vont dans la file ; au retour du site, la file est rejouée d'elle-même ;
#   4. fenêtre d'export (lot B6) : template des textures choisi pour le projet (sidecar d'export), Version Up, export
#      dans la version suivante au nom du template choisi, repris par une autre session ;
#   journal d'audit : évènements d'ArmorPaint et du sidecar dans un seul fichier, à chaque étape.
#
#   sh .tests/sg_context_app.sh [dossier de travail]
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

# storage/ et le cache de chemins de Toolkit qui le décrit (shotgun_home/) repartent ensemble de zéro.
rm -rf "$W/storage" "$W/shotgun_home" "$W/queue" "$W/out" "$W"/*.log "$W"/*.c "$W"/*.arm "$W/audit.jsonl" \
	"$W/offline.flag"
# Hook du sidecar : op find, pour relever ce qui est enregistré sur le site simulé.
cat > "$W/find_hook.py" <<'EOF'
def setup(session):
    def find(args):
        return session._connect().find(args["entity_type"], args.get("filters", []), args.get("fields", []))
    session.ops["find"] = find
EOF
export STUDIO_SG_BRIDGE="$T/sg_fixture.py" STUDIO_SG_PYTHON="$PY" STUDIO_TEST_WORKDIR="$W" STUDIO_TEST_SCENARIO=demo \
	STUDIO_TEST_TK_CORE="$SG/tk-core" STUDIO_TEST_TK_APPS="$SG" QT_QPA_PLATFORM=offscreen \
	STUDIO_SG_QUEUE="$W/queue" STUDIO_AUDIT_LOG="$W/audit.jsonl" STUDIO_TEST_OFFLINE_FLAG="$W/offline.flag" \
	STUDIO_SG_CHECK_INTERVAL=1 STUDIO_SG_HOOK="$W/find_hook.py"

# run <nom> [projet] [ui] : ArmorPaint sur le projet s'il est donné, sans interface sauf avec ui ; script <nom>.c (entrée
# standard), journal <nom>.log. Code de retour attendu : $RC_WANT (0 ; 1 quand le run signale des erreurs, lot B7).
RC_WANT=0
run() {
	cat > "$W/$1.c"
	background=--background
	if [ "${3:-}" = ui ]; then background=; fi
	rc=0
	perl -e 'alarm 300; exec @ARGV' "$APP" ${2:+"$2"} $background --script "$W/$1.c" -ApplePersistenceIgnoreState YES \
		> "$W/$1.log" 2>&1 || rc=$?
	[ $rc -eq $RC_WANT ] || ko "$1 : ArmorPaint rc $rc au lieu de $RC_WANT (voir $W/$1.log)"
}
line() { sed -n "s/^t: $2 //p" "$W/$1.log"; }
audit_lines() { if [ -f "$W/audit.jsonl" ]; then wc -l < "$W/audit.jsonl" | tr -d ' '; else echo 0; fi; }

# check <nom> : assertions Python (entrée standard) avec W, SITE, V1 ; t(journal, clé) : les valeurs des lignes
# « t: clé » de <journal>.log ; ev : les évènements du journal d'audit écrits depuis la ligne $FROM ; ctx et label : le
# contexte posé à l'étape 1.
check() {
	if FROM=${FROM:-0} "$PY" - "$W" "${V1:-}" > "$W/check.out" 2>&1 <<EOF
import json, os, re, sys
W, V1 = sys.argv[1:3]
SITE = "https://studio.test.shotgunstudio.com"
def text(name):
    with open(os.path.join(W, name + ".log"), encoding="utf-8", errors="replace") as f:
        return f.read()
def t(name, key):
    prefix = "t: %s " % key
    return [l[len(prefix):] for l in text(name).splitlines() if l.startswith(prefix)]
ev = []
if os.path.exists(os.path.join(W, "audit.jsonl")):
    with open(os.path.join(W, "audit.jsonl"), encoding="utf-8") as f:
        ev = [json.loads(l) for l in f.read().splitlines()[int(os.environ["FROM"]):]]
ctx = json.loads(t("ctx", "ctx")[0]) if os.path.exists(os.path.join(W, "ctx.log")) and t("ctx", "ctx") else {}
label = ctx.get("label")
$(cat)
EOF
	then ok "$1"; else
		ko "$1"
		tail -3 "$W/check.out" | sed 's/^/     /'
	fi
}

echo "== 1. contexte ShotGrid gardé dans le .arm"
run ctx <<EOF
int frame = 0;
void update() {
	if (++frame == 4) {
		console_log(string("t: title %s", sys_title()));
		script_quit();
	}
}
int main() {
	int j = studio_sg_run("next_version", "{\"task_id\":1,\"name\":\"robot\"}", 0);
	int c = studio_sg_run("list_commands", "{\"task_id\":1}", 0);
	if (studio_sg_ok(j) == 0 || studio_sg_ok(c) == 0) {
		console_log(string("t: failed %s %s", studio_sg_result(j), studio_sg_result(c)));
		script_quit();
		return 1;
	}
	console_log(string("t: set %d", studio_sg_context_set_json(studio_sg_get(c, "/data/context"))));
	console_log(string("t: ctx %s", studio_sg_context()));
	char *path = studio_sg_get(j, "/data/path");
	script_project_new();
	project_filepath_set(path);
	project_save(0);
	console_log(string("t: path %s", path));
	script_notify_on_update(update);
	return 0;
}
EOF
V1=$(line ctx path)
[ -n "$V1" ] && [ -f "$V1" ] || { echo "ctx failed, see $W/ctx.log" >&2; exit 1; }
WORK=$(dirname "$V1")
run loose <<EOF
int frame = 0;
void update() {
	if (++frame == 3) {
		script_quit();
	}
}
int main() {
	project_filepath_set("$W/loose.arm");
	project_save(0);
	script_notify_on_update(update);
	return 0;
}
EOF
check "contexte posé (tâche 1 du site), montré dans le titre" <<'EOF'
assert t("ctx", "set") == ["1"], t("ctx", "set")
assert (ctx["project_id"], ctx["task_id"], ctx["site"]) == (1, 1, SITE) and label, ctx
assert t("ctx", "title") == ["robot.v001 - %s - ArmorPaint" % label], t("ctx", "title")
EOF
check "enregistré dans le .arm (entrée studio_shotgrid.json), pas dans un projet sans contexte" <<'EOF'
entry = ('{"version":1,"context":%s}' % t("ctx", "ctx")[0]).encode()
assert entry in open(V1, "rb").read()
assert b"studio_shotgrid.json" not in open(os.path.join(W, "loose.arm"), "rb").read()
EOF
check "audit : project_save avec chemin, taille et contexte (sans contexte : pas de champ sg)" <<'EOF'
saves = [(e["path"], e.get("sg")) for e in ev if e["event"] == "project_save"]
assert saves == [(V1, ctx), (os.path.join(W, "loose.arm"), None)], saves
assert [e["size"] for e in ev if e["event"] == "project_save"][0] == os.path.getsize(V1)
for e in ev:
    assert e["source"] == "armorpaint" and re.match(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$", e["time"]), e
    assert e["pid"] > 0 and e["host"] and e["user"], e
EOF

FROM=$(audit_lines)
run reopen "$V1" <<EOF
int frame = 0;
void update() {
	frame++;
	if (frame == 3) {
		console_log(string("t: ctx %s", studio_sg_context()));
		console_log(string("t: title %s", sys_title()));
		script_project_new();
	}
	if (frame == 6) {
		console_log(string("t: new %s", studio_sg_context()));
		script_project_open("$W/loose.arm");
	}
	if (frame == 9) {
		console_log(string("t: loose [%s]", studio_sg_context()));
		console_log(string("t: loose_title %s", sys_title()));
		script_quit();
	}
}
int main() {
	script_notify_on_update(update);
	return 0;
}
EOF
check "rouvert : contexte relu du .arm et titre" <<'EOF'
assert [json.loads(c) for c in t("reopen", "ctx")] == [ctx], t("reopen", "ctx")
assert t("reopen", "title") == ["robot.v001 - %s - ArmorPaint" % label], t("reopen", "title")
EOF
check "nouveau projet : contexte gardé ; .arm sans contexte : retiré, titre sans contexte" <<'EOF'
assert [json.loads(c) for c in t("reopen", "new")] == [ctx], t("reopen", "new")
assert t("reopen", "loose") == ["[]"] and t("reopen", "loose_title") == ["loose - ArmorPaint"], t("reopen", "loose_title")
assert "studio: shotgrid context cleared: the opened project has none" in text("reopen")
EOF
check "audit : project_open avec le contexte du fichier ouvert" <<'EOF'
opens = [(e["path"], e.get("sg")) for e in ev if e["event"] == "project_open"]
assert opens == [(V1, ctx), (os.path.join(W, "loose.arm"), None)], opens
EOF

echo "== 2. ShotGrid injoignable : mode fichier, puis retour du site"
FROM=$(audit_lines)
: > "$W/offline.flag"
# Site injoignable et noms ShotGrid indisponibles sont des erreurs (console_error) : le run --background finit en code 1.
RC_WANT=1
run offline "$V1" <<EOF
int frame = 0;
int phase = 0;
int mark = 0;
// Où le préflight écrirait la couleur de base : nom du template ShotGrid, ou nom d'ArmorPaint dans le dossier choisi.
void dry(char *key) {
	char *r = studio_export_preflight("$W/out", "basic", 0);
	console_log(string("t: %s %d %d", key, string_index_of(r, "/textures/v001/robot_base_color.exr"),
	                   string_index_of(r, "$W/out/robot.v001_base_color.exr")));
}
void update() {
	frame++;
	if (phase == 0 && frame == 3) {
		console_log(string("t: show %d", studio_sg_show("open")));
		phase = 1;
	}
	else if (phase == 1 && studio_sg_offline() == 1) {
		phase = 2;
		mark  = frame;
	}
	else if (phase == 2 && frame == mark + 3) {
		console_log(string("t: title %s", sys_title()));
		console_log(string("t: show_again %d", studio_sg_show("open")));
		dry("dry_offline");
		iron_delete_file("$W/offline.flag");
		phase = 3;
	}
	else if (phase == 3 && studio_sg_offline() == 0) {
		phase = 4;
		mark  = frame;
	}
	else if (phase == 4 && frame == mark + 3) {
		console_log(string("t: back_title %s", sys_title()));
		dry("dry_online");
		script_quit();
	}
}
int main() {
	script_notify_on_update(update);
	return 0;
}
EOF
check "hors ligne : dit une fois, titre marqué, File Open refusé aussitôt (sans erreur Workfiles)" <<'EOF'
log = text("offline")
assert t("offline", "show") == ["1"] and t("offline", "show_again") == ["0"], (t("offline", "show"), t("offline", "show_again"))
assert t("offline", "title") == ["robot.v001 - %s (ShotGrid offline) - ArmorPaint" % label], t("offline", "title")
assert log.count("ShotGrid unreachable (") == 1 and "publishes are queued until ShotGrid is back" in log, log[-2000:]
assert "ShotGrid is offline: File Open needs the ShotGrid site" in log
assert "ShotGrid Workfiles:" not in log
EOF
check "hors ligne : textures aux noms d'ArmorPaint, dit une fois" <<'EOF'
template, armorpaint = map(int, t("offline", "dry_offline")[0].split())
assert template < 0 and armorpaint >= 0, t("offline", "dry_offline")
assert text("offline").count("ShotGrid texture naming: ShotGrid is unreachable, textures keep ArmorPaint names") == 1
EOF
check "retour du site trouvé par l'essai programmé (ArmorPaint au repos) : titre et noms du template" <<'EOF'
assert t("offline", "back_title") == ["robot.v001 - %s - ArmorPaint" % label], t("offline", "back_title")
assert re.search(r"(^|\s)ShotGrid is back$", text("offline"), re.M), text("offline")[-2000:]
template, armorpaint = map(int, t("offline", "dry_online")[0].split())
assert template >= 0 and armorpaint < 0, t("offline", "dry_online")
EOF
check "audit : sg_offline (raison) puis sg_online (rien en file)" <<'EOF'
net = [(e["event"], e.get("reason", e.get("pending"))) for e in ev if e["event"].startswith("sg_")]
assert [n[0] for n in net] == ["sg_offline", "sg_online"] and "Connection refused" in net[0][1] and net[1][1] == 0, net
EOF

echo "== 3. ShotGrid perdu en plein publish : file d'attente, rejouée au retour du site"
FROM=$(audit_lines)
RC_WANT=0
cat > "$WORK/robot.export_settings.json" <<EOF
{"export": {"preset": "basic", "dir": "$W/unused", "mode": 3, "padding": false,
 "presets": {"basic": {"disabled": ["opacity", "occlusion", "roughness", "metallic", "height", "emission", "subsurface"]}}}}
EOF
export STUDIO_TEST_OFFLINE_AT=create:PublishedFile:2
run publish "$V1" ui <<EOF
int job = 0;
int frame = 0;
int phase = 0;
int mark = 0;
void poll() {
	frame++;
	if (phase == 0 && job != 0 && studio_sg_state(job) == 3) {
		console_log(string("t: publish ok=%d queued=%d online=%s", studio_sg_ok(job), studio_sg_length(job, "/data/queued"),
		                   studio_sg_get(job, "/sg/online")));
		console_log(string("t: error [%s]", studio_sg_get(job, "/error/message")));
		studio_sg_release(job);
		job   = 0;
		phase = 1;
		mark  = frame;
	}
	else if (phase == 1 && frame >= mark + 3) {
		console_log(string("t: offline %d pending %d", studio_sg_offline(), studio_sg_pending()));
		console_log(string("t: title %s", sys_title()));
		iron_delete_file("$W/offline.flag");
		phase = 2;
	}
	else if (phase == 2 && studio_sg_offline() == 0 && studio_sg_offline_busy() == 0 && studio_sg_pending() == 0) {
		phase = 3;
		mark  = frame;
	}
	else if (phase == 3 && frame >= mark + 3) {
		console_log(string("t: back_title %s", sys_title()));
		int f = studio_sg_run("find", "{\"entity_type\":\"PublishedFile\",\"fields\":[\"code\"]}", 0);
		int d = studio_sg_run("find", "{\"entity_type\":\"PublishedFileDependency\"}", 0);
		console_log(string("t: published %d %d", studio_sg_length(f, "/data"), studio_sg_length(d, "/data")));
		studio_sg_release(f);
		studio_sg_release(d);
		script_quit();
	}
}
int main() {
	job = studio_sg_start("publish", "{\"task_id\":1,\"description\":\"offline pass\"}", 600000);
	script_notify_on_update(poll);
	return 0;
}
EOF
unset STUDIO_TEST_OFFLINE_AT
check "publish abouti hors ligne : projet enregistré, 2 textures en file, ArmorPaint hors ligne" <<'EOF'
assert t("publish", "publish") == ["ok=1 queued=2 online=false"], (t("publish", "publish"), t("publish", "error"))
assert t("publish", "offline") == ["1 pending 2"], t("publish", "offline")
assert t("publish", "title") == ["robot.v002 - %s (ShotGrid offline) - ArmorPaint" % label], t("publish", "title")
EOF
check "retour du site : file rejouée d'elle-même, 3 publishes (textures dépendantes du projet), titre rétabli" <<'EOF'
assert t("publish", "published") == ["3 2"], t("publish", "published")
assert t("publish", "back_title") == ["robot.v002 - %s - ArmorPaint" % label], t("publish", "back_title")
assert re.search(r"(^|\s)ShotGrid is back: registering 2 queued publishes$", text("publish"), re.M)
assert re.search(r"(^|\s)ShotGrid: 2 queued publishes registered$", text("publish"), re.M)
assert not os.listdir(os.path.join(W, "queue")) or not [f for f in os.listdir(os.path.join(W, "queue")) if f.endswith(".json")]
EOF
check "audit : copie, publish et file (sidecar), hors ligne puis en ligne (2 en file), rejeu" <<'EOF'
names = [e["event"] for e in ev]
for n in ("project_open", "project_copy", "publish", "sg_offline", "sg_online"):
    assert names.count(n) == 1, (n, names)
saves = [e["path"] for e in ev if e["event"] == "project_save"]
assert saves == [V1, os.path.join(os.path.dirname(V1), "robot.v002.arm")], saves  # enregistré avant la copie, puis v002
assert names.count("publish_queued") == 2 and names.count("publish_replayed") == 2, names
assert names.index("publish_queued") < names.index("sg_offline") < names.index("sg_online") < names.index("publish_replayed"), names
by = {e["event"]: e for e in ev}
assert by["project_copy"]["source"] == "armorpaint" and by["project_copy"]["path"].endswith("robot.v001.arm"), by["project_copy"]
assert by["project_copy"]["from"] == V1 and by["project_copy"]["size"] > 0, by["project_copy"]  # le projet copié
assert by["sg_online"]["pending"] == 2, by["sg_online"]
queued = [e["key"] for e in ev if e["event"] == "publish_queued"]
assert sorted(e["key"] for e in ev if e["event"] == "publish_replayed") == sorted(queued), names
assert {e["source"] for e in ev if e["event"].startswith("publish")} == {"sidecar"}
assert len({(e["host"], e["user"]) for e in ev}) == 1 and len({e["pid"] for e in ev}) == 2, {(e["source"], e["pid"]) for e in ev}
EOF

echo "== 4. Fenêtre d'export : template des textures choisi pour le projet, Version Up (lot B6)"
FROM=$(audit_lines)
V2=$WORK/robot.v002.arm
ASSET=$(dirname "$(dirname "$WORK")")
cat > "$WORK/robot.export_settings.json" <<EOF
{"export": {"preset": "file:basic", "dir": "$W/unused", "mode": 0, "padding": false,
 "presets": {"file:basic": {"disabled": ["opacity", "normal", "occlusion", "roughness", "metallic", "height", "emission", "subsurface"]}}}}
EOF
printf '{"template": "armorpaint_asset_textures_publish", "version_up": true, "result": "%s"}\n' "$W/win.result.json" > "$W/win.test.json"
rm -f "$W/win.result.json"
# Nommage ShotGrid actif d'emblée, comme lancé par ShotGrid Desktop (ici, rien n'a encore démarré le sidecar).
export STUDIO_EXPORT_WINDOW_TEST="$W/win.test.json" STUDIO_SG_NAMING=1
run win "$V2" ui <<EOF
int frame = 0;
void update() {
	frame++;
	if (frame == 3) {
		studio_export_window_show();
	}
	else if (frame > 3 && studio_export_window_is_open() == 0) {
		console_log(string("t: path %s", project_filepath_get()));
		console_log(string("t: title %s", sys_title()));
		script_quit();
	}
}
int main() {
	script_notify_on_update(update);
	return 0;
}
EOF
unset STUDIO_EXPORT_WINDOW_TEST
check "fenêtre : templates possibles (celui du réglage d'abord), version 2 du fichier de travail" <<'EOF'
r = json.load(open(os.path.join(W, "win.result.json")))
sg = r["sg_naming_first"]
assert sg["templates"] == ["armorpaint_asset_textures", "armorpaint_asset_textures_publish"], sg
assert (sg["template"], sg["default_template"], sg["name"], sg["version"]) == ("armorpaint_asset_textures", "armorpaint_asset_textures", "robot", 2), sg
EOF
check "template choisi : gardé dans le sidecar du projet, nommage redemandé" <<'EOF'
r = json.load(open(os.path.join(W, "win.result.json")))
assert r["template"]["template"] == "armorpaint_asset_textures_publish" and r["template"]["chosen"] is True, r["template"]
side = json.load(open(os.path.join(os.path.dirname(V1), "robot.export_settings.json")))
assert side["texture_template"] == "armorpaint_asset_textures_publish" and side["export"]["preset"] == "file:basic", side
EOF
check "Version Up : robot.v003.arm enregistré (audit), titre ; la fenêtre suit la version 3" <<'EOF'
r = json.load(open(os.path.join(W, "win.result.json")))
v3 = os.path.join(os.path.dirname(V1), "robot.v003.arm")
assert r["version_up"]["path"] == v3 and os.path.isfile(v3), r["version_up"]
assert r["version_up"]["sg_naming"]["version"] == 3, r["version_up"]
assert t("win", "path") == [v3], t("win", "path")
assert t("win", "title") == ["robot.v003 - %s - ArmorPaint" % label], t("win", "title")
assert r["sg"]["current"] == "armorpaint_asset_textures_publish" and r["sg"]["version"] == "version 3", r["sg"]
assert [e["path"] for e in ev if e["event"] == "project_save"] == [v3], [e["event"] for e in ev]
EOF
check "export : la couleur de base dans publish/armorpaint/textures/v003 (template choisi, version 3)" <<'EOF'
r = json.load(open(os.path.join(W, "win.result.json")))
asset = os.path.dirname(os.path.dirname(os.path.dirname(V1)))
want = os.path.join(asset, "publish", "armorpaint", "textures", "v003", "robot_base_color.exr")
assert r["export"]["ok"] is True, r["export"]
assert [f["path"] for f in r["export"]["report"]["files"] if not f.get("skip")] == [want], r["export"]["report"]["files"]
assert os.path.isfile(want), want
assert "textures/v003/robot_base_color.exr" in r["naming"]["note"], r["naming"]
EOF
run later "$ASSET/work/armorpaint/robot.v003.arm" <<EOF
int main() {
	char *r = studio_export_preflight("$W/unused", "basic", 0);
	console_log(string("t: publish_v003 %d", string_index_of(r, "/publish/armorpaint/textures/v003/robot_base_color.exr")));
	return 0;
}
EOF
unset STUDIO_SG_NAMING
check "session suivante (--background) : le template choisi nomme encore les textures" <<'EOF'
assert t("later", "publish_v003") and int(t("later", "publish_v003")[0]) > 0, t("later", "publish_v003")
EOF

[ $FAIL -eq 0 ] && echo "sg_context_app: tout passe" || echo "sg_context_app: ÉCHECS (journaux dans $W)"
exit $FAIL
