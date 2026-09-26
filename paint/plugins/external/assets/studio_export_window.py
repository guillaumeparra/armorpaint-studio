#!/usr/bin/env python3
# Fenêtre d'export de textures du studio (lot B6). Lancée par external/studio_export_window.c à la place de la boîte
# d'export d'ArmorPaint ; protocole décrit dans studio_export_window.h. Python 3.9+ et PySide6.
#
# stdout et stdin sont réservés au protocole (une ligne JSON par message) ; logs et erreurs partent sur stderr, que
# ArmorPaint recopie dans son log. La fenêtre s'arrête sur EOF de stdin (ArmorPaint fermé ou tué).
#
# STUDIO_EXPORT_WINDOW_TEST=<fichier.json> : essai sans intervention (.tests/export_window_app.sh). Le fichier donne
# les choix {"preset","color_space","dir","mode","padding"} ; la fenêtre les applique, attend le préflight, exporte,
# écrit {"state","preflight","export","summary"} dans "result", puis se ferme.

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

        files_title = QtWidgets.QLabel("Files")
        files_title.setObjectName("section")
        root.addWidget(files_title)
        self.files = QtWidgets.QTreeWidget()
        self.files.setHeaderLabels(["File", "Format", "Color space", "Size"])
        self.files.setRootIsDecorated(False)
        self.files.setAlternatingRowColors(True)
        self.files.setSelectionMode(QtWidgets.QAbstractItemView.NoSelection)
        self.files.setFocusPolicy(QtCore.Qt.NoFocus)
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
        scroll.setMaximumHeight(self.fontMetrics().lineSpacing() * 5 + 4)
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
        bits = project["bits"]
        self.project.setText(
            "%s  ·  %d × %d  ·  %d-bit layers"
            % (os.path.basename(project["path"]) or project["name"], project["width"], project["height"], bits)
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
        for w in (self.preset, self.space, self.mode, self.padding, self.dir, self.browse):
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
        self.files.clear()
        issues = []
        if report is not None:
            for f in report.get("files", []):
                item = QtWidgets.QTreeWidgetItem(
                    [
                        os.path.basename(f["path"]),
                        format_label(f),
                        f.get("color_space", ""),
                        size_label(f.get("size_estimate", 0)),
                    ]
                )
                item.setToolTip(0, f["path"] + ("\nReplaces an existing file" if f.get("exists") else ""))
                item.setTextAlignment(3, QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)
                if f.get("exists"):
                    item.setForeground(0, QtGui.QBrush(QtGui.QColor(WARNING)))
                self.files.addTopLevelItem(item)
            issues = report.get("issues", [])
        if issues:
            lines = []
            for blocking, message in group_issues(issues):
                lines.append(
                    '<span style="color:%s">%s</span>&nbsp; %s'
                    % (ERROR if blocking else WARNING, "✕" if blocking else "!", html.escape(message))
                )
            self.issues.setText("<br>".join(lines))
            self.issues_frame.show()
        else:
            self.issues_frame.hide()
        if report is None:
            self.summary.setText("")
            return
        count = len(report.get("files", []))
        blocking = sum(1 for i in issues if i.get("level") == "blocking")
        if blocking:
            self.summary.setText(
                '<span style="color:%s">Blocked: %d problem%s</span>' % (ERROR, blocking, "s" if blocking > 1 else "")
            )
        else:
            self.summary.setText(
                "%d file%s  ·  %s uncompressed"
                % (count, "s" if count != 1 else "", size_label(report.get("total_size", 0)))
            )

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
            count = len(report.get("files", []))
            self.summary.setText(
                '<span style="color:%s">Exported %d file%s with %s</span>'
                % (SUCCESS, count, "s" if count != 1 else "", preset_label(report.get("preset", "")))
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
        self.dir.setText(t["dir"])

    def _finish_test(self):
        self.test_log["summary"] = self.summary.text()
        self.test_log["files"] = [self.files.topLevelItem(i).text(0) for i in range(self.files.topLevelItemCount())]
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
