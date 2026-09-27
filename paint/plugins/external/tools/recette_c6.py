#!/usr/bin/env python3
# Recette C6 : avant l'aller-retour dans ArmorPaint, vérifie le poste et la configuration Toolkit du studio sur le vrai
# site ShotGrid, par le sidecar d'ArmorPaint lui-même (même script, même protocole que studio_shotgrid.c). Pour chaque
# point, le rapport dit ce qui va et quoi corriger. Fiche de recette : docs/RECETTE_C6.md.
#
#   tools/recette_c6.py                      poste, site, session ShotGrid Desktop, projets : lecture seule
#   tools/recette_c6.py --project <id|nom>   + tâches de l'artiste dans le projet, configurations de pipeline
#   tools/recette_c6.py --task <id>          + le moteur tk-armorpaint dans le contexte de la tâche : apps, templates,
#                                              réglages du publish, types de publish, nom des textures. Comme ArmorPaint
#                                              à la première ouverture du menu ShotGrid, crée les dossiers de la tâche.
#
# Options : --python (interpréteur du sidecar ; défaut STUDIO_SG_PYTHON, sinon python3, comme ArmorPaint), --bridge
# (script du sidecar ; défaut STUDIO_SG_BRIDGE, sinon assets/studio_sg_bridge.py), --report <fichier.md> (le rapport en
# Markdown, à joindre à la fiche), --timeout <s> (par requête, défaut 300 : un premier amorçage télécharge la
# configuration du projet). Les réglages STUDIO_SG_* de l'environnement sont ceux d'ArmorPaint : lancer l'outil dans
# l'environnement du lanceur (rez…). Code de retour : 0 sans échec, 1 sinon.

import argparse
import datetime
import json
import os
import platform
import select
import socket
import subprocess
import sys
import tempfile
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
EXT = os.path.dirname(TOOLS)
HOOK = os.path.join(TOOLS, "recette_c6_hook.py")

OK, WARN, FAIL, INFO = "ok", "attention", "ÉCHEC", "info"

# Réglages montrés au rapport (jamais STUDIO_SG_SCRIPT_KEY : seulement s'il est défini).
SETTINGS = ("STUDIO_SG_PYTHON", "STUDIO_PYTHON", "STUDIO_SGTK_PATH", "SHOTGUN_BUNDLE_CACHE_PATH", "SHOTGUN_HOME",
            "STUDIO_SG_HOST", "SHOTGUN_SITE", "STUDIO_SG_SCRIPT_NAME", "STUDIO_SG_ENGINE", "STUDIO_SG_PLUGIN_ID",
            "STUDIO_SG_BASE_CONFIGURATION", "STUDIO_SG_WORK_TEMPLATES", "STUDIO_SG_TEXTURE_TEMPLATES",
            "STUDIO_SG_TEXTURE_KEYS", "STUDIO_SG_PUBLISH_TYPE", "STUDIO_SG_NET_TIMEOUT", "STUDIO_SG_HOOK",
            "STUDIO_SG_QUEUE", "STUDIO_AUDIT_LOG", "STUDIO_PREFS", "OCIO")


class Report(object):
    def __init__(self):
        self.rows = []

    def add(self, status, what, detail="", fix=""):
        self.rows.append((status, what, detail or "", fix or ""))
        print("%-9s %s%s" % (status, what, " : " + detail if detail else ""), flush=True)
        if fix and status in (FAIL, WARN):
            print("          -> " + fix, flush=True)

    def count(self, status):
        return sum(1 for r in self.rows if r[0] == status)

    def markdown(self, title, facts):
        cell = lambda s: s.replace("|", "\\|").replace("\n", " ")  # noqa: E731
        lines = ["# %s" % title, ""]
        lines += ["- %s : %s" % (k, cell(v)) for k, v in facts]
        lines += ["", "| État | Point | Détail | À faire |", "|---|---|---|---|"]
        lines += ["| %s | %s | %s | %s |" % tuple(cell(c) for c in row) for row in self.rows]
        lines += ["", "%d échec(s), %d point(s) d'attention." % (self.count(FAIL), self.count(WARN)), ""]
        return "\n".join(lines)


