# Collecteur de tk-multi-publish2 pour ArmorPaint (lot C4). Configuration du studio :
#   collector: "{self}/collector.py:{engine}/tk-multi-publish2/basic/collector.py"
#   collector_settings: {"Work Template": armorpaint_asset_work, "Export Preset": <preset studio>}
#
# Éléments collectés :
#   armorpaint.session  le projet ouvert (.arm), vignette du viewport ; plugin publish_session.py
#   armorpaint.texture  sous le projet, une texture telle que l'écrit l'export : une par texture du preset et par
#                       variante (objet, atlas), ou une séquence <UDIM> pour l'export par tuile ; plugin
#                       publish_textures.py, qui exporte au publish, une seule fois, sans les textures décochées
# La collecte n'écrit rien : ArmorPaint fait le préflight de l'export (requête host publish_preflight). Les textures
# sont nommées par le template Toolkit des textures (op texture_pattern du sidecar), comme à l'export.
#
# Réglages de l'export, le premier qui existe :
#   1. le dernier export de la fenêtre d'export pour ce projet (le sidecar du projet, studio_export_choices.py) ;
#   2. l'export courant de la session, celui que refait Ctrl+E ;
#   3. le preset studio du réglage "Export Preset", couches visibles, par tuile si le maillage a des tuiles UDIM.
# Sans aucun, le projet est publié seul. Une erreur de l'export (preset disparu…) est signalée, sans passer au suivant.

import collections
import os
import tempfile

import sgtk

HookBaseClass = sgtk.get_hook_baseclass()

EXPORT_MODE_VISIBLE = 0  # export_mode_t (paint/sources/enums.h)
EXPORT_MODE_PER_UDIM_TILE = 3


def set_thumbnail(item, path):
    """Vignette de l'élément. En image (QPixmap) quand la fenêtre de publish est ouverte : une capture de l'artiste dans
    la fenêtre la remplace alors, alors que tk-multi-publish2 garde toujours une vignette donnée par son chemin."""
    try:
        from sgtk.platform.qt import QtGui

        if QtGui is not None and QtGui.QApplication.instance() is not None:
            pixmap = QtGui.QPixmap(path)
            if not pixmap.isNull():
                item.thumbnail = pixmap
                return
    except Exception:  # noqa: BLE001 — pas de Qt utilisable : par le chemin
        pass
    item.set_thumbnail_from_path(path)


