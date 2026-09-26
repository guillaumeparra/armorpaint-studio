# Opérations du sidecar ShotGrid (lot C2), sur Toolkit (tk-core). Protocole et cycle de vie : studio_sg_bridge.py.
#
# Authentification : celle de ShotGrid Desktop (la session que l'artiste y a ouverte), ou un script ShotGrid pour les
# nœuds de ferme (STUDIO_SG_SCRIPT_NAME/KEY). Le sidecar ne demande jamais de mot de passe : une session absente ou
# expirée donne l'erreur auth_required, et l'artiste se reconnecte dans ShotGrid Desktop.
#
# Toolkit : les opérations qui ont besoin des templates (fichiers de travail, publish) amorcent la configuration du
# projet par ToolkitManager, comme une intégration DCC. L'amorçage peut remplacer tk-core par celui du projet
# (« core swap ») : un processus ne sert donc qu'un projet Toolkit. Une requête sur un autre projet répond
# restart_required, et le superviseur relance le sidecar. Les requêtes au niveau du site (projets, tâches…) n'amorcent
# rien.
#
# Réglages : variables d'environnement STUDIO_SG_*, et un hook studio optionnel (STUDIO_SG_HOOK, chemin d'un .py qui
# définit setup(session)) pour changer les réglages, remplacer ou ajouter des opérations. Voir README.md.

import glob
import importlib
import importlib.util
import logging
import os
import re
import sys

from studio_sg_bridge import BridgeError

log = logging.getLogger("studio_sg")

VERSION_IN_NAME = re.compile(r"[._-]v\d+(?=\.|$)", re.IGNORECASE)


def _env_map(name, default):
    # "Asset=armorpaint_asset_work,Shot=armorpaint_shot_work"
    value = os.environ.get(name)
    if not value:
        return dict(default)
    result = {}
    for item in value.split(","):
        key, _, template = item.partition("=")
        if key.strip() and template.strip():
            result[key.strip()] = template.strip()
    return result


def default_settings():
    return {
        # Moteur démarré après l'amorçage : les hooks de la config studio en attendent souvent un. "none" : aucun.
        # tk-armorpaint (external/tk-armorpaint) : les fenêtres des apps Toolkit (op show_app). Absent de la
        # configuration du projet, l'amorçage continue sans moteur : seules les fenêtres manquent.
        "engine": os.environ.get("STUDIO_SG_ENGINE", "tk-armorpaint"),
        "plugin_id": os.environ.get("STUDIO_SG_PLUGIN_ID", "basic.armorpaint"),
        # Configuration de repli si le projet n'a pas de PipelineConfiguration (descripteur Toolkit), None : erreur.
        "base_configuration": os.environ.get("STUDIO_SG_BASE_CONFIGURATION") or None,
        # Templates des fichiers de travail par type d'entité de la tâche (templates.yml de la config).
        "work_templates": _env_map(
            "STUDIO_SG_WORK_TEMPLATES", {"Asset": "armorpaint_asset_work", "Shot": "armorpaint_shot_work"}
        ),
        "publish_type": os.environ.get("STUDIO_SG_PUBLISH_TYPE", "ArmorPaint Project"),
        "default_name": os.environ.get("STUDIO_SG_DEFAULT_NAME", "scene"),
        "entity_types": ["Asset", "Shot"],
    }


def publish_name(path):
    """Nom de publish à la convention Toolkit : le nom de fichier sans son numéro de version (scene.v003.arm ->
    scene.arm), qui regroupe les versions d'un même élément dans ShotGrid."""
    base = os.path.basename(path)
    matches = list(VERSION_IN_NAME.finditer(base))
    if not matches:
        return base
    last = matches[-1]
    return base[: last.start()] + base[last.end():]


def version_in_name(path):
    matches = list(VERSION_IN_NAME.finditer(os.path.basename(path)))
    return int(re.sub(r"\D", "", matches[-1].group(0))) if matches else None


