# Banc du sidecar ShotGrid (lot C2), côté Python : le sidecar tourne dans un sous-processus, sur le site simulé de
# sg_fixture.py, et on lui parle par son protocole comme le fait studio_shotgrid.c.
#
#   STUDIO_TEST_TK_CORE=<tk-core> python3 .tests/sg_test.py [-v] [Classe.test]

import json
import os
import select
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(os.path.dirname(HERE), "assets")
sys.path.insert(0, ASSETS)


def write(path, content="arm"):
    with open(path, "wb" if isinstance(content, bytes) else "w") as f:
        f.write(content)


def clean_env(**extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith(("SHOTGUN_", "STUDIO_SG", "TANK_", "SGTK_"))}
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env.update({k: v for k, v in extra.items() if v is not None})
    return env


class Sidecar(object):
    """Un sidecar lancé comme par le superviseur : stdin/stdout en pipes, stderr capturé."""

    def __init__(self, workdir, argv=None, **env):
        self.workdir = workdir
        self.err_path = os.path.join(workdir, "stderr-%d.txt" % time.monotonic_ns())
        self.err = open(self.err_path, "w")
        argv = argv or [sys.executable, os.path.join(HERE, "sg_fixture.py"), workdir]
        self.proc = subprocess.Popen(
            argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.err, env=clean_env(**env),
            start_new_session=True,
        )
        self.next_id = 1
        self.hello = self.read(30)

    def readline(self, timeout):
        deadline = time.monotonic() + timeout
        line = b""
        while not line.endswith(b"\n"):
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([self.proc.stdout], [], [], left)[0]:
                raise AssertionError("no reply within %ss; stderr:\n%s" % (timeout, self.stderr()[-3000:]))
            chunk = os.read(self.proc.stdout.fileno(), 1)
            if not chunk:
                return line.decode() if line else None
            line += chunk
        return line.decode()

    def read(self, timeout=30):
        line = self.readline(timeout)
        if line is None:
            return None
        return json.loads(line)

    def send_raw(self, text):
        self.proc.stdin.write(text.encode())
        self.proc.stdin.flush()

    def call(self, op, args=None, timeout=60):
        message_id = self.next_id
        self.next_id += 1
        self.send_raw(json.dumps({"id": message_id, "op": op, "args": args if args is not None else {}}) + "\n")
        reply = self.read(timeout)
        assert reply is not None, "sidecar exited; stderr:\n%s" % self.stderr()[-3000:]
        assert reply["id"] == message_id, reply
        return reply

    def data(self, op, args=None, timeout=60):
        reply = self.call(op, args, timeout)
        assert reply["ok"], "%s failed: %s\nstderr:\n%s" % (op, reply, self.stderr()[-3000:])
        return reply["data"]

    def error(self, op, args=None, timeout=60):
        reply = self.call(op, args, timeout)
        assert not reply["ok"], "%s should fail: %s" % (op, reply)
        return reply["error"]["code"], reply

    def stderr(self):
        self.err.flush()
        with open(self.err_path) as f:
            return f.read()

    def close(self):
        if not self.proc.stdin.closed:
            self.proc.stdin.close()
        if self.proc.poll() is None:
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.proc.stdout.close()
        self.err.close()


class Base(unittest.TestCase):
    def setUp(self):
        self.workdir = tempfile.mkdtemp(prefix="sg_test_")
        self.sidecars = []

    def tearDown(self):
        for s in self.sidecars:
            s.close()
        shutil.rmtree(self.workdir, ignore_errors=True)

    def start(self, scenario=None, **env):
        s = Sidecar(self.workdir, STUDIO_TEST_SCENARIO=scenario, **env)
        self.sidecars.append(s)
        self.assertTrue(s.hello["ok"], s.hello)
        return s