class SidecarError(Exception):
    pass


class Sidecar(object):
    """Le sidecar lancé comme par le superviseur d'ArmorPaint (studio_sg.c) : une ligne JSON par message sur
    stdin/stdout, stderr dans un fichier (le journal de l'amorçage). Une réponse "restart" le relance et rejoue la
    requête, comme le superviseur."""

    def __init__(self, argv, env, timeout, log_path):
        self.argv, self.env, self.timeout, self.log_path = argv, env, timeout, log_path
        self.proc = None
        self.log = open(log_path, "a")
        self.next_id = 1
        self.hello = None

    def start(self):
        self.proc = subprocess.Popen(self.argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log,
                                     env=self.env, start_new_session=True)
        self.hello = self._read(self.timeout)
        if self.hello is None or self.hello.get("id") != 0:
            raise SidecarError("the sidecar did not start: %s" % (self.hello or self.tail()))
        return self.hello

    def _read(self, timeout):
        deadline = time.monotonic() + timeout
        line = b""
        while not line.endswith(b"\n"):
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([self.proc.stdout], [], [], left)[0]:
                raise SidecarError("no reply within %d s" % timeout)
            chunk = os.read(self.proc.stdout.fileno(), 65536 if line else 1)
            if not chunk:
                return None
            line += chunk
        return json.loads(line.decode())

    def call(self, op, args=None):
        for _ in range(2):
            message_id = self.next_id
            self.next_id += 1
            request = json.dumps({"id": message_id, "op": op, "args": args or {}}) + "\n"
            self.proc.stdin.write(request.encode())
            self.proc.stdin.flush()
            while True:
                reply = self._read(self.timeout)
                if reply is None:
                    raise SidecarError("the sidecar stopped during %s: %s" % (op, self.tail()))
                if "host" not in reply:
                    break
                # Pas d'ArmorPaint ici : aucune requête de la recette n'en envoie.
                refusal = {"host": reply["host"], "ok": False,
                           "error": {"code": "unavailable", "message": "no ArmorPaint in the C6 recipe tool"}}
                self.proc.stdin.write((json.dumps(refusal) + "\n").encode())
                self.proc.stdin.flush()
            if not reply.get("restart"):
                return reply
            self.close()  # changement de projet Toolkit : le sidecar s'arrête, on le relance et on rejoue
            self.start()
        return reply

    def tail(self, size=2000):
        self.log.flush()
        with open(self.log_path) as f:
            return f.read()[-size:]

    def close(self):
        if self.proc is None:
            return
        if not self.proc.stdin.closed:
            self.proc.stdin.close()
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.proc.stdout.close()
        self.proc = None


def error_of(reply):
    e = reply.get("error") or {}
    return e.get("code", "?"), e.get("message", "")