def _shotgun_home():
    if os.environ.get("SHOTGUN_HOME"):
        return os.path.expanduser(os.environ["SHOTGUN_HOME"])
    if sys.platform == "darwin":
        return os.path.expanduser("~/Library/Caches/Shotgun")
    if sys.platform == "win32":
        return os.path.join(os.environ.get("APPDATA", ""), "Shotgun")
    return os.path.expanduser("~/.shotgun")


def _version_key(path):
    # .../tk-core/v0.21.7/python -> (0, 21, 7)
    name = os.path.basename(os.path.dirname(path))
    return tuple(int(n) for n in re.findall(r"\d+", name))


def core_candidates():
    """Où chercher tk-core, dans l'ordre : STUDIO_SGTK_PATH (dossier contenant le paquet sgtk), puis le cache de
    bundles de ShotGrid Desktop, version la plus récente d'abord."""
    candidates = []
    if os.environ.get("STUDIO_SGTK_PATH"):
        candidates.append(os.environ["STUDIO_SGTK_PATH"])
    caches = [os.environ.get("SHOTGUN_BUNDLE_CACHE_PATH"), os.path.join(_shotgun_home(), "bundle_cache")]
    found = []
    for cache in caches:
        if cache:
            found += glob.glob(os.path.join(cache, "app_store", "tk-core", "v*", "python"))
    candidates += sorted(found, key=_version_key, reverse=True)
    return candidates


def import_sgtk():
    """Importe sgtk, depuis le sys.path ou les emplacements de core_candidates(). (module, chemin, erreur)."""
    tried = []
    for path in [None] + core_candidates():
        if path is not None:
            if not os.path.isdir(os.path.join(path, "sgtk")):
                tried.append(path)
                continue
            sys.path.insert(0, path)
        try:
            module = importlib.import_module("sgtk")
            return module, os.path.dirname(os.path.dirname(module.__file__)), None
        except ImportError as e:
            if path is not None:
                sys.path.remove(path)
                tried.append("%s (%s)" % (path, e))
    return None, None, "tk-core not found (set STUDIO_SGTK_PATH or install ShotGrid Desktop); tried: %s" % (
        ", ".join(tried) or "sys.path only"
    )


class _SkipEngine(Exception):
    pass


def _entity(value, name="entity"):
    if (
        not isinstance(value, dict)
        or not isinstance(value.get("type"), str)
        or not isinstance(value.get("id"), int)
        or isinstance(value.get("id"), bool)
    ):
        raise BridgeError("bad_request", '%s must be {"type":<string>,"id":<int>}' % name)
    return {"type": value["type"], "id": value["id"]}


def _int(args, key, required=True):
    value = args.get(key)
    if value is None and not required:
        return None
    if not isinstance(value, int) or isinstance(value, bool):
        raise BridgeError("bad_request", "%s must be an integer" % key)
    return value


def _str(args, key, required=True):
    value = args.get(key)
    if value is None and not required:
        return None
    if not isinstance(value, str) or not value:
        raise BridgeError("bad_request", "%s must be a non-empty string" % key)
    return value