class TestProtocol(Base):
    def test_hello(self):
        s = self.start()
        self.assertEqual(s.hello["id"], 0)
        data = s.hello["data"]
        self.assertEqual(data["protocol"], 1)
        self.assertTrue(data["toolkit"]["available"])
        self.assertIn("list_workfiles", data["ops"])

    def test_malformed_requests(self):
        s = self.start()
        s.send_raw('{"id":41,"op":\n')
        reply = s.read()
        self.assertEqual((reply["id"], reply["error"]["code"]), (41, "bad_request"))  # id repris du préfixe
        s.send_raw("\n   \n")  # lignes vides : ignorées, pas de réponse
        s.send_raw('{"id":42,"op":"ping","args":[1]}\n')
        self.assertEqual(s.read()["error"]["code"], "bad_request")
        s.send_raw('{"id":43}\n')
        self.assertEqual(s.read()["error"]["code"], "bad_request")
        s.send_raw("[1,2]\n")
        reply = s.read()
        self.assertEqual((reply["id"], reply["error"]["code"]), (None, "bad_request"))
        self.assertEqual(s.error("no_such_op")[0], "unknown_op")
        self.assertEqual(s.error("list_entities", {"project_id": "1"})[0], "bad_request")
        self.assertEqual(s.error("list_entities", {"project_id": True})[0], "bad_request")
        self.assertEqual(s.error("list_entities", {"project_id": 1, "entity_type": "Sequence"})[0], "bad_request")
        self.assertEqual(s.data("ping"), {})  # toujours vivant

    def test_replies_are_one_ascii_line(self):
        hook = os.path.join(self.workdir, "hook.py")
        with open(hook, "w") as f:
            f.write(
                "import subprocess\n"
                "def setup(session):\n"
                "    def echo(args):\n"
                "        print('print() from a hook')\n"
                "        subprocess.run(['echo', 'stdout of a child process'])\n"
                "        return args\n"
                "    session.ops['echo'] = echo\n"
            )
        s = self.start(STUDIO_SG_HOOK=hook)
        text = "Élodie « 日本 »   fin\nligne 2\t\u0000"
        self.assertEqual(s.data("echo", {"text": text}), {"text": text})
        err = s.stderr()
        self.assertIn("print() from a hook", err)
        self.assertIn("stdout of a child process", err)
        self.assertEqual(s.data("ping"), {})  # le protocole n'a pas été pollué

    def test_hook_cannot_read_protocol_stdin(self):
        hook = os.path.join(self.workdir, "hook.py")
        with open(hook, "w") as f:
            f.write(
                "import sys\n"
                "def setup(session):\n"
                "    session.ops['steal'] = lambda args: {'read': sys.stdin.read()}\n"
            )
        s = self.start(STUDIO_SG_HOOK=hook)
        self.assertEqual(s.data("steal"), {"read": ""})
        self.assertEqual(s.data("ping"), {})

    def test_eof_and_shutdown(self):
        s = self.start()
        s.proc.stdin.close()
        self.assertEqual(s.proc.wait(10), 0)
        s2 = self.start()
        self.assertEqual(s2.data("shutdown"), {})
        self.assertIsNone(s2.readline(10))
        self.assertEqual(s2.proc.wait(10), 0)

    def test_exception_in_op_is_reported(self):
        hook = os.path.join(self.workdir, "hook.py")
        with open(hook, "w") as f:
            f.write("def setup(session):\n    session.ops['boom'] = lambda args: 1 / 0\n")
        s = self.start(STUDIO_SG_HOOK=hook)
        code, reply = s.error("boom")
        self.assertEqual(code, "internal")
        self.assertIn("ZeroDivisionError", reply["error"]["message"])
        self.assertIn("Traceback", s.stderr())
        self.assertEqual(s.data("ping"), {})


class TestAuth(Base):
    def test_desktop_session(self):
        s = self.start()
        auth = s.data("auth")
        self.assertEqual(auth["method"], "session")
        self.assertEqual(auth["login"], "artist")
        self.assertEqual(auth["user"]["name"], "Ada Artist")

    def test_no_session(self):
        s = self.start("no_session")
        code, reply = s.error("auth")
        self.assertEqual(code, "auth_required")
        self.assertIn("ShotGrid Desktop", reply["error"]["message"])
        self.assertEqual(s.error("list_projects")[0], "auth_required")

    def test_rejected_session(self):
        s = self.start("auth_fault")
        self.assertEqual(s.error("auth")[0], "auth_required")

    def test_offline(self):
        s = self.start("offline")
        self.assertEqual(s.error("auth")[0], "sg_unreachable")
        self.assertEqual(s.error("list_tasks")[0], "sg_unreachable")

    def test_never_prompts(self):
        # tk-core renouvelle une session expirée par une invite de mot de passe : elle doit devenir une erreur.
        hook = os.path.join(self.workdir, "hook.py")
        with open(hook, "w") as f:
            f.write(
                "import importlib\n"
                "def setup(session):\n"
                "    def renew(args):\n"
                "        session._connect()\n"
                "        auth = importlib.import_module('tank.authentication.interactive_authentication')\n"
                "        auth.renew_session(session.user)\n"
                "    session.ops['renew'] = renew\n"
            )
        s = self.start(STUDIO_SG_HOOK=hook)
        started = time.monotonic()
        code, reply = s.error("renew", timeout=10)
        self.assertEqual(code, "auth_required")
        self.assertLess(time.monotonic() - started, 5)