class ArmorPaintSessionCollector(HookBaseClass):
    @property
    def settings(self):
        settings = dict(super().settings or {})
        settings.update(
            {
                "Work Template": {
                    "type": "template",
                    "default": None,
                    "description": "Template of the ArmorPaint work files (templates.yml), for the version to publish "
                    "and the version up after the publish.",
                },
                "Export Preset": {
                    "type": "str",
                    "default": None,
                    "description": "Studio export preset for the textures of a project that was never exported.",
                },
            }
        )
        return settings

    def process_current_session(self, settings, parent_item):
        path = self.parent.engine.host_request("current_path").get("path") or ""
        session_item = self._collect_session(settings, parent_item, path)
        if not path:
            self.logger.warning("The project has never been saved: its textures can be published once it is saved.")
            return
        try:
            self._collect_textures(settings, session_item, path)
        except Exception as e:  # noqa: BLE001 — les textures ne doivent jamais empêcher de publier le projet
            self.logger.error("Textures not collected: %s" % (e,))

    # ---- Projet ------------------------------------------------------------------------------------------------------

    def _collect_session(self, settings, parent_item, path):
        engine = self.parent.engine
        item = parent_item.create_item("armorpaint.session", "ArmorPaint Project", os.path.basename(path) or "Untitled")
        item.set_icon_from_path(os.path.join(engine.disk_location, "icon_256.png"))
        name = self._setting(settings, "Work Template")
        work_template = engine.get_template_by_name(name) if name else None
        if work_template is not None:
            item.properties["work_template"] = work_template
        mismatch = self._context_mismatch(path, item.context) if path else None
        if mismatch:
            item.properties["context_mismatch"] = mismatch  # refusé à la validation, projet et textures
        thumbnail = self._thumbnail()
        if thumbnail:
            set_thumbnail(item, thumbnail)
            item.properties["armorpaint_thumbnail"] = thumbnail
        self.logger.info("Collected the ArmorPaint project: %s" % (path or "never saved",))
        return item

    def _context_mismatch(self, path, context):
        """Message si le projet appartient à une autre entité ou étape que le contexte du publish, None sinon. Ouvert
        autrement que par File Open (menu File d'ArmorPaint, fichiers récents), le projet garde le contexte ShotGrid de
        la dernière fenêtre : ses publishes iraient à la tâche d'un autre élément. Un chemin inconnu du path cache ne
        dit rien : pas de contrôle."""
        try:
            found = self.parent.sgtk.context_from_path(path)
        except Exception as e:  # noqa: BLE001 — TankError : pas de contrôle
            self.logger.debug("Context of %s: %s" % (path, e))
            return None
        for kind in ("entity", "step"):
            mine, theirs = getattr(found, kind), getattr(context, kind)
            if mine and (not theirs or (mine["type"], mine["id"]) != (theirs["type"], theirs["id"])):
                return (
                    "The project belongs to %s, not to the publish context %s: open it with ShotGrid > File Open..., "
                    "which switches to its task." % (found, context)
                )
        return None

    def _thumbnail(self):
        path = os.path.join(tempfile.gettempdir(), "armorpaint_publish_%d.png" % os.getpid())
        try:
            return self.parent.engine.host_request("publish_thumbnail", {"path": path}).get("path")
        except Exception as e:  # noqa: BLE001 — une vignette manquante n'empêche pas de publier
            self.logger.warning("No thumbnail from the ArmorPaint viewport: %s" % (e,))
            return None

    # ---- Textures ----------------------------------------------------------------------------------------------------

    def _collect_textures(self, settings, session_item, path):
        naming, naming_error = self._naming(path)
        found = self._export_settings(settings, path, naming)
        if found is None:
            self.logger.warning(
                "No texture export for this project: export the textures once (File > Export Textures) or set the "
                "Export Preset of the collector. The project is published alone."
            )
            return
        choices, source, reply = found
        export = reply.get("export") or {}
        report = export.get("report")
        if not report:
            self.logger.error("Texture export (%s): %s" % (source, export.get("error") or "no report"))
            return
        # Partagé par les textures (publish_textures.py) : l'export se fait une fois, au publish de la première.
        session_item.properties["armorpaint_textures"] = {
            "path": path,
            "naming": naming,
            "naming_error": naming_error,
            "choices": choices,
            "source": source,
            "report": report,
            "export": None,
        }
        self.logger.info("Textures: preset %s, from %s" % (report.get("preset"), source))
        if naming_error:
            self.logger.error("Texture names: %s" % (naming_error,))
        for issue in report.get("issues") or []:
            level = issue.get("level")
            log = {"blocking": self.logger.error, "warning": self.logger.warning}.get(level, self.logger.info)
            log("Texture export: %s" % (issue.get("message"),))

        groups = collections.OrderedDict()
        for f in report.get("files") or []:
            if not f.get("skip"):
                groups.setdefault((f.get("texture") or "", f.get("variant") or ""), []).append(f)
        version = self._version(session_item, path)
        template = self.parent.sgtk.templates.get(naming["template"]) if naming.get("template") else None
        if template is not None and "version" not in template.keys:
            # Sans {version}, chaque publish des textures aurait le chemin du précédent : pas de template de travail
            # (il rendrait ce conflit bloquant), et publish_textures.py décoche les textures (voir versioned).
            self.logger.warning("The texture template %s has no {version} key." % (template.name,))
            template = None
        for files in groups.values():
            self._collect_texture(session_item, files, version, template)

    def _collect_texture(self, session_item, files, version, template):
        import studio_sg_session  # module du sidecar, à côté de studio_sg_bridge.py

        tiles = [f for f in files if f.get("udim")]
        sequence = files[0].get("sequence") if tiles else None
        path = sequence or files[0]["path"]
        item = session_item.create_item(
            "armorpaint.texture", "UDIM Texture" if sequence else "Texture", os.path.basename(path)
        )
        icons = os.path.join(self.parent.disk_location, "hooks", "icons")
        item.set_icon_from_path(os.path.join(icons, "image_sequence.png" if sequence else "texture.png"))
        item.context_change_allowed = False  # nommée d'après le contexte du fichier de travail
        item.properties["path"] = path
        if sequence:
            item.properties["sequence_paths"] = [f["path"] for f in files]
            item.properties["udims"] = [f["udim"] for f in tiles]
        item.properties["texture"] = files[0].get("texture") or ""
        item.properties["variant"] = files[0].get("variant") or ""
        item.properties["files"] = [
            [f.get("texture") or "", f.get("variant") or "", f.get("udim") or ""] for f in files
        ]
        item.properties["publish_name"] = studio_sg_session.publish_name(path)
        if version is not None:
            item.properties["publish_version"] = version
        if template is not None:
            item.properties["work_template"] = template
        # Un publish ne doit pas être réécrit par l'export suivant : textures nommées par un template avec {version},
        # ou dont le nom porte une version (<file> dans les noms d'ArmorPaint : robot_v003_base.png).
        item.properties["versioned"] = template is not None or studio_sg_session.version_in_name(path) is not None
        thumbnail = session_item.properties.get("armorpaint_thumbnail")
        if thumbnail:
            set_thumbnail(item, thumbnail)
        self.logger.info("Collected the texture: %s" % (path,))
        return item

    def _naming(self, path):
        """Modèle des noms de textures pour path ({"template", "pattern", "pattern_udim"} de l'op texture_pattern ;
        template None : noms d'ArmorPaint) et l'erreur qui l'a empêché (None sinon). Toujours passé à ArmorPaint avec
        l'export : il ne peut pas le demander au sidecar, occupé par la fenêtre de publish."""
        session = self.parent.engine.sidecar_session()
        if session is None or os.environ.get("STUDIO_SG_NAMING") == "0":
            return {"template": None}, None
        try:
            naming = session.op_texture_pattern({"path": path})
        except Exception as e:  # noqa: BLE001 — BridgeError (template_missing…) ou erreur Toolkit
            message = getattr(e, "message", None) or e
            return {"template": None}, "%s (%s)" % (message, getattr(e, "code", type(e).__name__))
        if naming.get("template") is None:
            self.logger.info("Textures keep ArmorPaint names: %s" % (naming.get("reason"),))
        return naming, None

    def _export_settings(self, settings, path, naming):
        """(choix, source, réponse du préflight) des premiers réglages d'export trouvés (voir l'en-tête), None sinon."""
        import studio_export_choices  # module du sidecar, à côté de studio_sg_bridge.py

        choices, error = studio_export_choices.choices_from_sidecar(path)
        if error:
            self.logger.warning("Export settings of the project ignored: %s" % (error,))
        if choices is not None:
            return choices, "the last export of the Export Textures window", self._preflight(naming, choices)
        reply = self._preflight(naming, None)
        if (reply.get("export") or {}).get("code") != "no_preset":
            return None, "the current export of the session (Ctrl+E)", reply
        preset = self._setting(settings, "Export Preset")
        if not preset:
            return None
        choices = {"preset": preset, "dir": os.path.dirname(path), "mode": EXPORT_MODE_VISIBLE}
        reply = self._preflight(naming, choices)
        issues = ((reply.get("export") or {}).get("report") or {}).get("issues") or []
        if any(i.get("code") == "udim_not_per_tile" for i in issues):
            choices["mode"] = EXPORT_MODE_PER_UDIM_TILE
            reply = self._preflight(naming, choices)
        return choices, "the Export Preset of the collector (%s)" % (preset,), reply

    def _preflight(self, naming, choices):
        args = {"naming": naming}
        if choices is not None:
            args["choices"] = choices
        return self.parent.engine.host_request("publish_preflight", args)

    def _version(self, session_item, path):
        work_template = session_item.properties.get("work_template")
        if work_template is not None and work_template.validate(path):
            version = work_template.get_fields(path).get("version")
            if version is not None:
                return version
        import studio_sg_session

        return studio_sg_session.version_in_name(path)

    @staticmethod
    def _setting(settings, name):
        setting = settings.get(name)
        return setting.value if setting is not None else None
