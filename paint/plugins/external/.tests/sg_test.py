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
SITE = "https://studio.test.shotgunstudio.com"  # sg_fixture.HOST


def write(path, content="arm"):
    with open(path, "wb" if isinstance(content, bytes) else "w") as f:
        f.write(content)


def clean_env(**extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith(("SHOTGUN_", "STUDIO_SG", "TANK_", "SGTK_"))}
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env.update({k: v for k, v in extra.items() if v is not None})
    return env


class HostRefusal(Exception):
    """Refus d'une requête host par le faux ArmorPaint (réponse "ok":false)."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


class Sidecar(object):
    """Un sidecar lancé comme par le superviseur : stdin/stdout en pipes, stderr capturé. host : ce qui répond à ses
    requêtes host pendant une requête (FakeArmorPaint), comme studio_workfiles.c dans l'app."""

    def __init__(self, workdir, argv=None, **env):
        self.workdir = workdir
        self.host = None
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
        while True:
            reply = self.read(timeout)
            assert reply is not None, "sidecar exited; stderr:\n%s" % self.stderr()[-3000:]
            if "host" not in reply:
                break
            self.answer_host(reply)
        assert reply["id"] == message_id, reply
        return reply

    def answer_host(self, request):
        assert self.host is not None, "unexpected host request: %s" % request
        try:
            reply = {"host": request["host"], "ok": True, "data": self.host(request["op"], request.get("args") or {})}
        except HostRefusal as e:
            reply = {"host": request["host"], "ok": False, "error": {"code": e.code, "message": str(e)}}
        self.send_raw(json.dumps(reply) + "\n")

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
        # Marqueurs malgré les types des clés (texture_ext à choix, UDIM séquence ; channel filtré :
        # test_texture_name_refused_by_template).
        self.assertEqual(tp["pattern"], os.path.join(tex, "robot_<channel>.<ext>"))
        self.assertEqual(tp["pattern_udim"], os.path.join(tex, "robot_<channel>.<UDIM>.<ext>"))
        # Lot B6 : les templates qui peuvent nommer ces textures (ceux des Shots ne se remplissent pas depuis un
        # Asset), celui du réglage d'abord ; nom et version du fichier de travail.
        self.assertEqual(tp["templates"], ["armorpaint_asset_textures", "armorpaint_asset_textures_publish"])
        self.assertEqual((tp["default_template"], tp["chosen"], tp["name"], tp["version"]),
                         ("armorpaint_asset_textures", False, "robot", 1))
        # Un autre template : demandé, puis choisi pour le projet (sidecar de la fenêtre d'export, commun aux versions).
        pub = os.path.join(os.path.dirname(os.path.dirname(work)), "publish", "armorpaint", "textures", "v001")
        tp = s.data("texture_pattern", {"path": nv["path"], "template": "armorpaint_asset_textures_publish"})
        self.assertEqual((tp["template"], tp["chosen"]), ("armorpaint_asset_textures_publish", True))
        self.assertEqual(tp["pattern"], os.path.join(pub, "robot_<channel>.<ext>"))
        sidecar = os.path.join(work, "robot.export_settings.json")
        write(sidecar, json.dumps({"version": 1, "texture_template": "armorpaint_asset_textures_publish"}))
        tp = s.data("texture_pattern", {"path": nv["path"]})
        self.assertEqual((tp["template"], tp["chosen"]), ("armorpaint_asset_textures_publish", True))
        self.assertEqual(tp["pattern_udim"], os.path.join(pub, "robot_<channel>.<UDIM>.<ext>"))
        # Choisi mais absent de la configuration, ou sans canal : erreur qui nomme le sidecar, pas de repli silencieux.
        write(sidecar, json.dumps({"version": 1, "texture_template": "nope"}))
        code, reply = s.error("texture_pattern", {"path": nv["path"]})
        self.assertEqual(code, "template_missing")
        self.assertIn("robot.export_settings.json", reply["error"]["message"])
        self.assertEqual(reply["error"]["templates"], ["armorpaint_asset_textures", "armorpaint_asset_textures_publish"])
        self.assertEqual(s.error("texture_pattern", {"path": nv["path"], "template": "armorpaint_asset_work"})[0],
                         "template_missing")
        os.remove(sidecar)

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

        # project_id d'un autre site (contexte gardé dans un .arm, lot C5) : ignoré, le projet vient du chemin.
        other = {"path": nv["path"], "project_id": 999, "site": "https://elsewhere.shotgrid.autodesk.com"}
        self.assertEqual(s2.data("texture_pattern", other)["template"], "armorpaint_asset_textures")
        self.assertIn("context of another ShotGrid site ignored", s2.stderr())
        # Du site de la session, il compte : un autre projet que celui amorcé demande un redémarrage.
        self.assertEqual(s2.error("texture_pattern", dict(other, site=SITE))[0], "restart_required")

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
        # Sans interface, workfiles2 ne s'enregistre pas : la section contexte du moteur reste, et Publish... si les
        # modules Qt s'importent (tk-multi-publish2 s'enregistre toujours : son API sert sans fenêtre, op publish).
        commands = [c for c in menu["commands"] if c["app_instance"] != "tk-multi-publish2"]
        self.assertEqual([c["name"] for c in commands], self.CONTEXT_COMMANDS)
        self.assertEqual({c["type"] for c in commands}, {"context_menu"})
        self.assertEqual(menu["favourites"], ["Publish..."] if has_qt() else [])
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
        self.assertEqual([c["name"] for c in other["commands"] if c["type"] == "context_menu"], self.CONTEXT_COMMANDS)

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
        self.assertEqual(menu["favourites"], ["File Open...", "File Save...", "Publish..."])  # réglage menu_favourites

    def test_context_of_another_site(self):
        # Contexte gardé dans un .arm enregistré pour un autre site (lot C5) : ses identifiants ne valent rien ici.
        s = self.start("demo", STUDIO_SG_NO_UI="1")
        menu = s.data("list_commands", {"task_id": 1})
        self.assertEqual(menu["context"]["site"], SITE)
        other = s.data("list_commands", {"task_id": 2, "site": "https://elsewhere.shotgrid.autodesk.com"})
        self.assertEqual(other["context"]["task_id"], 1)
        self.assertIn("context of another ShotGrid site ignored", s.stderr())
        same = s.data("list_commands", {"task_id": 2, "site": "HTTPS://studio.test.shotgunstudio.com/"})
        self.assertEqual(same["context"]["task_id"], 2)

    def test_engine_missing(self):
        s = self.start()  # config sans tk-armorpaint
        code, reply = s.error("list_commands", {"task_id": 1})
        self.assertEqual(code, "app_unavailable")
        self.assertIn("tk-armorpaint", reply["error"]["message"])