class TestSite(Base):
    def test_queries(self):
        s = self.start()
        self.assertEqual([p["name"] for p in s.data("list_projects")], ["Demo", "Other"])  # sans l'archivé
        assets = s.data("list_assets", {"project_id": 1})
        self.assertEqual([(a["code"], a["sg_asset_type"]) for a in assets], [("robot", "Prop"), ("tree", "Env")])
        self.assertEqual([t["id"] for t in s.data("list_tasks")], [1, 3])  # mine par défaut
        self.assertEqual([t["id"] for t in s.data("list_tasks", {"project_id": 1})], [1])
        tasks = {t["id"]: t for t in s.data("list_tasks", {"project_id": 1, "mine": False})}
        self.assertEqual(sorted(tasks), [1, 2, 4])
        self.assertEqual((tasks[4]["entity"]["type"], tasks[4]["entity"]["name"]), ("Shot", "sq010_0010"))
        self.assertNotIn("sg_asset_type", tasks[4]["entity"])
        self.assertEqual(tasks[2]["entity"]["name"], "tree")
        task = s.data("list_tasks", {"project_id": 1})[0]
        self.assertEqual(
            (task["entity"]["name"], task["entity"]["sg_asset_type"], task["step.Step.code"],
             task["project.Project.name"]),
            ("robot", "Prop", "Texture", "Demo"),
        )
        tasks = s.data("list_tasks", {"entity": {"type": "Asset", "id": 2}})
        self.assertEqual([t["content"] for t in tasks], ["lookdev"])
        self.assertEqual(s.error("list_tasks", {"mine": False})[0], "bad_request")
        self.assertEqual(s.error("list_tasks", {"entity": {"type": "Asset"}})[0], "bad_request")

    def test_launch_context(self):
        s = self.start(SHOTGUN_ENTITY_TYPE="Task", SHOTGUN_ENTITY_ID="1")
        ctx = s.data("context")
        self.assertEqual((ctx["type"], ctx["id"], ctx["content"]), ("Task", 1, "texturing"))
        self.assertEqual(ctx["entity"]["id"], 1)
        s2 = self.start()
        self.assertIsNone(s2.data("context"))


