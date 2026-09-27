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
#
# Mode dégradé (lot C5) : chaque appel à l'API ShotGrid dit au superviseur si le site a répondu (_watch_network, champ
# "sg" des réponses), et les connexions ont un délai (STUDIO_SG_NET_TIMEOUT, 30 s) : ShotGrid injoignable ne bloque
# jamais longtemps. Un publish que ShotGrid injoignable fait échouer une fois ses fichiers écrits est mis en file
# d'attente (studio_sg_queue.py, dossier STUDIO_SG_QUEUE) et rejoué plus tard (op replay_publishes). Les publishes
# enregistrés, mis en file, rejoués ou écartés vont au journal d'audit (studio_audit.py, fichier STUDIO_AUDIT_LOG).

import datetime
import errno
import glob
import importlib
import importlib.util
import logging
import os
import re
import socket
import sys

import studio_audit
import studio_export_choices
import studio_sg_bridge
import studio_sg_queue
from studio_sg_bridge import BridgeError

log = logging.getLogger("studio_sg")

# v003 entre deux séparateurs : robot.v003.arm, et robot_v003_base_color.1001.png (textures nommées <file>_<channel>).
VERSION_IN_NAME = re.compile(r"[._-]v\d+(?=[._-]|$)", re.IGNORECASE)


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
        # Templates des textures exportées par type d'entité (lot B3, op texture_pattern), et noms de leurs clés pour
        # le canal (base, nor…), la tuile UDIM et l'extension (png, jpg, exr).
        "texture_templates": _env_map(
            "STUDIO_SG_TEXTURE_TEMPLATES", {"Asset": "armorpaint_asset_textures", "Shot": "armorpaint_shot_textures"}
        ),
        "texture_keys": dict(
            {"channel": "channel", "udim": "UDIM", "ext": "texture_ext"}, **_env_map("STUDIO_SG_TEXTURE_KEYS", {})
        ),
        "publish_type": os.environ.get("STUDIO_SG_PUBLISH_TYPE", "ArmorPaint Project"),
        "default_name": os.environ.get("STUDIO_SG_DEFAULT_NAME", "scene"),
        "entity_types": ["Asset", "Shot"],
    }


def publish_name(path):
    """Nom de publish à la convention Toolkit : le nom de fichier sans son numéro de version (scene.v003.arm ->
    scene.arm, robot_v003_base.png -> robot_base.png), qui regroupe les versions d'un même élément dans ShotGrid."""
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


def _seconds(name, default):
    try:
        return float(os.environ.get(name) or default)
    except ValueError:
        log.warning("%s is not a number of seconds: %s used", name, default)
        return default


# Erreurs qui disent ShotGrid injoignable, par nom de classe (voir Session.classify) : réseau, DNS (httplib2 enveloppe
# gaierror dans ServerNotFoundError), proxy, réponse HTTP en erreur (ProtocolError : 502, 503…), et les erreurs
# système d'une connexion qui n'aboutit pas.
_UNREACHABLE = {"ProtocolError", "ConnectionError", "TimeoutError", "SSLError", "URLError", "gaierror", "timeout",
                "RemoteDisconnected", "ServerNotFoundError", "ProxyError", "IncompleteRead"}
_NET_ERRNO = {errno.ENETUNREACH, errno.EHOSTUNREACH, errno.ENETDOWN, errno.EHOSTDOWN, errno.ECONNREFUSED,
              errno.ECONNRESET, errno.ECONNABORTED, errno.ETIMEDOUT}


def unreachable(e):
    """Vrai si l'erreur, ou celle qu'elle enveloppe (ShotgunPublishError de register_publish, erreur relancée par un
    hook), dit ShotGrid injoignable."""
    seen = set()
    while e is not None and id(e) not in seen:
        seen.add(id(e))
        if {cls.__name__ for cls in type(e).__mro__} & _UNREACHABLE:
            return True
        if isinstance(e, OSError) and e.errno in _NET_ERRNO:
            return True
        e = e.__cause__ or e.__context__
    return False


