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
#
# Réseau coupé (lot C5, mode dégradé) : STUDIO_TEST_OFFLINE_FLAG=<fichier>, le site est injoignable tant que ce
# fichier existe ; STUDIO_TEST_OFFLINE_AT=<méthode>:<type>:<n> (create:PublishedFile:2), le n-ième de ces appels
# échoue et crée le fichier ; STUDIO_TEST_LOST_AT, pareil, mais l'appel a eu lieu avant d'échouer (réponse perdue).

import errno
import json
import os
import re
import shutil
import subprocess
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


def _bundle_version(path):
    """Version d'une app ou d'un framework Toolkit local : le tableau de .shotgrid/README.md pour les copies du dépôt,
    sinon le tag du clone git (pas celui d'un dépôt qui le contiendrait). Les frameworks sont appariés par version
    minimale : une version fausse peut les faire refuser."""
    readme = os.path.join(os.path.dirname(path), "README.md")
    if os.path.exists(readme):
        with open(readme, encoding="utf-8") as f:
            found = re.search(r"^\|\s*`%s`\s*\|\s*(v[\d.]+)\s*\|" % re.escape(os.path.basename(path)), f.read(), re.M)
        if found:
            return found.group(1)
    git = lambda *args: subprocess.run(["git", "-C", path] + list(args), capture_output=True, text=True).stdout.strip()
    top = git("rev-parse", "--show-toplevel")
    if top and os.path.realpath(top) == os.path.realpath(path):
        return git("describe", "--tags", "--abbrev=0") or "v0.0.0"
    return "v0.0.0"


def _demo_environments(config):
    """Scénario "demo" : le moteur tk-armorpaint (external/tk-armorpaint), tk-multi-workfiles2 et tk-multi-publish2
    dans tous les environnements, comme dans une config studio. Les apps viennent de copies locales
    (STUDIO_TEST_TK_APPS : dossier contenant tk-multi-workfiles2, tk-multi-publish2, tk-framework-shotgunutils et
    tk-framework-qtwidgets, .shotgrid/ du dépôt), par descripteurs "path". Publish : les hooks du moteur ; dans les
    environnements de tâche, projet et textures copiés dans le dossier publish, sinon publiés en place ; les textures
    toujours en place avec STUDIO_TEST_TEXTURES_COPY=0 (plugin sans Publish Template)."""
    apps = os.environ.get("STUDIO_TEST_TK_APPS")
    for app in ("tk-multi-workfiles2", "tk-multi-publish2", "tk-framework-shotgunutils", "tk-framework-qtwidgets"):
        if not apps or not os.path.isdir(os.path.join(apps, app)):
            sys.exit("STUDIO_TEST_TK_APPS must contain %s (scenario demo)" % app)
    engine = os.path.join(os.path.dirname(HERE), "tk-armorpaint")
    copy_textures = os.environ.get("STUDIO_TEST_TEXTURES_COPY") != "0"

    def location(name, version=None):
        path = engine if name == "tk-armorpaint" else os.path.join(apps, name)
        return {"type": "path", "path": path, "version": version or _bundle_version(path)}

    hooks = "{self}/%s.py:{engine}/tk-multi-publish2/basic/%s.py"
    for env, kind in (("project", None), ("asset", None), ("shot", None), ("asset_step", "asset"), ("shot_step", "shot")):
        workfiles = {
            "location": location("tk-multi-workfiles2"),
            "hook_scene_operation": "{engine}/tk-multi-workfiles2/scene_operation.py",
            "saveas_default_name": "scene",
            "file_extensions": [],
        }
        for setting, template in (("template_work", "work"), ("template_work_area", "work_area"),
                                  ("template_publish", "publish"), ("template_publish_area", "publish_area")):
            workfiles[setting] = "armorpaint_%s_%s" % (kind, template) if kind else None
        template = lambda name: "armorpaint_%s_%s" % (kind, name) if kind else None
        publish = {
            "location": location("tk-multi-publish2"),
            "collector": hooks % ("collector", "collector"),
            "collector_settings": {"Work Template": template("work")},
            "publish_plugins": [
                {"name": "Publish to Flow Production Tracking", "hook": "{self}/publish_file.py", "settings": {}},
                {
                    "name": "Publish ArmorPaint Project",
                    "hook": hooks % ("publish_file", "publish_session"),
                    "settings": {"Publish Template": template("publish")},
                },
                {
                    "name": "Upload for review",
                    "hook": hooks % ("upload_version", "upload_version"),
                    "settings": {},
                },
                {
                    "name": "Publish Textures",
                    "hook": hooks % ("publish_file", "publish_textures"),
                    "settings": {"Publish Template": template("textures_publish")} if copy_textures else {},
                },
            ],
            "modal": True,
        }
        data = {
            "engines": {
                "tk-armorpaint": {
                    "location": location("tk-armorpaint", "v0.1.0"),
                    "apps": {"tk-multi-workfiles2": workfiles, "tk-multi-publish2": publish},
                    "menu_favourites": [
                        {"app_instance": "tk-multi-workfiles2", "name": "File Open..."},
                        {"app_instance": "tk-multi-workfiles2", "name": "File Save..."},
                        {"app_instance": "tk-multi-publish2", "name": "Publish..."},
                    ],
                }
            },
            "frameworks": {
                "tk-framework-shotgunutils_v5.x.x": {"location": location("tk-framework-shotgunutils")},
                "tk-framework-qtwidgets_v2.x.x": {"location": location("tk-framework-qtwidgets")},
            },
        }
        with open(os.path.join(config, "env", env + ".yml"), "w") as f:
            json.dump(data, f, indent=2)  # le JSON est du YAML