class TestToolkit(Base):
    def test_workfiles_versions_publish(self):
        s = self.start()
        empty = s.data("list_workfiles", {"task_id": 1})
        self.assertEqual((empty["template"], empty["files"], empty["publishes"]), ("armorpaint_asset_work", [], []))
        err = s.stderr()
        # L'amorçage a basculé sur le tk-core copié dans la configuration, comme en production.
        self.assertIn("Toolkit ready for project 1", err)
        self.assertIn(os.path.join("install", "core", "python"), err.split("Toolkit ready for project 1")[1])

        nv = s.data("next_version", {"task_id": 1, "name": "robot"})
        expected = os.path.join(self.workdir, "storage", "demo", "assets", "Prop", "robot", "TXT", "work", "armorpaint")
        self.assertEqual(nv["path"], os.path.join(expected, "robot.v001.arm"))
        self.assertEqual((nv["version"], nv["publish_name"]), (1, "robot.arm"))
        self.assertTrue(os.path.isdir(expected))  # dossiers créés par le schéma Toolkit
        write(nv["path"], "arm")

        self.assertEqual(s.data("next_version", {"task_id": 1, "name": "robot"})["version"], 2)
        write(os.path.join(expected, "robot.v005.arm"), "arm")
        write(os.path.join(expected, "notes.txt"), "hors template")
        self.assertEqual(s.data("next_version", {"task_id": 1, "name": "robot"})["version"], 6)
        self.assertEqual(s.data("next_version", {"task_id": 1, "name": "detail"})["version"], 1)
        self.assertEqual(s.data("next_version", {"task_id": 1})["name"], "scene")

        files = s.data("list_workfiles", {"task_id": 1})["files"]
        self.assertEqual([(f["name"], f["version"]) for f in files], [("robot", 1), ("robot", 5)])
        self.assertEqual(files[0]["size"], 3)

        publish = s.data(
            "register_publish",
            {"task_id": 1, "path": os.path.join(expected, "robot.v005.arm"), "description": "première"},
        )
        self.assertEqual((publish["name"], publish["version_number"]), ("robot.arm", 5))
        publishes = s.data("list_workfiles", {"task_id": 1})["publishes"]
        self.assertEqual(len(publishes), 1)
        self.assertEqual(publishes[0]["path"], os.path.join(expected, "robot.v005.arm"))
        self.assertEqual(publishes[0]["description"], "première")

        # Un publish à une version plus haute que les fichiers de travail compte aussi.
        v9 = os.path.join(self.workdir, "robot.v009.arm")
        write(v9, "arm")
        s.data("register_publish", {"task_id": 1, "path": v9, "name": "robot.arm"})
        self.assertEqual(s.data("next_version", {"task_id": 1, "name": "robot"})["version"], 10)

        self.assertEqual(s.error("register_publish", {"task_id": 1, "path": "/nope/x.v001.arm"})[0], "not_found")
        noversion = os.path.join(self.workdir, "robot.arm")
        write(noversion, "arm")
        self.assertEqual(s.error("register_publish", {"task_id": 1, "path": noversion})[0], "bad_request")
        self.assertEqual(s.data("register_publish", {"task_id": 1, "path": noversion, "version": 11})["version_number"], 11)
        self.assertEqual(s.error("list_workfiles", {"task_id": 999})[0], "not_found")

        thumb = os.path.join(self.workdir, "thumb.png")
        write(thumb, b"\x89PNG")
        self.assertIn("attachment_id", s.data("upload_thumbnail", {"entity": {"type": "Task", "id": 1}, "path": thumb}))
        self.assertEqual(
            s.error("upload_thumbnail", {"entity": {"type": "Task", "id": 1}, "path": thumb + "x"})[0], "not_found"
        )

    def test_project_switch_restarts(self):
        s = self.start()
        s.data("list_workfiles", {"task_id": 1})
        code, reply = s.error("list_workfiles", {"task_id": 3})
        self.assertEqual(code, "restart_required")
        self.assertTrue(reply["restart"])
        self.assertIsNone(s.readline(10))  # le sidecar s'arrête après la réponse
        self.assertEqual(s.proc.wait(10), 0)
        s2 = self.start()
        self.assertEqual(s2.data("list_workfiles", {"task_id": 3})["files"], [])
        self.assertEqual(s2.data("list_projects")[0]["name"], "Demo")  # le site reste accessible

    def test_texture_pattern(self):
        s = self.start()
        nv = s.data("next_version", {"task_id": 1, "name": "robot"})
        work = os.path.dirname(nv["path"])
        tp = s.data("texture_pattern", {"path": nv["path"]})
        tex = os.path.join(work, "textures", "v001")
        self.assertEqual(tp["template"], "armorpaint_asset_textures")
        # Marqueurs malgré les types des clés (channel alphanumeric, texture_ext à choix, UDIM séquence).
        self.assertEqual(tp["pattern"], os.path.join(tex, "robot_<channel>.<ext>"))
        self.assertEqual(tp["pattern_udim"], os.path.join(tex, "robot_<channel>.<UDIM>.<ext>"))

        # Un Shot, dossiers absents du path cache (type d'entité d'après le template de travail) : template sans clé
        # UDIM ; le projet déjà amorcé sert sans project_id.
        shot_work = os.path.join(self.workdir, "storage", "demo", "sequences", "sq010", "sq010_0010", "TXT", "work", "armorpaint")
        tp = s.data("texture_pattern", {"path": os.path.join(shot_work, "fx.v012.arm")})
        self.assertEqual(tp["template"], "armorpaint_shot_textures")
        self.assertEqual(tp["pattern"], os.path.join(shot_work, "textures", "v012", "fx_<channel>.<ext>"))
        self.assertIsNone(tp["pattern_udim"])

        # Hors pipeline : pas de template, ArmorPaint garde ses noms.
        tp = s.data("texture_pattern", {"path": os.path.join(self.workdir, "loose.arm")})
        self.assertIsNone(tp["template"])
        self.assertIn("matches no template", tp["reason"])
        self.assertEqual(s.error("texture_pattern", {})[0], "bad_request")

        # Un sidecar neuf : le projet vient du chemin (racine de stockage + tank_name), pas de la dernière tâche de
        # l'artiste (projet 2).
        s2 = self.start()
        self.assertEqual(s2.data("texture_pattern", {"path": nv["path"]})["template"], "armorpaint_asset_textures")
        self.assertIn("Toolkit ready for project 1", s2.stderr())

    def test_texture_pattern_settings(self):
        # Pas de template de textures pour les Assets : pas de nommage ; un template sans {channel} : erreur.
        s = self.start(STUDIO_SG_TEXTURE_TEMPLATES="Shot=armorpaint_shot_textures")
        nv = s.data("next_version", {"task_id": 1, "name": "robot"})
        tp = s.data("texture_pattern", {"path": nv["path"], "project_id": 1})
        self.assertIsNone(tp["template"])
        self.assertIn("no texture template configured for Asset", tp["reason"])
        s2 = self.start(STUDIO_SG_TEXTURE_TEMPLATES="Asset=armorpaint_asset_work")
        code, reply = s2.error("texture_pattern", {"path": nv["path"], "project_id": 1})
        self.assertEqual(code, "template_missing")
        self.assertIn("{channel}", reply["error"]["message"])
        s3 = self.start(STUDIO_SG_TEXTURE_TEMPLATES="Asset=nope")
        self.assertEqual(s3.error("texture_pattern", {"path": nv["path"], "project_id": 1})[0], "template_missing")

    def test_missing_template(self):
        s = self.start("no_template")
        code, reply = s.error("next_version", {"task_id": 1})
        self.assertEqual(code, "template_missing")
        self.assertIn("armorpaint_asset_work", reply["error"]["message"])


