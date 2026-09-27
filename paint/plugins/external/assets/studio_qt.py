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


# Thème Painter (fenêtre subzero) : panneaux gris #333333, barres #2b2b2b, titres de section en capitales, curseurs
# fins à poignée en anneau, chevrons fins. Couleurs relevées sur le panneau Shader Settings de Painter. Les images
# des sous-contrôles (flèche des menus, coche) sont des SVG écrits au démarrage dans un dossier temporaire : une
# feuille de style Qt ne les prend que par fichier.
PAINTER_ACCENT = "#378ef0"
PAINTER_ICONS = {
    "chevron_down": '<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10" viewBox="0 0 10 10">'
                    '<path d="M1.5 3.5l3.5 3.5 3.5-3.5" fill="none" stroke="%s" stroke-width="1.2"/></svg>',
    "chevron_right": '<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10" viewBox="0 0 10 10">'
                     '<path d="M3.5 1.5l3.5 3.5-3.5 3.5" fill="none" stroke="%s" stroke-width="1.2"/></svg>',
    "check": '<svg xmlns="http://www.w3.org/2000/svg" width="12" height="12" viewBox="0 0 12 12">'
             '<path d="M2.6 6.3l2.3 2.3 4.5-5" fill="none" stroke="%s" stroke-width="1.8"/></svg>',
}
_painter_icons = None  # QTemporaryDir des SVG, vivant autant que l'application


def painter_icon(name, color="#9a9a9a"):
    """Chemin d'une image SVG du thème Painter (écrite à la première demande)."""
    global _painter_icons
    if _painter_icons is None:
        _painter_icons = QtCore.QTemporaryDir()
    path = os.path.join(_painter_icons.path(), "%s_%s.svg" % (name, color.lstrip("#")))
    if not os.path.isfile(path):
        with open(path, "w") as f:
            f.write(PAINTER_ICONS[name] % color)
    return path.replace("\\", "/")