def png(path, rgb=(200, 80, 30), size=4):
    """Un vrai PNG (la fenêtre Publish le charge en vignette)."""
    import struct
    import zlib

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + bytes(rgb) * size for _ in range(size))
    header = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    data = chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    write(path, b"\x89PNG\r\n\x1a\n" + data)


class FakeArmorPaint(object):
    """ArmorPaint pour les hooks du publish : les requêtes host de studio_workfiles.c et studio_publish.c, réponses de
    même forme, sur des fichiers. path : le projet ouvert ("" : jamais enregistré) ; current : l'export courant
    (Ctrl+E), des choix comme ceux de la fenêtre d'export, None s'il n'y en a pas eu ; tiles : les tuiles UDIM du
    maillage ; textures : celles du preset ; ext : leur format. refuse : {op: (code, message)}, requêtes refusées."""

    def __init__(self, path=""):
        self.path = path
        self.current = None
        self.tiles = ["1001", "1002"]
        self.textures = ["base", "nor"]
        self.ext = "png"
        self.refuse = {}
        self.requests = []

    def __call__(self, op, args):
        self.requests.append((op, args))
        if op in self.refuse:
            raise HostRefusal(*self.refuse[op])
        return getattr(self, "op_" + op)(args)

    def ops(self):
        return [op for op, _ in self.requests]

    def op_current_path(self, args):
        return {"path": self.path}

    def op_save(self, args):
        if not self.path:
            raise HostRefusal("no_path", "the project has never been saved: use File Save")
        write(self.path, "arm %s" % os.path.basename(self.path))

    def op_save_as(self, args):
        self.path = args["path"]
        self.op_save(args)

    def op_publish_thumbnail(self, args):
        png(args["path"])
        return {"path": args["path"], "width": 4, "height": 4}

    def op_publish_copy(self, args):
        if not self.path:
            raise HostRefusal("not_saved", "the project has never been saved")
        if args["path"] == self.path or not args["path"].endswith(".arm"):
            raise HostRefusal("bad_request", "path of the copy (.arm) expected")
        os.makedirs(os.path.dirname(args["path"]), exist_ok=True)
        shutil.copyfile(self.path, args["path"])
        return {"path": args["path"], "size": os.path.getsize(args["path"])}

    def op_publish_preflight(self, args):
        return self._export(args, False)

    def op_publish_export(self, args):
        return self._export(args, True)

    def _export(self, args, write_files):
        if not self.path:
            raise HostRefusal("not_saved", "the project has never been saved: save it first (File Save...)")
        naming = args.get("naming") or {}
        template = naming.get("template")
        choices = args.get("choices") or self.current
        reply = {"path": self.path, "naming_template": template}
        if choices is None:
            reply["export"] = {"ok": False, "report": None, "code": "no_preset",
                               "error": "no studio export in this session: export the textures once"}
            return reply
        skip = {tuple(k) for k in (args.get("skip") or []) + (choices.get("skip") or [])}
        per_tile = choices.get("mode") == 3 and self.tiles
        name = os.path.splitext(publish_name_of(self.path))[0]
        files, issues = [], []
        if self.tiles and not per_tile:
            issues.append({"level": "warning", "code": "udim_not_per_tile", "message": "UDIM tiles: export per tile"})
        for texture in self.textures:
            for tile in (self.tiles if per_tile else [""]):
                if template:
                    pattern = (naming["pattern_udim"] if tile else naming["pattern"]).replace("<channel>", texture)
                    pattern = pattern.replace("<ext>", self.ext)
                else:
                    pattern = os.path.join(
                        choices["dir"], "%s_%s%s.%s" % (name, texture, ".<UDIM>" if tile else "", self.ext)
                    )
                path = pattern.replace("<UDIM>", tile)
                files.append({"path": path, "texture": texture, "variant": "", "udim": tile,
                              "sequence": pattern if tile else None, "width": 64, "height": 64, "format": self.ext,
                              "bits": 8, "channels": 3, "compression": "", "color_space": "sRGB - Texture",
                              "size_estimate": 12288, "exists": os.path.exists(path),
                              "skip": (texture, "", tile) in skip or (texture, "*", "*") in skip})
        written = [f["path"] for f in files if not f["skip"]]
        if not written:
            issues.append({"level": "blocking", "code": "no_selection", "message": "No file selected"})
        ok = not any(i["level"] == "blocking" for i in issues)
        if ok and write_files:
            for path in written:
                png(path, (30, 120, 200))
        report = {"ok": ok, "preset": choices["preset"], "color_space_override": None,
                  "destination": os.path.dirname(written[0]) if written else choices["dir"], "width": 64, "height": 64,
                  "source_bits": 8, "mode": "per UDIM tile" if per_tile else "visible",
                  "total_size": 12288 * len(written),
                  "manifest": None, "files": files, "issues": issues}
        reply["export"] = {"ok": ok, "report": report, "error": "" if ok else "blocked by the preflight"}
        return reply