def _demo_production(db, e):
    """Scénario "demo" (démo Workfiles, demo_workfiles.sh) : une petite production autour du site de base, avec
    assez de tâches pour l'artiste sur des Assets et des Shots. Les identifiants du site de base ne bougent pas."""
    project = e["project"]
    step_shot = db.create("Step", {"code": "Lookdev", "short_name": "LDV", "entity_type": "Shot"})

    def task(content, entity, step, status="ip"):
        return db.create(
            "Task",
            {"content": content, "entity": entity, "project": project, "step": step,
             "task_assignees": [e["artist"]], "sg_status_list": status},
        )

    for code, asset_type in (("drone", "Prop"), ("crate", "Prop"), ("rock", "Env")):
        asset = db.create("Asset", {"code": code, "sg_asset_type": asset_type, "project": project})
        e["demo_task_%s" % code] = task("texturing", asset, e["step"], "wtg" if code == "rock" else "ip")
    sq020 = db.create("Sequence", {"code": "sq020", "project": project})
    for code, sequence in (("sq010_0020", e["sequence"]), ("sq020_0010", sq020)):
        shot = db.create("Shot", {"code": code, "sg_sequence": sequence, "project": project})
        e["demo_task_%s" % code] = task("lookdev", shot, step_shot)


def _edit_templates(config, old, new):
    path = os.path.join(config, "core", "templates.yml")
    with open(path) as f:
        text = f.read()
    if old not in text:
        sys.exit("sg_fixture: %r not in %s" % (old, path))
    with open(path, "w") as f:
        f.write(text.replace(old, new))


def _observe(db):
    """Les appels à mockgun disent au sidecar si le site a répondu (studio_sg_bridge.network) : un vrai site passe par
    Shotgun._call_rpc, que le sidecar surveille, pas mockgun. Et le réseau coupé sur commande (voir l'en-tête)."""
    flag = os.environ.get("STUDIO_TEST_OFFLINE_FLAG")
    triggers = {}
    for name in ("STUDIO_TEST_OFFLINE_AT", "STUDIO_TEST_LOST_AT"):
        spec = os.environ.get(name)
        if spec:
            method, entity_type, count = spec.split(":")
            triggers[(method, entity_type)] = [int(count), name == "STUDIO_TEST_LOST_AT"]
    if triggers and not flag:
        sys.exit("STUDIO_TEST_OFFLINE_AT and STUDIO_TEST_LOST_AT need STUDIO_TEST_OFFLINE_FLAG")
    db.info = lambda: {"version": list(db.server_info["version"]), "s3_uploads_enabled": False}

    def network(online, reason=None):
        bridge = sys.modules.get("studio_sg_bridge")
        if bridge is not None and hasattr(bridge, "network"):
            bridge.network(online, reason)

    def unreachable():
        error = ConnectionRefusedError(errno.ECONNREFUSED, "Connection refused (simulated, %s)" % HOST)
        network(False, str(error))
        return error

    def observed(method, fn):
        def call(*args, **kwargs):
            if flag and os.path.exists(flag):
                raise unreachable()
            trigger = triggers.get((method, args[0] if args and isinstance(args[0], str) else None))
            if trigger is not None:
                trigger[0] -= 1
                if trigger[0] == 0:
                    if trigger[1]:
                        fn(*args, **kwargs)  # fait sur le site, mais la réponse se perd
                    with open(flag, "w"):
                        pass
                    raise unreachable()
            result = fn(*args, **kwargs)
            network(True)
            return result

        return call

    for method in ("info", "find", "find_one", "create", "update", "delete", "revive", "batch", "upload",
                   "upload_thumbnail", "schema_read", "schema_entity_read", "schema_field_read"):
        setattr(db, method, observed(method, getattr(db, method)))