def apply_painter_look(app):
    app.setStyle("Fusion")
    QtCore.QLocale.setDefault(QtCore.QLocale.c())  # 0.5 et non 0,5 : nombres de l'interface anglaise, comme Painter
    font = app.font()
    font.setPixelSize(12)  # même taille sous macOS (72 dpi logiques) et Linux (96)
    app.setFont(font)
    p = QtGui.QPalette()
    colors = {
        QtGui.QPalette.Window: "#333333",
        QtGui.QPalette.WindowText: "#c8c8c8",
        QtGui.QPalette.Base: "#252525",
        QtGui.QPalette.AlternateBase: "#2d2d2d",
        QtGui.QPalette.Text: "#d2d2d2",
        QtGui.QPalette.Button: "#2d2d2d",
        QtGui.QPalette.ButtonText: "#d2d2d2",
        QtGui.QPalette.Highlight: PAINTER_ACCENT,
        QtGui.QPalette.HighlightedText: "#ffffff",
        QtGui.QPalette.ToolTipBase: "#252525",
        QtGui.QPalette.ToolTipText: "#d2d2d2",
        QtGui.QPalette.PlaceholderText: "#6e6e6e",
        QtGui.QPalette.Link: PAINTER_ACCENT,
    }
    for role, color in colors.items():
        p.setColor(role, QtGui.QColor(color))
    for role in (QtGui.QPalette.WindowText, QtGui.QPalette.Text, QtGui.QPalette.ButtonText):
        p.setColor(QtGui.QPalette.Disabled, role, QtGui.QColor("#6e6e6e"))
    app.setPalette(p)
    app.setStyleSheet(
        """
        QToolTip { color: #d2d2d2; background: #252525; border: 1px solid #4d4d4d; padding: 3px 5px; }
        QFrame#bar { background: #2b2b2b; }
        QFrame#separator { background: #262626; min-height: 1px; max-height: 1px; border: none; }
        QLabel#title { color: #e6e6e6; font-size: 14px; font-weight: bold; }
        QLabel#section { color: #e6e6e6; font-weight: bold; }
        QLabel#muted { color: %(muted)s; }
        QLabel#caption { color: %(muted)s; font-size: 11px; }
        QLabel#tag { color: %(muted)s; border: 1px solid #4d4d4d; border-radius: 8px; padding: 1px 7px; }
        QLabel#banner { background: #3d3222; color: %(warning)s; border: 1px solid #5a4526; border-radius: 3px;
                        padding: 5px 7px; }

        QPushButton { padding: 3px 12px; min-height: 17px; border: 1px solid #4d4d4d; border-radius: 3px;
                      background: #2d2d2d; }
        QPushButton:hover { background: #383838; border-color: #5c5c5c; }
        QPushButton:pressed { background: #262626; }
        QPushButton:disabled { color: #7a7a7a; background: #3e3e3e; border-color: #3e3e3e; }
        QPushButton#primary { background: %(accent)s; border-color: %(accent)s; color: #ffffff; font-weight: bold; }
        QPushButton#primary:hover { background: #4b9bf3; border-color: #4b9bf3; }
        QPushButton#primary:disabled { background: #3e3e3e; border-color: #3e3e3e; color: #7a7a7a; }
        QPushButton#link { padding: 2px 4px; border: none; background: transparent; color: %(accent)s; }
        QPushButton#link:hover { text-decoration: underline; }

        QLineEdit, QSpinBox, QDoubleSpinBox { background: #252525; border: 1px solid #4d4d4d; border-radius: 3px;
                                              padding: 3px 6px; selection-background-color: %(accent)s; }
        QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-color: %(accent)s; }
        QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled { color: #6e6e6e; border-color: #3e3e3e; }
        QSpinBox#value, QDoubleSpinBox#value { background: transparent; border: 1px solid transparent; padding: 1px 2px; }
        QSpinBox#value:hover, QDoubleSpinBox#value:hover { border-color: #4d4d4d; }
        QSpinBox#value:focus, QDoubleSpinBox#value:focus { background: #252525; border-color: %(accent)s; }

        QComboBox { background: #2d2d2d; border: 1px solid #4d4d4d; border-radius: 3px; padding: 3px 22px 3px 8px; }
        QComboBox:hover { border-color: #5c5c5c; }
        QComboBox:on, QComboBox:focus { border-color: %(accent)s; }
        QComboBox:disabled { color: #6e6e6e; border-color: #3e3e3e; }
        QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: center right; width: 22px;
                               border: none; }
        QComboBox::down-arrow { image: url(%(chevron)s); width: 10px; height: 10px; }
        QComboBox::down-arrow:disabled { image: url(%(chevron_off)s); }
        QComboBox QAbstractItemView { background: #2b2b2b; border: 1px solid #4d4d4d; outline: 0;
                                      selection-background-color: %(accent)s; selection-color: #ffffff; }

        QCheckBox { spacing: 7px; }
        QCheckBox::indicator { width: 12px; height: 12px; border: 1px solid #909090; border-radius: 2px;
                               background: #2b2b2b; }
        QCheckBox::indicator:hover { border-color: #c8c8c8; }
        QCheckBox::indicator:checked { background: #b9b9b9; border-color: #b9b9b9; image: url(%(check)s); }
        QCheckBox::indicator:disabled { border-color: #555555; }
        QCheckBox::indicator:checked:disabled { background: #555555; border-color: #555555; }

        QSlider { min-height: 16px; }
        QSlider::groove:horizontal { height: 2px; background: #4a4a4a; }
        QSlider::sub-page:horizontal { height: 2px; background: #b9b9b9; }
        QSlider::add-page:horizontal { height: 2px; background: #4a4a4a; }
        QSlider::handle:horizontal { width: 9px; height: 9px; margin: -6px 0; border: 2px solid #b9b9b9;
                                     border-radius: 6px; background: #333333; }
        QSlider::handle:horizontal:hover { border-color: #e6e6e6; }
        QSlider::sub-page:horizontal:disabled, QSlider::handle:horizontal:disabled { background: #555555;
                                                                                     border-color: #555555; }

        QToolButton#group { border: none; background: transparent; color: #e6e6e6; font-weight: bold;
                            padding: 2px 0; }
        QToolButton#reset { border: none; background: transparent; color: %(muted)s; padding: 0 2px; }
        QToolButton#reset:hover { color: #e6e6e6; }

        QScrollArea { background: transparent; border: none; }
        QScrollArea > QWidget > QWidget { background: transparent; }
        QScrollBar:vertical { background: #262626; width: 10px; margin: 0; }
        QScrollBar::handle:vertical { background: #4a4a4a; min-height: 24px; margin: 1px; }
        QScrollBar::handle:vertical:hover { background: #5c5c5c; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }

        QTreeWidget { background: #2b2b2b; border: 1px solid #262626; }
        QHeaderView::section { background: #2b2b2b; color: %(muted)s; border: none; border-bottom: 1px solid #262626;
                               padding: 4px 6px; }
        QMenu { background: #2b2b2b; border: 1px solid #4d4d4d; }
        QMenu::item:selected { background: %(accent)s; color: #ffffff; }
        """
        % {"accent": PAINTER_ACCENT, "muted": MUTED, "warning": WARNING, "chevron": painter_icon("chevron_down", "#b0b0b0"),
           "chevron_off": painter_icon("chevron_down", "#5c5c5c"), "check": painter_icon("check", "#2b2b2b")}
    )


