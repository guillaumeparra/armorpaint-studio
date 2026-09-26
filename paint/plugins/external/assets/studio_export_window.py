#!/usr/bin/env python3
# Fenêtre d'export de textures du studio (lot B6). Lancée par external/studio_export_window.c à la place de la boîte
# d'export d'ArmorPaint ; protocole décrit dans studio_export_window.h. Python 3.9+ et PySide6.
#
# stdout et stdin sont réservés au protocole (une ligne JSON par message) ; logs et erreurs partent sur stderr, que
# ArmorPaint recopie dans son log. La fenêtre s'arrête sur EOF de stdin (ArmorPaint fermé ou tué).
#
# STUDIO_EXPORT_WINDOW_TEST=<fichier.json> : essai sans intervention (.tests/export_window_app.sh). Le fichier donne
# les choix {"preset","color_space","dir","mode","padding"}, et en option "skip" (fichiers décochés : [[texture,
# variant, udim]...]) et "group" ("map" ou "set") ; la fenêtre les applique, attend le préflight, exporte, écrit
# {"state","preflight","export","summary","files","groups"} dans "result", puis se ferme.

import ctypes
import ctypes.util
import html
import json
import os
import sys
import threading

from PySide6 import QtCore, QtGui, QtWidgets

MODES = ["Visible", "Selected", "Per Object", "Per UDIM Tile"]
PREFLIGHT_DELAY_MS = 250
# Montrés à l'artiste. Les notes (choix du preset : 8 bits, JPG, espace...) restent dans le rapport et le manifeste ;
# les fichiers existants sont en couleur dans la liste et comptés dans le résumé.
SHOWN_LEVELS = ("blocking", "warning")
FILE_KEY_ROLE = QtCore.Qt.UserRole

ACCENT = "#2f6fb0"
MUTED = "#8c8c8c"
ERROR = "#e5534b"
WARNING = "#d9a441"
SUCCESS = "#57ab5a"


def log(message):
    sys.stderr.write("%s\n" % message)
    sys.stderr.flush()


# ---- Protocole ---------------------------------------------------------------------------------------------------------


def _claim_stdio():
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


def preset_label(name):
    return name.replace("_", " ").replace("-", " ").title()


def format_label(f):
    fmt = f.get("format", "").upper()
    bits = f.get("bits", 8)
    if fmt == "EXR" or (fmt == "TIFF" and bits == 32):
        return "%s %s" % (fmt, "half" if bits == 16 else "float")
    return "%s %d-bit" % (fmt, bits)


