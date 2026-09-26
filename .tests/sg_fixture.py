# Site ShotGrid simulé pour le banc du lot C2 : mockgun (fourni par tk-core) avec le schéma d'un vrai site (celui des
# tests de tk-core), une vraie configuration Toolkit sur disque (sg_fixture/config) et une session ShotGrid Desktop
# enregistrée comme le fait Desktop. Le sidecar tourne sur ce site sans modification : seules les connexions réseau
# sont détournées vers mockgun.
#
# Le détournement survit au core swap. L'amorçage d'une configuration distribuée copie tk-core dans la configuration
# et bascule dessus (comme en production) : des patchs posés sur les modules importés au démarrage disparaîtraient.
# Le banc travaille donc sur une copie de tk-core dont les deux usines de connexion (user_impl et connection)
# appellent install() ci-dessous au chargement ; la copie faite par l'amorçage en hérite.
#
#   python3 sg_fixture.py <dossier de travail>     lance le sidecar sur le site simulé (tests de bout en bout)
#   STUDIO_SG_BRIDGE=.../sg_fixture.py             idem dans l'app, dossier de travail dans STUDIO_TEST_WORKDIR
#   import sg_fixture; sg_fixture.setup(dossier)   le prépare dans le processus (tests unitaires)
#
# STUDIO_TEST_TK_CORE : dossier racine de tk-core. STUDIO_TEST_SCENARIO : variante du site (voir setup()).

import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(os.path.dirname(HERE), "assets")
HOST = "https://studio.test.shotgunstudio.com"
LOGIN = "artist"


TARGET = None  # connexion rendue par les usines patchées : la base mockgun, ou un faux serveur selon le scénario

_PATCHED = {
    "authentication/user_impl.py": "_shotgun_instance_factory",
    "util/shotgun/connection.py": "create_sg_connection",
}


def install(namespace):
    """Appelé à la fin des modules patchés de tk-core (à chaque import, core swap compris)."""
    for relative, name in _PATCHED.items():
        if namespace["__name__"].endswith(relative[:-3].replace("/", ".")):
            namespace[name] = lambda *args, **kwargs: TARGET


def _tk_core(workdir):
    """Copie de tk-core (sans tests ni docs) aux usines de connexion patchées. (racine source, racine copiée)."""
    source = os.environ.get("STUDIO_TEST_TK_CORE")
    if not source or not os.path.isdir(os.path.join(source, "python", "sgtk")):
        sys.exit("STUDIO_TEST_TK_CORE must point to a tk-core checkout")
    root = os.path.join(workdir, "tk-core")
    if os.path.exists(root):
        shutil.rmtree(root)
    shutil.copytree(source, root, ignore=shutil.ignore_patterns(".git", "tests", "docs", "developer"))
    for relative in _PATCHED:
        with open(os.path.join(root, "python", "tank", relative), "a") as f:
            f.write('\n\nimport sys as _s\n_s.modules["sg_fixture"].install(globals())\n')
    return source, root


