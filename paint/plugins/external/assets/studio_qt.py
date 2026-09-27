#!/usr/bin/env python3
# Socle commun des fenêtres Qt du studio (export du lot B6, subzero du chantier E) : processus lancé par ArmorPaint
# (studio_window.c), protocole d'une ligne JSON par message sur stdin et stdout, application macOS d'arrière-plan, thème
# sombre, échelle de l'interface d'ArmorPaint. Python 3.9+ et PySide6.
#
# stdout et stdin sont réservés au protocole ; logs et erreurs partent sur stderr, que ArmorPaint recopie dans son log.

import ctypes
import ctypes.util
import json
import os
import sys
import threading

from PySide6 import QtCore, QtGui, QtWidgets

ACCENT = "#2f6fb0"
MUTED = "#8c8c8c"
ERROR = "#e5534b"
WARNING = "#d9a441"
SUCCESS = "#57ab5a"


def log(message):
    sys.stderr.write("%s\n" % message)
    sys.stderr.flush()


# ---- Protocole ---------------------------------------------------------------------------------------------------------


def claim_stdio():
    # Copies privées de stdin/stdout pour le protocole ; les descripteurs 0 et 1 du processus vont sur /dev/null et
    # stderr : un print() égaré ou une bibliothèque bavarde ne corrompt pas le protocole.
    proto_in = os.fdopen(os.dup(0), "rb", buffering=0)
    proto_out = os.fdopen(os.dup(1), "w", encoding="ascii", newline="\n")
    null = os.open(os.devnull, os.O_RDONLY)
    os.dup2(null, 0)
    os.close(null)
    os.dup2(2, 1)
    sys.stdin = open(os.devnull, "r")
    sys.stdout = sys.stderr
    return proto_in, proto_out


class Channel(QtCore.QObject):
    """Requêtes vers ArmorPaint ; les réponses et évènements arrivent par un thread de lecture, remis au thread Qt."""

    received = QtCore.Signal(object)
    closed = QtCore.Signal()

    def __init__(self, proto_in, proto_out):
        super().__init__()
        self._out = proto_out
        self._next = 0
        self._pending = {}
        self.events = {}
        self.received.connect(self._dispatch)
        threading.Thread(target=self._read, args=(proto_in,), daemon=True).start()

    def _read(self, proto_in):
        buf = b""
        while True:
            chunk = proto_in.read(65536)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    self.received.emit(json.loads(line.decode("utf-8", "replace")))
                except ValueError:
                    log("ignored: %.200s" % line)
        self.closed.emit()

    def request(self, op, args, callback):
        self._next += 1
        self._pending[self._next] = callback
        message = {"id": self._next, "op": op, "args": args or {}}
        self._out.write(json.dumps(message, ensure_ascii=True, separators=(",", ":")) + "\n")
        self._out.flush()

    def _dispatch(self, message):
        if not isinstance(message, dict):
            return
        if "event" in message:
            handler = self.events.get(message["event"])
            if handler is not None:
                handler(message)
            return
        callback = self._pending.pop(message.get("id"), None)
        if callback is None:
            return
        if message.get("ok"):
            callback(message.get("data"), None)
        else:
            callback(None, message.get("error") or {"code": "error", "message": "no message"})


# ---- macOS --------------------------------------------------------------------------------------------------------------


class MacApp(object):
    """NSApp du processus, par le runtime Objective-C : application d'arrière-plan (ni Dock ni Cmd+Tab), qui rend le
    premier plan à ArmorPaint en se masquant. Même montage que le moteur tk-armorpaint."""

    def __init__(self):
        objc = ctypes.cdll.LoadLibrary(ctypes.util.find_library("objc"))
        objc.objc_getClass.restype = ctypes.c_void_p
        objc.sel_registerName.restype = ctypes.c_void_p
        send = ctypes.cast(objc.objc_msgSend, ctypes.c_void_p).value
        self._objc = objc
        self._send_ptr = send
        self._sel = objc.sel_registerName
        self._send_id = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)(send)
        self._send_long = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_long)(send)
        self._send_obj = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)(send)
        self._send_bool = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_bool)(send)
        self._app = self._send_id(objc.objc_getClass(b"NSApplication"), self._sel(b"sharedApplication"))

    def make_accessory(self):
        self._send_long(self._app, self._sel(b"setActivationPolicy:"), 1)  # NSApplicationActivationPolicyAccessory

    def activate(self):
        self._send_bool(self._app, self._sel(b"activateIgnoringOtherApps:"), True)

    def hide(self):
        self._send_obj(self._app, self._sel(b"hide:"), None)

    def backing_scale(self):
        # [[NSScreen mainScreen] backingScaleFactor] : l'écran de la fenêtre active, celle d'ArmorPaint.
        screen = self._send_id(self._objc.objc_getClass(b"NSScreen"), self._sel(b"mainScreen"))
        if not screen:
            return 1.0
        send = ctypes.CFUNCTYPE(ctypes.c_double, ctypes.c_void_p, ctypes.c_void_p)(self._send_ptr)
        return float(send(screen, self._sel(b"backingScaleFactor")))


