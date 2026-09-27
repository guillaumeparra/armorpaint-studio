# Publish du projet ArmorPaint (lot C4), élément armorpaint.session du collecteur. Configuration du studio :
#   hook: "{self}/publish_file.py:{engine}/tk-multi-publish2/basic/publish_session.py"
#   settings: {"Publish Template": armorpaint_asset_publish}
#
# Comme dans les moteurs des DCC : le projet est enregistré, copié à l'emplacement du template de publish (sans
# template, publié en place), enregistré dans ShotGrid, puis ouvert à sa version suivante. La copie est écrite par
# ArmorPaint (requête host publish_copy) : le .arm désigne ses fichiers (textures importées, maillage source, envmap…)
# par des chemins relatifs à son dossier, qu'une copie du fichier perdrait. Type de publish : celui du sidecar
# (STUDIO_SG_PUBLISH_TYPE, "ArmorPaint Project"), que liste la fenêtre File Open.

import os

import sgtk
from sgtk.util.filesystem import ensure_folder_exists

HookBaseClass = sgtk.get_hook_baseclass()


class ArmorPaintSessionPublishPlugin(HookBaseClass):
    @property
    def icon(self):
        return os.path.join(self.parent.disk_location, "hooks", "icons", "publish.png")

    @property
    def description(self):
        return """
        Publishes the ArmorPaint project to Flow Production Tracking. The project is saved first. With a publish
        template, ArmorPaint writes a copy of the project there, its imported files still found from the copy;
        without one, the project is published in place.<br><br>
        After the publish, the project is saved as its next version (<code>robot.v003.arm</code> is published,
        <code>robot.v004.arm</code> is opened). If that version already exists, the validation fails and offers to
        save to the next free version.
        """

    @property
    def settings(self):
        settings = dict(super().settings or {})
        settings["Publish Template"] = {
            "type": "template",
            "default": None,
            "description": "Template of the published projects (templates.yml). Without it, the project is published "
            "in place.",
        }
        return settings

    @property
    def item_filters(self):
        return ["armorpaint.session"]

    def accept(self, settings, item):
        if settings.get("Publish Template").value:
            item.context_change_allowed = False  # le chemin publié vient du contexte du fichier de travail
        if not self._current_path():
            self.logger.warning("The ArmorPaint project has never been saved.", extra=self._save_as_action())
        return {"accepted": True, "checked": True}

    def validate(self, settings, item):
        path = self._current_path()
        if not path:
            message = "The ArmorPaint project has never been saved."
            self.logger.error(message, extra=self._save_as_action())
            raise Exception(message)
        path = sgtk.util.ShotgunPath.normalize(path)
        work_template = item.properties.get("work_template")
        if work_template is not None and not work_template.validate(path):
            self.logger.warning(
                "The project does not match the work file template %s." % (work_template.name,),
                extra=self._save_as_action(),
            )
        next_path, version = self._get_next_version_info(path, item)
        if next_path and os.path.exists(next_path):
            while os.path.exists(next_path):
                next_path, version = self._get_next_version_info(next_path, item)
            message = "The next version of this project already exists on disk."
            self.logger.error(
                message,
                extra={
                    "action_button": {
                        "label": "Save to v%s" % (version,),
                        "tooltip": "Save the project to the next free version, v%s" % (version,),
                        "callback": lambda: self._save_as(next_path),
                    }
                },
            )
            raise Exception(message)
        name = settings.get("Publish Template").value
        publish_template = self.parent.engine.get_template_by_name(name) if name else None
        if name and publish_template is None:
            raise Exception("Publish template '%s' is not defined in the project configuration." % (name,))
        if publish_template is not None:
            item.properties["publish_template"] = publish_template
        item.properties["path"] = path
        item.properties["publish_type"] = self._publish_type()
        return super().validate(settings, item)

    def publish(self, settings, item):
        path = sgtk.util.ShotgunPath.normalize(self._current_path())
        if path != item.properties["path"]:
            raise Exception("The open project is now %s: collect again." % (path,))
        self.parent.engine.host_request("save")
        super().publish(settings, item)

    def finalize(self, settings, item):
        super().finalize(settings, item)
        self._save_to_next_version(item.properties["path"], item, self._save_as)

    def _copy_to_publish(self, settings, item):
        # À la place de la copie du fichier (publish_file.py) : ArmorPaint écrit la copie, voir l'en-tête.
        path = item.properties["path"]
        publish_path = self.get_publish_path(settings, item)
        if not publish_path or os.path.normcase(os.path.normpath(publish_path)) == os.path.normcase(
            os.path.normpath(path)
        ):
            return
        ensure_folder_exists(os.path.dirname(publish_path))
        self.parent.engine.host_request("publish_copy", {"path": publish_path})
        self.logger.info("Copied the project to %s" % (publish_path,))

    # ---- ArmorPaint --------------------------------------------------------------------------------------------------

    def _current_path(self):
        return self.parent.engine.host_request("current_path").get("path") or ""

    def _save_as(self, path):
        ensure_folder_exists(os.path.dirname(path))
        self.parent.engine.host_request("save_as", {"path": path})

    def _publish_type(self):
        session = self.parent.engine.sidecar_session()
        return session.settings["publish_type"] if session is not None else "ArmorPaint Project"

    def _save_as_action(self):
        app = self.parent.engine.apps.get("tk-multi-workfiles2")
        if app is None or not hasattr(app, "show_file_save_dlg"):
            return None
        return {
            "action_button": {
                "label": "Save As...",
                "tooltip": "Save the project in the work area of its task",
                "callback": app.show_file_save_dlg,
            }
        }
