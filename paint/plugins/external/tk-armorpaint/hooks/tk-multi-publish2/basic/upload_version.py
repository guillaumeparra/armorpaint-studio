# Version de review du projet ArmorPaint (lot C4), élément armorpaint.session du collecteur. Configuration du studio,
# après le plugin du projet (publish_session.py), dont elle reprend le PublishedFile :
#   hook: "{self}/upload_version.py:{engine}/tk-multi-publish2/basic/upload_version.py"
#   settings: {"Image Size": 0}
#
# L'image de review est une capture du viewport d'ArmorPaint au moment du publish (requête host publish_thumbnail),
# envoyée comme média de la Version (sg_uploaded_movie) ; ShotGrid en tire la vignette. La Version est liée au
# PublishedFile du projet (le plugin du projet décoché : à aucun) et à aucun fichier local : la capture est temporaire,
# effacée après l'envoi. ShotGrid injoignable (publish du projet mis en file d'attente, lot C5) : pas de Version, un
# avertissement, le publish continue ; elle n'est pas rejouée avec la file.

import os
import sys
import tempfile

import sgtk

HookBaseClass = sgtk.get_hook_baseclass()


class ArmorPaintUploadVersionPlugin(HookBaseClass):
    @property
    def icon(self):
        return os.path.join(self.parent.disk_location, "hooks", "icons", "review.png")

    @property
    def description(self):
        return """
        Creates a <b>Version</b> in Flow Production Tracking for review, with an image of the ArmorPaint viewport
        captured at publish time. The Version is linked to the published project, the context entity and the task,
        and can be reviewed on the project's Media page.<br><br>
        When Flow Production Tracking is unreachable (the project publish is queued), no Version is created.
        """

    @property
    def settings(self):
        return {
            "Image Size": {
                "type": "int",
                "default": 0,
                "description": "Longest side of the review image, in pixels. 0: the size of the viewport.",
            },
        }

    @property
    def item_filters(self):
        return ["armorpaint.session"]

    def accept(self, settings, item):
        return {"accepted": True, "checked": True}

    def validate(self, settings, item):
        size = settings["Image Size"].value
        if not isinstance(size, int) or size < 0:
            raise Exception("Image Size must be a number of pixels, or 0 for the size of the viewport: %r" % (size,))
        return super().validate(settings, item)

    def publish(self, settings, item):
        item.properties.pop("sg_version_data", None)
        publish_data = item.properties.get("sg_publish_data") or {}
        name = os.path.splitext(os.path.basename(item.properties.get("path") or item.name))[0]
        offline = publish_data.get("queued") or getattr(sys.modules.get("studio_sg_bridge"), "OFFLINE", None)
        if offline:
            self.logger.warning("ShotGrid unreachable: no review Version for %s (%s)" % (name, offline))
            return
        path = os.path.join(tempfile.gettempdir(), "armorpaint_review_%d.png" % os.getpid())
        image = self.parent.engine.host_request(
            "publish_thumbnail", {"path": path, "size": settings["Image Size"].value}
        )
        data = {
            "project": item.context.project,
            "code": name,
            "description": item.description,
            "entity": item.context.entity or item.context.project,
            "sg_task": item.context.task,
        }
        if publish_data.get("id"):
            data["published_files"] = [{"type": publish_data.get("type", "PublishedFile"), "id": publish_data["id"]}]
        shotgun = self.parent.shotgun
        version = None
        try:
            version = shotgun.create("Version", data)
            shotgun.upload("Version", version["id"], path, "sg_uploaded_movie")
        except Exception as e:
            session = sys.modules.get("studio_sg_session")
            if session is None or not session.unreachable(e):
                raise
            self.logger.warning(
                "ShotGrid unreachable: %s (%s)"
                % ("review Version %s created without its image" % (name,) if version else "no review Version", e)
            )
            return
        finally:
            try:
                os.remove(path)
            except OSError:
                pass
        item.properties["sg_version_data"] = version
        self.logger.info("Review Version %s created (%dx%d)" % (name, image.get("width", 0), image.get("height", 0)))

    def finalize(self, settings, item):
        version = item.properties.get("sg_version_data")
        if version:
            self.logger.info(
                "Review Version uploaded",
                extra={
                    "action_show_in_shotgun": {
                        "label": "Show Version",
                        "tooltip": "Reveal the version in Flow Production Tracking.",
                        "entity": version,
                    }
                },
            )