# ---- Présentation -------------------------------------------------------------------------------------------------------


def apply_look(app):
    app.setStyle("Fusion")
    p = QtGui.QPalette()
    colors = {
        QtGui.QPalette.Window: "#232323",
        QtGui.QPalette.WindowText: "#dcdcdc",
        QtGui.QPalette.Base: "#1b1b1b",
        QtGui.QPalette.AlternateBase: "#262626",
        QtGui.QPalette.Text: "#dcdcdc",
        QtGui.QPalette.Button: "#2e2e2e",
        QtGui.QPalette.ButtonText: "#dcdcdc",
        QtGui.QPalette.Highlight: ACCENT,
        QtGui.QPalette.HighlightedText: "#ffffff",
        QtGui.QPalette.ToolTipBase: "#2e2e2e",
        QtGui.QPalette.ToolTipText: "#dcdcdc",
        QtGui.QPalette.PlaceholderText: "#6e6e6e",
    }
    for role, color in colors.items():
        p.setColor(role, QtGui.QColor(color))
    for role in (QtGui.QPalette.WindowText, QtGui.QPalette.Text, QtGui.QPalette.ButtonText):
        p.setColor(QtGui.QPalette.Disabled, role, QtGui.QColor("#666666"))
    app.setPalette(p)
    app.setStyleSheet(
        """
        QComboBox, QLineEdit { padding: 4px 8px; border: 1px solid #3a3a3a; border-radius: 4px; background: #1b1b1b; }
        QComboBox:focus, QLineEdit:focus { border-color: %(accent)s; }
        QComboBox:disabled, QLineEdit:disabled { color: #666666; }
        QPushButton { padding: 5px 16px; border: 1px solid #3a3a3a; border-radius: 4px; background: #2e2e2e; }
        QPushButton:hover { background: #363636; }
        QPushButton:disabled { color: #666666; background: #262626; }
        QPushButton#primary { background: %(accent)s; border-color: %(accent)s; color: white; font-weight: 600; }
        QPushButton#primary:hover { background: #3a7cc0; }
        QPushButton#primary:disabled { background: #2a3a4a; border-color: #2a3a4a; color: #8a9aaa; }
        QTreeWidget { border: 1px solid #333333; border-radius: 4px; }
        QHeaderView::section { background: #262626; color: %(muted)s; border: none; padding: 4px 6px; }
        QLabel#muted { color: %(muted)s; }
        QLabel#tag { color: %(muted)s; border: 1px solid #3a3a3a; border-radius: 9px; padding: 1px 8px; }
        QLabel#section { color: %(muted)s; font-weight: 600; }
        QPushButton#link { padding: 2px 6px; border: none; background: transparent; color: %(accent)s; }
        QPushButton#link:hover { text-decoration: underline; background: transparent; }
        QPushButton#link:disabled { color: #555555; }
        QFrame#issues { background: #1f1f1f; border: 1px solid #333333; border-radius: 4px; }
        QFrame#issues QScrollArea, QFrame#issues QLabel { background: transparent; border: none; }
        """
        % {"accent": ACCENT, "muted": MUTED}
    )


def muted(text=""):
    label = QtWidgets.QLabel(text)
    label.setObjectName("muted")
    label.setWordWrap(True)
    return label


def ui_scale(mac):
    """Facteur Qt pour suivre l'échelle d'ArmorPaint (STUDIO_UI_SCALE, en pixels physiques) : Qt compte en points, et
    applique déjà la densité de l'écran (Retina : 2)."""
    try:
        scale = float(os.environ.get("STUDIO_UI_SCALE", "0"))
    except ValueError:
        return None
    if scale <= 0 or os.environ.get("QT_SCALE_FACTOR"):
        return None
    density = mac.backing_scale() if mac is not None else 1.0
    factor = scale / density if density > 0 else scale
    return factor if abs(factor - 1.0) > 0.05 else None


# ---- Démarrage ----------------------------------------------------------------------------------------------------------


def start(name):
    """Prend le protocole, crée l'application Qt (échelle d'ArmorPaint, arrière-plan sous macOS, thème) et le canal.
    Rend (app, mac, channel) ; mac vaut None hors macOS, en offscreen, ou sans accès à NSApplication."""
    proto_in, proto_out = claim_stdio()
    mac = None
    if sys.platform == "darwin" and os.environ.get("QT_QPA_PLATFORM") != "offscreen":
        try:
            mac = MacApp()
        except Exception as e:  # noqa: BLE001
            log("no access to NSApplication: %s" % e)
    factor = ui_scale(mac)
    if factor is not None:
        os.environ["QT_SCALE_FACTOR"] = "%.3f" % factor  # avant QApplication
    app = QtWidgets.QApplication([sys.argv[0]])
    app.setApplicationName(name)
    app.setQuitOnLastWindowClosed(False)
    if mac is not None:
        try:
            mac.make_accessory()
        except Exception as e:  # noqa: BLE001
            log("could not make the window a background application: %s" % e)
            mac = None
    apply_look(app)
    return app, mac, Channel(proto_in, proto_out)