def _site_key(url):
    return (url or "").strip().rstrip("/").lower()


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
        self.queue = studio_sg_queue.Queue.from_env()  # file d'attente des publishes (lot C5), None sans dossier
        self._publishing = 0  # un publish est en cours (op publish, fenêtre ouverte par show_app)
        self._placeholders = {}  # id provisoire (négatif) d'un publish mis en file -> sa clé
        timeout = _seconds("STUDIO_SG_NET_TIMEOUT", 30.0)
        socket.setdefaulttimeout(timeout if timeout > 0 else None)  # shotgun_api3 et tk-core n'en posent pas
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
            "queue": self.queue.directory if self.queue is not None else None,
            "net_timeout": socket.getdefaulttimeout(),
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
        self._watch_network()
        return fn(args)

    def classify(self, e):
        """Traduit une exception en BridgeError. Par nom de classe : après un core swap, les classes de tk-core et
        de shotgun_api3 ne sont plus celles importées au démarrage."""
        names = {cls.__name__ for cls in type(e).__mro__}
        message = str(e) or type(e).__name__
        if names & {"AuthenticationFault", "MissingTwoFactorAuthenticationFault", "AuthenticationError",
                    "AuthenticationCancelled", "IncompleteCredentials"}:
            return BridgeError("auth_required", "ShotGrid authentication required: %s" % message)
        if unreachable(e):
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

    @staticmethod
    def _watch_network():
        """Chaque appel à l'API ShotGrid dit au superviseur si le site a répondu (studio_sg_bridge.network) : l'état
        hors ligne d'ArmorPaint (lot C5). Ils passent tous par Shotgun._call_rpc, remplacé sur chaque copie chargée de
        shotgun_api3 (celle de tk-core, puis celle d'un core swap) ; refait à chaque requête, tk-core important
        shotgun_api3 à la demande. Les envois de fichiers (vignettes) n'y passent pas : leurs erreurs le disent."""
        for name, module in list(sys.modules.items()):
            if not (name == "shotgun_api3" or name.endswith(".shotgun_api3") or name.endswith("shotgun_api3.shotgun")):
                continue
            cls = vars(module).get("Shotgun")  # vars : pas de __getattr__ de module (imports paresseux)
            if not isinstance(cls, type) or "_call_rpc" not in vars(cls) or vars(cls).get("_studio_watched"):
                continue

            def call_rpc(sg, *args, _call_rpc=cls._call_rpc, **kwargs):
                try:
                    result = _call_rpc(sg, *args, **kwargs)
                except Exception as e:
                    if unreachable(e):
                        studio_sg_bridge.network(False, str(e) or type(e).__name__)
                    elif "Fault" in {c.__name__ for c in type(e).__mro__}:
                        studio_sg_bridge.network(True)  # le site a répondu, par une erreur
                    raise
                studio_sg_bridge.network(True)
                return result

            cls._call_rpc = call_rpc
            cls._studio_watched = True

    def _queue_publishes(self):
        """Remplace sgtk.util.register_publish du core chargé (à refaire après un core swap, comme _forbid_prompts ;
        les hooks de tk-multi-publish2 l'appellent par sgtk.util) : voir _register_or_queue."""
        util = self.sgtk.util
        if getattr(util.register_publish, "_studio_original", None) is not None:
            return
        original = util.register_publish

        def register_publish(tk, context, path, name, version_number, **kwargs):
            return self._register_or_queue(original, tk, context, path, name, version_number, kwargs)

        register_publish.__doc__ = original.__doc__
        register_publish._studio_original = original
        util.register_publish = register_publish

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

    def _site(self):
        """URL du site ShotGrid de la session, sans requête."""
        self._connect()
        return self.user.host

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
        self._queue_publishes()
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

    def op_check_online(self, args):
        """Le site répond-il ? Une requête légère (info), sans amorçage ni authentification : pour sortir du mode
        hors ligne (lot C5). Erreur sg_unreachable sinon."""
        info = self._connect().info() or {}
        return {"online": True, "site": self._site(), "version": info.get("version")}

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

    def _project_from_path(self, path):
        """Projet d'un chemin, comme Toolkit le range : <racine d'un LocalStorage>/<tank_name du projet>/… ; None hors
        de ces dossiers. Le lien symbolique /tmp -> /private/tmp de macOS compte : le chemin est aussi comparé résolu."""
        field = {"darwin": "mac_path", "win32": "windows_path"}.get(sys.platform, "linux_path")
        sg = self._connect()
        roots = [s[field] for s in sg.find("LocalStorage", [], [field]) if s.get(field)]
        names = []
        for candidate in (os.path.normpath(path), os.path.realpath(path)):
            for root in roots:
                for r in (os.path.normpath(root), os.path.realpath(root)):
                    if os.path.normcase(candidate).startswith(os.path.normcase(r.rstrip(os.sep)) + os.sep):
                        names.append(candidate[len(r.rstrip(os.sep)) + 1 :].split(os.sep)[0])
        for name in dict.fromkeys(names):
            project = sg.find_one("Project", [["tank_name", "is", name]], ["id"])
            if project is not None:
                return project["id"]
        return None

    def op_texture_pattern(self, args):
        """Modèle du chemin des textures exportées depuis le fichier de travail path (lot B3) : le template des textures
        du type d'entité (réglage texture_templates), rempli avec les champs du fichier (nom, version…) et de son
        contexte. Les clés du canal, de la tuile UDIM et de l'extension (réglage texture_keys) y restent en marqueurs
        <channel>, <UDIM>, <ext>, qu'ArmorPaint remplace à chaque texture (studio_export_naming.c). pattern : sans
        tuile ; pattern_udim : avec (null si le template n'a pas de clé UDIM, ou si elle est obligatoire pour
        pattern). template null (et reason) : fichier hors des templates du projet, ou pas de template de textures
        pour son type d'entité ; ArmorPaint garde alors ses noms. Projet : project_id (sauf d'un autre site,
        _other_site), sinon celui du chemin (_project_from_path), sinon celui déjà amorcé, sinon _default_project().
        Un autre template (lot B6) : args template, sinon celui choisi pour le projet dans la fenêtre d'export
        ("texture_template" de son sidecar, studio_export_choices.py) ; chosen le dit. Pour la fenêtre, la réponse
        donne aussi default_template (celui du réglage, null sans), templates (ceux du projet qui nomment des textures
        et se remplissent depuis path) et name, version (champs du fichier de travail, null s'il n'en a pas) ; l'erreur
        template_missing les porte aussi, pour choisir un autre template."""
        path = _str(args, "path")
        project_id = _int(args, "project_id", required=False)
        if self._other_site(args):
            project_id = None
        project_id = project_id or self._project_from_path(path)
        if project_id is not None:
            tk = self.tk_for_project(project_id)
        else:
            tk = self.tk or self.tk_for_project(self._default_project())
        work = tk.template_from_path(path)
        if work is None:
            return {"template": None, "reason": "%s matches no template of the project configuration" % path}
        ctx = tk.context_from_path(path)
        entity_type = (ctx.entity or {}).get("type")
        if entity_type is None:  # dossiers absents du path cache : le type d'après le template de travail
            entity_type = next((t for t, n in self.settings["work_templates"].items() if n == work.name), None)
        fields = work.get_fields(path)
        default = self.settings["texture_templates"].get(entity_type)
        chosen = _str(args, "template", required=False) or studio_export_choices.texture_template(path)
        info = {
            "default_template": default,
            "chosen": chosen is not None,
            "templates": self._texture_templates(tk, ctx, fields, default),
            "name": fields.get("name"),
            "version": fields.get("version"),
        }
        name = chosen or default
        if name is None:
            return dict(info, template=None,
                        reason="no texture template configured for %s (%s)" % (entity_type, work.name))
        template = tk.templates.get(name)
        where = " (chosen for this project in %s)" % studio_export_choices.sidecar_path(path) if chosen else ""
        if template is None:
            raise BridgeError("template_missing", "template '%s' is not defined in the project configuration%s"
                              % (name, where), details=info)
        keys = self.settings["texture_keys"]
        for role in ("channel", "ext"):
            if keys[role] not in template.keys:
                raise BridgeError(
                    "template_missing", "template '%s' has no {%s} key (texture %s)%s" % (name, keys[role], role, where),
                    details=info,
                )
        pattern, pattern_udim = self._texture_fill(template, ctx, fields)
        if pattern is None and pattern_udim is None:
            raise BridgeError(
                "template_missing", "template '%s' cannot be filled from %s (%s)%s" % (name, path, work.name, where),
                details=info,
            )
        return dict(info, template=name, pattern=pattern, pattern_udim=pattern_udim)

    def _texture_fill(self, template, ctx, fields, quiet=False):
        """(pattern, pattern_udim) de template rempli avec le contexte et les champs du fichier de travail ; None pour
        celui qui ne se remplit pas (dit au journal, sauf quiet)."""
        keys = self.settings["texture_keys"]
        try:
            values = ctx.as_template_fields(template)
        except Exception as e:  # noqa: BLE001 — TankError : contexte sans dossiers ; les champs du fichier suffisent
            log.debug("texture_pattern: no context fields for %s: %s", template, e)
            values = {}
        values.update(fields)
        values.pop(keys["udim"], None)
        # Marqueurs hors du type des clés (ignore_types, API interne de Template stable depuis tk-core 0.18) : une clé
        # channel en alphanumeric ou à choix refuserait "<channel>". Les autres champs sont validés normalement.
        values.update({keys["channel"]: "<channel>", keys["ext"]: "<ext>"})
        ignore = [keys["channel"], keys["ext"], keys["udim"]]

        def resolve(fields, skip_defaults=False):
            try:
                return template._apply_fields(fields, ignore_types=ignore, skip_defaults=skip_defaults)
            except self.sgtk.TankError as e:
                log.log(logging.DEBUG if quiet else logging.INFO, "texture_pattern: %s: %s", template, e)
                return None

        # Sans tuile : la clé UDIM (une séquence a un défaut, %04d) ne doit pas remplir sa section optionnelle.
        # skip_defaults=True écarte les sections dont une clé n'a pas de valeur, défaut compris : les autres clés
        # reçoivent donc leur défaut explicitement.
        defaults = {k: key.default for k, key in template.keys.items() if key.default is not None and k != keys["udim"]}
        pattern = resolve(dict(defaults, **values), skip_defaults=True)
        pattern_udim = resolve(dict(values, **{keys["udim"]: "<UDIM>"})) if keys["udim"] in template.keys else None
        return pattern, pattern_udim

    def _texture_templates(self, tk, ctx, fields, default):
        """Noms des templates de chemin du projet qui peuvent nommer les textures de ce fichier de travail : clés du
        canal et de l'extension, et remplis par son contexte et ses champs. Celui du réglage d'abord."""
        keys = self.settings["texture_keys"]
        names = []
        for name, template in sorted(tk.templates.items()):
            if not isinstance(template, self.sgtk.TemplatePath):
                continue
            if keys["channel"] not in template.keys or keys["ext"] not in template.keys:
                continue
            if any(p is not None for p in self._texture_fill(template, ctx, fields, quiet=True)):
                names.append(name)
        return sorted(names, key=lambda n: n != default)

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

    def _other_site(self, args):
        """Vrai si args donne un site (celui du contexte, engine.context_info) qui n'est pas celui de la session : ses
        identifiants (task_id, project_id) sont alors ignorés (projet enregistré ailleurs, contexte gardé dans son
        .arm, lot C5)."""
        site = _str(args, "site", required=False)
        if site and _site_key(site) != _site_key(self._site()):
            log.warning("context of another ShotGrid site ignored (%s, this site is %s)", site, self._site())
            return True
        return False

    def _engine_for(self, args):
        """Le moteur tk-armorpaint dans le contexte demandé : task_id, sinon project_id, sinon _default_project().
        Sans task_id, le moteur garde son contexte courant dans ce projet (celui de la dernière fenêtre). site :
        _other_site()."""
        task_id = _int(args, "task_id", required=False)
        project_id = _int(args, "project_id", required=False)
        if self._other_site(args):
            task_id = project_id = None
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
        self._publishing += 1  # la fenêtre Publish peut publier : mise en file d'attente possible (lot C5)
        try:
            entry["callback"]()
        finally:
            self._publishing -= 1
        engine = self.sgtk.platform.current_engine() or engine  # une app a pu changer de contexte
        self.engine = engine
        return {"command": command, "context": engine.context_info()}

    def op_publish(self, args):
        """Publie le projet ouvert dans ArmorPaint sans fenêtre (lot C4) : collecte, validation, publish et finalisation
        de tk-multi-publish2, avec les hooks du moteur (tk-armorpaint/hooks/tk-multi-publish2), comme le bouton Publish
        de sa fenêtre. Les hooks agissent sur le projet par des requêtes host : ArmorPaint lance cette requête en job.
        Contexte comme _engine_for(). description : le commentaire des publishes ; skip : éléments à ne pas publier,
        par nom (robot_base.<UDIM>.png) ou par type (armorpaint.texture). Une tâche décochée par son plugin (texture sans
        version) reste décochée. Retourne {"context", "publishes": [{"item","type","id","name","path","version_number",
        "published_file_type"}], "versions": [{"item","id","code"}] (Versions de review, upload_version.py), "skipped",
        "finalized", "messages": [{"level","message"}]} (avertissements et erreurs des hooks) ; finalized faux :
        publiés, mais la finalisation (version suivante du projet) a échoué, voir messages. ShotGrid devenu injoignable
        pendant le publish (lot C5) : les publishes restants sont mis en file d'attente, "queued" (leur clé) à la place
        de "id" ; la liste de ces clés dans "queued". Erreurs :
        publish_invalid (validation, rien n'est publié), sg_unreachable (ShotGrid injoignable avant le publish, rien
        n'est écrit), publish_failed (au publish : ce qui est déjà publié est dans le message), nothing_to_publish,
        app_unavailable (pas de tk-multi-publish2)."""
        description = args.get("description")
        if description is not None and not isinstance(description, str):
            raise BridgeError("bad_request", "description must be a string")
        skip = args.get("skip") or []
        if not isinstance(skip, list) or not all(isinstance(s, str) for s in skip):
            raise BridgeError("bad_request", "skip must be a list of strings")
        engine = self._engine_for(args)
        app = next((a for a in engine.apps.values() if a.name == "tk-multi-publish2"), None)
        if app is None:
            raise BridgeError(
                "app_unavailable", "tk-multi-publish2 is not in the %s environment of %s" % (engine.name, engine.context)
            )
        messages = []

        class Messages(logging.Handler):
            def emit(self, record):
                messages.append({"level": record.levelname.lower(), "message": record.getMessage()})

        # Le logger de l'app : celui de ses hooks (sgtk.env.<env>.tk-armorpaint.tk-multi-publish2.hook.<fichier>) en
        # descend, comme pour la fenêtre, qui y branche son journal.
        handler = Messages(logging.WARNING)
        app.logger.addHandler(handler)
        self._publishing += 1
        try:
            return self._publish(engine, app.create_publish_manager(), description, skip, messages)
        finally:
            self._publishing -= 1
            app.logger.removeHandler(handler)
            self.engine = self.sgtk.platform.current_engine() or engine

    def _publish(self, engine, manager, description, skip, messages):
        log.info("publish: %s", engine.context)
        manager.collect_session()
        skipped = []
        for item in manager.tree:
            if item.name in skip or item.type_spec in skip:
                item.active = False
                skipped.append(item.name)
            if description is not None:
                item.description = description
        tasks = [(i, t) for i in manager.tree if i.active for t in i.tasks if t.active]
        if not tasks:
            raise BridgeError("nothing_to_publish", "nothing to publish in %s" % engine.context)
        try:
            failures = manager.validate()
            if failures:
                message = "; ".join("%s (%s): %s" % (t.item.name, t.name, e or "not valid") for t, e in failures)
                if any(isinstance(e, BaseException) and unreachable(e) for _, e in failures):
                    raise BridgeError("sg_unreachable", "ShotGrid unreachable, nothing published: %s" % message)
                raise BridgeError("publish_invalid", message)
            try:
                manager.publish()
            except Exception as e:  # noqa: BLE001 — l'erreur du plugin, avec ce qui est déjà publié ou en file
                data = [(i.name, i.properties.get("sg_publish_data")) for i in manager.tree]
                done = [name for name, d in data if d and not d.get("queued")]
                queued = [name for name, d in data if d and d.get("queued")]
                raise BridgeError(
                    "publish_failed",
                    "%s%s%s" % (e, " (already published: %s)" % ", ".join(done) if done else "",
                                " (queued until ShotGrid is back: %s)" % ", ".join(queued) if queued else ""),
                )
            finalized = True
            try:
                manager.finalize()
            except Exception as e:  # noqa: BLE001 — publiés : la réponse le dit, avec l'erreur
                log.warning("publish: finalize failed: %s", e)
                messages.append({"level": "error", "message": "finalize: %s" % (e,)})
                finalized = False
        finally:
            self._reset_pixmap_check(manager)
        publishes = []
        for item in manager.tree:
            data = item.properties.get("sg_publish_data")
            if not data:
                continue
            path = data.get("path")
            queued = data.get("queued")
            publishes.append(
                {
                    "item": item.name,
                    "type": item.type_spec,
                    "id": data.get("id") if (data.get("id") or 0) > 0 else None,  # négatif : en file, provisoire
                    "name": data.get("name") or data.get("code"),
                    "path": (path.get("local_path") if isinstance(path, dict) else None) or item.properties.get("path"),
                    "version_number": data.get("version_number"),
                    "published_file_type": item.properties.get("publish_type"),
                    "queued": queued,
                }
            )
        versions = [
            {"item": item.name, "id": v.get("id"), "code": v.get("code")}
            for item in manager.tree
            for v in [item.properties.get("sg_version_data")]
            if v
        ]
        return {
            "context": engine.context_info(),
            "publishes": publishes,
            "versions": versions,
            "skipped": skipped,
            "finalized": finalized,
            "messages": messages,
            "queued": [p["queued"] for p in publishes if p["queued"]],
        }

    @staticmethod
    def _reset_pixmap_check(manager):
        # tk-multi-publish2 décide une fois par processus si les images (vignettes, icônes) sont utilisables : sans
        # QApplication, jamais plus. Après un publish sans fenêtre, la fenêtre Publish n'en aurait plus.
        module = sys.modules.get(type(manager.tree.root_item).__module__)
        if module is not None and hasattr(module, "_qt_pixmap_is_usable"):
            module._qt_pixmap_is_usable = None

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

    # ---- File d'attente des publishes (lot C5) ----------------------------------------------------------------------
    # Un publish écrit ses fichiers (copie du projet, export des textures) avant de les enregistrer dans ShotGrid
    # (register_publish). ShotGrid injoignable à ce moment-là, l'enregistrement est gardé dans la file
    # (studio_sg_queue.py) et le publish continue : le hook reçoit un PublishedFile provisoire, et les publishes
    # suivants du même passage vont aussi dans la file, sans attendre le délai réseau de chacun. Le rejeu
    # (replay_publishes) les enregistre plus tard dans l'ordre, avec leur date d'origine. Limite : le statut des
    # anciens publishes qu'un publish rejoué remplace n'est pas effacé (finalisation de tk-multi-publish2, sautée).

    def _register_or_queue(self, register, tk, context, path, name, version_number, kwargs):
        """register_publish (tk-core), inscrit au journal d'audit. Pendant un publish (op publish, fenêtre Publish,
        voir _publishing), ShotGrid injoignable le met en file d'attente au lieu d'échouer : un parent en file (ses
        enfants dépendent de son PublishedFile provisoire), ShotGrid déjà vu injoignable, ou l'échec réseau de
        l'enregistrement."""
        queue = self.queue if self._publishing and not kwargs.get("dry_run") else None
        parents = [self._placeholders[i] for i in kwargs.get("dependency_ids") or [] if i in self._placeholders]
        if queue is not None and (parents or studio_sg_bridge.OFFLINE):
            reason = "parent publish queued" if parents else studio_sg_bridge.OFFLINE
            return self._queue_publish(context, path, name, version_number, kwargs, parents, reason)
        try:
            publish = register(tk, context, path, name, version_number, **kwargs)
        except Exception as e:
            if queue is None or not unreachable(e):
                self._audit_publish("publish_failed", context, path, name, version_number, error=str(e))
                raise
            # Les envois de vignettes ne passent pas par _watch_network : l'état hors ligne est posé ici.
            studio_sg_bridge.network(False, str(e) or type(e).__name__)
            entity = getattr(e, "entity", None)  # ShotgunPublishError : le PublishedFile, s'il a été créé
            return self._queue_publish(context, path, name, version_number, kwargs, [], str(e), entity, e)
        if not kwargs.get("dry_run"):
            self._audit_publish("publish", context, path, name, version_number, id=(publish or {}).get("id"))
        return publish

    def _queue_publish(self, context, path, name, version_number, kwargs, parents, error, entity=None, cause=None):
        """Met le publish en file d'attente, rend son PublishedFile provisoire : id négatif, pour ses enfants
        (_register_or_queue), et "queued", sa clé. Déjà créé (entity), c'est le vrai, complété au rejeu."""
        kwargs = dict(kwargs)
        thumbnail = kwargs.pop("thumbnail_path", None)
        created_at = kwargs.pop("created_at", None)
        if isinstance(created_at, datetime.datetime):
            created_at = studio_sg_queue.iso(created_at.astimezone(datetime.timezone.utc))
        record = {
            "site": self._site(),
            "project_id": (context.project or {}).get("id"),
            "task_id": (context.task or {}).get("id"),
            "context": self._context_dict(context),
            "path": path,
            "name": name,
            "version_number": version_number,
            "published_file_type": kwargs.pop("published_file_type", None),
            "comment": kwargs.pop("comment", None),
            "created_at": created_at,
            "dependency_ids": [i for i in kwargs.pop("dependency_ids", None) or [] if i not in self._placeholders],
            "depends_on": parents,
            "kwargs": kwargs,
            "entity_id": entity["id"] if entity else None,
            "error": error,
        }
        try:
            key = self.queue.add(record, thumbnail)
        except Exception as e:  # noqa: BLE001 — pas de file : le publish échoue, comme sans elle
            log.error("publish queue: %s could not be queued: %s", path, e)
            self._audit_publish("publish_failed", context, path, name, version_number, error="%s (not queued: %s)" % (error, e))
            message = "ShotGrid unreachable (%s) and the publish could not be queued: %s" % (error, e)
            if cause is not None:
                raise RuntimeError(message) from cause
            raise RuntimeError(message) from e
        if entity:
            publish = dict(entity, queued=key)
        else:
            placeholder = -1 - len(self._placeholders)
            self._placeholders[placeholder] = key
            publish = {
                "type": "PublishedFile",
                "id": placeholder,
                "code": os.path.basename(path),
                "name": name,
                "version_number": version_number,
                "path": {"local_path": path},
                "queued": key,
            }
        log.warning("ShotGrid unreachable (%s): publish of %s queued as %s", error, path, key)
        self._audit_publish(
            "publish_queued", context, path, name, version_number, key=key, id=entity["id"] if entity else None,
            depends_on=parents or None, error=error,
        )
        return publish

    @staticmethod
    def _context_dict(context):
        # Comme Context.to_dict(), sans l'utilisateur : context.user peut interroger ShotGrid (injoignable ici).
        # Context.from_dict() le relit.
        def link(entity):
            return {k: entity[k] for k in ("type", "id", "name") if k in entity} if entity else None

        return {
            "project": link(context.project),
            "entity": link(context.entity),
            "step": link(context.step),
            "task": link(context.task),
            "additional_entities": [link(e) for e in context.additional_entities or []],
            "source_entity": link(context.source_entity),
        }

    def _audit_publish(self, event, context, path, name, version_number, **fields):
        project, task = getattr(context, "project", None) or {}, getattr(context, "task", None) or {}
        studio_audit.write(
            event, site=self.user.host if self.user is not None else None, sg_login=getattr(self.user, "login", None),
            project_id=project.get("id"), task_id=task.get("id"), path=path, name=name, version_number=version_number,
            **fields
        )

    def _audit_record(self, event, record, **fields):
        studio_audit.write(
            event, site=record.get("site"), sg_login=getattr(self.user, "login", None),
            project_id=record.get("project_id"), task_id=record.get("task_id"), path=record.get("path"),
            name=record.get("name"), version_number=record.get("version_number"), **fields
        )

    _PENDING_FIELDS = ("key", "created", "site", "project_id", "task_id", "path", "name", "version_number",
                       "published_file_type", "depends_on", "entity_id", "attempts", "last_attempt", "error")

    def op_pending_publishes(self, args):
        """Publishes en file d'attente, dans l'ordre, sans requête ShotGrid : {"queue" (dossier, null sans file),
        "count", "publishes": [{"key","created","site","project_id","task_id","path","name","version_number",
        "published_file_type","depends_on","entity_id","attempts","last_attempt","error"}]}."""
        records = self.queue.records() if self.queue is not None else []
        return {
            "queue": self.queue.directory if self.queue is not None else None,
            "count": len(records),
            "publishes": [{f: r.get(f) for f in self._PENDING_FIELDS} for r in records],
        }

    def op_replay_publishes(self, args):
        """Enregistre dans ShotGrid les publishes en file d'attente d'un projet : project_id, sinon celui de
        l'instance Toolkit s'il en a, sinon celui du plus ancien. Un projet par appel (un processus, un projet
        Toolkit) : "projects" liste les autres projets qui en ont. Seuls ceux du site de la session sont rejoués.
        Dans l'ordre de la file : un parent (le projet) avant ses enfants (ses textures), qui en dépendent. Un publish
        déjà créé (avant l'échec, ou réponse perdue) n'est pas recréé : il est complété (vignette, dépendances). Un
        échec réseau arrête le rejeu ("stopped" : sa raison) ; un autre échec laisse le publish en file ("failed") et
        le rejeu continue. Retourne {"project_id", "replayed": [{"key","id","path","name","version_number"}],
        "failed": [{"key","path","error"}], "stopped", "pending" (restants sur ce site, tous projets), "projects",
        "other_sites" (en file pour un autre site)}."""
        project_id = _int(args, "project_id", required=False)
        result = {"project_id": None, "replayed": [], "failed": [], "stopped": None, "pending": 0, "projects": [],
                  "other_sites": 0}
        if self.queue is None:
            return result
        site = _site_key(self._site())
        records = [r for r in self.queue.records() if _site_key(r.get("site")) == site]
        if project_id is None:
            ids = [r.get("project_id") for r in records if r.get("project_id")]
            project_id = self.tk_project_id if self.tk_project_id in ids else (ids[0] if ids else None)
        mine = [r["key"] for r in records if r.get("project_id") == project_id]
        if mine:
            result["project_id"] = project_id
            self._replay(self.tk_for_project(project_id), mine, result)
        rest = self.queue.records()
        here = [r for r in rest if _site_key(r.get("site")) == site]
        result["pending"] = len(here)
        result["other_sites"] = len(rest) - len(here)
        result["projects"] = sorted({r["project_id"] for r in here if r.get("project_id") not in (None, project_id)})
        return result

    def _replay(self, tk, keys, result):
        for key in keys:
            record = self.queue.load(key)  # relu : un parent rejoué a réécrit ses dépendances
            if record is None:
                continue
            waiting = [k for k in record.get("depends_on") or [] if self.queue.load(k) is not None]
            if waiting:
                result["failed"].append({"key": key, "path": record.get("path"),
                                         "error": "waiting for its parent publish %s" % ", ".join(waiting)})
                continue
            if record.get("depends_on"):
                log.warning("publish queue: %s replayed without its discarded parent %s", key, record["depends_on"])
            record["attempts"] = (record.get("attempts") or 0) + 1
            record["last_attempt"] = studio_sg_queue.iso(studio_sg_queue.utc_now())
            try:
                publish = self._replay_one(tk, record)
            except Exception as e:  # noqa: BLE001 — gardé en file, avec l'erreur
                entity = getattr(e, "entity", None)
                if entity:
                    record["entity_id"] = entity["id"]
                record["error"] = str(e) or type(e).__name__
                self.queue.save(record)
                if unreachable(e):
                    studio_sg_bridge.network(False, record["error"])
                    log.warning("publish queue: ShotGrid unreachable, replay stopped at %s: %s", key, record["error"])
                    result["stopped"] = record["error"]
                    return
                log.error("publish queue: %s not replayed: %s", key, record["error"])
                result["failed"].append({"key": key, "path": record.get("path"), "error": record["error"]})
                self._audit_record("publish_failed", record, key=key, error=record["error"], replay=True)
                continue
            record["entity_id"] = publish["id"]
            self.queue.save(record)  # interrompu d'ici à remove(), le rejeu suivant le retrouve par entity_id
            for child in self.queue.records():
                if key in (child.get("depends_on") or []):
                    child["depends_on"] = [k for k in child["depends_on"] if k != key]
                    child["dependency_ids"] = list(child.get("dependency_ids") or []) + [publish["id"]]
                    self.queue.save(child)
            self.queue.remove(record)
            log.info("publish queue: %s registered as PublishedFile %d", key, publish["id"])
            result["replayed"].append({"key": key, "id": publish["id"], "path": record.get("path"),
                                       "name": record.get("name"), "version_number": record.get("version_number")})
            self._audit_record("publish_replayed", record, key=key, id=publish["id"], queued_at=record.get("created"))

    def _replay_one(self, tk, record):
        context = self._replay_context(tk, record)
        kwargs = dict(record.get("kwargs") or {})
        thumbnail = self.queue.thumbnail_path(record)
        ids = list(record.get("dependency_ids") or [])
        existing = self._published(tk, record)
        if existing is not None:
            log.info("publish queue: %s already in ShotGrid (PublishedFile %d), completed", record["key"], existing["id"])
            self._complete_publish(tk, context, existing, thumbnail, ids, kwargs)
            return existing
        register = self.sgtk.util.register_publish
        register = getattr(register, "_studio_original", register)  # pas d'audit "publish" : rejoué, pas publié
        kwargs.update(comment=record.get("comment") or "", published_file_type=record.get("published_file_type"),
                      thumbnail_path=thumbnail, dependency_ids=ids)
        created_at = studio_sg_queue.parse_iso(record.get("created_at") or record.get("created"))
        call = (tk, context, record["path"], record["name"], record["version_number"])
        try:
            return register(*call, created_at=created_at, **kwargs)
        except Exception as e:
            if record.get("created_at") or created_at is None or unreachable(e) or getattr(e, "entity", None):
                raise
            # La date d'origine refusée (droits sur le site) : enregistré à l'heure du rejeu.
            log.warning("publish queue: %s: created_at refused (%s), registered at the current time", record["key"], e)
            return register(*call, **kwargs)

    def _replay_context(self, tk, record):
        data = record.get("context")
        if isinstance(data, dict) and data.get("project"):
            try:
                return self.sgtk.Context.from_dict(tk, data)
            except Exception as e:  # noqa: BLE001 — relu depuis ShotGrid ci-dessous
                log.warning("publish queue: %s: context unreadable (%s)", record["key"], e)
        if record.get("task_id"):
            return tk.context_from_entity("Task", record["task_id"])
        return tk.context_from_entity("Project", record["project_id"])

    @staticmethod
    def _published(tk, record):
        """Le PublishedFile du publish s'il existe déjà : créé avant l'échec (entity_id), ou dont la réponse s'est
        perdue (même projet, tâche, nom, version et fichier)."""
        sg = tk.shotgun
        fields = ["code", "name", "version_number", "path"]
        if record.get("entity_id"):
            found = sg.find_one("PublishedFile", [["id", "is", record["entity_id"]]], fields)
            if found is not None:
                return found
        if not record.get("project_id"):
            return None
        task = {"type": "Task", "id": record["task_id"]} if record.get("task_id") else None
        filters = [
            ["project", "is", {"type": "Project", "id": record["project_id"]}],
            ["task", "is", task],
            ["name", "is", record["name"]],
            ["version_number", "is", record["version_number"]],
        ]
        target = os.path.normcase(os.path.normpath(record["path"]))
        for found in sg.find("PublishedFile", filters, fields):
            path = found.get("path")
            local = path.get("local_path") if isinstance(path, dict) else None
            if local and os.path.normcase(os.path.normpath(local)) == target:
                return found
            if not local and found.get("code") == os.path.basename(record["path"]):
                return found
        return None

    def _complete_publish(self, tk, context, entity, thumbnail, ids, kwargs):
        # Ce que register_publish fait après la création (vignettes, dépendances), sans doubler l'existant.
        sg = tk.shotgun
        if thumbnail:
            sg.upload_thumbnail("PublishedFile", entity["id"], thumbnail)
            if kwargs.get("update_entity_thumbnail") and context.entity:
                sg.upload_thumbnail(context.entity["type"], context.entity["id"], thumbnail)
            if kwargs.get("update_task_thumbnail") and context.task:
                sg.upload_thumbnail("Task", context.task["id"], thumbnail)
        wanted = list(ids)
        paths = kwargs.get("dependency_paths") or []
        if paths:
            wanted += [p["id"] for p in self.sgtk.util.find_publish(tk, paths).values()]
        link = {"type": "PublishedFile", "id": entity["id"]}
        have = {
            (d.get("dependent_published_file") or {}).get("id")
            for d in sg.find("PublishedFileDependency", [["published_file", "is", link]], ["dependent_published_file"])
        }
        batch = [
            {
                "request_type": "create",
                "entity_type": "PublishedFileDependency",
                "data": {"published_file": link, "dependent_published_file": {"type": "PublishedFile", "id": i}},
            }
            for i in dict.fromkeys(wanted)
            if i not in have
        ]
        if batch:
            sg.batch(batch)

    def op_discard_publishes(self, args):
        """Écarte des publishes de la file d'attente (keys) : jamais rejoués, leurs fichiers vont dans discarded/ du
        dossier de la file. Leurs enfants encore en file seront enregistrés sans dépendre d'eux. Retourne
        {"discarded", "missing"} (clés)."""
        keys = args.get("keys")
        if not isinstance(keys, list) or not keys or not all(studio_sg_queue.valid_key(k) for k in keys):
            raise BridgeError("bad_request", "keys must be a non-empty list of publish queue keys")
        discarded, missing = [], []
        for key in keys:
            record = self.queue.load(key) if self.queue is not None else None
            if record is None:
                missing.append(key)
                continue
            self.queue.discard(record)
            discarded.append(key)
            log.info("publish queue: %s discarded (%s)", key, record.get("path"))
            self._audit_record("publish_discarded", record, key=key)
        return {"discarded": discarded, "missing": missing}