def separator():
    line = QtWidgets.QFrame()
    line.setObjectName("separator")
    return line


def section_title(text):
    """Titre de section à la Painter : capitales, gras."""
    label = QtWidgets.QLabel(text.upper())
    label.setObjectName("section")
    return label


class Group(QtWidgets.QWidget):
    """Groupe repliable à la Painter : chevron fin et titre gras en léger retrait sous le titre de section, contenu
    en retrait du chevron (body, un QVBoxLayout)."""

    toggled = QtCore.Signal(bool)
    OFFSET = 6  # chevron
    INDENT = 20  # contenu

    def __init__(self, title, opened=True):
        super().__init__()
        lay = QtWidgets.QVBoxLayout(self)
        lay.setContentsMargins(self.OFFSET, 0, 0, 0)
        lay.setSpacing(6)
        self.head = QtWidgets.QToolButton()
        self.head.setObjectName("group")
        self.head.setCheckable(True)
        self.head.setToolButtonStyle(QtCore.Qt.ToolButtonTextBesideIcon)
        self.head.setIconSize(QtCore.QSize(10, 10))
        self.head.setText(title)
        self.box = QtWidgets.QWidget()
        self.body = QtWidgets.QVBoxLayout(self.box)
        self.body.setContentsMargins(self.INDENT - self.OFFSET, 0, 0, 0)
        self.body.setSpacing(10)
        lay.addWidget(self.head)
        lay.addWidget(self.box)
        self.head.toggled.connect(self._toggled)
        self.head.setChecked(opened)
        self._toggled(opened)

    def _toggled(self, on):
        self.head.setIcon(QtGui.QIcon(painter_icon("chevron_down" if on else "chevron_right")))
        self.box.setVisible(on)
        self.toggled.emit(on)

    def is_open(self):
        return self.head.isChecked()


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


def start(name, painter=False):
    """Prend le protocole, crée l'application Qt (échelle d'ArmorPaint, arrière-plan sous macOS, thème : celui du
    studio, ou celui de Painter) et le canal. Rend (app, mac, channel) ; mac vaut None hors macOS, en offscreen, ou sans
    accès à NSApplication."""
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
    if painter:
        apply_painter_look(app)
    else:
        apply_look(app)
    return app, mac, Channel(proto_in, proto_out)