def setup(workdir, scenario=None):
    """Prépare le site dans ce processus. Retourne un dict des entités créées (et "db", la base mockgun).

    scenario : None ; "no_session" (Desktop jamais connecté) ; "auth_fault" (le serveur rejette la session) ;
    "offline" (le serveur est injoignable) ; "no_template" (la config n'a pas les templates ArmorPaint) ; "demo"
    (plus de tâches pour l'artiste, voir _demo_production). Variantes des templates : STUDIO_TEST_UDIM_DEFAULT=0 (clé
    UDIM sans default: "<UDIM>"), STUDIO_TEST_CHANNEL_FILTER=<filtre> (filter_by de la clé du canal)."""
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
    if scenario == "demo":
        _demo_environments(config)
    if os.environ.get("STUDIO_TEST_UDIM_DEFAULT") == "0":  # clé UDIM sans default: "<UDIM>" (une séquence : %04d)
        _edit_templates(config, '        default: "<UDIM>"\n', "")
    channel_filter = os.environ.get("STUDIO_TEST_CHANNEL_FILTER")
    if channel_filter:  # canal filtré : alphanumeric refuse base_color
        channel = "    channel:\n        type: str\n"
        _edit_templates(config, channel, channel + "        filter_by: %s\n" % channel_filter)
    if scenario == "no_template":
        with open(os.path.join(config, "core", "templates.yml"), "w") as f:
            f.write("keys:\n    name: {type: str}\npaths:\n    maya_asset_work: 'assets/{name}.ma'\nstrings: {}\n")

    fixtures = os.path.join(tk_source, "tests", "fixtures", "mockgun")
    mockgun.Shotgun.set_schema_paths(
        os.path.join(fixtures, "schema.pickle"), os.path.join(fixtures, "schema_entity.pickle")
    )
    db = mockgun.Shotgun(HOST, "mock", "mock")
    db.server_info = {"version": (8, 50, 0)}
    # Le schéma par projet (project_entity, que passent les frameworks Toolkit) n'existe pas dans mockgun : un seul
    # schéma pour tout le site.
    schema_read, entity_read, field_read = db.schema_read, db.schema_entity_read, db.schema_field_read
    db.schema_read = lambda project_entity=None: schema_read()
    db.schema_entity_read = lambda project_entity=None: entity_read()
    db.schema_field_read = lambda entity_type, field_name=None, project_entity=None: field_read(entity_type, field_name)
    # Un lien vers un fichier local (le chemin d'un publish) porte link_type "local" sur un vrai site, que lit
    # sgtk.util.resolve_publish_path (conflits de publish, File Open) ; mockgun ne l'ajoute pas. batch passe par create.
    create = db.create

    def create_with_link_type(entity_type, data, return_fields=None):
        for value in data.values():
            if isinstance(value, dict) and "local_path" in value:
                value.setdefault("link_type", "local")
        return create(entity_type, data, return_fields)

    db.create = create_with_link_type

    # upload (média d'une Version) : mockgun ne l'implémente pas. Comme un vrai site : le fichier doit exister, le champ
    # reçoit la pièce jointe, la réponse est l'id de l'Attachment. Le fichier n'est pas gardé (vignettes de même).
    uploads = []

    def upload(entity_type, entity_id, path, field_name=None, display_name=None, tag_list=None):
        if not os.path.isfile(path):
            raise ValueError("Path must be a valid file, got '%s'" % path)
        name = display_name or os.path.basename(path)
        if field_name:
            db.update(entity_type, entity_id, {field_name: {"name": name, "link_type": "upload"}})
        uploads.append(name)
        return len(uploads)

    db.upload = upload

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
    for code in ("Texture", "UDIM Image"):  # textures (tk-armorpaint/hooks/tk-multi-publish2/basic/publish_textures.py)
        e["publish_type_" + code.split()[0].lower()] = db.create("PublishedFileType", {"code": code})
    if scenario == "demo":
        _demo_production(db, e)
    _observe(db)

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