class Session(object):
    def __init__(self):
        self.settings = default_settings()
        self.ops = {}
        for name in dir(self):
            if name.startswith("op_"):
                self.ops[name[3:]] = getattr(self, name)
        self.sgtk, self.core_path, self.core_error = import_sgtk()
        self._core_at_start = self.sgtk
        self.user = None
        self.sg = None
        self.human = None
        self.tk = None
        self.tk_project_id = None
        self.engine = None
        if self.sgtk is not None:
            self._setup_toolkit_logging()
            self._forbid_prompts()
        self._load_hook()

    # ---- Cycle de vie -----------------------------------------------------------------------------------------------

    def _load_hook(self):
        path = os.environ.get("STUDIO_SG_HOOK")
        if not path:
            return
        spec = importlib.util.spec_from_file_location("studio_sg_hook", path)
        if spec is None:
            raise RuntimeError("STUDIO_SG_HOOK: cannot load %s" % path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        if hasattr(module, "setup"):
            module.setup(self)
        log.info("hook loaded: %s", path)

    def hello(self):
        return {
            "python": sys.version.split()[0],
            "toolkit": {
                "available": self.sgtk is not None,
                "core": self.core_path,
                "version": self._core_version(),
                "error": self.core_error,
            },
            "ops": sorted(self.ops),
        }

    def close(self):
        if self.engine is not None:
            try:
                self.engine.destroy()
            except Exception:  # noqa: BLE001 — on s'arrête de toute façon
                log.debug("engine destroy failed", exc_info=True)

    def call(self, op, args):
        fn = self.ops.get(op)
        if fn is None:
            raise BridgeError("unknown_op", "unknown operation '%s'" % op)
        return fn(args)

    def classify(self, e):
        """Traduit une exception en BridgeError. Par nom de classe : après un core swap, les classes de tk-core et
        de shotgun_api3 ne sont plus celles importées au démarrage."""
        names = {cls.__name__ for cls in type(e).__mro__}
        message = str(e) or type(e).__name__
        if names & {"AuthenticationFault", "MissingTwoFactorAuthenticationFault", "AuthenticationError",
                    "AuthenticationCancelled", "IncompleteCredentials"}:
            return BridgeError("auth_required", "ShotGrid authentication required: %s" % message)
        if names & {"ProtocolError", "ConnectionError", "TimeoutError", "SSLError", "URLError", "gaierror",
                    "timeout", "RemoteDisconnected"}:
            return BridgeError("sg_unreachable", "ShotGrid unreachable: %s" % message)
        if "Fault" in names:
            return BridgeError("sg_error", "ShotGrid error: %s" % message)
        if names & {"TankBootstrapError", "TankError"}:
            return BridgeError("toolkit_error", "Toolkit: %s" % message)
        return BridgeError("internal", "%s: %s" % (type(e).__name__, message))

    # ---- Toolkit, authentification ----------------------------------------------------------------------------------

    def _core_version(self):
        if self.sgtk is None:
            return None
        try:
            return self.sgtk.pipelineconfig_utils.get_currently_running_api_version()
        except Exception:  # noqa: BLE001
            return None

    def _setup_toolkit_logging(self):
        manager = self.sgtk.LogManager()
        manager.initialize_base_file_handler("tk-armorpaint")
        if os.environ.get("STUDIO_SG_DEBUG"):
            manager.global_debug = True
        handler = logging.StreamHandler(sys.stderr)
        handler.setLevel(logging.DEBUG if os.environ.get("STUDIO_SG_DEBUG") else logging.WARNING)
        handler.setFormatter(logging.Formatter("%(levelname)s %(name)s: %(message)s"))
        manager.initialize_custom_handler(handler)

    def _forbid_prompts(self):
        # Une session expirée ferait demander le mot de passe par tk-core, sur le terminal (qui n'est pas le nôtre :
        # le sidecar est dans un groupe de processus d'arrière-plan, il serait suspendu) ou dans une fenêtre Qt (il
        # n'y en a pas). Les deux points d'entrée sont des fonctions de module : on les remplace. À refaire après
        # chaque core swap, sur le nouveau module.
        auth = importlib.import_module("tank.authentication.interactive_authentication")

        def refuse(*args, **kwargs):
            raise BridgeError(
                "auth_required",
                "ShotGrid session expired or missing: log in again in ShotGrid Desktop, then retry",
            )

        auth.renew_session = refuse
        auth.authenticate = refuse

    def _require_toolkit(self):
        if self.sgtk is None:
            raise BridgeError("toolkit_unavailable", self.core_error)

    def _connect(self):
        if self.sg is not None:
            return self.sg
        self._require_toolkit()
        authentication = self.sgtk.authentication
        authenticator = authentication.ShotgunAuthenticator()
        script = os.environ.get("STUDIO_SG_SCRIPT_NAME")
        if script:
            host = os.environ.get("STUDIO_SG_HOST") or os.environ.get("SHOTGUN_SITE")
            if not host or not os.environ.get("STUDIO_SG_SCRIPT_KEY"):
                raise BridgeError("auth_required", "STUDIO_SG_SCRIPT_NAME needs STUDIO_SG_SCRIPT_KEY and STUDIO_SG_HOST")
            user = authenticator.create_script_user(script, os.environ["STUDIO_SG_SCRIPT_KEY"], host)
        else:
            # Pas de user.are_credentials_expired() : elle répond « expirée » quand le serveur refuse la connexion,
            # et l'artiste irait se reconnecter pour rien. La session est vérifiée par la première vraie requête, qui
            # distingue AuthenticationFault (auth_required) d'une erreur réseau (sg_unreachable).
            user = authenticator.get_default_user()
            if user is None:
                raise BridgeError("auth_required", "no ShotGrid session: log in to ShotGrid Desktop, then retry")
        self.sgtk.set_authenticated_user(user)
        self.user = user
        self.sg = user.create_sg_connection()
        return self.sg

    def _human_user(self):
        if self.human is None:
            sg = self._connect()
            login = getattr(self.user, "login", None)
            if login:
                self.human = sg.find_one("HumanUser", [["login", "is", login]], ["name", "login", "email"])
        return self.human

    def tk_for_project(self, project_id):
        """Instance Toolkit du projet, amorcée au premier appel. Un seul projet par processus (voir l'en-tête)."""
        if self.tk is not None:
            if self.tk_project_id == project_id:
                return self.tk
            raise BridgeError(
                "restart_required", "Toolkit is bootstrapped for project %d" % self.tk_project_id, restart=True
            )
        if importlib.import_module("sgtk") is not self._core_at_start:
            # Un amorçage précédent a échoué après avoir changé de core : l'état du processus n'est plus sûr.
            raise BridgeError("restart_required", "a previous bootstrap left another tk-core loaded", restart=True)
        self._connect()
        manager = self.sgtk.bootstrap.ToolkitManager(self.user)
        manager.plugin_id = self.settings["plugin_id"]
        if self.settings["base_configuration"]:
            manager.base_configuration = self.settings["base_configuration"]
        manager.progress_callback = lambda value, message: log.info("bootstrap %3d%% %s", int(value * 100), message)
        engine_name = self.settings["engine"]
        captured = {}

        def before_engine(ctx):
            captured["tk"] = ctx.sgtk
            if engine_name == "none":
                raise _SkipEngine()

        manager.pre_engine_start_callback = before_engine
        entity = {"type": "Project", "id": project_id}
        try:
            self.engine = manager.bootstrap_engine(engine_name if engine_name != "none" else "tk-shell", entity)
            tk = self.engine.sgtk
        except _SkipEngine:
            tk = captured["tk"]
        except Exception as e:  # noqa: BLE001
            if "tk" not in captured:
                raise
            # La configuration est chargée, seul le moteur a échoué (absent de l'environnement du projet…) : les
            # templates et le publish n'en ont pas besoin.
            log.warning("engine %s did not start (%s), continuing without engine", engine_name, e)
            tk = captured["tk"]
        # Après un éventuel core swap : nouveaux modules, nouvel utilisateur.
        self.sgtk = importlib.import_module("sgtk")
        self._forbid_prompts()
        self.user = self.sgtk.get_authenticated_user() or self.user
        self.sg = tk.shotgun
        self.tk = tk
        self.tk_project_id = project_id
        log.info(
            "Toolkit ready for project %d (core %s, %s)",
            project_id,
            self._core_version(),
            os.path.dirname(os.path.dirname(self.sgtk.__file__)),
        )
        return tk

    # ---- Tâches, contexte, templates --------------------------------------------------------------------------------

    def _task(self, task_id):
        task = self._connect().find_one(
            "Task", [["id", "is", task_id]], ["content", "entity", "project", "step", "sg_status_list"]
        )
        if task is None:
            raise BridgeError("not_found", "Task %d not found" % task_id)
        if not task.get("project"):
            raise BridgeError("not_found", "Task %d has no project" % task_id)
        return task

    def _task_entity(self, task):
        """La tâche avec les noms de ses liens, pour context_from_entity_dictionary (sans requête de plus). Les
        liens rendus par ShotGrid portent déjà "name" ; le site simulé (mockgun) ne le fait pas pour tous."""
        sg = self._connect()
        found = sg.find_one("Task", [["id", "is", task["id"]]], ["project.Project.name", "step.Step.code"]) or {}
        entity = dict(task)
        entity["type"] = "Task"
        entity["project"] = dict(task["project"], name=task["project"].get("name") or found.get("project.Project.name"))
        if task.get("step"):
            entity["step"] = dict(task["step"], name=task["step"].get("name") or found.get("step.Step.code"))
        link = task.get("entity")
        if link and not link.get("name"):
            linked = sg.find_one(link["type"], [["id", "is", link["id"]]], ["code", "name"]) or {}
            entity["entity"] = dict(link, name=linked.get("code") or linked.get("name"))
        return entity

    def _work_area(self, task_id, create_folders):
        """(tk, contexte, template, champs) de la tâche. Champs None si les dossiers de la tâche n'existent pas et
        que create_folders est faux (il n'y a alors aucun fichier de travail)."""
        task = self._task(task_id)
        tk = self.tk_for_project(task["project"]["id"])
        ctx = tk.context_from_entity("Task", task_id)
        entity_type = (ctx.entity or {}).get("type")
        template_name = self.settings["work_templates"].get(entity_type)
        if template_name is None:
            raise BridgeError("template_missing", "no work template configured for %s tasks" % entity_type)
        template = tk.templates.get(template_name)
        if template is None:
            similar = sorted(name for name in tk.templates if "armorpaint" in name)
            raise BridgeError(
                "template_missing",
                "template '%s' is not defined in the project configuration (templates.yml)%s"
                % (template_name, "; found: " + ", ".join(similar) if similar else ""),
            )
        try:
            fields = ctx.as_template_fields(template, validate=True)
        except Exception as e:  # noqa: BLE001 — TankError : dossiers de la tâche absents du path cache
            if not create_folders:
                log.debug("work area of task %d not ready: %s", task_id, e)
                return tk, ctx, template, None
            tk.create_filesystem_structure("Task", task_id)
            fields = ctx.as_template_fields(template, validate=True)
        return tk, ctx, template, fields

    def _work_files(self, tk, template, fields, name):
        fields = dict(fields)
        skip = ["version"]
        if name is None:
            skip.append("name")
        else:
            fields["name"] = name
        files = []
        for path in tk.paths_from_template(template, fields, skip_keys=skip, skip_missing_optional_keys=True):
            values = template.get_fields(path)
            try:
                stat = os.stat(path)
            except OSError:
                continue
            files.append(
                {
                    "path": path,
                    "name": values.get("name"),
                    "version": values.get("version"),
                    "mtime": stat.st_mtime,
                    "size": stat.st_size,
                }
            )
        files.sort(key=lambda f: (f["name"] or "", f["version"] or 0))
        return files

    def _publishes(self, task_id, name=None):
        filters = [
            ["task", "is", {"type": "Task", "id": task_id}],
            ["published_file_type.PublishedFileType.code", "is", self.settings["publish_type"]],
        ]
        if name is not None:
            filters.append(["name", "is", name])
        publishes = self._connect().find(
            "PublishedFile",
            filters,
            ["name", "code", "version_number", "path", "description", "created_by", "created_at"],
            order=[{"field_name": "version_number", "direction": "desc"}],
        )
        for publish in publishes:
            path = publish.get("path") or {}
            publish["path"] = path.get("local_path") if isinstance(path, dict) else None
        return publishes

    # ---- Opérations -------------------------------------------------------------------------------------------------

    def op_ping(self, args):
        return {}

    def op_shutdown(self, args):
        return {}

    def op_auth(self, args):
        """Vérifie la session (ou le script) par une requête, et retourne l'utilisateur."""
        self._connect()
        human = self._human_user()
        if human is None:
            self.sg.find_one("Project", [], ["id"])  # script : rien n'a encore touché le serveur
        return {
            "host": self.user.host,
            "login": getattr(self.user, "login", None),
            "user": human,
            "method": "script" if os.environ.get("STUDIO_SG_SCRIPT_NAME") else "session",
        }

    def op_context(self, args):
        """Contexte de lancement transmis par ShotGrid Desktop (SHOTGUN_ENTITY_TYPE/ID), ou null."""
        entity_type = os.environ.get("SHOTGUN_ENTITY_TYPE")
        entity_id = os.environ.get("SHOTGUN_ENTITY_ID")
        if not entity_type or not entity_id or not entity_id.isdigit():
            return None
        fields = ["project", "code", "name", "content", "entity", "step"]
        schema = self._connect().schema_field_read(entity_type)
        entity = self.sg.find_one(entity_type, [["id", "is", int(entity_id)]], [f for f in fields if f in schema])
        if entity is None:
            raise BridgeError("not_found", "%s %s not found" % (entity_type, entity_id))
        return entity

    def op_list_projects(self, args):
        return self._connect().find(
            "Project",
            [["archived", "is_not", True], ["is_template", "is_not", True]],
            ["name", "tank_name", "sg_status"],
            order=[{"field_name": "name", "direction": "asc"}],
        )

    def op_list_entities(self, args):
        project_id = _int(args, "project_id")
        entity_type = args.get("entity_type", "Asset")
        if entity_type not in self.settings["entity_types"]:
            raise BridgeError("bad_request", "entity_type must be one of %s" % ", ".join(self.settings["entity_types"]))
        fields = ["code", "description", "sg_status_list"]
        fields += {"Asset": ["sg_asset_type"], "Shot": ["sg_sequence"]}.get(entity_type, [])
        return self._connect().find(
            entity_type,
            [["project", "is", {"type": "Project", "id": project_id}]],
            fields,
            order=[{"field_name": "code", "direction": "asc"}],
        )

    def op_list_assets(self, args):
        return self.op_list_entities(dict(args, entity_type="Asset"))

    def op_list_tasks(self, args):
        """Tâches d'un projet et/ou d'une entité ; mine (défaut : vrai sans entité) = assignées à l'utilisateur."""
        filters = []
        project_id = _int(args, "project_id", required=False)
        if project_id is not None:
            filters.append(["project", "is", {"type": "Project", "id": project_id}])
        if args.get("entity") is not None:
            filters.append(["entity", "is", _entity(args["entity"])])
        mine = args.get("mine", args.get("entity") is None)
        if mine:
            human = self._human_user()
            if human is None:
                raise BridgeError("bad_request", "mine needs a user session (not a script login)")
            filters.append(["task_assignees", "is", {"type": "HumanUser", "id": human["id"]}])
        if not filters:
            raise BridgeError("bad_request", "list_tasks needs project_id, entity or mine")
        sg = self._connect()
        tasks = sg.find(
            "Task",
            filters,
            ["content", "entity", "project", "step", "sg_status_list", "task_assignees", "due_date",
             "step.Step.code", "project.Project.name"],
            order=[{"field_name": "content", "direction": "asc"}],
        )
        self._name_links(sg, [t["entity"] for t in tasks if t.get("entity")])
        return tasks

    @staticmethod
    def _name_links(sg, links):
        """Complète les liens {type, id} par "name" (le code de l'entité) et, pour un Asset, "sg_asset_type" : une
        requête par type. ShotGrid met en général "name" dans les liens, mais pas toujours (ni mockgun), et un champ
        pointé ("entity.Asset.code") sur un lien polymorphe échoue dès qu'une tâche est sur un autre type."""
        by_type = {}
        for link in links:
            by_type.setdefault(link["type"], []).append(link)
        for entity_type, group in by_type.items():
            fields = ["code", "sg_asset_type"] if entity_type == "Asset" else ["code"]
            ids = sorted({link["id"] for link in group})
            found = {e["id"]: e for e in sg.find(entity_type, [["id", "in", ids]], fields)}
            for link in group:
                entity = found.get(link["id"], {})
                if entity.get("code") is not None:
                    link["name"] = entity["code"]
                if entity_type == "Asset":
                    link["sg_asset_type"] = entity.get("sg_asset_type")

    def op_list_workfiles(self, args):
        """Fichiers de travail de la tâche sur disque (template de travail) et publishes ArmorPaint."""
        task_id = _int(args, "task_id")
        name = _str(args, "name", required=False)
        tk, ctx, template, fields = self._work_area(task_id, create_folders=False)
        files = self._work_files(tk, template, fields, name) if fields is not None else []
        return {"template": template.name, "files": files, "publishes": self._publishes(task_id)}

    def op_next_version(self, args):
        """Prochaine version libre pour name dans la tâche, et son chemin. Crée les dossiers de la tâche (schéma
        Toolkit) s'ils n'existent pas : l'appelant va y sauver."""
        task_id = _int(args, "task_id")
        name = _str(args, "name", required=False) or self.settings["default_name"]
        tk, ctx, template, fields = self._work_area(task_id, create_folders=True)
        versions = [f["version"] or 0 for f in self._work_files(tk, template, fields, name)]
        probe = dict(fields, name=name, version=1)
        pub_name = publish_name(template.apply_fields(probe))
        versions += [p.get("version_number") or 0 for p in self._publishes(task_id, pub_name)]
        version = max(versions, default=0) + 1
        path = template.apply_fields(dict(fields, name=name, version=version))
        while os.path.exists(path):  # un fichier hors template (copie manuelle…) occupe déjà le numéro
            version += 1
            path = template.apply_fields(dict(fields, name=name, version=version))
        return {"name": name, "version": version, "path": path, "publish_name": pub_name}

    # ---- Apps Toolkit (moteur tk-armorpaint) -------------------------------------------------------------------------

    def _default_project(self):
        """Projet d'une fenêtre sans contexte donné : celui du lancement par ShotGrid Desktop, sinon celui de la
        dernière tâche assignée à l'artiste."""
        launch = self.op_context({})
        if launch:
            if launch.get("type") == "Project":
                return launch["id"]
            if launch.get("project"):
                return launch["project"]["id"]
        human = self._human_user()
        if human is not None:
            task = self._connect().find_one(
                "Task",
                [["task_assignees", "is", {"type": "HumanUser", "id": human["id"]}]],
                ["project"],
                order=[{"field_name": "id", "direction": "desc"}],
            )
            if task and task.get("project"):
                return task["project"]["id"]
        raise BridgeError(
            "bad_request", "no project for show_app: pass project_id or task_id, or launch ArmorPaint from ShotGrid Desktop"
        )

    def _engine_for(self, args):
        """Le moteur tk-armorpaint dans le contexte demandé : task_id, sinon project_id, sinon _default_project().
        Sans task_id, le moteur garde son contexte courant dans ce projet (celui de la dernière fenêtre)."""
        task_id = _int(args, "task_id", required=False)
        project_id = _int(args, "project_id", required=False)
        task = None
        if task_id is not None:
            task = self._task(task_id)
            project_id = task["project"]["id"]
        elif project_id is None:
            project_id = self._default_project()
        self.tk_for_project(project_id)
        engine_name = self.settings["engine"]
        engine = self.sgtk.platform.current_engine()
        if engine is None or engine.name != engine_name:
            raise BridgeError(
                "app_unavailable",
                "engine %s is not running for project %d: it must be in the environments of the project's Toolkit "
                "configuration (see the log for the bootstrap)" % (engine_name, project_id),
            )
        if task is not None and (engine.context.task or {}).get("id") != task_id:
            # Dossiers de la tâche d'abord, comme au lancement d'un DCC (tk-multi-launchapp) : sans eux, les apps
            # dont les templates en dépendent (workfiles2) ne se chargent pas dans le nouveau contexte.
            engine.sgtk.create_filesystem_structure("Task", task_id)
            ctx = engine.sgtk.context_from_entity_dictionary(self._task_entity(task))
            self.sgtk.platform.change_context(ctx)
            engine = self.sgtk.platform.current_engine()
        return engine

    def op_list_commands(self, args):
        """Les commandes du moteur tk-armorpaint (le menu ShotGrid d'ArmorPaint), dans l'ordre d'enregistrement :
        [{"name","type","app","app_instance","description"}], type "context_menu" pour la section contexte ;
        favourites : noms des commandes du réglage menu_favourites du moteur. Même contexte que show_app."""
        engine = self._engine_for(args)
        commands = []
        for name, entry in engine.commands.items():
            props = entry.get("properties") or {}
            app = props.get("app")
            commands.append(
                {
                    "name": name,
                    "type": props.get("type") or "default",
                    "app": app.display_name if app is not None else None,
                    "app_instance": app.instance_name if app is not None else None,
                    "description": props.get("description"),
                }
            )
        favourites = []
        for fav in engine.get_setting("menu_favourites") or []:
            for c in commands:
                if c["app_instance"] == fav.get("app_instance") and c["name"] == fav.get("name"):
                    favourites.append(c["name"])
        return {"context": engine.context_info(), "commands": commands, "favourites": favourites, "has_ui": engine.has_ui}

    def op_show_app(self, args):
        """Lance une commande du moteur tk-armorpaint et répond quand elle a fini : pour une fenêtre d'app Toolkit,
        à sa fermeture. command : un nom de list_commands, ou "open" / "save" (File Open... / File Save... de
        tk-multi-workfiles2). Pendant ce temps, l'app agit sur la scène par des requêtes host (hooks du moteur) :
        ArmorPaint lance donc cette requête en job, sans délai. Contexte comme _engine_for(). Retourne le contexte
        à la fin (une app a pu en changer)."""
        command = _str(args, "command")
        command = {"open": "File Open...", "save": "File Save..."}.get(command, command)
        engine = self._engine_for(args)
        entry = engine.commands.get(command)
        if entry is None:
            hint = "" if engine.has_ui else " (no Qt: PySide is not importable by %s, apps with windows are not loaded)" % sys.executable
            raise BridgeError(
                "app_unavailable",
                "command '%s' not available in %s for %s%s; available: %s"
                % (command, engine.name, engine.context, hint, ", ".join(sorted(engine.commands)) or "none"),
            )
        log.info("show_app: %s in %s", command, engine.context)
        entry["callback"]()
        engine = self.sgtk.platform.current_engine() or engine  # une app a pu changer de contexte
        self.engine = engine
        return {"command": command, "context": engine.context_info()}

    def op_register_publish(self, args):
        """Crée le PublishedFile d'un fichier existant, lié à la tâche (sgtk.util.register_publish)."""
        task_id = _int(args, "task_id")
        path = _str(args, "path")
        if not os.path.exists(path):
            raise BridgeError("not_found", "file to publish does not exist: %s" % path)
        thumbnail = _str(args, "thumbnail_path", required=False)
        if thumbnail is not None and not os.path.exists(thumbnail):
            raise BridgeError("not_found", "thumbnail does not exist: %s" % thumbnail)
        version = _int(args, "version", required=False)
        if version is None:
            version = version_in_name(path)
        if version is None:
            raise BridgeError("bad_request", "version is required when the file name carries none")
        name = _str(args, "name", required=False) or publish_name(path)
        dependencies = args.get("dependency_paths") or []
        if not isinstance(dependencies, list) or not all(isinstance(d, str) for d in dependencies):
            raise BridgeError("bad_request", "dependency_paths must be a list of strings")
        task = self._task(task_id)
        tk = self.tk_for_project(task["project"]["id"])
        ctx = tk.context_from_entity("Task", task_id)
        publish = self.sgtk.util.register_publish(
            tk,
            ctx,
            path,
            name,
            version,
            comment=args.get("description") or "",
            published_file_type=_str(args, "type", required=False) or self.settings["publish_type"],
            thumbnail_path=thumbnail,
            dependency_paths=dependencies,
        )
        local = (publish.get("path") or {}).get("local_path") if isinstance(publish.get("path"), dict) else None
        return {
            "id": publish["id"],
            "name": publish.get("name"),
            "code": publish.get("code"),
            "version_number": publish.get("version_number"),
            "path": local or path,
        }

    def op_upload_thumbnail(self, args):
        entity = _entity(args.get("entity"))
        path = _str(args, "path")
        if not os.path.exists(path):
            raise BridgeError("not_found", "thumbnail does not exist: %s" % path)
        return {"attachment_id": self._connect().upload_thumbnail(entity["type"], entity["id"], path)}
