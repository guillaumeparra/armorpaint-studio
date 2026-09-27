# Moteur Toolkit d'ArmorPaint (lots C3 et C4).
#
# ArmorPaint est en C, sans Python ni Qt : le moteur tourne dans le sidecar ShotGrid (assets/studio_sg_bridge.py),
# qui l'amorce comme une intégration DCC (ToolkitManager). C'est le montage des moteurs Adobe (tk-photoshopcc) : les
# fenêtres des apps Toolkit (tk-multi-workfiles2, tk-multi-publish2…) s'ouvrent dans le processus du sidecar, et ce
# qu'elles font sur la scène revient à ArmorPaint par le canal host du protocole (host_request()). Les hooks d'app du
# moteur sont dans hooks/<app>/ ; la configuration du studio les désigne par "{engine}/<app>/<hook>.py".
#
# Les fenêtres sont modales : ArmorPaint les ouvre par la requête show_app, qui dure jusqu'à leur fermeture. Le
# QApplication n'est créé qu'à la première fenêtre (les autres requêtes du sidecar n'affichent rien). Ensuite, le
# sidecar fait tourner sa boucle d'événements entre deux requêtes (studio_sg_bridge.IDLE) : sans elle, macOS déclare
# le processus bloqué et lui retient clavier et souris s'il reçoit le premier plan. Sur macOS, le processus est une
# application d'arrière-plan (ni Dock ni Cmd+Tab), qui rend le premier plan à ArmorPaint à la fermeture d'une fenêtre.

import ctypes
import ctypes.util

import os
import subprocess
import sys
import webbrowser

import sgtk
from sgtk.platform import Engine


class _MacApp(object):
    """NSApp du sidecar, par le runtime Objective-C (pas de PyObjC dans le Python du studio)."""

    def __init__(self):
        objc = ctypes.cdll.LoadLibrary(ctypes.util.find_library("objc"))
        objc.objc_getClass.restype = ctypes.c_void_p
        objc.sel_registerName.restype = ctypes.c_void_p
        send = ctypes.cast(objc.objc_msgSend, ctypes.c_void_p).value
        self._sel = objc.sel_registerName
        self._send_id = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)(send)
        self._send_long = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_long)(send)
        self._send_obj = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)(send)
        self._app = self._send_id(objc.objc_getClass(b"NSApplication"), self._sel(b"sharedApplication"))

    def make_accessory(self):
        return self._send_long(self._app, self._sel(b"setActivationPolicy:"), 1)  # NSApplicationActivationPolicyAccessory

    def hide(self):
        self._send_obj(self._app, self._sel(b"hide:"), None)  # macOS active l'application d'avant : ArmorPaint


