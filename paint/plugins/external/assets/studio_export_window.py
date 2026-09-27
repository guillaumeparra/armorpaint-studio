#!/usr/bin/env python3
# Fenêtre d'export de textures du studio (lot B6). Lancée par external/studio_export_window.c à la place de la boîte
# d'export d'ArmorPaint ; protocole décrit dans studio_export_window.h. Python 3.9+ et PySide6.
#
# stdout et stdin sont réservés au protocole (une ligne JSON par message) ; logs et erreurs partent sur stderr, que
# ArmorPaint recopie dans son log. La fenêtre s'arrête sur EOF de stdin (ArmorPaint fermé ou tué). Le socle (protocole,
# application macOS, thème, échelle) est dans studio_qt.py, commun avec la fenêtre subzero.
#
# Sidecar du projet : <dossier du .arm>/<nom sans .arm ni version>.export_settings.json ("robot_v003.arm" ->
# "robot.export_settings.json", commun aux versions), décrit dans studio_export_choices.py. La fenêtre l'écrit (ArmorPaint
# n'y touche pas) : "presets", les presets enregistrés par l'artiste ("Save as preset") ; "export", ses choix, remis à
# l'ouverture (preset, espace, mode, marge, dossier, et par preset : bits et résolution par map, maps désactivées,
# fichiers décochés, modèle de nom des fichiers) ; "texture_template", le template ShotGrid des textures choisi pour le
# projet, écrit dès qu'il change (le sidecar ShotGrid le lit pour nommer les textures). Le publish ShotGrid (lot C4) le
# lit aussi.
# Les jeux de textures (atlas) ne sont pas dans le sidecar : ils changent dans le projet (op "atlases"), enregistrés
# avec le .arm comme ceux de l'onglet Atlases d'ArmorPaint.
# Écrit après un export abouti, un enregistrement de preset et à la fermeture ; pas de sidecar pour un projet jamais
# enregistré. Les presets studio des dossiers (data/, STUDIO_EXPORT_PRESETS) sont seulement lus.
#
# STUDIO_EXPORT_WINDOW_TEST=<fichier.json> : essai sans intervention (.tests/export_window_app.sh). Le fichier donne
# des choix, tous optionnels (absents : ceux du sidecar ou d'ArmorPaint) : "preset", "color_space", "dir", "mode",
# "padding", "skip" (fichiers décochés : [[texture, variant, udim]...]), "group" ("map" ou "set"), "overrides"
# ({map: {"bits", "resolution"}}), "disabled" (maps), "bits_all" et "resolution_all" (appliqués à toutes les maps),
# "naming" (modèle de nom des fichiers), "atlases" ({"names", "objects"} : jeux de textures, envoyés à ArmorPaint avant
# le reste), "save_preset" (nom : enregistré dans le sidecar avant l'export), "no_export" (fermer sans exporter),
# "screenshot" (image PNG de la fenêtre à la fin), "template" (template ShotGrid des textures, choisi avant le reste) et
# "version_up" (true : Version Up avant le reste). La fenêtre les applique, attend le préflight, exporte, écrit
# {"state","restored","preflight","export","summary","files","groups","maps","naming","sets","sg","sg_naming_first",
# "template","version_up"...} dans "result", puis se ferme.

import copy
import html
import json
import os
import re
import sys

from PySide6 import QtCore, QtGui, QtWidgets

import studio_qt
from studio_export_choices import preset_settings, read_sidecar, sidecar_path, write_sidecar
from studio_qt import ACCENT, ERROR, MUTED, SUCCESS, WARNING, log, muted  # noqa: F401

MODES = ["Visible", "Selected", "Per Object", "Per UDIM Tile"]
PREFLIGHT_DELAY_MS = 250
# Montrés à l'artiste. Les notes (choix du preset : 8 bits, JPG, espace...) restent dans le rapport et le manifeste ;
# les fichiers existants sont en couleur dans la liste et comptés dans le résumé.
SHOWN_LEVELS = ("blocking", "warning")
FILE_KEY_ROLE = QtCore.Qt.UserRole
RESOLUTIONS = [8192, 4096, 2048, 1024, 512, 256]
PRESET_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")
MARKER = re.compile(r"<([^<>]*)>")
TILE_OBJECT = re.compile(r"^(.+)\.(1\d{3})$")  # cube.1001 : une tuile UDIM de cube, séparée à l'import
NEW_SET = "__new__"

# ---- Présentation -------------------------------------------------------------------------------------------------------


def preset_label(name):
    return name.replace("_", " ").replace("-", " ").title()


def bits_label(fmt, bits):
    fmt = fmt.upper()
    if fmt == "EXR" or (fmt == "TIFF" and bits == 32):
        return "%d-bit %s" % (bits, "half" if bits == 16 else "float")
    return "%d-bit" % bits


def format_label(f):
    return "%s %s" % (f.get("format", "").upper(), bits_label(f.get("format", ""), f.get("bits", 8)))


def resolution_label(f):
    return "%d × %d" % (f.get("width", 0), f.get("height", 0)) if f.get("width") else ""


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


def naming_problem(pattern, variables):
    """Ce qui cloche dans un modèle de nom (comme studio_naming_check), None s'il est bon."""
    if not pattern.strip():
        return "enter a file name pattern"
    known = {v.lower() for v in variables}
    for m in MARKER.finditer(pattern):
        if m.group(1).lower() not in known:
            return "unknown variable <%s>" % m.group(1)
    return None


def object_groups(objects):
    """[(nom, [indices])] dans l'ordre d'ArmorPaint : les tuiles UDIM d'un objet (cube.1001, cube.1002...) ensemble."""
    groups = {}
    for i, o in enumerate(objects):
        m = TILE_OBJECT.match(o["name"])
        groups.setdefault(m.group(1) if m else o["name"], []).append(i)
    return list(groups.items())