def publish_name_of(path):
    from studio_sg_session import publish_name

    return publish_name(path)


# Hook du sidecar pour les tests : lit la base du site simulé (PublishedFile et leurs dépendances).
FIND_HOOK = """
def setup(session):
    def find(args):
        return session._connect().find(args["entity_type"], args.get("filters", []), args.get("fields", []))
    session.ops["find"] = find
"""


class PublishBase(Base):
    def start_publish(self, **env):
        hook = os.path.join(self.workdir, "find_hook.py")
        write(hook, FIND_HOOK)
        s = self.start("demo", STUDIO_SG_NO_UI="1", STUDIO_SG_HOOK=hook, **env)
        nv = s.data("next_version", {"task_id": 1, "name": "robot"})
        host = FakeArmorPaint(nv["path"])
        host.op_save({})
        s.host = host
        self.work = os.path.dirname(nv["path"])
        self.publish_area = os.path.join(os.path.dirname(os.path.dirname(self.work)), "publish", "armorpaint")
        return s, host

    def published(self, s):
        fields = ["code", "name", "version_number", "path", "description", "task", "image",
                  "published_file_type.PublishedFileType.code"]
        files = {f["id"]: f for f in s.data("find", {"entity_type": "PublishedFile", "fields": fields})}
        for dep in s.data("find", {"entity_type": "PublishedFileDependency",
                                   "fields": ["published_file", "dependent_published_file"]}):
            depends_on = files[dep["published_file"]["id"]].setdefault("depends_on", [])
            depends_on.append(dep["dependent_published_file"]["id"])
        return files


