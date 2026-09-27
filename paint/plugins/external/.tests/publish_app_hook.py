# Hook du sidecar (STUDIO_SG_HOOK) pour publish_app.sh : relève ce que le publish a fait sur le site simulé, dans le
# fichier STUDIO_TEST_PUBLISH_RESULT (JSON, écrit à la fin de l'op) :
#   {"result" | "error" | "dialog": ..., "published": [PublishedFile et leurs dépendances], "thumbnails": [...],
#    "versions": [Version], "uploads": [...]}
# op publish : sa réponse, ou son erreur. op show_app "Publish..." : la fenêtre de tk-multi-publish2 est pilotée comme
# par l'artiste (bouton Publish : validation, publish, finalisation), son état final relevé, puis elle est fermée.
# mockgun ignore les vignettes, et le site simulé ne garde pas les médias des Versions : leurs envois sont relevés ici
# (fichier présent, taille du PNG, part de pixels du rouge orangé de la peinture du banc, vue par le viewport).

import json
import os
import struct


def png_size(path):
    try:
        with open(path, "rb") as f:
            head = f.read(24)
        return list(struct.unpack(">II", head[16:24])) if head[:8] == b"\x89PNG\r\n\x1a\n" else None
    except OSError:
        return None


def warm_ratio(path):
    """Part des pixels (un sur 16 de chaque côté) nettement plus rouges que bleus : l'objet peint en orange."""
    try:
        from PySide6.QtGui import QImage
    except ImportError:
        return None
    image = QImage(path)
    if image.isNull():
        return None
    points = [(x, y) for x in range(0, image.width(), 16) for y in range(0, image.height(), 16)]
    warm = 0
    for x, y in points:
        c = image.pixelColor(x, y)
        warm += c.red() - c.blue() > 60
    return round(warm / len(points), 4) if points else None


def shot(path, **extra):
    info = {"path": path, "exists": os.path.isfile(path), "size": png_size(path), "warm": warm_ratio(path)}
    info.update(extra)
    return info


def setup(session):
    out = os.environ.get("STUDIO_TEST_PUBLISH_RESULT")
    if not out:
        return
    thumbnails = []
    uploads = []

    def watch_thumbnails():
        sg = session._connect()
        if getattr(sg, "_studio_thumbnails", False):
            return
        upload_thumbnail, upload = sg.upload_thumbnail, sg.upload

        def watched_upload_thumbnail(entity_type, entity_id, path, **kwargs):
            thumbnails.append(shot(path, type=entity_type, id=entity_id))
            return upload_thumbnail(entity_type, entity_id, path, **kwargs)

        def watched_upload(entity_type, entity_id, path, field_name=None, *args, **kwargs):
            uploads.append(shot(path, type=entity_type, id=entity_id, field=field_name))
            return upload(entity_type, entity_id, path, field_name, *args, **kwargs)

        sg.upload_thumbnail = watched_upload_thumbnail
        sg.upload = watched_upload
        sg._studio_thumbnails = True

    def dump(data):
        sg = session._connect()
        kind = "published_file_type.PublishedFileType.code"
        fields = ["code", "name", "version_number", "path", "description", "task", kind]
        files = sg.find("PublishedFile", [], fields)
        deps = sg.find("PublishedFileDependency", [], ["published_file", "dependent_published_file"])
        for f in files:
            f["type_code"] = f.pop(kind, None)
            f["depends_on"] = sorted(
                d["dependent_published_file"]["id"] for d in deps if d["published_file"]["id"] == f["id"]
            )
        versions = sg.find("Version", [], ["code", "published_files", "entity", "sg_task", "description",
                                           "sg_uploaded_movie", "sg_path_to_movie"])
        data.update({"published": files, "thumbnails": thumbnails, "versions": versions, "uploads": uploads})
        with open(out + ".tmp", "w") as f:
            json.dump(data, f, indent=1, default=str)
        os.replace(out + ".tmp", out)

    publish = session.ops["publish"]

    def publish_and_dump(args):
        watch_thumbnails()
        try:
            result = publish(args)
        except Exception as e:
            dump({"error": {"code": getattr(e, "code", type(e).__name__), "message": str(e)}})
            raise
        dump({"result": result})
        return result

    show_app = session.ops["show_app"]

    def show_app_and_publish(args):
        if args.get("command") != "Publish...":
            return show_app(args)
        watch_thumbnails()
        engine = session._engine_for(args)
        engine._ensure_qt()  # le QApplication, pour le minuteur qui pilote la fenêtre modale
        qt = session.sgtk.platform.qt
        tries = [0]

        def is_publish_dialog(widget):
            # tk-core montre une sous-classe de la fenêtre de l'app (__AppDialog_TkWidgetWrapper__).
            return any(c.__name__ == "AppDialog" for c in type(widget).__mro__)

        def drive():
            dialog = next((w for w in qt.QtGui.QApplication.allWidgets() if is_publish_dialog(w)), None)
            if dialog is None or not dialog.isVisible():
                tries[0] += 1
                if tries[0] < 600:
                    qt.QtCore.QTimer.singleShot(100, drive)
                    return
                # Pas de fenêtre au bout d'une minute : relevé, et les fenêtres ouvertes fermées (fin de show_app).
                tops = qt.QtGui.QApplication.topLevelWidgets()
                dump({"dialog": {"status": "no publish dialog: %s" % [type(w).__name__ for w in tops if w.isVisible()],
                                 "items": [], "published_items": []}})
                for w in tops:
                    w.close()
                return
            tree = dialog._publish_manager.tree
            items = [
                {"name": i.name, "type": i.type_spec, "checked": i.checked,
                 "tasks": [t.name for t in i.tasks if t.active]}
                for i in tree
            ]
            try:
                dialog.do_publish()
                status = dialog._overlay.ui.label.text().replace("\n", " ")
                published = [i.name for i in tree if "sg_publish_data" in i.properties]
                dump({"dialog": {"status": status, "items": items, "published_items": published}})
            except Exception as e:  # noqa: BLE001 — relevé dans le résultat, la fenêtre est fermée quand même
                dump({"dialog": {"status": "hook error: %s" % (e,), "items": items, "published_items": []}})
            dialog.window().close()

        qt.QtCore.QTimer.singleShot(100, drive)
        return show_app(args)

    session.ops["publish"] = publish_and_dump
    session.ops["show_app"] = show_app_and_publish