def composition(files, mode):
    """« 9 maps × 7 UDIM tiles » : ce que représentent les fichiers. Par jeu et par tuile (lot D1), chaque jeu sur ses
    tuiles seulement : « 9 maps × 6 texture set tiles (2 sets, 6 UDIM tiles) »."""
    maps = len({f.get("texture") for f in files})
    tiles = len({f.get("udim") for f in files if f.get("udim")})
    variants = len({f.get("variant") for f in files if f.get("variant")})
    pairs = len({(f.get("variant"), f.get("udim")) for f in files if f.get("variant") and f.get("udim")})
    parts = [plural(maps, "map")]
    if variants and tiles and pairs < variants * tiles:
        parts.append("%s (%s, %s)" % (plural(pairs, "texture set tile"), plural(variants, "set"), plural(tiles, "UDIM tile")))
        return " × ".join(parts)
    if variants:
        parts.append(plural(variants, "object" if mode in (1, 2) else "texture set"))
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
        self.presets = []  # presets du menu : ceux des dossiers (lus) et ceux du sidecar du projet
        self.per_preset = {}  # clé du preset -> {"overrides", "disabled", "skip", "naming"}
        self.naming_error = None  # modèle de nom refusé : pas d'export
        self.sets_busy = False  # jeux de textures envoyés, réponse attendue
        self.sg_busy = False  # template ShotGrid ou version suivante demandés, réponse attendue
        self.sidecar = None  # chemin, None sans projet enregistré
        self.sidecar_data = {}
        self.sidecar_saved = None  # contenu tel que lu ou écrit : rien n'est réécrit s'il n'a pas changé
        self.sidecar_error = None
        self.expanded = set()  # groupes ouverts
        self.test_log = {}
        self.setWindowTitle("Export Textures")
        self.resize(860, 880)
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
        self.save_preset = QtWidgets.QPushButton("Save as preset...")
        self.save_preset.setObjectName("link")
        self.save_preset.clicked.connect(lambda: self._save_preset())
        self.delete_preset = QtWidgets.QPushButton("Delete")
        self.delete_preset.setObjectName("link")
        self.delete_preset.setToolTip("Remove this preset from the project")
        self.delete_preset.clicked.connect(self._delete_preset)
        preset_row = QtWidgets.QHBoxLayout()
        preset_row.addWidget(self.preset, 1)
        preset_row.addWidget(self.save_preset)
        preset_row.addWidget(self.delete_preset)
        self.description = muted()
        preset_box = QtWidgets.QVBoxLayout()
        preset_box.setSpacing(4)
        preset_box.addLayout(preset_row)
        preset_box.addWidget(self.description)
        form.addRow("Preset", preset_box)

        self.space = QtWidgets.QComboBox()
        self.space.setToolTip(
            "OCIO colour space of the colour textures (base colour, emission...).\n"
            "From preset keeps each texture's own. Data textures (normal, roughness, height...) stay raw."
        )
        self.space.currentIndexChanged.connect(self._on_space)
        self.ocio = muted()
        space_box = QtWidgets.QVBoxLayout()
        space_box.setSpacing(4)
        space_box.addWidget(self.space)
        space_box.addWidget(self.ocio)
        form.addRow("Color space", space_box)

        self.mode = QtWidgets.QComboBox()
        self.mode.addItems(MODES)
        self.mode.currentIndexChanged.connect(self.schedule)
        self.mode.currentIndexChanged.connect(lambda *_: self._update_sets_note())
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

        # ShotGrid (lot B6) : template Toolkit qui nomme les textures, choisi pour le projet (toutes ses versions), et
        # version du fichier de travail, que Version Up fait passer à la suivante. Seulement sous un template.
        self.sg_template = QtWidgets.QComboBox()
        self.sg_template.setToolTip(
            "Toolkit template that names the textures of this project, kept for all its versions.\n"
            "It gives the folders too: the destination above is not used."
        )
        self.sg_template.currentIndexChanged.connect(self._on_sg_template)
        self.sg_version = muted()
        self.version_up = QtWidgets.QPushButton("Version Up")
        self.version_up.setObjectName("link")
        self.version_up.clicked.connect(lambda: self._version_up())
        sg_row = QtWidgets.QHBoxLayout()
        sg_row.addWidget(self.sg_template, 1)
        sg_row.addSpacing(12)
        sg_row.addWidget(self.sg_version)
        sg_row.addWidget(self.version_up)
        self.sg_note = muted()
        self.sg_note.setTextFormat(QtCore.Qt.RichText)
        self.sg_widget = QtWidgets.QWidget()
        sg_box = QtWidgets.QVBoxLayout(self.sg_widget)
        sg_box.setContentsMargins(0, 0, 0, 0)
        sg_box.setSpacing(4)
        sg_box.addLayout(sg_row)
        sg_box.addWidget(self.sg_note)
        self.sg_label = QtWidgets.QLabel("ShotGrid")
        form.addRow(self.sg_label, self.sg_widget)
        self.sg_label.hide()
        self.sg_widget.hide()

        # Nom des fichiers : le modèle du preset, modifiable pour cet export (gardé par preset dans le sidecar).
        self.naming = QtWidgets.QLineEdit()
        self.naming.setToolTip(
            "File name of each map. <variable> is replaced by its value; a [section] is dropped when a variable in it\n"
            "is empty. A '/' makes folders under the destination."
        )
        self.naming.textEdited.connect(self._on_naming)
        self.variables = QtWidgets.QPushButton("Variables")
        self.variables.setToolTip("Insert a variable at the cursor")
        self.variables.setFocusPolicy(QtCore.Qt.NoFocus)  # la sélection du champ reste, pour [section]
        self.variables.setMenu(QtWidgets.QMenu(self.variables))
        self.reset_naming = QtWidgets.QPushButton("Reset")
        self.reset_naming.setObjectName("link")
        self.reset_naming.setToolTip("Back to the preset's file names")
        self.reset_naming.clicked.connect(self._reset_naming)
        naming_row = QtWidgets.QHBoxLayout()
        naming_row.addWidget(self.naming, 1)
        naming_row.addWidget(self.variables)
        naming_row.addWidget(self.reset_naming)
        self.naming_note = muted()
        self.naming_note.setTextFormat(QtCore.Qt.RichText)
        naming_box = QtWidgets.QVBoxLayout()
        naming_box.setSpacing(4)
        naming_box.addLayout(naming_row)
        naming_box.addWidget(self.naming_note)
        form.addRow("File names", naming_box)
        root.addLayout(form)

        # Jeux de textures (atlas d'ArmorPaint) : seulement quand le projet a plusieurs objets.
        self.sets_section = QtWidgets.QWidget()
        sets_layout = QtWidgets.QVBoxLayout(self.sets_section)
        sets_layout.setContentsMargins(0, 0, 0, 0)
        sets_layout.setSpacing(6)
        sets_header = QtWidgets.QHBoxLayout()
        sets_title = QtWidgets.QLabel("Texture sets")
        sets_title.setObjectName("section")
        sets_header.addWidget(sets_title)
        self.sets_note = muted()
        self.sets_note.setWordWrap(False)
        sets_header.addWidget(self.sets_note, 1)
        self.rename_set = QtWidgets.QPushButton("Rename...")
        self.rename_set.setObjectName("link")
        self.rename_set.setToolTip("Rename a texture set (the name goes into the file names: <atlas>)")
        self.rename_set.clicked.connect(self._on_rename_set)
        sets_header.addWidget(self.rename_set)
        self.merge_sets = QtWidgets.QPushButton("One set")
        self.merge_sets.setObjectName("link")
        self.merge_sets.setToolTip("Put every object back into the first set: one set of maps for all")
        self.merge_sets.clicked.connect(self._merge_sets)
        sets_header.addWidget(self.merge_sets)
        sets_layout.addLayout(sets_header)
        self.sets = QtWidgets.QTreeWidget()
        self.sets.setHeaderLabels(["Object", "Texture set"])
        self.sets.setRootIsDecorated(False)
        self.sets.setAlternatingRowColors(True)
        self.sets.setSelectionMode(QtWidgets.QAbstractItemView.NoSelection)
        self.sets.setFocusPolicy(QtCore.Qt.NoFocus)
        header_view = self.sets.header()
        header_view.setStretchLastSection(False)
        header_view.setSectionResizeMode(0, QtWidgets.QHeaderView.Stretch)
        header_view.setSectionResizeMode(1, QtWidgets.QHeaderView.ResizeToContents)
        sets_layout.addWidget(self.sets)
        self.sets_layers = muted()
        sets_layout.addWidget(self.sets_layers)
        self.sets_section.hide()
        root.addWidget(self.sets_section)

        # Maps : une ligne par texture du preset, avec ses bits et sa résolution pour cet export.
        maps_header = QtWidgets.QHBoxLayout()
        maps_title = QtWidgets.QLabel("Maps")
        maps_title.setObjectName("section")
        maps_header.addWidget(maps_title)
        maps_header.addStretch(1)
        self.bits_all = QtWidgets.QComboBox()
        self.bits_all.setToolTip("Set the bit depth of every map (the closest one its format accepts)")
        self.bits_all.setSizeAdjustPolicy(QtWidgets.QComboBox.AdjustToContents)
        self.bits_all.addItem("Set all bits...", None)
        for bits in (8, 16, 32):
            self.bits_all.addItem("%d-bit" % bits, bits)
        self.bits_all.activated.connect(self._on_bits_all)
        maps_header.addWidget(self.bits_all)
        self.res_all = QtWidgets.QComboBox()
        self.res_all.setToolTip("Set the resolution of every map")
        self.res_all.setSizeAdjustPolicy(QtWidgets.QComboBox.AdjustToContents)
        self.res_all.activated.connect(self._on_resolution_all)
        maps_header.addWidget(self.res_all)
        self.reset_maps = QtWidgets.QPushButton("Reset")
        self.reset_maps.setObjectName("link")
        self.reset_maps.setToolTip("Back to the preset's maps, bit depths and resolutions")
        self.reset_maps.clicked.connect(self._reset_maps)
        maps_header.addWidget(self.reset_maps)
        root.addLayout(maps_header)
        self.maps = QtWidgets.QTreeWidget()
        self.maps.setHeaderLabels(["Map", "Format", "Bits", "Resolution", "Color space", "Size"])
        self.maps.setRootIsDecorated(False)
        self.maps.setAlternatingRowColors(True)
        self.maps.setSelectionMode(QtWidgets.QAbstractItemView.NoSelection)
        self.maps.setFocusPolicy(QtCore.Qt.NoFocus)
        self.maps.itemChanged.connect(self._on_map_checked)
        header_view = self.maps.header()
        header_view.setStretchLastSection(False)
        header_view.setSectionResizeMode(0, QtWidgets.QHeaderView.Stretch)
        for column in (1, 2, 3, 4, 5):
            header_view.setSectionResizeMode(column, QtWidgets.QHeaderView.ResizeToContents)
        root.addWidget(self.maps, 3)

        # Fichiers : seulement quand il y a plusieurs jeux (objets, tuiles UDIM), pour en décocher.
        self.files_section = QtWidgets.QWidget()
        files_layout = QtWidgets.QVBoxLayout(self.files_section)
        files_layout.setContentsMargins(0, 0, 0, 0)
        files_layout.setSpacing(6)
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
        self.group.addItem("Set / Object / Tile", "set")
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
        files_layout.addLayout(files_header)
        self.files = QtWidgets.QTreeWidget()
        self.files.setHeaderLabels(["File", "Format", "Resolution", "Size"])
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
        files_layout.addWidget(self.files, 1)
        self.files_section.hide()
        root.addWidget(self.files_section, 4)

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

    # -- état d'ArmorPaint et sidecar

    def _on_state(self, data, error):
        if error is not None:
            self.summary.setText('<span style="color:%s">%s</span>' % (ERROR, html.escape(error.get("message", ""))))
            return
        if self.sidecar is None and data["project"].get("path"):
            # Premier état : le sidecar est connu ; ses presets sont relus par ArmorPaint (textures, erreurs).
            self.sidecar = sidecar_path(data["project"]["path"])
            self.sidecar_data, self.sidecar_error = read_sidecar(self.sidecar)
            self.sidecar_saved = copy.deepcopy(self.sidecar_data)
            presets = self.sidecar_data.get("presets")
            if isinstance(presets, dict) and presets:
                given = [{"name": n, "preset": p} for n, p in sorted(presets.items()) if isinstance(p, dict)]
                self.channel.request("state", {"presets": given}, self._on_state)
                return
        self._setup(data)

    def _show_project(self):
        project = self.state["project"]
        parts = [os.path.basename(project["path"]) or project["name"]]
        objects, tiles = project.get("objects", []), project.get("tiles", [])
        parts.append(plural(len(objects), "object"))
        if tiles:
            parts.append(plural(len(tiles), "UDIM tile"))
        parts.append("%d × %d" % (project["width"], project["height"]))
        parts.append("%d-bit layers" % project["bits"])
        self.project.setText("  ·  ".join(parts))
        self.project.setToolTip(
            "Objects: %s%s\nExport settings: %s"
            % (", ".join(objects), "\nUDIM tiles: %s" % ", ".join(tiles) if tiles else "",
               self.sidecar or "saved with the project once it is saved")
        )

    def _setup(self, data):
        self.state = data
        self._show_project()

        sidecar_presets = self.sidecar_data.get("presets") if isinstance(self.sidecar_data.get("presets"), dict) else {}
        self.presets = []
        for p in data.get("project_presets", []):
            self.presets.append(dict(p, key="project:" + p["name"], project=True, shipped=False, path=self.sidecar,
                                     json=sidecar_presets.get(p["name"])))
        for p in data["presets"]:
            self.presets.append(dict(p, key="file:" + p["name"], project=False))

        prefs = self.sidecar_data.get("export") if isinstance(self.sidecar_data.get("export"), dict) else None
        restored = self._restore(data, prefs)

        widgets = (self.preset, self.space, self.mode, self.padding, self.dir, self.group)
        for w in widgets:
            w.blockSignals(True)
        self._fill_presets(restored["preset"])
        self.space.clear()
        self.space.addItem("From preset", "")
        for cs in data["color_spaces"]:
            self.space.addItem(cs, cs)
        current = restored["color_space"]
        if current and self.space.findData(current) < 0:
            self.space.addItem("%s (unknown)" % current, current)
        self.space.setCurrentIndex(max(0, self.space.findData(current)))
        ocio = data["ocio"]
        self.ocio.setText("OCIO config: %s" % ocio["config"] if ocio["ready"] else "OCIO unavailable: %s" % ocio["status"])
        mode = restored["mode"]
        self.mode.setCurrentIndex(mode if isinstance(mode, int) and 0 <= mode < len(MODES) else 0)
        self.padding.setChecked(bool(restored["padding"]))
        self.dir.setText(restored["dir"] or "")
        self.group.setCurrentIndex(max(0, self.group.findData(restored.get("group", "map"))))
        for w in widgets:
            w.blockSignals(False)
        self._fill_resolutions()
        self._fill_variables()
        self._show_sg()
        self._set_enabled(True)
        self.space.setEnabled(ocio["ready"])
        self._show_sets()
        self._on_preset()
        if self.sidecar_error:
            log("export settings not read: %s" % self.sidecar_error)
        if self.test is not None:
            self.test_log["restored"] = dict(self.choices(), key=self.preset_key())
            self._run_test_setup()

    def _restore(self, data, prefs):
        """Choix remis à l'ouverture : ceux du sidecar du projet, sinon l'export courant d'ArmorPaint."""
        if prefs is not None:
            for key, value in (prefs.get("presets") or {}).items():
                if isinstance(value, dict):
                    self.per_preset[key] = preset_settings(value)
            return {
                "preset": prefs.get("preset") or "",
                "color_space": prefs.get("color_space") or "",
                "mode": prefs.get("mode", data["mode"]),
                "padding": prefs.get("padding", data["padding"]),
                "dir": prefs.get("dir") or data["dir"],
                "group": prefs.get("group", "map"),
            }
        key = ("project:" if data.get("preset_project") else "file:") + data["preset"] if data["preset"] else ""
        if key:
            skip = {tuple(k) for k in data.get("skip", [])}
            disabled = {k[0] for k in skip if k[1:] == ("*", "*")}
            self.per_preset[key] = {
                "overrides": data.get("overrides") or {},
                "disabled": disabled,
                "skip": {k for k in skip if k[1:] != ("*", "*")},
                "naming": data.get("naming") or None,
            }
        return {"preset": key, "color_space": data["color_space"], "mode": data["mode"], "padding": data["padding"],
                "dir": data["dir"], "group": "map"}

    def _fill_presets(self, chosen):
        """Presets du projet, puis ceux des dossiers de presets, puis ceux livrés avec l'app."""
        self.preset.blockSignals(True)
        self.preset.clear()

        def rank(p):
            return (0 if p["project"] else 2 if p.get("shipped") else 1, preset_label(p["name"]))

        previous = None
        for p in sorted(self.presets, key=rank):
            group = rank(p)[0]
            if previous is not None and group != previous:
                self.preset.insertSeparator(self.preset.count())
            previous = group
            self.preset.addItem(preset_label(p["name"]) + ("  ·  project" if p["project"] else ""), p)
            if not p["valid"]:
                self.preset.model().item(self.preset.count() - 1).setEnabled(False)
        if chosen and ":" not in chosen:
            chosen = "file:" + chosen
        index = self._preset_index(chosen)
        if index < 0 or not (self.preset.itemData(index) or {}).get("valid"):
            index = self._preset_index("file:basic")
        if index < 0:
            index = next((i for i in range(self.preset.count()) if (self.preset.itemData(i) or {}).get("valid")), 0)
        self.preset.setCurrentIndex(index)
        self.preset.blockSignals(False)

    def _preset_index(self, key):
        return next((i for i in range(self.preset.count()) if (self.preset.itemData(i) or {}).get("key") == key), -1)

    def _fill_resolutions(self):
        self.res_all.clear()
        self.res_all.addItem("Set all resolutions...", None)
        self.res_all.addItem("Project (%s)" % self._size_label(0), 0)
        for r in RESOLUTIONS:
            self.res_all.addItem(self._size_label(r), r)

    def _size_label(self, resolution):
        """Taille du fichier pour une résolution (plus grand côté ; 0 : celle du projet), comme ArmorPaint la calcule."""
        project = self.state["project"]
        w, h = project["width"], project["height"]
        longest = max(w, h)
        if resolution and longest and resolution != longest:
            w, h = max(1, int(w * resolution / longest + 0.5)), max(1, int(h * resolution / longest + 0.5))
        return "%d" % w if w == h else "%d × %d" % (w, h)

    def _on_raise(self, message):
        mode = message.get("mode")
        if isinstance(mode, int) and 0 <= mode < len(MODES) and not self.busy:
            self.mode.setCurrentIndex(mode)
        self.to_front()

    def _on_host_gone(self):
        log("ArmorPaint closed the connection")
        self._save_settings()
        QtWidgets.QApplication.instance().quit()

    # -- choix

    def current(self):
        return self.preset.currentData() or {}

    def preset_key(self):
        return self.current().get("key", "")

    def settings(self, key=None):
        key = key or self.preset_key()
        return self.per_preset.setdefault(key, {"overrides": {}, "disabled": set(), "skip": set(), "naming": None})

    def textures(self):
        return self.current().get("textures", [])

    def overrides(self):
        """Réglages qui diffèrent du preset, pour ses textures seulement."""
        out = {}
        stored = self.settings()["overrides"]
        for t in self.textures():
            o = stored.get(t["name"], {})
            diff = {k: o[k] for k in ("bits", "resolution") if k in o and o[k] != t[k]}
            if diff:
                out[t["name"]] = diff
        return out

    def effective(self, t):
        o = self.settings()["overrides"].get(t["name"], {})
        return o.get("bits", t["bits"]), o.get("resolution", t["resolution"])

    def choices(self):
        preset = self.current()
        s = self.settings()
        names = {t["name"] for t in self.textures()}
        disabled = sorted(n for n in s["disabled"] if n in names)
        args = {
            "preset": preset.get("name", ""),
            "color_space": self.space.currentData() or "",
            "dir": self.dir.text().strip(),
            "mode": self.mode.currentIndex(),
            "padding": self.padding.isChecked(),
            "skip": [list(k) for k in sorted(s["skip"]) if k[0] not in s["disabled"]] + [[n, "*", "*"] for n in disabled],
            "overrides": self.overrides(),
        }
        naming = self.naming_override()
        if naming:
            args["naming"] = naming
        if preset.get("project"):
            args["preset_json"] = preset.get("json") or {}
            args["preset_source"] = self.sidecar or ""
        return args

    def _on_preset(self):
        preset = self.current()
        text = preset.get("description", "")
        if preset.get("project"):
            text = (text + "\n" if text else "") + "Saved with the project: %s" % (self.sidecar or "")
        elif not preset.get("shipped", True):
            text += "\n%s" % preset.get("path", "")
        self.description.setText(text)
        self.description.setToolTip(preset.get("path", "") or "")
        self.delete_preset.setVisible(bool(preset.get("project")))
        self._show_maps()
        self._show_naming()
        self.schedule()

    def _on_space(self, *_):
        self._show_maps()
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
                  self.check_all, self.check_none, self.maps, self.bits_all, self.res_all, self.reset_maps,
                  self.save_preset, self.delete_preset, self.naming, self.variables, self.reset_naming, self.sets,
                  self.rename_set, self.merge_sets, self.sg_template, self.version_up):
            w.setEnabled(enabled)
        if enabled and self.state is not None:
            self.space.setEnabled(self.state["ocio"]["ready"])
            template = self.state.get("naming_template") is not None
            for w in (self.naming, self.variables, self.reset_naming):
                w.setEnabled(not template)
            for w in (self.sets, self.rename_set, self.merge_sets):
                w.setEnabled(not self.sets_busy)
            self._update_sg_enabled()
            self.save_preset.setEnabled(self.sidecar is not None)
            self.save_preset.setToolTip(
                "Save these maps, bit depths, resolutions, colour space and file names as a preset of this project"
                if self.sidecar else "Save the project first: its presets are kept next to it"
            )
        self._update_export_button()

    def _update_export_button(self):
        blocked = self.report is not None and not self.report.get("ok", True)
        self.export.setEnabled(self.state is not None and not self.busy and bool(self.dir.text().strip()) and not blocked
                               and self.naming_error is None and not self.sets_busy and not self.sg_busy)

    # -- nom des fichiers

    def _fill_variables(self):
        menu = self.variables.menu()
        menu.clear()
        for v in self.state.get("naming_variables", []):
            action = menu.addAction("<%s>    %s" % (v["name"], v["description"]))
            action.triggered.connect(lambda _=False, n=v["name"]: self._insert_naming("<%s>" % n))
        menu.addSeparator()
        action = menu.addAction("[ ]    optional section: dropped when a variable in it is empty")
        action.triggered.connect(self._insert_section)

    def preset_naming(self):
        return self.current().get("naming") or ""

    def naming_override(self):
        """Modèle de nom choisi pour cet export s'il diffère de celui du preset (sans objet sous un template ShotGrid)."""
        naming = self.settings().get("naming")
        if not naming or naming == self.preset_naming() or (self.state or {}).get("naming_template") is not None:
            return None
        return naming

    def _show_naming(self):
        if self.state is None:
            return
        template = self.state.get("naming_template")
        self.naming.setText(template if template is not None else self.naming_override() or self.preset_naming())
        self.naming.setCursorPosition(0)
        self._check_naming()

    def _check_naming(self):
        template = self.state.get("naming_template")
        variables = [v["name"] for v in self.state.get("naming_variables", [])]
        self.naming_error = None if template is not None else naming_problem(self.naming.text(), variables)
        overridden = self.naming_override() is not None
        self.naming.setStyleSheet("border-color: %s;" % ERROR if self.naming_error
                                  else "color: %s;" % ACCENT if overridden else "")
        self.reset_naming.setVisible(overridden or self.naming_error is not None)
        self._update_naming_note()
        self._update_export_button()

    def _on_naming(self, text):
        self.settings()["naming"] = text if text != self.preset_naming() else None
        self._check_naming()
        if self.naming_error is None:
            self.schedule()

    def _insert_naming(self, text):
        self.naming.insert(text)  # à la place de la sélection
        self.naming.setFocus()
        self._on_naming(self.naming.text())

    def _insert_section(self):
        field = self.naming
        if field.hasSelectedText():
            field.insert("[%s]" % field.selectedText())
        else:
            field.insert("[]")
            field.cursorBackward(False)
        field.setFocus()
        self._on_naming(field.text())

    def _reset_naming(self):
        self.settings()["naming"] = None
        self._show_naming()
        self.schedule()

    def _update_naming_note(self):
        """Sous le champ : l'erreur, sinon un nom de fichier de cet export, et les maps qui gardent leur propre nom."""
        if self.state is None:
            return
        template = self.state.get("naming_template")
        if template is not None:
            note = "Names come from the ShotGrid template of this project"
            files = [f for f in self._report_files() if self._selected(f)]
            work = os.path.dirname(self.state["project"].get("path") or "")
            if files and work:
                note += ", e.g. " + os.path.relpath(files[0]["path"], work)
            self.naming_note.setText(html.escape(note))
            return
        if self.naming_error:
            self.naming_note.setText('<span style="color:%s">%s</span>' % (ERROR, html.escape(self.naming_error)))
            return
        own = [t["name"] for t in self.textures() if t.get("naming")]
        files = [f for f in self._report_files() if self._selected(f) and f.get("texture") not in own]
        parts = []
        if files:
            dest = self.dir.text().strip()
            path = files[0]["path"]
            parts.append("e.g. " + html.escape(os.path.relpath(path, dest) if dest and path.startswith(dest) else
                                                os.path.basename(path)))
        if own:
            parts.append(html.escape("%s: own name from the preset" % ", ".join(own)))
        self.naming_note.setText("  ·  ".join(parts))

    # -- ShotGrid : template des textures et version du fichier de travail (lot B6)

    def sg_naming(self):
        """Nommage ShotGrid du projet ({"template", "default_template", "chosen", "templates", "name", "version",
        "error"}, studio_export_naming_json), None hors pipeline."""
        info = (self.state or {}).get("sg_naming")
        return info if isinstance(info, dict) else None

    def _show_sg(self):
        info = self.sg_naming()
        shown = info is not None and bool(info.get("templates") or info.get("template") or info.get("error"))
        self.sg_label.setVisible(shown)
        self.sg_widget.setVisible(shown)
        if not shown:
            return
        usable = list(info.get("templates") or [])
        default = info.get("default_template")
        chosen = info.get("template") or self.sidecar_data.get("texture_template") or default
        self.sg_template.blockSignals(True)
        self.sg_template.clear()
        for name in usable + ([chosen] if chosen and chosen not in usable else []):
            suffix = "  (default)" if name == default else "" if name in usable else "  (not usable)"
            self.sg_template.addItem(name + suffix, name)
        self.sg_template.setCurrentIndex(max(0, self.sg_template.findData(chosen)))
        self.sg_template.blockSignals(False)
        version = info.get("version")
        self.sg_version.setText("version %d" % version if isinstance(version, int) else "")
        project = os.path.basename(self.state["project"].get("path") or "")
        self.version_up.setToolTip("Save %s as the next version of the work file: the textures then go to that version"
                                   % (project or "the project"))
        self.sg_note.setText('<span style="color:%s">%s</span>' % (ERROR, html.escape(info["error"]))
                             if info.get("error") else "")
        self._update_sg_enabled()

    def _update_sg_enabled(self):
        info = self.sg_naming() or {}
        ready = self.state is not None and not self.busy and not self.sg_busy
        self.sg_template.setEnabled(ready and self.sidecar is not None and not self.sidecar_error
                                    and self.sg_template.count() > 1)
        self.version_up.setEnabled(ready and isinstance(info.get("version"), int) and bool(info.get("name")))

    def _say_sg(self, text, color=ERROR):
        self.sg_note.setText('<span style="color:%s">%s</span>' % (color, html.escape(text)))

    def _on_sg_template(self, *_):
        name = self.sg_template.currentData()
        if name and self.sg_naming() is not None:
            self._choose_template(name)

    def _choose_template(self, name, then=None):
        """Template des textures du projet : écrit dans son sidecar ("texture_template", retiré pour celui du réglage),
        où le sidecar ShotGrid le lit pour l'export et le publish ; puis le nommage est redemandé à ArmorPaint."""
        info = self.sg_naming() or {}
        data = dict(self.sidecar_data)
        if name == info.get("default_template"):
            data.pop("texture_template", None)
        else:
            data["texture_template"] = name
        if data != self.sidecar_data:
            if self.sidecar is None or self.sidecar_error:
                self._say_sg("Template not kept: %s" % (self.sidecar_error or "save the project first"))
                return then() if then else None
            data["version"] = 1
            try:
                write_sidecar(self.sidecar, data)
            except OSError as e:
                self._say_sg("Template not kept: %s" % e)
                return then() if then else None
            self.sidecar_data = data
            self.sidecar_saved = copy.deepcopy(data)
        self.sg_busy = True
        self._say_sg("Asking ShotGrid...", color=MUTED)
        self._update_sg_enabled()
        self._update_export_button()
        self.channel.request("naming", {"refresh": True}, lambda data, error: self._on_sg_reply(data, error, then))

    def _version_up(self, then=None):
        self.sg_busy = True
        self._say_sg("Saving the next version...", color=MUTED)
        self._update_sg_enabled()
        self._update_export_button()
        self.channel.request("version_up", {}, lambda data, error: self._on_sg_reply(data, error, then, True))

    def _on_sg_reply(self, data, error, then, version_up=False):
        """Réponse de naming (data : le nommage) ou de version_up (data : {"path", "sg_naming"})."""
        self.sg_busy = False
        reply = data
        if error is None:
            if version_up:
                self.state["project"]["path"] = data["path"]
                self._show_project()
                data = data.get("sg_naming")
            self.state["sg_naming"] = data if isinstance(data, dict) else None
            self.state["naming_template"] = (data or {}).get("template") if isinstance(data, dict) else None
            self._show_sg()
            self._set_enabled(not self.busy)
            self._show_naming()
            if version_up and not (data or {}).get("error"):
                self._say_sg("Saved %s" % os.path.basename(self.state["project"]["path"]), color=SUCCESS)
            self.schedule()
        else:
            self._say_sg(error.get("message", ""))
        self._update_sg_enabled()
        self._update_export_button()
        if self.test is not None:
            self.test_log["version_up" if version_up else "template"] = reply if error is None else {"error": error}
        if then:
            then()

    # -- jeux de textures

    def _atlases(self):
        return (self.state or {}).get("atlases") or {"names": [], "objects": [], "layers": []}

    def _show_sets(self):
        """Une ligne par objet (ses tuiles UDIM ensemble), avec son jeu ; pas de section pour un seul objet."""
        atlases = self._atlases()
        groups = object_groups(atlases["objects"])
        self.sets_section.setVisible(len(groups) > 1)
        tree = self.sets
        tree.clear()
        if len(groups) < 2:
            return
        names = atlases["names"]
        for name, indices in groups:
            label = name if len(indices) == 1 else "%s  (%s)" % (name, plural(len(indices), "UDIM tile"))
            item = QtWidgets.QTreeWidgetItem([label, ""])
            item.setFlags(QtCore.Qt.ItemIsEnabled | QtCore.Qt.ItemNeverHasChildren)
            item.setToolTip(0, ", ".join(atlases["objects"][i]["name"] for i in indices))
            tree.addTopLevelItem(item)
            box = QtWidgets.QComboBox()
            current = {atlases["objects"][i]["atlas"] for i in indices}
            if len(current) > 1:
                box.addItem("(several)", None)
            for k, n in enumerate(names):
                box.addItem(n, k)
            box.insertSeparator(box.count())
            box.addItem("New set...", NEW_SET)
            box.setCurrentIndex(max(0, box.findData(current.pop())) if len(current) == 1 else 0)
            box.activated.connect(lambda _, b=box, idx=indices: self._on_set_chosen(idx, b.currentData()))
            tree.setItemWidget(item, 1, box)
            item.setSizeHint(0, QtCore.QSize(0, box.sizeHint().height() + 2))
        rows = min(tree.topLevelItemCount(), 4)  # au-delà, défilement : les maps gardent la place
        tree.setFixedHeight(tree.header().sizeHint().height() + tree.sizeHintForRow(0) * rows + 6)
        layers = atlases.get("layers") or []
        self.sets_layers.setText("Limited to a set: %s" % ", ".join("%s (%s)" % (l["layer"], names[l["atlas"]])
                                                                   for l in layers if 0 <= l["atlas"] < len(names)))
        self.sets_layers.setVisible(bool(layers))
        self.rename_set.setVisible(bool(names))
        self._update_sets_note()

    def _used_sets(self):
        return sorted({o["atlas"] for o in self._atlases()["objects"]})

    def _update_sets_note(self):
        used = len(self._used_sets())
        mode = self.mode.currentIndex()
        if mode not in (0, 3):
            text = "used by the Visible and Per UDIM Tile exports"
        elif used > 1 and mode == 0:
            text = "Visible: one set of maps per texture set (<atlas> in the names)"
        elif used > 1:
            text = "Per UDIM Tile: one set of maps per texture set and tile (<atlas> in the names)"
        else:
            text = "all objects in one set of maps%s; choose New set... to split them" % (" per tile" if mode == 3 else "")
        self.sets_note.setText(text)
        self.merge_sets.setVisible(used > 1)

    def _set_name_dialog(self, title, label, text=""):
        name, ok = QtWidgets.QInputDialog.getText(self, title, label, text=text)
        return name.strip() if ok else None

    def _on_set_chosen(self, indices, choice):
        atlases = self._atlases()
        names = list(atlases["names"])
        objects = [o["atlas"] for o in atlases["objects"]]
        if choice is None:
            return
        if choice == NEW_SET:
            name = self._set_name_dialog("New texture set", "Name of the texture set:", "set_%d" % (len(names) + 1))
            if not name:
                self._show_sets()
                return
            if name in names:
                choice = names.index(name)
            else:
                names.append(name)
                choice = len(names) - 1
        for i in indices:
            objects[i] = choice
        self._send_atlases(names, objects)

    def _on_rename_set(self):
        names = self._atlases()["names"]
        if not names:
            return
        menu = QtWidgets.QMenu(self)
        for k, n in enumerate(names):
            menu.addAction(n).setData(k)
        action = menu.exec(self.rename_set.mapToGlobal(QtCore.QPoint(0, self.rename_set.height())))
        if action is None:
            return
        k = action.data()
        name = self._set_name_dialog("Rename texture set", "New name of %s:" % names[k], names[k])
        if name and name != names[k]:
            renamed = list(names)
            renamed[k] = name
            self._send_atlases(renamed, [o["atlas"] for o in self._atlases()["objects"]])

    def _merge_sets(self):
        atlases = self._atlases()
        first = self._used_sets()[0] if atlases["objects"] else 0
        self._send_atlases(atlases["names"], [first] * len(atlases["objects"]))

    def _send_atlases(self, names, objects):
        """Les jeux changent dans le projet d'ArmorPaint (enregistrés avec le .arm) ; il vérifie et répond."""
        self.sets_busy = True
        self._set_enabled(not self.busy)
        self.channel.request("atlases", {"names": names, "objects": objects}, self._on_atlases)

    def _on_atlases(self, data, error):
        self.sets_busy = False
        if error is not None:
            self._say_error("Texture sets not changed: %s" % error.get("message", ""))
        elif not data.get("ok"):
            self._say_error("Texture sets not changed: %s" % data.get("error", ""))
        if data is not None and data.get("atlases"):
            self.state["atlases"] = data["atlases"]
        self._show_sets()
        self._set_enabled(not self.busy)
        if self.test is not None:
            self.test_log["atlases_reply"] = data if error is None else {"error": error}
            if self.test.get("_atlases_first"):
                self.test["_atlases_first"] = False
                self._run_test_setup()
                return
        self.schedule()

    # -- maps

    def _show_maps(self):
        """Une ligne par texture du preset : case (map exportée), format, bits et résolution de cet export, espace."""
        if self.state is None:
            return
        tree = self.maps
        tree.blockSignals(True)
        tree.clear()
        s = self.settings()
        space = self.space.currentData() or ""
        for t in self.textures():
            bits, resolution = self.effective(t)
            item = QtWidgets.QTreeWidgetItem([t["name"], t["format"].upper(), "", "", "", ""])
            item.setFlags(QtCore.Qt.ItemIsEnabled | QtCore.Qt.ItemIsUserCheckable | QtCore.Qt.ItemNeverHasChildren)
            item.setData(0, FILE_KEY_ROLE, t["name"])
            item.setCheckState(0, QtCore.Qt.Unchecked if t["name"] in s["disabled"] else QtCore.Qt.Checked)
            item.setToolTip(0, "%s: %d channel%s, file suffix \"%s\"" % (t["name"], t["channels"],
                                                                         "" if t["channels"] == 1 else "s", t["suffix"]))
            item.setText(4, space if space and t.get("color") else t["color_space"])
            item.setTextAlignment(5, QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)
            tree.addTopLevelItem(item)

            bits_box = QtWidgets.QComboBox()
            for b in t["bits_options"]:
                bits_box.addItem(bits_label(t["format"], b), b)
            bits_box.setCurrentIndex(max(0, bits_box.findData(bits)))
            bits_box.setEnabled(len(t["bits_options"]) > 1)
            bits_box.setToolTip("Preset: %s" % bits_label(t["format"], t["bits"]))
            bits_box.activated.connect(lambda _, n=t["name"], box=bits_box: self._set_override(n, "bits", box.currentData()))
            tree.setItemWidget(item, 2, bits_box)

            res_box = QtWidgets.QComboBox()
            res_box.addItem("Project (%s)" % self._size_label(0), 0)
            for r in sorted(set(RESOLUTIONS + [t["resolution"], resolution] if t["resolution"] or resolution
                                else RESOLUTIONS), reverse=True):
                if r:
                    res_box.addItem(self._size_label(r), r)
            res_box.setCurrentIndex(max(0, res_box.findData(resolution)))
            res_box.setToolTip("Preset: %s" % ("project resolution" if not t["resolution"] else self._size_label(t["resolution"])))
            res_box.activated.connect(lambda _, n=t["name"], box=res_box: self._set_override(n, "resolution", box.currentData()))
            tree.setItemWidget(item, 3, res_box)
            item.setSizeHint(0, QtCore.QSize(0, max(bits_box.sizeHint().height(), res_box.sizeHint().height()) + 2))
            self._mark_overridden(item, t)
            self._mark_enabled(item)
        tree.blockSignals(False)
        self._update_map_sizes()
        self._fit_maps()

    def _mark_overridden(self, item, t):
        """Réglages qui s'écartent du preset : en couleur, pour les voir d'un coup d'œil."""
        o = self.overrides().get(t["name"], {})
        for column, key in ((2, "bits"), (3, "resolution")):
            box = self.maps.itemWidget(item, column)
            if box is not None:
                box.setStyleSheet("color: %s;" % ACCENT if key in o else "")

    def _mark_enabled(self, item):
        """Map désactivée : grisée, ses réglages gardés mais figés."""
        on = item.checkState(0) == QtCore.Qt.Checked
        color = QtGui.QColor("#dcdcdc" if on else "#666666")
        for column in (0, 1, 4, 5):
            item.setForeground(column, QtGui.QBrush(color))
        for column in (2, 3):
            box = self.maps.itemWidget(item, column)
            if box is not None:
                box.setEnabled(on and (column == 3 or box.count() > 1))

    def _fit_maps(self):
        rows = max(1, self.maps.topLevelItemCount())
        row = self.maps.sizeHintForRow(0) if self.maps.topLevelItemCount() else 24
        self.maps.setMaximumHeight(self.maps.header().height() + row * rows + 6)

    def _set_override(self, name, field, value):
        t = next((t for t in self.textures() if t["name"] == name), None)
        if t is None:
            return
        stored = self.settings()["overrides"]
        entry = stored.setdefault(name, {})
        if value == t[field]:
            entry.pop(field, None)
        else:
            entry[field] = value
        if not entry:
            stored.pop(name, None)
        for i in range(self.maps.topLevelItemCount()):
            item = self.maps.topLevelItem(i)
            if item.data(0, FILE_KEY_ROLE) == name:
                self._mark_overridden(item, t)
        self.schedule()

    def _on_bits_all(self, index):
        bits = self.bits_all.itemData(index)
        self.bits_all.setCurrentIndex(0)
        if bits is None:
            return
        self._apply_all_bits(bits)

    def _apply_all_bits(self, bits):
        for t in self.textures():
            options = t["bits_options"]
            lower = [b for b in options if b <= bits]
            self._set_override(t["name"], "bits", max(lower) if lower else min(options))
        self._show_maps()

    def _on_resolution_all(self, index):
        resolution = self.res_all.itemData(index)
        self.res_all.setCurrentIndex(0)
        if resolution is None:
            return
        self._apply_all_resolutions(resolution)

    def _apply_all_resolutions(self, resolution):
        for t in self.textures():
            self._set_override(t["name"], "resolution", resolution)
        self._show_maps()

    def _reset_maps(self):
        s = self.settings()
        s["overrides"].clear()
        s["disabled"].clear()
        self._show_maps()
        self._show_files(self.report)
        self.schedule()

    def _on_map_checked(self, item, column):
        if column != 0:
            return
        name = item.data(0, FILE_KEY_ROLE)
        disabled = self.settings()["disabled"]
        if item.checkState(0) == QtCore.Qt.Checked:
            disabled.discard(name)
        else:
            disabled.add(name)
        self.maps.blockSignals(True)
        self._mark_enabled(item)
        self.maps.blockSignals(False)
        self._show_files(self.report)
        self._update_map_sizes()
        self._update_summary()
        self.schedule()

    def _update_map_sizes(self):
        files = [f for f in self._report_files() if self._selected(f)]
        for i in range(self.maps.topLevelItemCount()):
            item = self.maps.topLevelItem(i)
            name = item.data(0, FILE_KEY_ROLE)
            size = sum(f.get("size_estimate", 0) for f in files if f.get("texture") == name)
            item.setText(5, size_label(size) if size else "")

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
            if self.report is None and data.get("error"):
                message = data["error"].strip()
                self.summary.setText('<span style="color:%s">%s</span>' % (ERROR, html.escape(message.splitlines()[0])))
                self.issues.setText(html.escape(message).replace("\n", "<br>"))
                self._fit_issues(message.count("\n") + 1)
                self.issues_frame.show()
            if self.test is not None and "preflight" not in self.test_log:
                self.test_log["preflight"] = data
                QtCore.QTimer.singleShot(0, self.run_export if not self.test.get("no_export") else self._finish_test)
        self._update_export_button()

    def _show_report(self, report):
        self._show_files(report)
        self._update_map_sizes()
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
        self._update_naming_note()
        self._update_summary()

    def _fit_issues(self, lines):
        """Hauteur des lignes affichées, 5 au plus (au-delà, défilement)."""
        width = max(100, self.issues_scroll.viewport().width() or self.width() - 60)
        height = self.issues.heightForWidth(width)
        cap = self.fontMetrics().lineSpacing() * 5 + 4
        self.issues_scroll.setFixedHeight(min(cap, height if height > 0 else self.fontMetrics().lineSpacing() * lines))

    # -- liste des fichiers

    def _report_files(self):
        """Fichiers du dernier rapport qui sont ceux du preset affiché (le rapport peut dater du preset précédent)."""
        if self.report is None or self.report.get("preset") != self.current().get("name"):
            return []
        return self.report.get("files", [])

    def _selected(self, f):
        s = self.settings()
        return f.get("texture") not in s["disabled"] and file_key(f) not in s["skip"]

    def _show_files(self, report):
        """Arbre des fichiers à cocher, quand il y a plusieurs jeux (objets, tuiles UDIM) : groupé par map ou par
        objet / tuile, groupes fermés, avec une case pour tout le groupe. Les maps désactivées n'y figurent pas."""
        tree = self.files
        scroll = tree.verticalScrollBar().value()
        tree.blockSignals(True)
        tree.clear()
        disabled = self.settings()["disabled"]
        files = [f for f in self._report_files() if f.get("texture") not in disabled]
        sets = {(f.get("variant", ""), f.get("udim", "")) for f in self._report_files()}
        grouped = len(sets) > 1
        self.files_section.setVisible(grouped)
        self.composition.setText(composition(files, self.mode.currentIndex()) if files else "")
        tree.setRootIsDecorated(True)
        by_map = self.group.currentData() == "map"
        groups = {}
        for f in files if grouped else []:
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
                parent.setText(2, resolution_label(members[0]))
            parent.setTextAlignment(3, QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)
            tree.addTopLevelItem(parent)
            for f in members:
                parent.addChild(self._file_item(f, os.path.basename(f["path"]), in_group=by_map))
            parent.setExpanded(("group", name) in self.expanded)
        self._refresh_groups()
        tree.blockSignals(False)
        tree.verticalScrollBar().setValue(scroll)

    def _file_item(self, f, label, in_group=False):
        # Dans un groupe par map, format et résolution sont ceux du groupe : pas répétés sur chaque ligne.
        item = QtWidgets.QTreeWidgetItem(
            [label, "" if in_group else format_label(f), "" if in_group else resolution_label(f),
             size_label(f.get("size_estimate", 0))]
        )
        item.setFlags(QtCore.Qt.ItemIsEnabled | QtCore.Qt.ItemIsUserCheckable | QtCore.Qt.ItemNeverHasChildren)
        key = file_key(f)
        item.setData(0, FILE_KEY_ROLE, key)
        item.setData(3, FILE_KEY_ROLE, f.get("size_estimate", 0))
        item.setData(1, FILE_KEY_ROLE, bool(f.get("exists")))
        item.setCheckState(0, QtCore.Qt.Unchecked if key in self.settings()["skip"] else QtCore.Qt.Checked)
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
        """Taille cochée et « 5 of 7 » de chaque groupe."""
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
        skip = self.settings()["skip"]
        before = set(skip)
        for leaf in self._leaves():
            key = leaf.data(0, FILE_KEY_ROLE)
            if leaf.checkState(0) == QtCore.Qt.Checked:
                skip.discard(key)
            else:
                skip.add(key)
        if skip == before:
            return
        self.files.blockSignals(True)
        self._refresh_groups()
        self.files.blockSignals(False)
        self._update_map_sizes()
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
            if not self.summary.text().startswith("<span"):
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
        disabled = self.settings()["disabled"]
        files = [f for f in self._report_files() if f.get("texture") not in disabled]  # maps désactivées : hors compte
        chosen = [f for f in files if self._selected(f)]
        size = sum(f.get("size_estimate", 0) for f in chosen)
        replaced = sum(1 for f in chosen if f.get("exists"))
        text = "%s%s  ·  %s uncompressed" % (
            len(chosen) if len(chosen) == len(files) else "%d of %d" % (len(chosen), len(files)),
            " file" if len(files) == 1 else " files",
            size_label(size),
        )
        if replaced:
            text += '  ·  <span style="color:%s">%s replaced</span>' % (WARNING, plural(replaced, "existing file"))
        self.summary.setText(text)

    # -- presets du projet

    def _base_preset_json(self, preset):
        if preset.get("project"):
            return copy.deepcopy(preset.get("json") or {})
        with open(preset["path"], encoding="utf-8") as f:
            return json.load(f)

    def _save_preset(self, name=None):
        """Preset du projet : celui choisi, avec les maps, bits, résolutions et l'espace de cet export."""
        if self.sidecar is None:
            return
        preset = self.current()
        if name is None:
            suggestion = preset["name"] if preset.get("project") else "%s_custom" % preset["name"]
            name, ok = QtWidgets.QInputDialog.getText(self, "Save as preset", "Preset name (kept with the project):",
                                                      text=suggestion)
            if not ok:
                return
        name = name.strip()
        if not PRESET_NAME.match(name):
            self._say_error("Preset name: letters, digits, '_', '-' or '.', starting with a letter or digit")
            return
        if any(not p["project"] and p["name"] == name for p in self.presets):
            self._say_error("A studio preset is already called %s: choose another name" % name)
            return
        exists = any(p["project"] and p["name"] == name for p in self.presets)
        if exists and self.test is None and QtWidgets.QMessageBox.question(
                self, "Save as preset", "Replace the project preset %s?" % name) != QtWidgets.QMessageBox.Yes:
            return
        try:
            data = self._base_preset_json(preset)
        except (OSError, ValueError) as e:
            self._say_error("Cannot read %s: %s" % (preset.get("path"), e))
            return
        s = self.settings()
        info = {t["name"]: t for t in self.textures()}
        space = self.space.currentData() or ""
        textures = []
        for tex in data.get("textures", []):
            t = info.get(tex.get("name"))
            if t is None or t["name"] in s["disabled"]:
                continue
            bits, resolution = self.effective(t)
            if bits != t["bits"]:
                tex["bits"] = bits
            if resolution != t["resolution"]:
                tex["resolution"] = resolution
            if space and t.get("color"):
                tex["color_space"] = space
            textures.append(tex)
        if not textures:
            self._say_error("No map to save: enable at least one")
            return
        data["textures"] = textures
        naming = self.naming_override()
        if naming:
            data["naming"] = naming
        if preset.get("project"):
            data["description"] = preset.get("description", "")
        else:
            data["description"] = "From %s%s" % (preset_label(preset["name"]),
                                                ": " + preset["description"] if preset.get("description") else "")
        self.channel.request("inspect", {"name": name, "preset": data},
                             lambda info, error: self._on_saved_preset(name, data, info, error))

    def _on_saved_preset(self, name, data, info, error):
        if error is not None or not info.get("valid"):
            self._say_error("Preset %s not saved: %s" % (name, (error or {}).get("message") or info.get("error")))
            return
        previous = self.preset_key()
        key = "project:" + name
        presets = self.sidecar_data.setdefault("presets", {})
        if not isinstance(presets, dict):
            presets = self.sidecar_data["presets"] = {}
        presets[name] = data
        # Le preset porte maintenant les réglages : ils repartent de zéro ; les fichiers décochés restent.
        skip = set(self.settings(previous)["skip"])
        self.per_preset[key] = {"overrides": {}, "disabled": set(), "skip": skip, "naming": None}
        self.presets = [p for p in self.presets if p["key"] != key]
        self.presets.append(dict(info, key=key, project=True, shipped=False, path=self.sidecar, json=data))
        self._fill_presets(key)
        self.space.blockSignals(True)
        self.space.setCurrentIndex(0)  # l'espace est dans le preset
        self.space.blockSignals(False)
        self._save_settings()
        self._on_preset()
        self.summary.setText('<span style="color:%s">Preset %s saved with the project</span>' % (SUCCESS, html.escape(name)))
        if self.test is not None:
            self.test_log["saved_preset"] = name

    def _delete_preset(self):
        preset = self.current()
        if not preset.get("project"):
            return
        if QtWidgets.QMessageBox.question(self, "Delete preset", "Delete the project preset %s?"
                                          % preset["name"]) != QtWidgets.QMessageBox.Yes:
            return
        (self.sidecar_data.get("presets") or {}).pop(preset["name"], None)
        self.per_preset.pop(preset["key"], None)
        self.presets = [p for p in self.presets if p["key"] != preset["key"]]
        self._fill_presets("")
        self._save_settings()
        self._on_preset()

    def _say_error(self, message):
        log(message)
        self.summary.setText('<span style="color:%s">%s</span>' % (ERROR, html.escape(message)))
        if self.test is not None:
            self.test_log.setdefault("errors", []).append(message)

    # -- sidecar

    def _settings_json(self):
        keys = {p["key"] for p in self.presets}
        per = {}
        for key, s in self.per_preset.items():
            if key not in keys:
                continue
            entry = {}
            if s["overrides"]:
                entry["overrides"] = s["overrides"]
            if s["disabled"]:
                entry["disabled"] = sorted(s["disabled"])
            if s["skip"]:
                entry["skip"] = [list(k) for k in sorted(s["skip"])]
            if s.get("naming"):
                entry["naming"] = s["naming"]
            if entry:
                per[key] = entry
        return {
            "preset": self.preset_key(),
            "color_space": self.space.currentData() or "",
            "mode": self.mode.currentIndex(),
            "padding": self.padding.isChecked(),
            "dir": self.dir.text().strip(),
            "group": self.group.currentData(),
            "presets": per,
        }

    def _save_settings(self):
        """Choix et presets du projet dans le sidecar ; rien n'est écrit s'ils n'ont pas changé, ni si le sidecar était
        illisible (il n'est jamais écrasé)."""
        if self.sidecar is None or self.state is None or self.sidecar_error:
            return
        data = dict(self.sidecar_data)
        data["version"] = 1
        data["export"] = self._settings_json()
        if data == self.sidecar_saved:
            return
        try:
            write_sidecar(self.sidecar, data)
            self.sidecar_data = data
            self.sidecar_saved = copy.deepcopy(data)
        except OSError as e:
            log("export settings not saved: %s" % e)

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
            self.report = report
            self._update_map_sizes()
            self.summary.setText(
                '<span style="color:%s">Exported %s with %s</span>'
                % (SUCCESS, plural(count, "file"), preset_label(report.get("preset", "")))
            )
            self.cancel.setText("Close")
            self.reveal.show()
            self._save_settings()
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
        self._save_settings()
        super().closeEvent(event)
        if self.mac is not None:
            self.mac.hide()  # macOS rend le premier plan à ArmorPaint
        QtWidgets.QApplication.instance().quit()

    # -- essai sans intervention

    def _run_test_setup(self):
        t = self.test
        self.test_log["state"] = self.state
        self.test_log["sidecar"] = self.sidecar
        self.test_log.setdefault("sg_naming_first", copy.deepcopy(self.state.get("sg_naming")))
        if "atlases" in t and not t.get("_atlases_sent"):
            t["_atlases_sent"] = t["_atlases_first"] = True  # le reste à la réponse (_on_atlases)
            self._send_atlases(t["atlases"]["names"], t["atlases"]["objects"])
            return
        if "template" in t and not t.get("_template_sent"):
            t["_template_sent"] = True  # le reste à la réponse (_on_sg_reply)
            self._choose_template(t["template"], then=self._run_test_setup)
            return
        if t.get("version_up") and not t.get("_version_up_sent"):
            t["_version_up_sent"] = True
            self._version_up(then=self._run_test_setup)
            return
        if "preset" in t:
            key = t["preset"] if ":" in t["preset"] else "file:" + t["preset"]
            self.preset.setCurrentIndex(self._preset_index(key))
        if "color_space" in t:
            self.space.setCurrentIndex(self.space.findData(t["color_space"]))
        if "mode" in t:
            self.mode.setCurrentIndex(t["mode"])
        if "padding" in t:
            self.padding.setChecked(t["padding"])
        s = self.settings()
        if "skip" in t:
            s["skip"] = {tuple(k) for k in t["skip"]}
        if "disabled" in t:
            s["disabled"] = set(t["disabled"])
        if "overrides" in t:
            s["overrides"] = copy.deepcopy(t["overrides"])
        if "group" in t:
            self.group.setCurrentIndex(max(0, self.group.findData(t["group"])))
        self._show_maps()
        if "bits_all" in t:
            self._apply_all_bits(t["bits_all"])
        if "resolution_all" in t:
            self._apply_all_resolutions(t["resolution_all"])
        if "dir" in t:
            self.dir.setText(t["dir"])
        if "naming" in t:
            self.naming.setText(t["naming"])
            self._on_naming(t["naming"])
        if "save_preset" in t:
            self._save_preset(t["save_preset"])
        self.schedule()

    def _finish_test(self):
        self.test_log["summary"] = self.summary.text()
        self.test_log["choices"] = self.choices()
        self.test_log["files"] = [l.text(0) for l in self._leaves() if l.checkState(0) == QtCore.Qt.Checked]
        self.test_log["skipped"] = [l.text(0) for l in self._leaves() if l.checkState(0) != QtCore.Qt.Checked]
        tree = self.files
        self.test_log["groups"] = [tree.topLevelItem(i).text(0) for i in range(tree.topLevelItemCount())
                                   if tree.topLevelItem(i).childCount() > 0]
        self.test_log["files_shown"] = not self.files_section.isHidden()
        maps = []
        for i in range(self.maps.topLevelItemCount()):
            item = self.maps.topLevelItem(i)
            maps.append({
                "name": item.text(0),
                "enabled": item.checkState(0) == QtCore.Qt.Checked,
                "bits": self.maps.itemWidget(item, 2).currentData(),
                "resolution": self.maps.itemWidget(item, 3).currentData(),
                "resolution_label": self.maps.itemWidget(item, 3).currentText(),
                "color_space": item.text(4),
                "size": item.text(5),
            })
        self.test_log["maps"] = maps
        self.test_log["presets"] = [self.preset.itemText(i) for i in range(self.preset.count()) if self.preset.itemData(i)]
        self.test_log["composition"] = self.composition.text()
        self.test_log["project"] = self.project.text()
        self.test_log["issues"] = self.issues.text() if not self.issues_frame.isHidden() else ""
        self.test_log["naming"] = {"text": self.naming.text(), "note": self.naming_note.text(),
                                   "error": self.naming_error, "enabled": self.naming.isEnabled()}
        self.test_log["sg"] = {
            "shown": not self.sg_widget.isHidden(),
            "templates": [self.sg_template.itemText(i) for i in range(self.sg_template.count())],
            "current": self.sg_template.currentData(),
            "version": self.sg_version.text(),
            "note": self.sg_note.text(),
            "version_up_enabled": self.version_up.isEnabled(),
        }
        self.test_log["sets"] = {
            "shown": not self.sets_section.isHidden(),
            "note": self.sets_note.text(),
            "rows": [[self.sets.topLevelItem(i).text(0), self.sets.itemWidget(self.sets.topLevelItem(i), 1).currentText()]
                     for i in range(self.sets.topLevelItemCount())],
        }
        if self.test.get("screenshot"):
            self.grab().save(self.test["screenshot"])
        with open(self.test["result"], "w") as f:
            json.dump(self.test_log, f, indent=1)
        self.close()


def main():
    app, mac, channel = studio_qt.start("ArmorPaint Studio Export")
    test = None
    if os.environ.get("STUDIO_EXPORT_WINDOW_TEST"):
        with open(os.environ["STUDIO_EXPORT_WINDOW_TEST"]) as f:
            test = json.load(f)
    window = ExportWindow(channel, mac, test)
    window.to_front()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