@unittest.skipUnless(os.environ.get("STUDIO_TEST_TK_APPS"), "STUDIO_TEST_TK_APPS not set (Toolkit apps)")
@unittest.skipUnless(has_qt(), "PySide6 not importable by this Python (tk-multi-publish2 imports its Qt modules)")
class TestPublish(PublishBase):
    """Publish sans fenêtre (op publish, lot C4) : tk-multi-publish2 et les hooks du moteur
    (tk-armorpaint/hooks/tk-multi-publish2) sur le site simulé (scénario demo), contre un faux ArmorPaint. Le vrai
    ArmorPaint et la fenêtre Publish : publish_app.sh."""

    def test_publish_project_and_udim_textures(self):
        s, host = self.start_publish()
        host.current = {"preset": "basic", "dir": os.path.join(self.workdir, "elsewhere"), "mode": 3}
        result = s.data("publish", {"task_id": 1, "description": "first pass"}, timeout=180)
        self.assertTrue(result["finalized"], result)
        self.assertEqual(result["context"]["task_id"], 1)
        self.assertEqual([m for m in result["messages"] if m["level"] == "error"], [])
        by_item = {p["item"]: p for p in result["publishes"]}
        self.assertEqual(sorted(by_item), ["robot.v001.arm", "robot_base.<UDIM>.png", "robot_nor.<UDIM>.png"])

        # Le projet : copié par ArmorPaint (publish_copy) au template de publish, enregistré au type du sidecar.
        project = by_item["robot.v001.arm"]
        copy = os.path.join(self.publish_area, "robot.v001.arm")
        self.assertEqual((project["published_file_type"], project["version_number"]), ("ArmorPaint Project", 1))
        self.assertTrue(os.path.isfile(copy))
        self.assertIn(("publish_copy", {"path": copy, "context": result["context"]}), host.requests)

        # Les textures : exportées une fois, nommées par le template des textures (op texture_pattern), une séquence
        # <UDIM> par texture, copiées dans le dossier publish, dépendantes du projet.
        self.assertEqual(host.ops().count("publish_export"), 1)
        export = dict(host.requests)["publish_export"]
        self.assertEqual((export["naming"]["template"], export["skip"]), ("armorpaint_asset_textures", []))
        tex_work = os.path.join(self.work, "textures", "v001")
        tex_pub = os.path.join(self.publish_area, "textures", "v001")
        for tile in ("1001", "1002"):
            self.assertTrue(os.path.isfile(os.path.join(tex_work, "robot_base.%s.png" % tile)))
            self.assertTrue(os.path.isfile(os.path.join(tex_pub, "robot_base.%s.png" % tile)))
        base = by_item["robot_base.<UDIM>.png"]
        self.assertEqual((base["published_file_type"], base["version_number"]), ("UDIM Image", 1))
        files = self.published(s)
        self.assertEqual(len(files), 3)
        sg_base = files[base["id"]]
        self.assertEqual(sg_base["name"], "robot_base.<UDIM>.png")
        self.assertEqual(sg_base["published_file_type.PublishedFileType.code"], "UDIM Image")
        self.assertEqual(sg_base["path"]["local_path"], os.path.join(tex_pub, "robot_base.<UDIM>.png"))
        self.assertEqual(sg_base["depends_on"], [project["id"]])
        self.assertEqual(files[project["id"]]["path"]["local_path"], copy)
        self.assertEqual({f["description"] for f in files.values()}, {"first pass"})
        self.assertEqual({f["task"]["id"] for f in files.values()}, {1})

        # Vignette du viewport, une capture pour tous ; puis le projet passe à la version suivante.
        self.assertEqual(host.ops().count("publish_thumbnail"), 1)
        self.assertEqual(host.path, os.path.join(self.work, "robot.v002.arm"))
        self.assertTrue(os.path.isfile(host.path))
        self.assertEqual(host.ops()[-1], "save_as")

        # Deuxième publish, depuis la v002, une texture écartée : ni exportée ni publiée.
        host.requests = []
        result = s.data("publish", {"task_id": 1, "skip": ["robot_nor.<UDIM>.png"]}, timeout=180)
        self.assertEqual(sorted(p["item"] for p in result["publishes"]), ["robot.v002.arm", "robot_base.<UDIM>.png"])
        self.assertEqual(result["skipped"], ["robot_nor.<UDIM>.png"])
        export = dict(host.requests)["publish_export"]
        self.assertEqual(sorted(map(tuple, export["skip"])), [("nor", "", "1001"), ("nor", "", "1002")])
        self.assertFalse(os.path.exists(os.path.join(self.work, "textures", "v002", "robot_nor.1001.png")))
        self.assertTrue(os.path.isfile(os.path.join(self.publish_area, "textures", "v002", "robot_base.1002.png")))
        self.assertEqual(host.path, os.path.join(self.work, "robot.v003.arm"))
        names = sorted((f["name"], f["version_number"]) for f in self.published(s).values())
        self.assertEqual(names, [("robot.arm", 1), ("robot.arm", 2), ("robot_base.<UDIM>.png", 1),
                                 ("robot_base.<UDIM>.png", 2), ("robot_nor.<UDIM>.png", 1)])

    def test_udim_key_without_default(self):
        # Une séquence est enregistrée sous la valeur par défaut de sa clé : %04d sans default: "<UDIM>".
        s, host = self.start_publish(STUDIO_TEST_UDIM_DEFAULT="0")
        host.current = {"preset": "basic", "dir": self.workdir, "mode": 3}
        result = s.data("publish", {"task_id": 1, "skip": ["robot_nor.<UDIM>.png"]}, timeout=180)
        warnings = [m["message"] for m in result["messages"] if 'Set default: "<UDIM>"' in m["message"]]
        self.assertEqual(len(warnings), 1, result["messages"])
        self.assertIn("will list the texture as robot_base.%04d.png", warnings[0])
        paths = [f["path"]["local_path"] for f in self.published(s).values() if f["name"].startswith("robot_base")]
        self.assertEqual([os.path.basename(p) for p in paths], ["robot_base.%04d.png"])

    def test_texture_name_refused_by_template(self):
        # Le canal et l'extension remplacent des marqueurs que les clés ne filtrent pas (op texture_pattern) : un nom
        # que le template refuse empêche la copie vers le Publish Template, tk-multi-publish2 publierait en place.
        s, host = self.start_publish(STUDIO_TEST_CHANNEL_FILTER="alphanumeric")
        tp = s.data("texture_pattern", {"path": host.path})
        self.assertEqual(os.path.basename(tp["pattern_udim"]), "robot_<channel>.<UDIM>.<ext>")  # malgré le filtre
        host.current = {"preset": "basic", "dir": self.workdir, "mode": 3}
        host.textures = ["base_color", "nor"]
        code, reply = s.error("publish", {"task_id": 1}, timeout=180)
        self.assertEqual(code, "publish_invalid")
        message = reply["error"]["message"]
        refused = "robot_base_color.<UDIM>.png does not match the texture template armorpaint_asset_textures "
        self.assertIn(refused, message)
        self.assertIn("Illegal value 'base_color' does not fit filter_by 'alphanumeric'", message)
        self.assertNotIn("robot_nor", message)
        self.assertEqual(self.published(s), {})
        self.assertNotIn("publish_export", host.ops())

        host.textures, host.ext = ["base"], "tif"
        code, reply = s.error("publish", {"task_id": 1}, timeout=180)
        self.assertEqual(code, "publish_invalid")
        self.assertIn("Illegal value: 'tif' not in choices", reply["error"]["message"])
        self.assertEqual(self.published(s), {})

    def test_texture_name_refused_published_in_place(self):
        # Sans Publish Template, la texture est publiée où l'export l'écrit de toute façon : un avertissement.
        s, host = self.start_publish(STUDIO_TEST_CHANNEL_FILTER="alphanumeric", STUDIO_TEST_TEXTURES_COPY="0")
        host.current = {"preset": "basic", "dir": self.workdir, "mode": 3}
        host.textures = ["base_color"]
        result = s.data("publish", {"task_id": 1}, timeout=180)
        self.assertTrue(result["finalized"], result)
        warnings = [m["message"] for m in result["messages"] if "does not match the texture template" in m["message"]]
        self.assertEqual(len(warnings), 1, result["messages"])
        self.assertIn("will not recognize it", warnings[0])
        paths = [f["path"]["local_path"] for f in self.published(s).values() if f["name"].startswith("robot_base")]
        self.assertEqual(paths, [os.path.join(self.work, "textures", "v001", "robot_base_color.<UDIM>.png")])

    def test_export_settings_sources(self):
        # Sans export dans la session ni sidecar du projet : le preset du collecteur, par tuile (le maillage a des
        # tuiles UDIM). Sans ce réglage : le projet seul.
        s, host = self.start_publish()
        result = s.data("publish", {"task_id": 1}, timeout=180)
        self.assertEqual([p["item"] for p in result["publishes"]], ["robot.v001.arm"])
        self.assertTrue(any("No texture export" in m["message"] for m in result["messages"]), result["messages"])
        self.assertNotIn("publish_export", host.ops())

        # Le sidecar du projet (dernier export de la fenêtre d'export) passe avant l'export courant.
        import studio_export_choices

        host.current = {"preset": "basic", "dir": self.workdir, "mode": 0}
        sidecar = studio_export_choices.sidecar_path(host.path)
        write(sidecar, json.dumps({"export": {"preset": "karma", "dir": self.workdir, "mode": 3,
                                              "presets": {"karma": {}}}}))
        host.requests = []
        result = s.data("publish", {"task_id": 1}, timeout=180)
        export = dict(host.requests)["publish_export"]
        self.assertEqual((export["choices"]["preset"], export["choices"]["mode"]), ("karma", 3))

    def test_textures_without_version(self):
        # Noms d'ArmorPaint (pas de template) : la texture publiée en place serait réécrite par l'export suivant.
        s, host = self.start_publish(STUDIO_SG_NAMING="0")
        host.current = {"preset": "basic", "dir": os.path.join(self.workdir, "out"), "mode": 3}
        result = s.data("publish", {"task_id": 1}, timeout=180)
        self.assertEqual([p["item"] for p in result["publishes"]], ["robot.v001.arm"])
        self.assertTrue(any("no version in its name" in m["message"] for m in result["messages"]), result["messages"])
        self.assertNotIn("publish_export", host.ops())

    def test_invalid_publishes_nothing(self):
        s, host = self.start_publish()
        host.current = {"preset": "basic", "dir": self.workdir, "mode": 3}
        write(os.path.join(self.work, "robot.v002.arm"), "arm")  # la version suivante existe déjà
        code, reply = s.error("publish", {"task_id": 1}, timeout=180)
        self.assertEqual(code, "publish_invalid")
        self.assertIn("next version of this project already exists", reply["error"]["message"])
        self.assertEqual(self.published(s), {})
        self.assertNotIn("publish_export", host.ops())

        host.path = ""  # jamais enregistré
        code, reply = s.error("publish", {"task_id": 1}, timeout=180)
        self.assertEqual(code, "publish_invalid")
        self.assertIn("never been saved", reply["error"]["message"])
        self.assertEqual(s.error("publish", {"task_id": 1, "skip": ["armorpaint.session"]})[0], "nothing_to_publish")
        self.assertEqual(s.error("publish", {"task_id": 1, "skip": "all"})[0], "bad_request")

    def test_project_of_another_context(self):
        # Ouvert autrement que par File Open (menu File d'ArmorPaint, fichiers récents), le projet garde le contexte de
        # la dernière fenêtre : ici la tâche d'un autre asset (tree). Ses publishes iraient à cette tâche : refusé.
        s, host = self.start_publish()
        host.current = {"preset": "basic", "dir": self.workdir, "mode": 3}
        code, reply = s.error("publish", {"task_id": 2}, timeout=180)
        self.assertEqual(code, "publish_invalid")
        message = reply["error"]["message"]
        self.assertIn("robot.v001.arm (Publish ArmorPaint Project): The project belongs to", message)
        self.assertIn("robot_base.<UDIM>.png (Publish Textures): The project belongs to", message)
        self.assertIn("File Open", message)
        self.assertEqual(self.published(s), {})
        self.assertNotIn("publish_export", host.ops())
        # Textures seules, sans le projet : refusées aussi.
        code, reply = s.error("publish", {"task_id": 2, "skip": ["armorpaint.session"]}, timeout=180)
        self.assertEqual(code, "publish_invalid")
        self.assertEqual(self.published(s), {})
        # Dans le contexte de sa tâche, il se publie.
        result = s.data("publish", {"task_id": 1}, timeout=180)
        self.assertTrue(result["finalized"], result)
        self.assertEqual(len(self.published(s)), 3)

    def test_export_failure(self):
        s, host = self.start_publish()
        host.current = {"preset": "basic", "dir": self.workdir, "mode": 3}
        host.refuse["publish_export"] = ("write_failed", "disk full")
        code, reply = s.error("publish", {"task_id": 1}, timeout=180)
        self.assertEqual(code, "publish_failed")
        self.assertIn("disk full", reply["error"]["message"])
        self.assertIn("already published: robot.v001.arm", reply["error"]["message"])
        self.assertEqual(host.path, os.path.join(self.work, "robot.v001.arm"))  # pas de version suivante