def check_python(report, python, role, need_qt):
    code = ("import json, sys\ntry:\n    import PySide6\n    qt = PySide6.__version__\nexcept Exception:\n    qt = None\n"
            "print(json.dumps({'version': sys.version.split()[0], 'exe': sys.executable, 'qt': qt}))")
    try:
        out = subprocess.run([python, "-c", code], capture_output=True, text=True, timeout=120)
        info = json.loads(out.stdout.strip().splitlines()[-1])
    except Exception as e:  # noqa: BLE001 — introuvable, pas un Python, sortie illisible
        report.add(FAIL, "Python du %s" % role, "%s : %s" % (python, e),
                   "chemin complet d'un Python 3.9+ : ArmorPaint lancé depuis le Finder n'a qu'un PATH minimal")
        return None
    version = tuple(int(p) for p in info["version"].split(".")[:2] if p.isdigit())
    if version < (3, 9):
        report.add(FAIL, "Python du %s" % role, "%s %s" % (info["exe"], info["version"]), "Python 3.9 ou plus")
        return None
    report.add(OK, "Python du %s" % role, "%s %s" % (info["exe"], info["version"]))
    if info["qt"]:
        report.add(OK, "PySide6 (%s)" % role, info["qt"])
    elif need_qt:
        report.add(WARN, "PySide6 (%s)" % role, "absent : les fenêtres Toolkit (File Open, Publish...) ne s'ouvriront pas",
                   "un Python avec PySide6 : celui de ShotGrid Desktop, ou tools/setup_shotgrid.sh")
    else:
        report.add(WARN, "PySide6 (%s)" % role, "absent : la fenêtre d'export ne s'ouvrira pas",
                   "STUDIO_PYTHON vers un Python avec PySide6")
    return info


def main():
    parser = argparse.ArgumentParser(description="Recette C6 : poste et configuration Toolkit sur le vrai site ShotGrid.")
    parser.add_argument("--project", help="projet de la recette (id ou nom)")
    parser.add_argument("--task", type=int, help="tâche de la recette (id) : crée ses dossiers, comme ArmorPaint")
    parser.add_argument("--python", help="interpréteur du sidecar (défaut : STUDIO_SG_PYTHON, sinon python3)")
    parser.add_argument("--bridge", help="script du sidecar (défaut : STUDIO_SG_BRIDGE, sinon assets/studio_sg_bridge.py)")
    parser.add_argument("--report", help="écrit le rapport en Markdown dans ce fichier")
    parser.add_argument("--timeout", type=int, default=300, help="délai par requête, en secondes (défaut 300)")
    opts = parser.parse_args()

    report = Report()
    started = datetime.datetime.now().astimezone()
    python = opts.python or os.environ.get("STUDIO_SG_PYTHON") or "python3"
    bridge = opts.bridge or os.environ.get("STUDIO_SG_BRIDGE") or os.path.join(EXT, "assets", "studio_sg_bridge.py")
    work = tempfile.mkdtemp(prefix="recette_c6_")
    log_path = os.path.join(work, "sidecar.log")
    facts = [("date", started.strftime("%Y-%m-%d %H:%M %Z")), ("poste", "%s, %s %s" % (
        socket.gethostname(), platform.system(), platform.mac_ver()[0] or platform.release())),
        ("sidecar", "%s %s" % (python, bridge)), ("journal du sidecar", log_path)]
    for name in SETTINGS:
        if os.environ.get(name):
            facts.append((name, os.environ[name]))
    if os.environ.get("STUDIO_SG_SCRIPT_KEY"):
        facts.append(("STUDIO_SG_SCRIPT_KEY", "défini"))

    print("== Poste", flush=True)
    check_python(report, python, "sidecar ShotGrid", need_qt=True)
    export_python = os.environ.get("STUDIO_PYTHON")
    if export_python and export_python != python:
        check_python(report, export_python, "fenêtre d'export", need_qt=False)
    if not os.path.isfile(bridge):
        report.add(FAIL, "script du sidecar", "%s introuvable" % bridge, "STUDIO_SG_BRIDGE, ou data/ d'ArmorPaint")

    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", STUDIO_SG_HOOK=HOOK)
    env.pop("STUDIO_RECIPE_STUDIO_HOOK", None)
    if os.environ.get("STUDIO_SG_HOOK"):  # le hook du studio reste chargé, avant celui de la recette
        env["STUDIO_RECIPE_STUDIO_HOOK"] = os.environ["STUDIO_SG_HOOK"]
    sidecar = Sidecar([python, bridge], env, opts.timeout, log_path)
    try:
        if report.count(FAIL) == 0:
            run(report, sidecar, opts)
    except SidecarError as e:
        report.add(FAIL, "sidecar", str(e), "voir le journal du sidecar : %s" % log_path)
    finally:
        sidecar.close()

    print("\n%d échec(s), %d point(s) d'attention. Journal du sidecar : %s" % (
        report.count(FAIL), report.count(WARN), log_path), flush=True)
    if opts.report:
        with open(opts.report, "w", encoding="utf-8") as f:
            f.write(report.markdown("Recette C6 : poste et configuration", facts))
        print("Rapport : %s" % opts.report, flush=True)
    return 1 if report.count(FAIL) else 0