class ArmorPaintEngine(Engine):
    def __init__(self, *args, **kwargs):
        self._qt_ready = False
        self._mac = None
        self._modal_depth = 0
        super().__init__(*args, **kwargs)

    # ---- Description ------------------------------------------------------------------------------------------------

    @property
    def context_change_allowed(self):
        return True

    @property
    def host_info(self):
        return {"name": "ArmorPaint", "version": os.environ.get("STUDIO_ARMORPAINT_VERSION", "unknown")}

    @property
    def has_ui(self):
        # Vrai dès que Qt est importable : les apps enregistrent alors leurs commandes au démarrage du moteur, et les
        # fenêtres s'ouvrent à la demande. STUDIO_SG_NO_UI=1 pour un poste sans affichage (ferme).
        if os.environ.get("STUDIO_SG_NO_UI") == "1":
            return False
        from sgtk.platform.qt import QtGui

        return QtGui is not None and hasattr(QtGui, "QApplication")

    def init_engine(self):
        self.logger.debug("%s: init (context %s)", self, self.context)

    # La section contexte du menu ShotGrid, comme dans les moteurs des DCC (sans fenêtre : enregistrées même sans
    # Qt). tk-core vide les commandes à chaque chargement des apps : au démarrage et à chaque changement de contexte.
    def post_app_init(self):
        self._register_context_commands()

    def post_context_change(self, old_context, new_context):
        self._register_context_commands()

    def _register_context_commands(self):
        self.register_command("Jump to Flow Production Tracking", self._jump_to_sg, {"type": "context_menu"})
        self.register_command("Jump to File System", self._jump_to_fs, {"type": "context_menu"})

    def _jump_to_sg(self):
        url = self.context.shotgun_url
        self.logger.info("jump to %s", url)
        webbrowser.open(url)

    def _jump_to_fs(self):
        paths = self.context.filesystem_locations
        if not paths:
            self.logger.warning("no folder for %s (folders not created yet?)", self.context)
        for path in paths:
            self.logger.info("jump to %s", path)
            if sys.platform == "win32":
                os.startfile(path)
            else:
                subprocess.Popen(["open" if sys.platform == "darwin" else "xdg-open", path])

    def context_info(self, ctx=None):
        """Le contexte pour ArmorPaint (titre de la fenêtre, contexte de la prochaine fenêtre, gardé dans le .arm : le
        site dit à quel site ShotGrid appartiennent ces identifiants)."""
        ctx = ctx or self.context
        project, entity, step, task = (ctx.project or {}), (ctx.entity or {}), (ctx.step or {}), (ctx.task or {})
        parts = [p for p in (project.get("name"), entity.get("name"), step.get("name"), task.get("name")) if p]
        return {
            "site": self.sgtk.shotgun_url,
            "project_id": project.get("id"),
            "project": project.get("name"),
            "entity_type": entity.get("type"),
            "entity_id": entity.get("id"),
            "entity": entity.get("name"),
            "step": step.get("name"),
            "task_id": task.get("id"),
            "task": task.get("name"),
            "label": " / ".join(parts),
        }

    # ---- ArmorPaint -------------------------------------------------------------------------------------------------

    def host_request(self, op, args=None):
        """Demande une opération à ArmorPaint et attend sa réponse (data). HostError si ArmorPaint refuse."""
        bridge = sys.modules.get("studio_sg_bridge")
        channel = getattr(bridge, "HOST", None)
        if channel is None:
            raise sgtk.TankError("ArmorPaint is not connected: tk-armorpaint only runs in the ArmorPaint ShotGrid sidecar")
        args = dict(args or {})
        args.setdefault("context", self.context_info())
        return channel.request(op, args)

    def finalize_publish(self, item, logger, finalize):
        """Finalisation ShotGrid d'un publish, finalize (celle de publish_file.py : statut des publishes qu'il
        remplace), en mode dégradé (lot C5, voir studio_sg_session.py) : sautée pour un publish mis en file d'attente,
        et ShotGrid injoignable n'y est qu'un avertissement. Le dit dans logger. Faux si elle n'a pas eu lieu."""
        path = item.properties.get("path") or item.name
        data = item.properties.get("sg_publish_data") or {}
        if data.get("queued"):
            logger.warning(
                "ShotGrid unreachable: %s is queued, it will be registered in ShotGrid when the site is back (%s)"
                % (path, data["queued"])
            )
            return False
        try:
            finalize()
        except Exception as e:
            session = sys.modules.get("studio_sg_session")
            if session is None or not session.unreachable(e):
                raise
            logger.warning(
                "ShotGrid unreachable: %s is published, but the publishes it replaces keep their status (%s)" % (path, e)
            )
            return False
        return True

    def sidecar_session(self):
        """La session du sidecar (studio_sg_session.Session) : ses réglages et ses opérations, pour les hooks (le
        publish nomme les textures par son op texture_pattern). None hors du sidecar."""
        return getattr(sys.modules.get("studio_sg_bridge"), "SESSION", None)

    # ---- Qt ---------------------------------------------------------------------------------------------------------

    def _ensure_qt(self):
        from sgtk.platform.qt import QtGui

        app = QtGui.QApplication.instance()
        if app is None:
            app = QtGui.QApplication([sys.argv[0] if sys.argv else "tk-armorpaint"])
            app.setQuitOnLastWindowClosed(False)
            app.setApplicationName("ShotGrid for ArmorPaint")
        if not self._qt_ready:
            # Même si une app a créé le QApplication avant nous.
            if sys.platform == "darwin":
                try:
                    self._mac = _MacApp()
                    self._mac.make_accessory()
                except Exception as e:  # noqa: BLE001
                    self.logger.warning("could not make the sidecar a background application: %s", e)
                    self._mac = None
            bridge = sys.modules.get("studio_sg_bridge")
            if bridge is not None:
                bridge.IDLE = app.processEvents
            self._initialize_dark_look_and_feel()
            self._qt_ready = True
        return app

    def _create_dialog(self, title, bundle, widget, parent):
        from sgtk.platform.qt import QtCore

        dialog = super()._create_dialog(title, bundle, widget, parent)
        # Fenêtre d'un autre processus qu'ArmorPaint : elle doit passer et rester devant.
        dialog.setWindowFlags(dialog.windowFlags() | QtCore.Qt.WindowStaysOnTopHint)

        def to_front():
            dialog.raise_()
            dialog.activateWindow()

        QtCore.QTimer.singleShot(0, to_front)
        return dialog

    def show_busy(self, title, details):
        # tk-core l'appelle hors de toute fenêtre (création des dossiers d'un next_version lancé par un script) : pas
        # de QApplication pour un simple message, qui passe alors au log. Dans une fenêtre, l'écran d'attente habituel.
        from sgtk.platform.qt import QtGui

        if QtGui is None or QtGui.QApplication.instance() is None:
            self.logger.info("%s: %s", title, details)
            return
        super().show_busy(title, details)

    def show_modal(self, title, bundle, widget_class, *args, **kwargs):
        self._ensure_qt()
        self._modal_depth += 1
        try:
            return super().show_modal(title, bundle, widget_class, *args, **kwargs)
        finally:
            self._modal_depth -= 1
            if self._mac is not None and self._modal_depth == 0:  # pas sous une autre fenêtre encore ouverte
                self._mac.hide()

    def show_dialog(self, title, bundle, widget_class, *args, **kwargs):
        # Modale ici aussi : la requête show_app d'ArmorPaint dure jusqu'à la fermeture de la fenêtre.
        result = self.show_modal(title, bundle, widget_class, *args, **kwargs)
        return result[1] if result else None