@unittest.skipUnless(os.environ.get("STUDIO_TEST_TK_APPS"), "STUDIO_TEST_TK_APPS not set (Toolkit apps)")
@unittest.skipUnless(has_qt(), "PySide6 not importable by this Python (tk-multi-publish2 imports its Qt modules)")
class TestQueue(PublishBase):
    """Mode dégradé (lot C5) : ShotGrid injoignable pendant un publish, file d'attente et rejeu, sur le site simulé au
    réseau coupé à la demande (sg_fixture.py). La file et le journal d'audit sont lus sur disque, comme ArmorPaint."""

    def env(self, queue=True, **env):
        self.flag = os.path.join(self.workdir, "offline")
        self.queue_dir = os.path.join(self.workdir, "queue")
        self.audit = os.path.join(self.workdir, "audit.jsonl")
        return dict(STUDIO_SG_QUEUE=self.queue_dir if queue else None, STUDIO_AUDIT_LOG=self.audit,
                    STUDIO_TEST_OFFLINE_FLAG=self.flag, **env)

    def start_queue(self, queue=True, **env):
        s, host = self.start_publish(**self.env(queue, **env))
        host.current = {"preset": "basic", "dir": self.workdir, "mode": 3}
        return s, host

    def restart(self, s, **env):
        """ArmorPaint relancé : un autre sidecar (et un site simulé tout neuf, sans les publishes du précédent)."""
        s.close()
        self.sidecars.remove(s)
        return self.start("demo", STUDIO_SG_NO_UI="1", STUDIO_SG_HOOK=os.path.join(self.workdir, "find_hook.py"),
                          **self.env(**env))

    def online(self):
        if os.path.exists(self.flag):
            os.remove(self.flag)

    def records(self):
        import studio_sg_queue

        return studio_sg_queue.Queue(self.queue_dir).records()

    def events(self):
        if not os.path.exists(self.audit):
            return []
        with open(self.audit) as f:
            return [json.loads(line) for line in f]

    def test_queued_mid_publish_then_replayed(self):
        # Le projet est enregistré, puis ShotGrid tombe : les textures (fichiers écrits) vont dans la file.
        s, host = self.start_queue(STUDIO_TEST_OFFLINE_AT="create:PublishedFile:2")
        reply = s.call("publish", {"task_id": 1, "description": "offline pass"}, timeout=180)
        self.assertTrue(reply["ok"], reply)
        self.assertFalse(reply["sg"]["online"])
        self.assertIn("Connection refused", reply["sg"]["reason"])
        result = reply["data"]
        self.assertTrue(result["finalized"], result)
        self.assertEqual(result["context"]["site"], SITE)
        by_item = {p["item"]: p for p in result["publishes"]}
        project = by_item["robot.v001.arm"]
        self.assertGreater(project["id"], 0)
        self.assertIsNone(project["queued"])
        textures = [by_item["robot_base.<UDIM>.png"], by_item["robot_nor.<UDIM>.png"]]
        self.assertEqual([t["id"] for t in textures], [None, None])
        self.assertEqual(result["queued"], [t["queued"] for t in textures])
        messages = [m["message"] for m in result["messages"] if m["level"] == "warning"]
        self.assertEqual(len([m for m in messages if "is queued" in m]), 2, messages)
        self.assertEqual(len([m for m in messages if "keep their status" in m]), 1, messages)  # finalisation du projet
        self.assertEqual(host.path, os.path.join(self.work, "robot.v002.arm"))  # version suivante quand même

        records = self.records()
        self.assertEqual([r["key"] for r in records], result["queued"])
        for r in records:
            self.assertEqual((r["project_id"], r["task_id"], r["depends_on"]), (1, 1, []))
            self.assertEqual(r["dependency_ids"], [project["id"]])
            self.assertEqual((r["comment"], r["published_file_type"], r["version_number"]), ("offline pass", "UDIM Image", 1))
            self.assertEqual((r["site"], r["context"]["task"]["id"], r["attempts"]), (SITE, 1, 0))
            self.assertTrue(os.path.isfile(os.path.join(self.queue_dir, r["thumbnail"])))  # copie de la vignette
        pending = s.data("pending_publishes")
        self.assertEqual((pending["count"], pending["queue"]), (2, self.queue_dir))
        self.assertEqual([p["name"] for p in pending["publishes"]], ["robot_base.<UDIM>.png", "robot_nor.<UDIM>.png"])

        # Rejeu hors ligne : arrêté au premier, rien ne change.
        replay = s.data("replay_publishes")
        self.assertEqual((replay["replayed"], replay["failed"], replay["pending"]), ([], [], 2))
        self.assertIn("Connection refused", replay["stopped"])
        self.assertEqual([r["attempts"] for r in self.records()], [1, 0])

        # En ligne : enregistrés dans l'ordre, dépendants du projet, avec leur description.
        self.online()
        reply = s.call("replay_publishes")
        self.assertEqual(reply["sg"], {"online": True})
        replay = reply["data"]
        self.assertEqual([r["key"] for r in replay["replayed"]], result["queued"])
        self.assertEqual((replay["project_id"], replay["pending"], replay["stopped"], replay["failed"]), (1, 0, None, []))
        files = self.published(s)
        self.assertEqual(len(files), 3)
        for r in replay["replayed"]:
            self.assertEqual(files[r["id"]]["depends_on"], [project["id"]])
            self.assertEqual(files[r["id"]]["description"], "offline pass")
            self.assertEqual(files[r["id"]]["task"]["id"], 1)
        # À la date du publish, pas à celle du rejeu.
        import studio_sg_queue

        when = {f["id"]: f["created_at"] for f in s.data("find", {"entity_type": "PublishedFile", "fields": ["created_at"]})}
        for r, record in zip(replay["replayed"], records):
            self.assertEqual(studio_sg_queue.parse_iso(when[r["id"]]), studio_sg_queue.parse_iso(record["created"]))
        self.assertEqual(self.records(), [])
        self.assertEqual(s.data("replay_publishes")["replayed"], [])

        events = self.events()
        self.assertEqual([e["event"] for e in events],
                         ["publish", "publish_queued", "publish_queued", "publish_replayed", "publish_replayed"])
        self.assertEqual({(e["source"], e["project_id"], e["task_id"]) for e in events}, {("sidecar", 1, 1)})
        self.assertEqual(events[0]["id"], project["id"])
        self.assertEqual([e["key"] for e in events[1:3]], result["queued"])

    def test_lost_response_not_duplicated(self):
        # Le PublishedFile du projet est créé, mais la réponse se perd : tout part dans la file, les textures
        # dépendent du projet en file. Au rejeu, le projet est retrouvé, pas recréé.
        s, host = self.start_queue(STUDIO_TEST_LOST_AT="create:PublishedFile:1")
        result = s.data("publish", {"task_id": 1}, timeout=180)
        self.assertEqual([p["id"] for p in result["publishes"]], [None, None, None])
        records = self.records()
        self.assertEqual([r["key"] for r in records], result["queued"])
        project_key = records[0]["key"]
        self.assertEqual((records[0]["name"], records[0]["entity_id"]), ("robot.arm", None))
        self.assertEqual([r["depends_on"] for r in records[1:]], [[project_key], [project_key]])
        self.assertEqual([r["dependency_ids"] for r in records[1:]], [[], []])
        self.assertEqual(host.path, os.path.join(self.work, "robot.v002.arm"))

        self.online()
        created = list(self.published(s))
        self.assertEqual(len(created), 1)
        replay = s.data("replay_publishes")
        self.assertEqual(replay["replayed"][0]["id"], created[0])
        files = self.published(s)
        self.assertEqual(len(files), 3)
        for r in replay["replayed"][1:]:
            self.assertEqual(files[r["id"]]["depends_on"], created)

    def test_replay_interrupted_and_discard(self):
        s, host = self.start_queue(STUDIO_TEST_OFFLINE_AT="create:PublishedFile:1")
        result = s.data("publish", {"task_id": 1}, timeout=180)
        project_key, base_key, nor_key = result["queued"]

        # ArmorPaint relancé, et ShotGrid qui retombe au deuxième enregistrement du rejeu.
        self.online()
        s = self.restart(s, STUDIO_TEST_OFFLINE_AT="create:PublishedFile:2")
        self.assertEqual(s.data("pending_publishes")["count"], 3)
        replay = s.data("replay_publishes")
        self.assertEqual([r["key"] for r in replay["replayed"]], [project_key])
        self.assertIn("Connection refused", replay["stopped"])
        self.assertEqual(replay["pending"], 2)
        project_id = replay["replayed"][0]["id"]
        records = {r["key"]: r for r in self.records()}
        self.assertEqual((records[base_key]["depends_on"], records[base_key]["dependency_ids"]), ([], [project_id]))
        self.assertEqual((records[base_key]["attempts"], records[nor_key]["attempts"]), (1, 0))
        self.assertIn("Connection refused", records[base_key]["error"])

        # Écarté : jamais rejoué, gardé à part.
        self.assertEqual(s.data("discard_publishes", {"keys": [nor_key, "nope"]}),
                         {"discarded": [nor_key], "missing": ["nope"]})
        self.assertTrue(os.path.isfile(os.path.join(self.queue_dir, "discarded", nor_key + ".json")))
        self.assertEqual(s.error("discard_publishes", {"keys": ["../%s" % base_key]})[0], "bad_request")
        self.assertEqual(s.error("discard_publishes", {"keys": []})[0], "bad_request")

        self.online()
        replay = s.data("replay_publishes")
        self.assertEqual([r["key"] for r in replay["replayed"]], [base_key])
        self.assertEqual(self.published(s)[replay["replayed"][0]["id"]]["depends_on"], [project_id])
        self.assertEqual(s.data("pending_publishes")["count"], 0)
        events = [e["event"] for e in self.events()]
        self.assertEqual(events.count("publish_queued"), 3)
        self.assertEqual(events.count("publish_replayed"), 2)
        self.assertEqual(events.count("publish_discarded"), 1)

    def test_without_queue_the_publish_fails(self):
        s, host = self.start_queue(queue=False, STUDIO_TEST_OFFLINE_AT="create:PublishedFile:1")
        code, reply = s.error("publish", {"task_id": 1}, timeout=180)
        self.assertEqual(code, "publish_failed")
        self.assertIn("Connection refused", reply["error"]["message"])
        self.assertFalse(reply["sg"]["online"])
        self.assertEqual([e["event"] for e in self.events()], ["publish_failed"])
        self.assertEqual(s.data("pending_publishes"), {"queue": None, "count": 0, "publishes": []})
        self.assertEqual(s.data("replay_publishes")["replayed"], [])

    def test_offline_before_publish(self):
        # ShotGrid injoignable dès le départ : rien n'est écrit ni mis en file, l'erreur le dit.
        s, host = self.start_queue()
        write(self.flag, "")
        code, reply = s.error("publish", {"task_id": 1}, timeout=180)
        self.assertEqual(code, "sg_unreachable")
        self.assertFalse(reply["sg"]["online"])
        self.assertEqual(self.records(), [])
        self.assertNotIn("publish_copy", host.ops())
        self.assertEqual(s.error("check_online")[0], "sg_unreachable")
        self.online()
        reply = s.call("check_online")
        self.assertTrue(reply["ok"], reply)
        self.assertEqual(reply["sg"], {"online": True})
        self.assertEqual(reply["data"]["site"], SITE)
        result = s.data("publish", {"task_id": 1}, timeout=180)
        self.assertEqual((result["queued"], len(self.published(s))), ([], 3))