def run(report, sidecar, opts):
    print("\n== Sidecar et site", flush=True)
    hello = sidecar.start()["data"]
    toolkit = hello.get("toolkit") or {}
    if not toolkit.get("available"):
        report.add(FAIL, "Toolkit (tk-core)", toolkit.get("error") or "introuvable",
                   "STUDIO_SGTK_PATH, ou ShotGrid Desktop installé et connecté une fois (cache bundle_cache)")
        return
    report.add(OK, "Toolkit (tk-core)", "%s, %s" % (toolkit.get("version"), toolkit.get("core")))

    reply = sidecar.call("check_online")
    if not reply["ok"]:
        code, message = error_of(reply)
        fix = {
            "auth_required": "se connecter dans ShotGrid Desktop (ArmorPaint ne demande jamais de mot de passe)",
            "sg_unreachable": "réseau du poste (VPN, proxy : HTTPS_PROXY), adresse du site dans ShotGrid Desktop",
        }.get(code, "voir le journal du sidecar")
        report.add(FAIL, "site ShotGrid", "%s: %s" % (code, message), fix)
        return
    report.add(OK, "site ShotGrid joignable", "%s, version %s" % (
        reply["data"].get("site"), ".".join(str(v) for v in reply["data"].get("version") or [])))

    reply = sidecar.call("auth")
    if not reply["ok"]:
        code, message = error_of(reply)
        report.add(FAIL, "session ShotGrid", "%s: %s" % (code, message),
                   "se connecter dans ShotGrid Desktop (ArmorPaint ne demande jamais de mot de passe)")
        return
    auth = reply["data"]
    report.add(OK, "session ShotGrid", "%s (%s, %s)" % (auth.get("login"), auth.get("user", {}).get("name")
                                                        if isinstance(auth.get("user"), dict) else auth.get("user"),
                                                        auth.get("method")))

    reply = sidecar.call("list_projects")
    if not reply["ok"]:
        report.add(FAIL, "projets", "%s: %s" % error_of(reply))
        return
    projects = reply["data"]
    report.add(OK, "projets actifs", "%d" % len(projects))

    project = None
    if opts.project:
        wanted = opts.project.strip().lower()
        project = next((p for p in projects if str(p.get("id")) == wanted or (p.get("name") or "").lower() == wanted),
                       None)
        if project is None:
            report.add(FAIL, "projet de la recette", "%s introuvable parmi les projets actifs" % opts.project,
                       "id ou nom exact d'un projet actif (ni archivé, ni modèle)")
            return
        report.add(OK, "projet de la recette", "%s (id %d)" % (project.get("name"), project["id"]))
        project_checks(report, sidecar, project)

    if opts.task:
        task_checks(report, sidecar, opts.task, project)


def project_checks(report, sidecar, project):
    reply = sidecar.call("list_tasks", {"project_id": project["id"], "mine": True})
    if not reply["ok"]:
        report.add(FAIL, "tâches de l'artiste", "%s: %s" % error_of(reply))
    elif not reply["data"]:
        report.add(WARN, "tâches de l'artiste", "aucune tâche assignée dans ce projet",
                   "assigner une tâche d'asset (étape de texture) au compte de la recette")
    else:
        tasks = reply["data"]
        report.add(OK, "tâches de l'artiste", "%d" % len(tasks))
        for t in tasks[:15]:
            entity = t.get("entity") or {}
            step = t.get("step.Step.code") or ""
            report.add(INFO, "tâche %d" % t["id"], "%s %s / %s / %s" % (
                entity.get("type", ""), entity.get("name", ""), step, t.get("content", "")))
    reply = sidecar.call("recipe_site", {"project_id": project["id"]})
    if not reply["ok"]:
        report.add(FAIL, "configurations de pipeline", "%s: %s" % error_of(reply))
        return
    for row in reply["data"]["rows"]:
        report.add(*row)