def has_qt():
    try:
        import PySide6  # noqa: F401
    except ImportError:
        return False
    return True


@unittest.skipUnless(os.environ.get("STUDIO_TEST_TK_APPS"), "STUDIO_TEST_TK_APPS not set (tk-multi-workfiles2 clones)")
class TestEngine(Base):
    """Le moteur tk-armorpaint et le menu ShotGrid (scénario demo). Les fenêtres ne sont pas ouvertes ici : voir
    demo_workfiles.sh."""

    CONTEXT_COMMANDS = ["Jump to Flow Production Tracking", "Jump to File System"]

    def test_menu_without_qt(self):
        s = self.start("demo", STUDIO_SG_NO_UI="1")
        menu = s.data("list_commands", {"task_id": 1})
        self.assertEqual(menu["context"]["label"], "Demo / robot / Texture / texturing")
        self.assertEqual((menu["context"]["task_id"], menu["context"]["project_id"]), (1, 1))
        self.assertFalse(menu["has_ui"])
        # Sans Qt, workfiles2 ne s'enregistre pas : seule la section contexte du moteur reste.
        self.assertEqual([c["name"] for c in menu["commands"]], self.CONTEXT_COMMANDS)
        self.assertEqual({c["type"] for c in menu["commands"]}, {"context_menu"})
        self.assertEqual(menu["favourites"], [])
        # Les dossiers de la tâche sont créés avant le changement de contexte.
        work = os.path.join(self.workdir, "storage", "demo", "assets", "Prop", "robot", "TXT", "work", "armorpaint")
        self.assertTrue(os.path.isdir(work))
        code, reply = s.error("show_app", {"command": "open", "task_id": 1})
        self.assertEqual(code, "app_unavailable")
        self.assertIn("no Qt", reply["error"]["message"])

        # project_id seul garde le contexte courant du moteur s'il est dans ce projet.
        project = s.data("list_commands", {"project_id": 1})
        self.assertEqual(project["context"]["task_id"], 1)
        # Autre tâche : les commandes du moteur survivent au changement de contexte.
        other = s.data("list_commands", {"task_id": 2})
        self.assertEqual(other["context"]["label"], "Demo / tree / Texture / lookdev")
        self.assertEqual([c["name"] for c in other["commands"]], self.CONTEXT_COMMANDS)

    @unittest.skipUnless(has_qt(), "PySide6 not importable by this Python")
    def test_menu_with_workfiles(self):
        s = self.start("demo")
        menu = s.data("list_commands", {"task_id": 1})
        self.assertTrue(menu["has_ui"])
        names = [c["name"] for c in menu["commands"]]
        self.assertEqual([c["name"] for c in menu["commands"] if c["type"] == "context_menu"], self.CONTEXT_COMMANDS)
        self.assertIn("File Open...", names)
        self.assertIn("File Save...", names)
        wf = {c["name"]: c for c in menu["commands"]}["File Open..."]
        self.assertEqual((wf["app_instance"], wf["type"]), ("tk-multi-workfiles2", "default"))
        self.assertEqual(menu["favourites"], ["File Open...", "File Save..."])  # réglage menu_favourites

    def test_engine_missing(self):
        s = self.start()  # config sans tk-armorpaint
        code, reply = s.error("list_commands", {"task_id": 1})
        self.assertEqual(code, "app_unavailable")
        self.assertIn("tk-armorpaint", reply["error"]["message"])