class TestOffline(Base):
    """Mode dégradé (lot C5) sans Toolkit ni apps : l'état réseau des réponses, le délai des connexions, et les
    briques du sidecar (classement des erreurs, surveillance de shotgun_api3, file d'attente, journal d'audit)."""

    def test_net_timeout(self):
        self.assertEqual(self.start().hello["data"]["net_timeout"], 30.0)
        self.assertEqual(self.start(STUDIO_SG_NET_TIMEOUT="2.5").hello["data"]["net_timeout"], 2.5)
        self.assertIsNone(self.start(STUDIO_SG_NET_TIMEOUT="0").hello["data"]["net_timeout"])

    def test_replies_carry_the_network_state(self):
        s = self.start()
        reply = s.call("list_projects")
        self.assertEqual(reply["sg"], {"online": True})
        self.assertNotIn("sg", s.call("ping"))  # pas de requête ShotGrid
        self.assertEqual(s.data("check_online")["site"], SITE)
        s = self.start("offline")
        code, reply = s.error("list_projects")
        self.assertEqual(code, "sg_unreachable")
        self.assertEqual(reply["sg"]["online"], False)
        self.assertIn("503", reply["sg"]["reason"])
        self.assertEqual(s.error("check_online")[0], "sg_unreachable")

    def test_unreachable(self):
        import errno
        import socket

        from studio_sg_session import unreachable

        class ProtocolError(Exception):
            pass

        class Fault(Exception):
            pass

        def chained(inner):
            try:
                try:
                    raise inner
                except Exception as e:
                    raise RuntimeError("wrapped") from e
            except RuntimeError as e:
                return e

        self.assertTrue(unreachable(ConnectionRefusedError(errno.ECONNREFUSED, "refused")))
        self.assertTrue(unreachable(OSError(errno.ENETUNREACH, "network is unreachable")))
        self.assertTrue(unreachable(socket.timeout("timed out")))
        self.assertTrue(unreachable(socket.gaierror(8, "nodename nor servname provided")))
        self.assertTrue(unreachable(ProtocolError("503")))
        self.assertTrue(unreachable(chained(ConnectionResetError())))
        self.assertFalse(unreachable(OSError(errno.ENOSPC, "no space left")))
        self.assertFalse(unreachable(Fault("API update() CRUD ERROR")))
        self.assertFalse(unreachable(chained(ValueError("bad"))))
        self.assertFalse(unreachable(None))

    def test_watch_network(self):
        # Chaque appel de l'API (Shotgun._call_rpc) dit au superviseur si le site a répondu.
        import types

        import studio_sg_bridge
        from studio_sg_session import Session

        class Fault(Exception):
            pass

        class Shotgun(object):
            outcome = None

            def _call_rpc(self, method, params):
                if self.outcome is not None:
                    raise self.outcome
                return {"method": method}

        module = types.ModuleType("fake_vendor.shotgun_api3")
        module.Shotgun = Shotgun
        sys.modules[module.__name__] = module
        try:
            Session._watch_network()
            Session._watch_network()  # une seule fois
            sg = Shotgun()
            self.assertEqual(sg._call_rpc("info", None), {"method": "info"})
            self.assertEqual((studio_sg_bridge.OFFLINE, studio_sg_bridge._seen), (None, {"online": True}))
            sg.outcome = ConnectionRefusedError(61, "Connection refused")
            self.assertRaises(ConnectionRefusedError, sg._call_rpc, "find", None)
            self.assertIn("Connection refused", studio_sg_bridge.OFFLINE)
            self.assertFalse(studio_sg_bridge._seen["online"])
            sg.outcome = Fault("permission denied")  # le site a répondu
            self.assertRaises(Fault, sg._call_rpc, "update", None)
            self.assertEqual((studio_sg_bridge.OFFLINE, studio_sg_bridge._seen), (None, {"online": True}))
        finally:
            del sys.modules[module.__name__]
            studio_sg_bridge.network(True)

    def test_queue_store(self):
        import studio_sg_queue

        q = studio_sg_queue.Queue(os.path.join(self.workdir, "q"))
        self.assertEqual((q.keys(), q.records()), ([], []))
        thumb = os.path.join(self.workdir, "thumb.png")
        png(thumb)
        first = q.add({"path": "/a/robot.v001.arm", "name": "robot.arm"}, thumb)
        second = q.add({"path": "/a/robot_base.v001.png", "depends_on": [first]})
        self.assertEqual(q.keys(), [first, second])  # dans l'ordre de la mise en file
        record = q.load(first)
        self.assertEqual((record["version"], record["attempts"], record["thumbnail"]), (1, 0, first + ".png"))
        self.assertEqual(q.thumbnail_path(record), os.path.join(q.directory, first + ".png"))
        self.assertIsNotNone(studio_sg_queue.parse_iso(record["created"]))
        self.assertRaises(TypeError, q.add, {"when": object()})
        self.assertEqual(q.count(), 2)
        record["error"] = "later"
        q.save(record)
        self.assertEqual(q.load(first)["error"], "later")
        for bad in ("../" + first, first + ".json", "", None, "a/b"):
            self.assertFalse(studio_sg_queue.valid_key(bad), bad)
            self.assertIsNone(q.load(bad))
        q.discard(record)
        self.assertEqual(q.keys(), [second])
        self.assertTrue(os.path.isfile(os.path.join(q.directory, "discarded", first + ".png")))
        q.remove(q.load(second))
        self.assertEqual(q.count(), 0)
        write(os.path.join(q.directory, "broken.json"), "{")
        self.assertEqual(q.records(), [])  # illisible : sauté
        # une vignette hors de la file (enregistrement modifié à la main) n'est ni lue, ni supprimée, ni déplacée
        outside = os.path.join(self.workdir, "outside.png")
        png(outside)
        for name in (outside, "../outside.png", "..", "sub/x.png"):
            key = q.add({"path": "/a/robot.v002.arm"})
            record = dict(q.load(key), thumbnail=name)
            q.save(record)
            self.assertIsNone(q.thumbnail_path(record), name)
            (q.discard if name == ".." else q.remove)(q.load(key))
            self.assertIsNone(q.load(key), name)
            self.assertTrue(os.path.isfile(outside), name)
        self.assertTrue(os.path.isdir(q.directory))

    def test_audit(self):
        import studio_audit

        path = os.path.join(self.workdir, "audit.jsonl")
        old = os.environ.get("STUDIO_AUDIT_LOG")
        try:
            os.environ.pop("STUDIO_AUDIT_LOG", None)
            self.assertFalse(studio_audit.write("publish"))
            os.environ["STUDIO_AUDIT_LOG"] = path
            self.assertTrue(studio_audit.write("publish", path="/a/é.arm", id=3, error=None))
            self.assertTrue(studio_audit.write("publish_queued", key="k"))
            os.environ["STUDIO_AUDIT_LOG"] = os.path.join(self.workdir, "missing", "audit.jsonl")
            self.assertFalse(studio_audit.write("publish"))  # dossier absent : signalé, pas bloquant
        finally:
            os.environ.pop("STUDIO_AUDIT_LOG", None)
            if old is not None:
                os.environ["STUDIO_AUDIT_LOG"] = old
        with open(path, "rb") as f:
            lines = f.read().decode("ascii").splitlines()
        events = [json.loads(line) for line in lines]
        self.assertEqual([e["event"] for e in events], ["publish", "publish_queued"])
        self.assertEqual((events[0]["path"], events[0]["id"], events[0]["source"]), ("/a/é.arm", 3, "sidecar"))
        self.assertNotIn("error", events[0])
        self.assertEqual(set(events[1]), {"time", "source", "pid", "host", "user", "event", "key"})
        self.assertRegex(events[1]["time"], r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$")


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