def size_label(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return ("%d %s" if unit == "B" else "%.1f %s") % (n, unit)
        n /= 1024.0
    return "%d B" % n


def file_key(f):
    """Identité d'un fichier d'une préflight à l'autre (le chemin change avec le dossier) : texture du preset, objet ou
    atlas, tuile UDIM ; celle que studio_export_set_skip attend."""
    return (f.get("texture", ""), f.get("variant", ""), f.get("udim", ""))


def plural(n, word, words=None):
    return "%d %s" % (n, word if n == 1 else (words or word + "s"))


def set_label(f):
    """Jeu de textures d'un fichier : objet ou atlas, tuile."""
    parts = [p for p in (f.get("variant", ""), f.get("udim", "")) if p]
    return "  ·  ".join(parts) if parts else "All objects"


def composition(files, mode):
    """« 9 maps × 7 UDIM tiles » : ce que représentent les fichiers."""
    maps = len({f.get("texture") for f in files})
    tiles = len({f.get("udim") for f in files if f.get("udim")})
    variants = len({f.get("variant") for f in files if f.get("variant")})
    parts = [plural(maps, "map")]
    if variants:
        parts.append(plural(variants, "object" if mode == 2 else "texture set"))
    if tiles:
        parts.append(plural(tiles, "UDIM tile"))
    return " × ".join(parts)


def group_issues(issues):
    """(bloquant, message) ; les constats de même code et même texte sur plusieurs textures (« baseColor: written in
    16 bits... ») tiennent en une ligne. Les bloquants d'abord."""
    groups = {}
    for issue in issues:
        message = issue.get("message", "")
        name, sep, rest = message.partition(": ")
        if not sep or " " in name or not rest:
            name, rest = None, message
        key = (issue.get("level") == "blocking", issue.get("code"), rest)
        groups.setdefault(key, []).append(name)
    out = []
    for (blocking, _, rest), names in groups.items():
        names = [n for n in names if n]
        if len(names) > 1:
            shown = ", ".join(names[:4]) + (" +%d" % (len(names) - 4) if len(names) > 4 else "")
            out.append((blocking, "%s%s (%s)" % (rest[:1].upper(), rest[1:], shown)))
        else:
            out.append((blocking, "%s: %s" % (names[0], rest) if names else rest))
    return sorted(out, key=lambda item: not item[0])


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


# ---- Fenêtre -------------------------------------------------------------------------------------------------------------


class ExportWindow(QtWidgets.QDialog):
    def __init__(self, channel, mac, test=None):
        super().__init__(None, QtCore.Qt.Window | QtCore.Qt.WindowStaysOnTopHint)
        self.channel = channel
        self.mac = mac
        self.test = test
        self.state = None
        self.report = None
        self.busy = False
        self.in_flight = False
        self.dirty = False
        self.exported = None
        self.skipped = set()  # file_key des fichiers décochés, gardés d'une préflight à l'autre
        self.expanded = set()  # groupes ouverts
        self.test_log = {}
        self.setWindowTitle("Export Textures")
        self.resize(760, 740)
        self._build()
        self.timer = QtCore.QTimer(self, singleShot=True, interval=PREFLIGHT_DELAY_MS)
        self.timer.timeout.connect(self.preflight)
        channel.events["raise"] = self._on_raise
        channel.closed.connect(self._on_host_gone)
        self._set_enabled(False)
        self.summary.setText("Loading...")
        channel.request("state", None, self._on_state)

    # -- construction

    def _build(self):
        root = QtWidgets.QVBoxLayout(self)
        root.setContentsMargins(20, 18, 20, 16)
        root.setSpacing(12)

        header = QtWidgets.QHBoxLayout()
        title = QtWidgets.QLabel("Export Textures")
        font = title.font()
        font.setPointSizeF(font.pointSizeF() * 1.45)
        font.setWeight(QtGui.QFont.DemiBold)
        title.setFont(font)
        header.addWidget(title)
        header.addStretch(1)
        tag = QtWidgets.QLabel("ArmorPaint Studio")
        tag.setObjectName("tag")
        header.addWidget(tag, 0, QtCore.Qt.AlignVCenter)
        root.addLayout(header)
        self.project = muted()
        root.addWidget(self.project)

        form = QtWidgets.QFormLayout()
        form.setLabelAlignment(QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)
        form.setHorizontalSpacing(12)
        form.setVerticalSpacing(8)

        self.preset = QtWidgets.QComboBox()
        self.preset.currentIndexChanged.connect(self._on_preset)
        self.description = muted()
        preset_box = QtWidgets.QVBoxLayout()
        preset_box.setSpacing(4)
        preset_box.addWidget(self.preset)
        preset_box.addWidget(self.description)
        form.addRow("Preset", preset_box)

        self.space = QtWidgets.QComboBox()
        self.space.setToolTip(
            "OCIO colour space of the colour textures (base colour, emission...).\n"
            "From preset keeps each texture's own. Data textures (normal, roughness, height...) stay raw."
        )
        self.space.currentIndexChanged.connect(self.schedule)
        self.ocio = muted()
        space_box = QtWidgets.QVBoxLayout()
        space_box.setSpacing(4)
        space_box.addWidget(self.space)
        space_box.addWidget(self.ocio)
        form.addRow("Color space", space_box)

        self.mode = QtWidgets.QComboBox()
        self.mode.addItems(MODES)
        self.mode.currentIndexChanged.connect(self.schedule)
        self.padding = QtWidgets.QCheckBox("Padding")
        self.padding.setToolTip("Dilate the painted areas past the UV islands before exporting (as ArmorPaint's export).")
        layers_box = QtWidgets.QHBoxLayout()
        layers_box.addWidget(self.mode, 1)
        layers_box.addSpacing(12)
        layers_box.addWidget(self.padding)
        form.addRow("Layers", layers_box)

        self.dir = QtWidgets.QLineEdit()
        self.dir.setPlaceholderText("Destination folder")
        self.dir.textChanged.connect(self.schedule)
        browse = QtWidgets.QPushButton("Browse...")
        browse.clicked.connect(self._browse)
        self.browse = browse
        dir_box = QtWidgets.QHBoxLayout()
        dir_box.addWidget(self.dir, 1)
        dir_box.addWidget(browse)
        form.addRow("Destination", dir_box)
        root.addLayout(form)

        files_header = QtWidgets.QHBoxLayout()
        files_title = QtWidgets.QLabel("Files")
        files_title.setObjectName("section")
        files_header.addWidget(files_title)
        self.composition = muted()
        self.composition.setWordWrap(False)
        files_header.addWidget(self.composition, 1)
        self.group_label = muted("Group by")
        self.group_label.setWordWrap(False)
        files_header.addWidget(self.group_label)
        self.group = QtWidgets.QComboBox()
        self.group.addItem("Map", "map")
        self.group.addItem("Object / Tile", "set")
        self.group.currentIndexChanged.connect(lambda *_: self._show_files(self.report))
        files_header.addWidget(self.group)
        self.check_all = QtWidgets.QPushButton("All")
        self.check_all.setObjectName("link")
        self.check_all.clicked.connect(lambda: self._check_all(True))
        files_header.addWidget(self.check_all)
        self.check_none = QtWidgets.QPushButton("None")
        self.check_none.setObjectName("link")
        self.check_none.clicked.connect(lambda: self._check_all(False))
        files_header.addWidget(self.check_none)
        root.addLayout(files_header)
        self.files = QtWidgets.QTreeWidget()
        self.files.setHeaderLabels(["File", "Format", "Color space", "Size"])
        self.files.setRootIsDecorated(False)
        self.files.setAlternatingRowColors(True)
        self.files.setSelectionMode(QtWidgets.QAbstractItemView.NoSelection)
        self.files.setFocusPolicy(QtCore.Qt.NoFocus)
        self.files.setUniformRowHeights(True)
        self.files.itemChanged.connect(self._on_file_checked)
        self.files.itemExpanded.connect(self._remember_expanded)
        self.files.itemCollapsed.connect(self._remember_expanded)
        header_view = self.files.header()
        header_view.setStretchLastSection(False)
        header_view.setSectionResizeMode(0, QtWidgets.QHeaderView.Stretch)
        for column in (1, 2, 3):
            header_view.setSectionResizeMode(column, QtWidgets.QHeaderView.ResizeToContents)
        root.addWidget(self.files, 1)

        self.issues_frame = QtWidgets.QFrame()
        self.issues_frame.setObjectName("issues")
        self.issues = QtWidgets.QLabel()
        self.issues.setWordWrap(True)
        self.issues.setTextFormat(QtCore.Qt.RichText)
        self.issues.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
        scroll = QtWidgets.QScrollArea()
        scroll.setWidget(self.issues)
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        scroll.setSizeAdjustPolicy(QtWidgets.QAbstractScrollArea.AdjustToContents)
        scroll.setSizePolicy(QtWidgets.QSizePolicy.Preferred, QtWidgets.QSizePolicy.Maximum)
        self.issues_scroll = scroll
        self.issues.setAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignTop)
        issues_layout = QtWidgets.QVBoxLayout(self.issues_frame)
        issues_layout.setContentsMargins(10, 8, 10, 8)
        issues_layout.addWidget(scroll)
        self.issues_frame.hide()
        root.addWidget(self.issues_frame)

        footer = QtWidgets.QHBoxLayout()
        self.summary = muted()
        self.summary.setTextFormat(QtCore.Qt.RichText)
        footer.addWidget(self.summary, 1)
        self.reveal = QtWidgets.QPushButton("Open Folder")
        self.reveal.clicked.connect(self._reveal)
        self.reveal.hide()
        footer.addWidget(self.reveal)
        self.cancel = QtWidgets.QPushButton("Cancel")
        self.cancel.clicked.connect(self.close)
        footer.addWidget(self.cancel)
        self.export = QtWidgets.QPushButton("Export")
        self.export.setObjectName("primary")
        self.export.setDefault(True)
        self.export.clicked.connect(self.run_export)
        footer.addWidget(self.export)
        root.addLayout(footer)

    # -- état d'ArmorPaint

    def _on_state(self, data, error):
        if error is not None:
            self.summary.setText('<span style="color:%s">%s</span>' % (ERROR, html.escape(error.get("message", ""))))
            return
        self.state = data
        project = data["project"]
        parts = [os.path.basename(project["path"]) or project["name"]]
        objects, tiles = project.get("objects", []), project.get("tiles", [])
        parts.append(plural(len(objects), "object"))
        if tiles:
            parts.append(plural(len(tiles), "UDIM tile"))
        parts.append("%d × %d" % (project["width"], project["height"]))
        parts.append("%d-bit layers" % project["bits"])
        self.project.setText("  ·  ".join(parts))
        self.project.setToolTip(
            "Objects: %s%s" % (", ".join(objects), "\nUDIM tiles: %s" % ", ".join(tiles) if tiles else "")
        )

        widgets = (self.preset, self.space, self.mode, self.padding, self.dir)
        for w in widgets:
            w.blockSignals(True)
        self.preset.clear()
        presets = sorted(data["presets"], key=lambda p: (p["shipped"], preset_label(p["name"])))
        previous_shipped = None
        for p in presets:
            if previous_shipped is False and p["shipped"]:
                self.preset.insertSeparator(self.preset.count())
            previous_shipped = p["shipped"]
            self.preset.addItem(preset_label(p["name"]), p)
            if not p["valid"]:
                self.preset.model().item(self.preset.count() - 1).setEnabled(False)
        chosen = data["preset"] or ("basic" if any(p["name"] == "basic" for p in presets) else "")
        index = next((i for i in range(self.preset.count()) if (self.preset.itemData(i) or {}).get("name") == chosen), -1)
        if index < 0:
            index = next((i for i in range(self.preset.count()) if (self.preset.itemData(i) or {}).get("valid")), 0)
        self.preset.setCurrentIndex(index)

        self.space.clear()
        self.space.addItem("From preset", "")
        for cs in data["color_spaces"]:
            self.space.addItem(cs, cs)
        current = data["color_space"]
        if current and self.space.findData(current) < 0:
            self.space.addItem("%s (unknown)" % current, current)
        self.space.setCurrentIndex(max(0, self.space.findData(current)))
        ocio = data["ocio"]
        self.ocio.setText("OCIO config: %s" % ocio["config"] if ocio["ready"] else "OCIO unavailable: %s" % ocio["status"])

        self.mode.setCurrentIndex(data["mode"] if 0 <= data["mode"] < len(MODES) else 0)
        self.padding.setChecked(bool(data["padding"]))
        self.dir.setText(data["dir"])
        for w in widgets:
            w.blockSignals(False)
        self._set_enabled(True)
        self.space.setEnabled(ocio["ready"])
        self._on_preset()
        if self.test is not None:
            self._run_test_setup()

    def _on_raise(self, message):
        mode = message.get("mode")
        if isinstance(mode, int) and 0 <= mode < len(MODES) and not self.busy:
            self.mode.setCurrentIndex(mode)
        self.to_front()

    def _on_host_gone(self):
        log("ArmorPaint closed the connection")
        QtWidgets.QApplication.instance().quit()

    # -- choix

    def choices(self):
        preset = self.preset.currentData() or {}
        return {
            "preset": preset.get("name", ""),
            "color_space": self.space.currentData() or "",
            "dir": self.dir.text().strip(),
            "mode": self.mode.currentIndex(),
            "padding": self.padding.isChecked(),
            "skip": [list(k) for k in sorted(self.skipped)],
        }

    def _on_preset(self):
        preset = self.preset.currentData() or {}
        text = preset.get("description", "")
        if not preset.get("shipped", True):
            text += "\n%s" % preset.get("path", "")
        self.description.setText(text)
        self.description.setToolTip(preset.get("path", ""))
        self.schedule()

    def _browse(self):
        start = self.dir.text().strip() or QtCore.QDir.homePath()
        path = QtWidgets.QFileDialog.getExistingDirectory(self, "Destination folder", start)
        if path:
            self.dir.setText(path)

    def _reveal(self):
        if self.exported:
            QtGui.QDesktopServices.openUrl(QtCore.QUrl.fromLocalFile(self.exported))

    def _set_enabled(self, enabled):
        for w in (self.preset, self.space, self.mode, self.padding, self.dir, self.browse, self.files, self.group,
                  self.check_all, self.check_none):
            w.setEnabled(enabled)
        if enabled and self.state is not None:
            self.space.setEnabled(self.state["ocio"]["ready"])
        self._update_export_button()

    def _update_export_button(self):
        blocked = self.report is not None and not self.report.get("ok", True)
        self.export.setEnabled(self.state is not None and not self.busy and bool(self.dir.text().strip()) and not blocked)

    # -- préflight

    def schedule(self, *_):
        if self.state is None:
            return
        self.reveal.hide()
        self.dirty = True
        self._update_export_button()
        self.timer.start()

    def preflight(self):
        if self.state is None or self.busy:
            return
        if self.in_flight:
            self.dirty = True  # relancé à la réponse
            return
        args = self.choices()
        if not args["dir"]:
            self.report = None
            self._show_report(None)
            self.summary.setText("Choose a destination folder.")
            self._update_export_button()
            return
        self.dirty = False
        self.in_flight = True
        self.summary.setText("Checking...")
        self.channel.request("preflight", args, self._on_preflight)

    def _on_preflight(self, data, error):
        self.in_flight = False
        if self.dirty:
            self.timer.start()
            return
        if error is not None:
            self.report = None
            self._show_report(None)
            self.summary.setText('<span style="color:%s">%s</span>' % (ERROR, html.escape(error.get("message", ""))))
        else:
            self.report = data.get("report")
            self._show_report(self.report)
            if self.test is not None and "preflight" not in self.test_log:
                self.test_log["preflight"] = data
                QtCore.QTimer.singleShot(0, self.run_export)
        self._update_export_button()

    def _show_report(self, report):
        self._show_files(report)
        issues = [i for i in (report or {}).get("issues", []) if i.get("level") in SHOWN_LEVELS and i.get("code") != "exists"]
        if issues:
            lines = []
            for blocking, message in group_issues(issues):
                lines.append(
                    '<span style="color:%s">%s</span>&nbsp; %s'
                    % (ERROR if blocking else WARNING, "✕" if blocking else "!", html.escape(message))
                )
            self.issues.setText("<br>".join(lines))
            self._fit_issues(len(lines))
            self.issues_frame.show()
        else:
            self.issues_frame.hide()
        self._update_summary()

    def _fit_issues(self, lines):
        """Hauteur des lignes affichées, 5 au plus (au-delà, défilement)."""
        width = max(100, self.issues_scroll.viewport().width() or self.width() - 60)
        height = self.issues.heightForWidth(width)
        cap = self.fontMetrics().lineSpacing() * 5 + 4
        self.issues_scroll.setFixedHeight(min(cap, height if height > 0 else self.fontMetrics().lineSpacing() * lines))

    # -- liste des fichiers

    def _show_files(self, report):
        """Arbre des fichiers à cocher. Un seul jeu (pas d'objets ni de tuiles) : liste simple ; sinon groupé par map ou
        par objet / tuile, groupes fermés, avec une case pour tout le groupe."""
        tree = self.files
        scroll = tree.verticalScrollBar().value()
        tree.blockSignals(True)
        tree.clear()
        files = (report or {}).get("files", [])
        sets = {(f.get("variant", ""), f.get("udim", "")) for f in files}
        grouped = len(sets) > 1
        for w in (self.group_label, self.group):
            w.setVisible(grouped)
        for w in (self.check_all, self.check_none):
            w.setVisible(bool(files))
        self.composition.setText(composition(files, self.mode.currentIndex()) if files else "")
        tree.setRootIsDecorated(grouped)
        by_map = self.group.currentData() == "map"
        if not grouped:
            for f in files:
                tree.addTopLevelItem(self._file_item(f, os.path.basename(f["path"])))
        else:
            groups = {}
            for f in files:
                key = f.get("texture", "") if by_map else set_label(f)
                groups.setdefault(key, []).append(f)
            for name, members in groups.items():
                if by_map:
                    members.sort(key=lambda f: (f.get("variant", ""), int(f.get("udim") or 0)))
                parent = QtWidgets.QTreeWidgetItem([name, "", "", ""])
                parent.setFlags(QtCore.Qt.ItemIsEnabled | QtCore.Qt.ItemIsUserCheckable | QtCore.Qt.ItemIsAutoTristate)
                font = parent.font(0)
                font.setWeight(QtGui.QFont.DemiBold)
                parent.setFont(0, font)
                parent.setData(0, FILE_KEY_ROLE, ("group", name))
                if by_map:
                    parent.setText(1, format_label(members[0]))
                    parent.setText(2, members[0].get("color_space", ""))
                parent.setTextAlignment(3, QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)
                tree.addTopLevelItem(parent)
                for f in members:
                    parent.addChild(self._file_item(f, os.path.basename(f["path"]), in_group=by_map))
                parent.setExpanded(("group", name) in self.expanded)
        self._refresh_groups()
        tree.blockSignals(False)
        tree.verticalScrollBar().setValue(scroll)

    def _file_item(self, f, label, in_group=False):
        # Dans un groupe par map, format et espace sont ceux du groupe : pas répétés sur chaque ligne.
        item = QtWidgets.QTreeWidgetItem(
            [label, "" if in_group else format_label(f), "" if in_group else f.get("color_space", ""),
             size_label(f.get("size_estimate", 0))]
        )
        item.setFlags(QtCore.Qt.ItemIsEnabled | QtCore.Qt.ItemIsUserCheckable | QtCore.Qt.ItemNeverHasChildren)
        key = file_key(f)
        item.setData(0, FILE_KEY_ROLE, key)
        item.setData(3, FILE_KEY_ROLE, f.get("size_estimate", 0))
        item.setData(1, FILE_KEY_ROLE, bool(f.get("exists")))
        item.setCheckState(0, QtCore.Qt.Unchecked if key in self.skipped else QtCore.Qt.Checked)
        item.setToolTip(0, f["path"] + ("\nReplaces an existing file" if f.get("exists") else ""))
        item.setTextAlignment(3, QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)
        if f.get("exists"):
            item.setForeground(0, QtGui.QBrush(QtGui.QColor(WARNING)))
        return item

    def _leaves(self):
        tree = self.files
        for i in range(tree.topLevelItemCount()):
            top = tree.topLevelItem(i)
            if top.childCount() == 0:
                yield top
            for k in range(top.childCount()):
                yield top.child(k)

    def _refresh_groups(self):
        """Taille cochée et « 5/7 » de chaque groupe."""
        tree = self.files
        for i in range(tree.topLevelItemCount()):
            top = tree.topLevelItem(i)
            n = top.childCount()
            if n == 0:
                continue
            checked = [top.child(k) for k in range(n) if top.child(k).checkState(0) == QtCore.Qt.Checked]
            name = top.data(0, FILE_KEY_ROLE)[1]
            top.setText(0, "%s  (%d of %d)" % (name, len(checked), n) if len(checked) != n else "%s  (%d)" % (name, n))
            top.setText(3, size_label(sum(c.data(3, FILE_KEY_ROLE) or 0 for c in checked)))

    def _remember_expanded(self, item):
        key = item.data(0, FILE_KEY_ROLE)
        if item.isExpanded():
            self.expanded.add(key)
        else:
            self.expanded.discard(key)

    def _on_file_checked(self, item, column):
        if column != 0:
            return
        skipped = set(self.skipped)
        for leaf in self._leaves():
            key = leaf.data(0, FILE_KEY_ROLE)
            if leaf.checkState(0) == QtCore.Qt.Checked:
                skipped.discard(key)
            else:
                skipped.add(key)
        if skipped == self.skipped:
            return
        self.skipped = skipped
        self.files.blockSignals(True)
        self._refresh_groups()
        self.files.blockSignals(False)
        self._update_summary()
        self.schedule()  # existants, place disque : seulement les fichiers cochés

    def _check_all(self, checked):
        self.files.blockSignals(True)
        for leaf in self._leaves():
            leaf.setCheckState(0, QtCore.Qt.Checked if checked else QtCore.Qt.Unchecked)
        self.files.blockSignals(False)
        self._on_file_checked(None, 0)

    def _update_summary(self):
        report = self.report
        if report is None:
            self.summary.setText("")
            return
        issues = report.get("issues", [])
        blocking = [i for i in issues if i.get("level") == "blocking"]
        if blocking:
            only = blocking[0]["message"] if len(blocking) == 1 and blocking[0].get("code") == "no_selection" else None
            self.summary.setText(
                '<span style="color:%s">%s</span>'
                % (ERROR, "No file selected" if only else "Blocked: %s" % plural(len(blocking), "problem"))
            )
            return
        leaves = list(self._leaves())
        chosen = [l for l in leaves if l.checkState(0) == QtCore.Qt.Checked]
        size = sum(l.data(3, FILE_KEY_ROLE) or 0 for l in chosen)
        replaced = sum(1 for l in chosen if l.data(1, FILE_KEY_ROLE))
        text = "%s%s  ·  %s uncompressed" % (
            len(chosen) if len(chosen) == len(leaves) else "%d of %d" % (len(chosen), len(leaves)),
            " file" if len(leaves) == 1 else " files",
            size_label(size),
        )
        if replaced:
            text += '  ·  <span style="color:%s">%s replaced</span>' % (WARNING, plural(replaced, "existing file"))
        self.summary.setText(text)

    # -- export

    def run_export(self):
        if self.busy or not self.export.isEnabled():
            if self.test is not None and "export" not in self.test_log:
                self._finish_test()
            return
        self.timer.stop()
        self.busy = True
        self._set_enabled(False)
        self.export.setText("Exporting...")
        self.summary.setText("Exporting...")
        self.channel.request("export", self.choices(), self._on_export)

    def _on_export(self, data, error):
        self.busy = False
        self.export.setText("Export")
        self._set_enabled(True)
        if error is not None:
            self.summary.setText('<span style="color:%s">%s</span>' % (ERROR, html.escape(error.get("message", ""))))
        elif data.get("ok"):
            report = data.get("report") or {}
            self.exported = report.get("destination")
            count = sum(1 for f in report.get("files", []) if not f.get("skip"))
            self.summary.setText(
                '<span style="color:%s">Exported %s with %s</span>'
                % (SUCCESS, plural(count, "file"), preset_label(report.get("preset", "")))
            )
            self.cancel.setText("Close")
            self.reveal.show()
            self.report = report
        else:
            self.report = data.get("report")
            self._show_report(self.report)
            message = (data.get("error") or "").strip()
            if message:
                self.summary.setText('<span style="color:%s">%s</span>' % (ERROR, html.escape(message.splitlines()[0])))
                self.issues.setText(html.escape(message).replace("\n", "<br>"))
                self._fit_issues(message.count("\n") + 1)
                self.issues_frame.show()
        self._update_export_button()
        if self.test is not None:
            self.test_log["export"] = data if error is None else {"error": error}
            self._finish_test()

    # -- fenêtre

    def to_front(self):
        self.show()
        self.raise_()
        self.activateWindow()
        if self.mac is not None:
            self.mac.activate()

    def closeEvent(self, event):
        if self.busy:
            event.ignore()  # l'export tourne dans ArmorPaint : on attend sa réponse
            return
        super().closeEvent(event)
        if self.mac is not None:
            self.mac.hide()  # macOS rend le premier plan à ArmorPaint
        QtWidgets.QApplication.instance().quit()

    # -- essai sans intervention

    def _run_test_setup(self):
        t = self.test
        self.test_log["state"] = self.state
        i = next((i for i in range(self.preset.count()) if (self.preset.itemData(i) or {}).get("name") == t["preset"]), -1)
        self.preset.setCurrentIndex(i)
        if "color_space" in t:
            self.space.setCurrentIndex(self.space.findData(t["color_space"]))
        self.mode.setCurrentIndex(t.get("mode", 0))
        self.padding.setChecked(t.get("padding", False))
        self.skipped = {tuple(k) for k in t.get("skip", [])}
        self.group.setCurrentIndex(max(0, self.group.findData(t.get("group", "map"))))
        self.dir.setText(t["dir"])

    def _finish_test(self):
        self.test_log["summary"] = self.summary.text()
        self.test_log["files"] = [l.text(0) for l in self._leaves() if l.checkState(0) == QtCore.Qt.Checked]
        self.test_log["skipped"] = [l.text(0) for l in self._leaves() if l.checkState(0) != QtCore.Qt.Checked]
        tree = self.files
        self.test_log["groups"] = [tree.topLevelItem(i).text(0) for i in range(tree.topLevelItemCount())
                                   if tree.topLevelItem(i).childCount() > 0]
        self.test_log["composition"] = self.composition.text()
        self.test_log["project"] = self.project.text()
        self.test_log["issues"] = self.issues.text() if not self.issues_frame.isHidden() else ""
        with open(self.test["result"], "w") as f:
            json.dump(self.test_log, f, indent=1)
        self.close()


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


def main():
    proto_in, proto_out = _claim_stdio()
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
    app.setApplicationName("ArmorPaint Studio Export")
    app.setQuitOnLastWindowClosed(False)
    if mac is not None:
        try:
            mac.make_accessory()
        except Exception as e:  # noqa: BLE001
            log("could not make the window a background application: %s" % e)
            mac = None
    apply_look(app)
    test = None
    if os.environ.get("STUDIO_EXPORT_WINDOW_TEST"):
        with open(os.environ["STUDIO_EXPORT_WINDOW_TEST"]) as f:
            test = json.load(f)
    channel = Channel(proto_in, proto_out)
    window = ExportWindow(channel, mac, test)
    window.to_front()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