class TestCoreDiscovery(Base):
    def test_toolkit_unavailable(self):
        home = os.path.join(self.workdir, "home")
        os.makedirs(home)
        s = Sidecar(
            self.workdir, argv=[sys.executable, os.path.join(ASSETS, "studio_sg_bridge.py")], SHOTGUN_HOME=home,
            STUDIO_SGTK_PATH=os.path.join(self.workdir, "nowhere"),
        )
        self.sidecars.append(s)
        toolkit = s.hello["data"]["toolkit"]
        self.assertFalse(toolkit["available"])
        self.assertIn("STUDIO_SGTK_PATH", toolkit["error"])
        self.assertEqual(s.error("auth")[0], "toolkit_unavailable")
        self.assertEqual(s.data("ping"), {})

    def test_bundle_cache_picks_newest(self):
        import studio_sg_session

        home = os.path.join(self.workdir, "home")
        for version in ("v0.9.10", "v0.21.2", "v0.21.10", "v0.3.0"):
            os.makedirs(os.path.join(home, "bundle_cache", "app_store", "tk-core", version, "python"))
        old = dict(os.environ)
        try:
            os.environ.pop("STUDIO_SGTK_PATH", None)
            os.environ.pop("SHOTGUN_BUNDLE_CACHE_PATH", None)
            os.environ["SHOTGUN_HOME"] = home
            versions = [os.path.basename(os.path.dirname(p)) for p in studio_sg_session.core_candidates()]
        finally:
            os.environ.clear()
            os.environ.update(old)
        self.assertEqual(versions, ["v0.21.10", "v0.21.2", "v0.9.10", "v0.3.0"])

    def test_line_reader_idle(self):
        # Entre deux requêtes, le sidecar fait tourner la boucle d'événements de ses fenêtres (studio_sg_bridge.IDLE).
        import threading

        from studio_sg_bridge import LineReader

        r, w = os.pipe()
        reader = LineReader(r)
        ticks = []

        def writer():
            time.sleep(0.3)
            os.write(w, "a\nb".encode())
            time.sleep(0.2)
            os.write(w, "é\nc\n".encode())
            os.close(w)

        threading.Thread(target=writer).start()
        self.assertEqual(reader.readline(idle=lambda: ticks.append(1)), "a\n")
        self.assertGreaterEqual(len(ticks), 3)  # toutes les 50 ms pendant 0,3 s
        self.assertEqual(reader.readline(), "bé\n")  # une ligne coupée entre deux écritures
        self.assertEqual(reader.readline(), "c\n")
        self.assertEqual(reader.readline(idle=lambda: None), "")  # EOF
        os.close(r)

        def failing():
            raise RuntimeError("boom")

        r, w = os.pipe()
        os.write(w, b"x\n")
        reader = LineReader(r)
        self.assertEqual(reader.readline(idle=failing), "x\n")  # rien à attendre : idle pas appelé
        threading.Timer(0.2, lambda: (os.write(w, b"y\n"), os.close(w))).start()
        self.assertEqual(reader.readline(idle=failing), "y\n")  # idle en échec : désactivé, la lecture continue
        os.close(r)

    def test_publish_name(self):
        from studio_sg_session import publish_name, version_in_name

        cases = {
            "robot.v003.arm": ("robot.arm", 3),
            "robot_v12.arm": ("robot.arm", 12),
            "robot-V7.arm": ("robot.arm", 7),
            "v2robot.v004.arm": ("v2robot.arm", 4),
            "robot.arm": ("robot.arm", None),
            "robot.v001": ("robot", 1),
            "devis.v1.v002.arm": ("devis.v1.arm", 2),
        }
        for path, (name, version) in cases.items():
            self.assertEqual((publish_name("/a/" + path), version_in_name(path)), (name, version), path)


if __name__ == "__main__":
    if not os.environ.get("STUDIO_TEST_TK_CORE"):
        sys.exit("STUDIO_TEST_TK_CORE must point to a tk-core checkout")
    unittest.main()