def task_checks(report, sidecar, task_id, project):
    print("\n== Tâche %d : moteur, apps, templates" % task_id, flush=True)
    args = {"task_id": task_id}
    reply = sidecar.call("recipe_config", args)
    if not reply["ok"]:
        code, message = error_of(reply)
        fix = {
            "app_unavailable": "ajouter le moteur tk-armorpaint aux environnements de la configuration (README, C3)",
            "toolkit_error": "l'amorçage a échoué : voir le journal du sidecar",
            "not_found": "id d'une tâche du site",
        }.get(code, "voir le journal du sidecar")
        report.add(FAIL, "moteur tk-armorpaint dans le contexte de la tâche", "%s: %s" % (code, message), fix)
        return
    for row in reply["data"]["rows"]:
        report.add(*row)

    reply = sidecar.call("list_commands", args)
    if not reply["ok"]:
        report.add(FAIL, "menu ShotGrid", "%s: %s" % error_of(reply))
    else:
        data = reply["data"]
        names = [c.get("name") for c in data.get("commands") or []]
        report.add(OK, "menu ShotGrid", "contexte %s ; %d commandes%s" % (
            (data.get("context") or {}).get("label", "?"), len(names), "" if data.get("has_ui") else ", sans Qt"))
        for name, app in (("File Open...", "tk-multi-workfiles2"), ("File Save...", "tk-multi-workfiles2"),
                          ("Publish...", "tk-multi-publish2")):
            if name in names:
                report.add(OK, "commande %s" % name)
            else:
                report.add(FAIL if data.get("has_ui") else WARN, "commande %s" % name, "absente du menu",
                           "%s dans engines.tk-armorpaint.apps de cet environnement, et PySide6 dans le Python du "
                           "sidecar" % app)

    reply = sidecar.call("list_workfiles", args)
    if not reply["ok"]:
        code, message = error_of(reply)
        report.add(FAIL, "fichiers de travail", "%s: %s" % (code, message),
                   "templates armorpaint_*_work dans core/templates.yml (README, C2)" if code == "template_missing"
                   else "")
    else:
        data = reply["data"]
        report.add(OK, "fichiers de travail", "template %s ; %d fichier(s), %d publish(es) ArmorPaint" % (
            data.get("template"), len(data.get("files") or []), len(data.get("publishes") or [])))

    reply = sidecar.call("next_version", args)
    if not reply["ok"]:
        report.add(FAIL, "prochaine version", "%s: %s" % error_of(reply))
        return
    path = reply["data"]["path"]
    report.add(OK, "prochaine version", path)
    pattern_args = {"path": path}
    if project is not None:
        pattern_args["project_id"] = project["id"]
    reply = sidecar.call("texture_pattern", pattern_args)
    if not reply["ok"]:
        report.add(FAIL, "nom des textures exportées", "%s: %s" % error_of(reply),
                   "templates armorpaint_*_textures (README, B3)")
    elif reply["data"].get("template") is None:
        report.add(WARN, "nom des textures exportées", reply["data"].get("reason", ""),
                   "sans template, les textures gardent les noms d'ArmorPaint (README, B3)")
    else:
        data = reply["data"]
        report.add(OK, "nom des textures exportées", "%s : %s%s" % (
            data["template"], data.get("pattern"), " ; UDIM %s" % data["pattern_udim"] if data.get("pattern_udim") else ""))


if __name__ == "__main__":
    sys.exit(main())