def setup(workdir, scenario=None):
    """Prépare le site dans ce processus. Retourne un dict des entités créées (et "db", la base mockgun).

    scenario : None ; "no_session" (Desktop jamais connecté) ; "auth_fault" (le serveur rejette la session) ;
    "offline" (le serveur est injoignable) ; "no_template" (la config n'a pas les templates ArmorPaint)."""
    global TARGET
    scenario = scenario or os.environ.get("STUDIO_TEST_SCENARIO") or None
    workdir = os.path.abspath(workdir)
    sys.modules.setdefault("sg_fixture", sys.modules[__name__])
    tk_source, tk_core = _tk_core(workdir)
    os.environ["SHOTGUN_HOME"] = os.path.join(workdir, "shotgun_home")
    os.environ["STUDIO_SGTK_PATH"] = os.path.join(tk_core, "python")
    sys.path.insert(0, os.path.join(tk_core, "python"))
    from tank.authentication import session_cache
    from tank_vendor.shotgun_api3 import AuthenticationFault, ProtocolError
    from tank_vendor.shotgun_api3.lib import mockgun

    # Configuration de pipeline : copie de sg_fixture/config, racine de stockage dans le dossier de travail.
    os.environ["STUDIO_TEST_TK_CORE"] = tk_core  # core_api.yml de la config : la copie patchée
    config = os.path.join(workdir, "config")
    if os.path.exists(config):
        shutil.rmtree(config)
    shutil.copytree(os.path.join(HERE, "sg_fixture", "config"), config)
    storage = os.path.join(workdir, "storage")
    os.makedirs(storage, exist_ok=True)
    with open(os.path.join(config, "core", "roots.yml"), "w") as f:
        f.write("primary:\n    mac_path: %s\n    linux_path: %s\n    windows_path: null\n" % (storage, storage))
    if scenario == "no_template":
        with open(os.path.join(config, "core", "templates.yml"), "w") as f:
            f.write("keys:\n    name: {type: str}\npaths:\n    maya_asset_work: 'assets/{name}.ma'\nstrings: {}\n")

    fixtures = os.path.join(tk_source, "tests", "fixtures", "mockgun")
    mockgun.Shotgun.set_schema_paths(
        os.path.join(fixtures, "schema.pickle"), os.path.join(fixtures, "schema_entity.pickle")
    )
    db = mockgun.Shotgun(HOST, "mock", "mock")
    db.server_info = {"version": (8, 50, 0)}

    e = {"db": db, "storage": storage, "config": config}
    e["artist"] = db.create("HumanUser", {"login": LOGIN, "name": "Ada Artist", "email": "ada@studio.test"})
    e["other_user"] = db.create("HumanUser", {"login": "bob", "name": "Bob"})
    e["project"] = db.create("Project", {"name": "Demo", "tank_name": "demo", "archived": False, "is_template": False})
    e["project2"] = db.create("Project", {"name": "Other", "tank_name": "other", "archived": False, "is_template": False})
    e["archived"] = db.create("Project", {"name": "Old", "tank_name": "old", "archived": True, "is_template": False})
    e["storage_entity"] = db.create(
        "LocalStorage", {"code": "primary", "mac_path": storage, "linux_path": storage, "windows_path": None}
    )
    for project in (e["project"], e["project2"]):
        db.create(
            "PipelineConfiguration",
            {
                "code": "Primary",
                "project": project,
                "plugin_ids": "basic.*",
                "descriptor": "sgtk:descriptor:path?path=%s" % config,
            },
        )
    e["step"] = db.create("Step", {"code": "Texture", "short_name": "TXT", "entity_type": "Asset"})
    e["asset"] = db.create("Asset", {"code": "robot", "sg_asset_type": "Prop", "project": e["project"]})
    e["asset2"] = db.create("Asset", {"code": "tree", "sg_asset_type": "Env", "project": e["project"]})
    e["asset_other"] = db.create("Asset", {"code": "car", "sg_asset_type": "Prop", "project": e["project2"]})
    e["task"] = db.create(
        "Task",
        {
            "content": "texturing",
            "entity": e["asset"],
            "project": e["project"],
            "step": e["step"],
            "task_assignees": [e["artist"]],
            "sg_status_list": "ip",
        },
    )
    e["task_bob"] = db.create(
        "Task",
        {"content": "lookdev", "entity": e["asset2"], "project": e["project"], "step": e["step"],
         "task_assignees": [e["other_user"]]},
    )
    e["task_other"] = db.create(
        "Task",
        {"content": "texturing", "entity": e["asset_other"], "project": e["project2"], "step": e["step"],
         "task_assignees": [e["artist"]]},
    )
    # Une tâche sur un Shot, dans le même projet : le lien "entity" des tâches est polymorphe.
    e["sequence"] = db.create("Sequence", {"code": "sq010", "project": e["project"]})
    e["shot"] = db.create("Shot", {"code": "sq010_0010", "sg_sequence": e["sequence"], "project": e["project"]})
    e["task_shot"] = db.create(
        "Task",
        {"content": "fx", "entity": e["shot"], "project": e["project"], "step": e["step"],
         "task_assignees": [e["other_user"]]},
    )
    e["publish_type"] = db.create("PublishedFileType", {"code": "ArmorPaint Project"})

    # Le réseau : toute connexion ShotGrid aboutit à mockgun (ou échoue selon le scénario).
    class Offline(object):
        def __getattr__(self, name):
            def fail(*args, **kwargs):
                raise ProtocolError(HOST, 503, "Service Unavailable", {})

            return fail

    class Rejecting(object):
        def __getattr__(self, name):
            def fail(*args, **kwargs):
                raise AuthenticationFault("Unable to connect: session expired")

            return fail

    TARGET = {"offline": Offline(), "auth_fault": Rejecting()}.get(scenario, db)

    # La session de ShotGrid Desktop, là où Desktop l'écrit (sous SHOTGUN_HOME).
    if scenario != "no_session":
        session_cache.set_current_host(HOST)
        session_cache.set_current_user(HOST, LOGIN)
        session_cache.cache_session_data(HOST, LOGIN, "session-token-from-desktop")
    return e


def main():
    workdir = sys.argv[1] if len(sys.argv) == 2 else os.environ.get("STUDIO_TEST_WORKDIR")
    if not workdir:
        sys.exit("usage: sg_fixture.py <workdir> (or STUDIO_TEST_WORKDIR)")
    entities = setup(workdir)
    # Les identifiants créés, pour le banc C (qui ne parse pas le JSON) : sur stderr, pas sur le protocole.
    ids = {k: v["id"] for k, v in entities.items() if isinstance(v, dict) and "id" in v}
    sys.stderr.write("sg_fixture: %s\n" % json.dumps(ids, sort_keys=True))
    sys.path.insert(0, ASSETS)
    import studio_sg_bridge

    return studio_sg_bridge.main()


if __name__ == "__main__":
    sys.exit(main())
