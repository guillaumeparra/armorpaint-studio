#!/usr/bin/env python3
# Fenêtre subzero (chantier E2) : ouvre un .sbs ou un .sbsar, montre ses paramètres comme Painter, les rend par
# sbsrender (subzero_render.py) et crée ou met à jour le matériau correspondant dans ArmorPaint. Lancée par
# external/subzero.c ; protocole décrit dans subzero.h, socle Qt dans studio_qt.py. Python 3.9+ et PySide6.
#
# Rendu : un réglage relâché lance un rendu, qui tue celui en cours. Tant que le matériau n'est pas envoyé, c'est un
# aperçu 512 dans le cache ; ensuite, avec « Live » (par défaut), le rendu final va directement dans le dossier du
# matériau, puis ArmorPaint recharge ses textures (op update). Sans « Live », aperçu 512 et bouton Update.
#
# Matériau : <dossier du .arm>/<projet>_subzero/<Nom>/ (projet jamais enregistré : dossier demandé une fois), avec
# les textures <Nom>_<canal>.<ext> et la recette subzero.json (paquet, graphe, valeurs, preset, taille, format,
# sorties, version de sbsrender). Format (lot E6) : menu à côté de la taille, EXR half par défaut ($SUBZERO_FORMAT) ;
# une recette de la v0, sans format, reste en TGA. Un changement de format laisse les anciennes textures : ArmorPaint
# les retire du projet à la mise à jour, puis la fenêtre supprime leurs fichiers. <Nom> est un seul mot
# (« ColorfulPlanks ») : les règles de nom d'ArmorPaint (couleur ou donnée, canal) ne voient alors que le canal, et
# les noms de fichiers, qui servent de noms d'assets, restent uniques dans le projet. Décalque (couleur de base avec un
# alpha utile) : couleur réécrite sans alpha, alpha dans <Nom>_opacity.<ext>, car ArmorPaint multiplie la couleur par
# l'alpha (sauf en JPEG, qui n'a pas d'alpha).
#
# Couleur (lot E11, subzero_color.py) : après le rendu, chaque carte passe dans l'espace OCIO réglé (couleur de base
# srgb_tx et données Raw par défaut, sans conversion), dans la configuration d'ArmorPaint ; la liaison envoyée donne
# cet espace, et celui du rendu précédent (color_space_was) pour qu'une texture que l'artiste n'a pas changée suive le
# réglage. Vignettes et nuanciers passent par la transformation d'affichage du viewport. Le bouton Settings ouvre le
# panneau des réglages : chacun est une variable d'environnement (SUBZERO_OCIO_*, et sans ArmorPaint OCIO et
# STUDIO_OCIO_*), appliquée à la fenêtre ou enregistrée dans studio.env (studio_env.py). ArmorPaint n'a lu studio.env
# qu'à son démarrage : la fenêtre relit ses variables de couleur à l'ouverture, sauf celles de l'environnement.
#
# Viewer (subzero_viewer.py) : panneau carré à gauche des réglages, bouton Viewer ; $SUBZERO_VIEWER (défaut : ouvert
# sans ArmorPaint, fermé avec). Il montre les cartes du dernier rendu, touches 1 à 9 ou clic sur une vignette. Sans
# ArmorPaint, taille par défaut 512 ($SUBZERO_SIZE) et aperçus à la taille choisie, pour que le viewer montre ce qui
# sera exporté. Sans ArmorPaint aussi, navigateur de paquets à gauche (subzero_browser.py, bouton Browser) : un onglet
# par dossier de $SUBZERO_LIBRARY, et ceux ajoutés par « + ».
#
# SUBZERO_TEST=<fichier.json> : essai sans intervention (.tests/subzero_app.sh, .tests/subzero_window_ui.py). Le
# fichier donne "open" ({"file"} ou {"dir"} : paquet, ou matériau déjà envoyé à rééditer ; absent : celui de l'état),
# "graph", "preset", "values" ({id: valeur}), "size" (log2, ou null pour celle du projet), "format" (clé de
# subzero_render.FORMATS), "live", "dir" (dossier de
# base d'un projet jamais enregistré), "action" ("create", "update", "duplicate" ou "none"), "settings" ({variable:
# valeur ou null}, avec "save": true pour écrire studio.env), "result" (fichier écrit à la fin), "screenshot" et
# "settings_screenshot" (panneau Settings), "viewer" (ouvert ou non), puis, avant la capture, "viewer_map" (canal),
# "viewer_keys" ([{"key": caractère, "keypad", "vk" (code virtuel macOS), "scan"}]), "exposure", "gamma", "saturation",
# "zoom" ("fit", "1:1" ou un facteur) et "viewer_hover" ([x, y] : valeurs lues à ce pixel). Navigateur : "browser"
# (ouvert ou non), "browser_add" (dossier ajouté au démarrage), "browser_open" (nom d'un paquet de l'onglet courant,
# ouvert comme par un double clic, à la place de "open"), puis, avant la capture, "browser_tab" (dossier de l'onglet
# montré) et "browser_search" (texte). La fenêtre les applique, agit, écrit le résultat, puis se ferme.

import json
import math
import os
import shutil
import sys
import threading
import time

from PySide6 import QtCore, QtGui, QtWidgets

import studio_env
import studio_qt
import subzero_browser
import subzero_color
import subzero_exr
import subzero_meta
from subzero_meta import read_icons
import subzero_render
import subzero_viewer
from subzero_material import (CHANNEL_LABEL, CHANNEL_SUFFIX, CHANNELS, COLOR_VARS, MAP_VARS, NAME_TEMPLATE, RECIPE,
                              SUBZERO_VARS, apply_overrides, auto_mapping, check_template, condition, env_flag, env_values, links,
                              graph_values, log2_size, make_recipe, read_recipe, recipe_package, recipe_spaces, render_material,
                              render_values, shown, slug, stale_files, subzero_dir, write_recipe)
from studio_qt import ERROR, MUTED, PAINTER_ACCENT, SUCCESS, WARNING, log, muted
from subzero_viewer import read_tga

PREVIEW_SIZE = 9  # 512
THUMB = 72  # sept vignettes sur la largeur de la fenêtre
SOON_MS = 300  # réglage au clavier ou à la molette : rendu après ce délai sans nouveau changement
COLUMN = 560  # largeur de la colonne des réglages
WINDOW_HEIGHT = 860  # hauteur à l'ouverture, et côté du viewer, dans la limite de l'écran

# ---- Rendu en arrière-plan --------------------------------------------------------------------------------------------


class Worker(QtCore.QObject):
    """Un thread, un travail à la fois ; un nouveau travail tue le processus en cours et remplace celui en attente. Le
    résultat d'un travail remplacé est jeté. job(track, step) : track(proc) reçoit chaque processus lancé, step(data)
    remonte un résultat intermédiaire (rendu fini, avant les vignettes)."""

    stepped = QtCore.Signal(object, object)
    finished = QtCore.Signal(object, object)  # job, résultat ou exception

    def __init__(self):
        super().__init__()
        self._lock = threading.Lock()
        self._next = None
        self._procs = []
        self._event = threading.Event()
        self.busy = False
        threading.Thread(target=self._loop, daemon=True).start()

    def submit(self, job):
        with self._lock:
            self._next = job
            self.busy = True
            for p in self._procs:
                subzero_render.kill(p)
        self._event.set()

    def stop(self):
        """Tue le rendu en cours (fermeture de la fenêtre) ; sbsrender ne survit pas à la fenêtre."""
        with self._lock:
            self._next = lambda track, step: None
            for p in self._procs:
                subzero_render.kill(p)

    def _track(self, proc):
        with self._lock:
            self._procs.append(proc)
            if self._next is not None:
                subzero_render.kill(proc)

    def _superseded(self):
        with self._lock:
            return self._next is not None

    def _loop(self):
        while True:
            self._event.wait()
            self._event.clear()
            with self._lock:
                job, self._next = self._next, None
                self._procs = []
            if job is None:
                continue
            try:
                result = job(self._track, lambda data: None if self._superseded() else self.stepped.emit(job, data))
            except Exception as e:  # noqa: BLE001 - remonté à la fenêtre
                result = e
            with self._lock:
                superseded = self._next is not None
                if not superseded:
                    self.busy = False
            if not superseded:
                self.finished.emit(job, result)


def read_exr_thumb(path, proc=None):
    """Vignette d'un EXR (Qt ne le lit pas) : sans compression, ZIPS ou ZIP, un pixel sur n ; QImage nulle sinon.
    proc : transformation de couleur (subzero_color), appliquée aux valeurs flottantes."""
    try:
        w, h, kind, data = subzero_exr.thumbnail(path, THUMB * 2, floats=proc is not None)
    except (OSError, subzero_exr.Error) as e:
        log("thumbnail of %s: %s" % (os.path.basename(path), e))
        return QtGui.QImage()
    if kind == "gray":
        return QtGui.QImage(data, w, h, w, QtGui.QImage.Format_Grayscale8).copy()
    if proc is not None:
        return subzero_color.floats_image(data, w, h, proc)
    return QtGui.QImage(data, w, h, w * 3, QtGui.QImage.Format_RGB888).copy()


def load_thumbs(files, color=None, spaces=None):
    """{canal: QImage réduite}, vue comme dans le viewport (color : subzero_color.Color, spaces : {canal: espace du
    fichier}) ; absent si le fichier ne se lit pas."""
    thumbs = {}
    for ch, path in files.items():
        proc = color.view_processor((spaces or {}).get(ch), ch) if color is not None else None
        lower = path.lower()
        done = False
        if lower.endswith(".exr"):
            image = read_exr_thumb(path, proc)
            done = not image.isNull()
        else:
            image = read_tga(path) if lower.endswith(".tga") else QtGui.QImage()
        if image.isNull():
            image = QtGui.QImageReader(path).read()
        if not done and proc is not None:
            image = subzero_color.apply_image(image, proc)
        if not image.isNull():
            thumbs[ch] = image.scaled(THUMB * 2, THUMB * 2, QtCore.Qt.KeepAspectRatio, QtCore.Qt.SmoothTransformation)
    return thumbs


# ---- Éditeurs de paramètres -------------------------------------------------------------------------------------------


class Editor(QtWidgets.QWidget):
    """Widget d'un paramètre. changed(valeur, mode) : "drag" (curseur tenu), "soon" (clavier, molette), "now".
    kind, disposition à la Painter : "stacked" (libellé et valeur, curseur dessous), "inline" (libellé à gauche),
    "check" (case qui porte le libellé) ; value_widget : valeur montrée sur la ligne du libellé."""

    changed = QtCore.Signal(object, str)
    kind = "inline"

    def __init__(self, inp):
        super().__init__()
        self.inp = inp
        self.value_widget = None
        self.lay = QtWidgets.QVBoxLayout(self)
        self.lay.setContentsMargins(0, 0, 0, 0)
        self.lay.setSpacing(4)

    def set_value(self, value):
        raise NotImplementedError


class ValueBox(QtWidgets.QDoubleSpinBox):
    """Valeur à la Painter, sans zéros inutiles : 0.5, 1.3, 1."""

    def textFromValue(self, value):
        text = "%.*f" % (self.decimals(), value)
        if "." in text:
            text = text.rstrip("0").rstrip(".")
        return "0" if text == "-0" else text


class SliderEditor(Editor):
    """Curseur de 1 à 4 composantes, entier ou flottant ; angle en degrés à l'écran (tours pour sbsrender). Une
    composante : valeur sur la ligne du libellé ; plusieurs : une ligne par composante (nom, curseur, valeur)."""

    kind = "stacked"

    def __init__(self, inp, angle=False):
        super().__init__(inp)
        self._quiet = False
        typ = inp["type"]
        self.n = int(typ[-1]) if typ[-1].isdigit() else 1
        self.is_int = typ.startswith("int")
        self.angle = angle
        self.scale = 360.0 if angle else 1.0
        lo, hi = inp.get("min"), inp.get("max")
        comp = lambda v, k: (v[k] if isinstance(v, (list, tuple)) else v)  # noqa: E731
        labels = inp.get("component_labels") or []
        self.rows = []
        for k in range(self.n):
            a = comp(lo, k) if lo is not None else 0.0
            b = comp(hi, k) if hi is not None else 1.0
            if a is None or b is None or b <= a:
                a, b = (0, 10) if self.is_int else (0.0, 1.0)
            row = QtWidgets.QHBoxLayout()
            row.setSpacing(8)
            if self.n > 1:
                tag = QtWidgets.QLabel(labels[k] if k < len(labels) else "XYZW"[k])
                tag.setObjectName("muted")
                tag.setMinimumWidth(14)
                tag.setMaximumWidth(62)
                row.addWidget(tag)
            slider = QtWidgets.QSlider(QtCore.Qt.Horizontal)
            steps = int(b - a) if self.is_int else 1000
            slider.setRange(0, max(1, steps))
            if self.is_int:
                spin = QtWidgets.QSpinBox()
                spin.setRange(-10 ** 9 if not inp.get("clamp") else int(a), 10 ** 9 if not inp.get("clamp") else int(b))
            else:
                spin = ValueBox()
                step = float(inp.get("step") or 0.01) * self.scale
                spin.setDecimals(1 if angle else max(2, min(4, -int(math.floor(math.log10(step))) if step > 0 else 3)))
                spin.setSingleStep(step if step > 0 else 0.01)
                if inp.get("clamp"):
                    spin.setRange(a * self.scale, b * self.scale)
                else:
                    spin.setRange(-1e6, 1e6)
            spin.setKeyboardTracking(False)
            spin.setObjectName("value")
            spin.setFixedWidth(64)
            spin.setAlignment(QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)
            spin.setButtonSymbols(QtWidgets.QAbstractSpinBox.NoButtons)
            row.addWidget(slider, 1)
            if self.n > 1:
                row.addWidget(spin)
            else:
                self.value_widget = spin
            self.lay.addLayout(row)
            self.rows.append((slider, spin, a, b))
            slider.valueChanged.connect(lambda pos, k=k: self._slid(k, pos))
            slider.sliderReleased.connect(lambda: self.changed.emit(self.value(), "now"))
            spin.valueChanged.connect(lambda v, k=k: self._typed(k, v))

    def _to_pos(self, k, v):
        _, _, a, b = self.rows[k]
        if self.is_int:
            return int(round(v - a))
        return int(round((v - a) / (b - a) * 1000))

    def _from_pos(self, k, pos):
        _, _, a, b = self.rows[k]
        return a + pos if self.is_int else a + (b - a) * pos / 1000.0

    def value(self):
        vals = []
        for slider, spin, _, _ in self.rows:
            v = spin.value()
            vals.append(int(v) if self.is_int else round(v / self.scale, 6))
        return vals if self.n > 1 else vals[0]

    def set_value(self, value):
        self._quiet = True
        vals = value if isinstance(value, (list, tuple)) else [value]
        for k, (slider, spin, _, _) in enumerate(self.rows):
            v = vals[k] if k < len(vals) else 0
            spin.setValue(v * self.scale if not self.is_int else int(v))
            slider.setValue(max(slider.minimum(), min(slider.maximum(), self._to_pos(k, v))))
        self._quiet = False

    def _slid(self, k, pos):
        if self._quiet:
            return
        slider, spin, _, _ = self.rows[k]
        self._quiet = True
        v = self._from_pos(k, pos)
        spin.setValue(int(v) if self.is_int else v * self.scale)
        self._quiet = False
        self.changed.emit(self.value(), "drag" if slider.isSliderDown() else "soon")

    def _typed(self, k, v):
        if self._quiet:
            return
        slider = self.rows[k][0]
        self._quiet = True
        slider.setValue(max(slider.minimum(), min(slider.maximum(), self._to_pos(k, v / self.scale if not self.is_int else v))))
        self._quiet = False
        self.changed.emit(self.value(), "now")


class ToggleEditor(Editor):
    """Case à cocher qui porte le libellé du paramètre, comme dans Painter ; libellés propres de l'interrupteur
    (autres que False/True) en info-bulle."""

    kind = "check"

    def __init__(self, inp):
        super().__init__(inp)
        labels = inp.get("button_labels") or ()
        self.box = QtWidgets.QCheckBox(inp.get("label") or inp["identifier"])
        tip = [inp.get("description") or ""]
        if len(labels) == 2 and tuple(labels) != ("False", "True"):
            tip.append("Off: %s · On: %s" % tuple(labels))
        self.box.setToolTip("\n".join(t for t in tip if t))
        self.lay.addWidget(self.box)
        self.box.toggled.connect(lambda on: self.changed.emit(1 if on else 0, "now"))

    def set_value(self, value):
        self.box.blockSignals(True)
        self.box.setChecked(bool(value))
        self.box.blockSignals(False)


class ComboEditor(Editor):
    def __init__(self, inp):
        super().__init__(inp)
        self.combo = QtWidgets.QComboBox()
        for value, label in inp.get("items") or []:
            self.combo.addItem(str(label), value)
        self.lay.addWidget(self.combo)
        self.combo.activated.connect(lambda i: self.changed.emit(self.combo.itemData(i), "now"))

    def set_value(self, value):
        i = self.combo.findData(value)
        self.combo.blockSignals(True)
        self.combo.setCurrentIndex(i if i >= 0 else 0)
        self.combo.blockSignals(False)


def clamp01(values):
    return [max(0.0, min(1.0, float(x))) for x in values]


class ColorEditor(Editor):
    """Couleur d'un paramètre, dans l'espace du graphe (sRGB encodé, comme les cartes couleur de sbsrender). Lot E11 :
    le nuancier passe par la transformation d'affichage, comme les vignettes, et le sélecteur travaille dans l'espace
    de choix des couleurs ($SUBZERO_OCIO_PICKING, sinon le rôle color_picking)."""

    def __init__(self, inp, color=None):
        super().__init__(inp)
        self.alpha = inp["type"] == "float4"
        self.color = color
        self.shown = ""  # couleur du nuancier (#rrggbb)
        self.button = QtWidgets.QPushButton()
        self.button.setFixedHeight(20)
        self.lay.addWidget(self.button)
        self.button.clicked.connect(self._pick)
        self._value = inp["default"]

    def set_color(self, color):
        self.color = color
        self.set_value(self._value)

    def _spaces(self):
        """(espace du graphe, espace de choix) ; (None, None) sans OCIO."""
        c = self.color
        graph = c.source("base") if c is not None and c.ready else None
        return (graph, c.picking()[0] or graph) if graph else (None, None)

    def _convert(self, rgb, src, dst):
        try:
            return self.color.convert_rgb(rgb, src, dst)
        except subzero_color.Error as e:
            log(str(e))
            return list(rgb)

    def set_value(self, value):
        self._value = list(value)
        graph, picking = self._spaces()
        rgb = clamp01(self._value[:3])
        c = QtGui.QColor.fromRgbF(*clamp01(self.color.view_rgb(rgb, graph) if graph else rgb))
        self.shown = c.name()
        self.button.setStyleSheet("QPushButton { background: %s; border: 1px solid #4d4d4d; border-radius: 3px; "
                                  "padding: 0; min-height: 0; } QPushButton:hover { border-color: #8c8c8c; }" % c.name())
        tip = " ".join("%.3f" % x for x in self._value)
        if graph and picking != graph:
            tip += "\n%s: %s" % (picking, " ".join("%.3f" % x for x in self._convert(rgb, graph, picking)))
        self.button.setToolTip(tip)

    def _pick(self):
        graph, picking = self._spaces()
        rgb = [float(x) for x in self._value[:3]]
        convert = graph is not None and picking != graph
        if convert:
            rgb = self._convert(rgb, graph, picking)
        a = float(self._value[3]) if self.alpha and len(self._value) > 3 else 1.0
        start = QtGui.QColor.fromRgbF(*clamp01(rgb + [a]))
        title = (self.inp.get("label") or "") + (" (%s)" % picking if picking else "")
        options = QtWidgets.QColorDialog.ShowAlphaChannel if self.alpha else QtWidgets.QColorDialog.ColorDialogOptions()
        c = QtWidgets.QColorDialog.getColor(start, self, title, options)
        if c.isValid():
            rgb = [c.redF(), c.greenF(), c.blueF()]
            if convert:
                rgb = self._convert(rgb, picking, graph)
            v = [round(x, 6) for x in rgb] + ([round(c.alphaF(), 6)] if self.alpha else [])
            self.set_value(v)
            self.changed.emit(v, "now")


class ImageEditor(Editor):
    def __init__(self, inp):
        super().__init__(inp)
        row = QtWidgets.QHBoxLayout()
        self.path = QtWidgets.QLineEdit()
        self.path.setReadOnly(True)
        self.path.setPlaceholderText("None")
        browse = QtWidgets.QPushButton("Browse...")
        clear = QtWidgets.QPushButton("Clear")
        row.addWidget(self.path, 1)
        row.addWidget(browse)
        row.addWidget(clear)
        self.lay.addLayout(row)
        browse.clicked.connect(self._browse)
        clear.clicked.connect(lambda: (self.set_value(""), self.changed.emit("", "now")))

    def set_value(self, value):
        self.path.setText(value or "")

    def _browse(self):
        path, _ = QtWidgets.QFileDialog.getOpenFileName(self, "Image for %s" % (self.inp.get("label") or ""), "",
                                                        "Images (*.png *.tga *.jpg *.jpeg *.exr *.tif *.tiff *.bmp *.psd)")
        if path:
            self.set_value(path)
            self.changed.emit(path, "now")


def make_editor(inp, color=None):
    widget, typ = inp.get("widget"), inp["type"]
    if typ == "image":
        return ImageEditor(inp)
    if widget == "togglebutton":
        return ToggleEditor(inp)
    if widget == "combobox":
        return ComboEditor(inp)
    if widget == "color" and typ in ("float3", "float4"):
        return ColorEditor(inp, color)
    if widget == "angle":
        return SliderEditor(inp, angle=True)
    return SliderEditor(inp)


class ElidedLabel(QtWidgets.QLabel):
    """Une ligne, coupée par … si elle ne tient pas ; texte entier en info-bulle."""

    def __init__(self):
        super().__init__()
        self.setSizePolicy(QtWidgets.QSizePolicy.Ignored, QtWidgets.QSizePolicy.Preferred)

    def setText(self, text):
        super().setText(text)
        self.setToolTip(text)

    def paintEvent(self, event):
        painter = QtGui.QPainter(self)
        painter.setPen(self.palette().color(QtGui.QPalette.WindowText))
        text = self.fontMetrics().elidedText(self.text(), QtCore.Qt.ElideRight, self.width())
        painter.drawText(self.rect(), QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter, text)


class ParamRow(object):
    def __init__(self, inp, editor, box, reset):
        self.inp, self.editor, self.box, self.reset = inp, editor, box, reset


class Params(QtWidgets.QWidget):
    """Paramètres d'un graphe, groupés dans l'ordre du fichier, conditions de visibilité appliquées. Disposition de
    Painter : paramètres sans groupe en tête, puis groupes repliables ; ↺ (retour au défaut) visible si la valeur a
    changé."""

    changed = QtCore.Signal(str, object, str)

    def __init__(self):
        super().__init__()
        self.lay = QtWidgets.QVBoxLayout(self)
        self.lay.setContentsMargins(0, 0, 0, 0)
        self.lay.setSpacing(10)
        self.rows = {}
        self.groups = []  # [(studio_qt.Group, None pour les paramètres sans groupe ; [identifiants])]
        self.graph = None
        self.color = None  # subzero_color.Color des nuanciers

    def set_color(self, color):
        self.color = color
        for row in self.rows.values():
            if isinstance(row.editor, ColorEditor):
                row.editor.set_color(color)

    def _row(self, inp, editor):
        """Ligne d'un paramètre : (widget, bouton ↺)."""
        box = QtWidgets.QWidget()
        lay = QtWidgets.QVBoxLayout(box)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(2)
        head = QtWidgets.QHBoxLayout()
        head.setSpacing(8)
        reset = QtWidgets.QToolButton()
        reset.setObjectName("reset")
        reset.setText("↺")
        reset.setToolTip("Reset to default")
        policy = reset.sizePolicy()
        policy.setRetainSizeWhenHidden(True)
        reset.setSizePolicy(policy)
        label = QtWidgets.QLabel(inp.get("label") or inp["identifier"])
        if inp.get("description"):
            label.setToolTip(inp["description"])
        # ↺ juste avant la valeur : le bord droit des valeurs, des curseurs et des menus reste aligné.
        if editor.kind == "stacked":
            head.addWidget(label, 1)
            head.addWidget(reset)
            if editor.value_widget is not None:
                head.addWidget(editor.value_widget)
            lay.addLayout(head)
            lay.addWidget(editor)
        elif editor.kind == "check":
            head.addWidget(editor, 1)
            head.addWidget(reset)
            lay.addLayout(head)
        else:
            column = QtWidgets.QWidget()
            column.setFixedWidth(120)
            left = QtWidgets.QHBoxLayout(column)
            left.setContentsMargins(0, 0, 0, 0)
            left.setSpacing(4)
            left.addWidget(label, 1)
            left.addWidget(reset)
            head.addWidget(column)
            head.addWidget(editor, 1)
            lay.addLayout(head)
        return box, reset

    def build(self, graph, values):
        while self.lay.count():
            item = self.lay.takeAt(0)
            if item.widget() is not None:
                item.widget().deleteLater()
        self.rows, self.groups, self.graph = {}, [], graph
        order, members = [], {}
        for inp in graph["inputs"]:
            if not shown(inp):
                continue
            g = inp.get("group") or ""
            if g not in members:
                order.append(g)
                members[g] = []
            members[g].append(inp)
        order.sort(key=lambda g: g != "")  # sans groupe d'abord, comme dans Painter
        for g in order:
            if g:
                group = studio_qt.Group(g.replace("/", " / "))
                self.lay.addWidget(group)
                body = group.body
            else:
                group, body = None, self.lay
            for inp in members[g]:
                editor = make_editor(inp, self.color)
                editor.set_value(values.get(inp["identifier"], inp["default"]))
                box, reset = self._row(inp, editor)
                body.addWidget(box)
                ident = inp["identifier"]
                editor.changed.connect(lambda v, mode, ident=ident: self.changed.emit(ident, v, mode))
                reset.clicked.connect(lambda _=False, ident=ident: self._reset(ident))
                self.rows[ident] = ParamRow(inp, editor, box, reset)
            self.groups.append((group, [i["identifier"] for i in members[g]]))
        self.lay.addStretch(1)
        self.refresh(values)

    def _reset(self, ident):
        row = self.rows[ident]
        row.editor.set_value(row.inp["default"])
        self.changed.emit(ident, row.inp["default"], "now")

    def set_values(self, values):
        for ident, row in self.rows.items():
            row.editor.set_value(values.get(ident, row.inp["default"]))
        self.refresh(values)

    def refresh(self, values):
        if self.graph is None:
            return
        env = env_values(self.graph, values)
        for ident, row in self.rows.items():
            cond = row.inp.get("visibleif")
            row.box.setVisible(not cond or condition(cond, env))
            row.reset.setVisible(values.get(ident, row.inp["default"]) != row.inp["default"])
        for group, idents in self.groups:
            if group is not None:
                group.setVisible(any(not self.rows[i].box.isHidden() for i in idents))

    def modified(self):
        return any(not row.reset.isHidden() for row in self.rows.values())


# ---- Sans ArmorPaint (lot E9) -----------------------------------------------------------------------------------------


class LocalHost(QtCore.QObject):
    """Tient lieu d'ArmorPaint (subzero_app.py) : même interface que studio_qt.Channel (request, events, closed). L'état
    vient de l'environnement ; créer ou mettre à jour un matériau, c'est l'avoir exporté (la fenêtre a déjà écrit les
    textures et la recette). Réponses différées comme celles d'un vrai hôte."""

    closed = QtCore.Signal()
    standalone = True

    def __init__(self, state):
        super().__init__()
        self.state = state
        self.events = {}

    def request(self, op, args, callback):
        args = args or {}
        if op == "state":
            reply, error = dict(self.state), None
        elif op in ("create", "update"):
            reply, error = {"material": args.get("name"), "kept": []}, None
        else:
            reply, error = None, {"code": "unsupported", "message": "%s: not available without ArmorPaint" % op}
        QtCore.QTimer.singleShot(0, lambda: callback(reply, error))


# ---- Fenêtre -------------------------------------------------------------------------------------------------------------


# ---- Réglages (lot E11) -----------------------------------------------------------------------------------------------

WINDOW = "window"  # origine d'une variable changée par Apply, pas enregistrée


class SettingsDialog(QtWidgets.QDialog):
    """Panneau Settings : gestion couleur (celle d'ArmorPaint, ou OCIO et STUDIO_OCIO_* sans lui), espace des cartes
    livrées, espace de choix des couleurs. Chaque réglage est une variable d'environnement : Apply la change pour la
    fenêtre, Save l'écrit aussi dans studio.env. Une vraie variable d'environnement ne se change pas ici : studio.env ne
    passe pas devant."""

    def __init__(self, window):
        super().__init__(window)
        self.window = window
        self.fields = {}  # variable -> (widget, valeur voulue : fonction, None pour le défaut)
        self.setWindowTitle("subzero Settings")
        self.setModal(False)
        self.resize(540, 680)
        # Même facture que la fenêtre : sections défilantes, barre du bas (fichier, état, boutons).
        root = QtWidgets.QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)
        self.banner = QtWidgets.QLabel()
        self.banner.setObjectName("banner")
        self.banner.setWordWrap(True)
        self.banner.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
        self.banner.hide()
        banner_bar = QtWidgets.QFrame()
        banner_bar.setObjectName("bar")
        banner_lay = QtWidgets.QVBoxLayout(banner_bar)
        banner_lay.setContentsMargins(10, 10, 10, 10)
        banner_lay.addWidget(self.banner)
        self.banner.setVisible(False)
        self.banner_bar = banner_bar
        root.addWidget(banner_bar)
        self.scroll = QtWidgets.QScrollArea()
        self.scroll.setWidgetResizable(True)
        self.scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        self.scroll.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
        root.addWidget(self.scroll, 1)
        bottom = QtWidgets.QFrame()
        bottom.setObjectName("bar")
        foot = QtWidgets.QVBoxLayout(bottom)
        foot.setContentsMargins(10, 8, 10, 8)
        foot.setSpacing(6)
        root.addWidget(bottom)
        self.file_label = muted()
        self.file_label.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
        foot.addWidget(self.file_label)
        buttons = QtWidgets.QHBoxLayout()
        buttons.setSpacing(8)
        self.status = muted()
        close = QtWidgets.QPushButton("Close")
        self.apply_button = QtWidgets.QPushButton("Apply")
        self.apply_button.setToolTip("Use these settings in this window, without saving them")
        self.save_button = QtWidgets.QPushButton("Save")
        self.save_button.setObjectName("primary")
        buttons.addWidget(self.status, 1)
        buttons.addWidget(close)
        buttons.addWidget(self.apply_button)
        buttons.addWidget(self.save_button)
        foot.addLayout(buttons)
        close.clicked.connect(self.close)
        self.apply_button.clicked.connect(lambda: self._apply(False))
        self.save_button.clicked.connect(lambda: self._apply(True))
        self.reload()

    def reload(self):
        """Reconstruit le panneau depuis la fenêtre : configuration, variables et leur origine."""
        w, c = self.window, self.window.color
        self.fields = {}
        old = self.scroll.takeWidget()
        if old is not None:
            old.deleteLater()
        body = QtWidgets.QWidget()
        self.lay = QtWidgets.QVBoxLayout(body)
        self.lay.setContentsMargins(12, 10, 12, 14)
        self.lay.setSpacing(8)
        if w.host_mode:
            self._host_section(c)
        else:
            self._standalone_section(c)
        self._maps_section(c)
        self._colors_section(c)
        self.lay.addStretch(1)
        self.scroll.setWidget(body)
        notes = ([c.error] if c.error else []) + c.warnings + [e for _, _, e in w.plan().values() if e]
        if c.ready and c.picking()[1]:
            notes.append(c.picking()[1])
        self.banner.setText("\n".join(notes))
        self.banner.setVisible(bool(notes))
        self.banner_bar.setVisible(bool(notes))
        target = w.env_file()
        self.save_button.setEnabled(bool(target))
        self.file_label.setText("Save writes to %s" % target if target else
                                "Nothing to save to: $STUDIO_ENV is empty. Apply still works for this window.")

    # -- construction

    def _section(self, title, note=None):
        """Section à la Painter : trait (sauf la première), titre en capitales, note."""
        if self.lay.count():
            self.lay.addSpacing(4)
            self.lay.addWidget(studio_qt.separator())
        self.lay.addWidget(studio_qt.section_title(title))
        if note:
            self.lay.addWidget(muted(note))

    def _grid(self, parent=None):
        grid = QtWidgets.QGridLayout()
        grid.setContentsMargins(0, 2, 0, 2)
        grid.setHorizontalSpacing(8)
        grid.setVerticalSpacing(3)
        grid.setColumnMinimumWidth(0, 110)
        grid.setColumnStretch(1, 1)
        if parent is None:
            self.lay.addLayout(grid)
        else:
            parent.setLayout(grid)
        return grid

    def _row(self, grid, label, widget, var=None, value=None):
        """Ligne libellé, réglage ; var : sa variable, montrée dessous avec son origine ; value : sa valeur voulue."""
        r = grid.rowCount()
        grid.addWidget(QtWidgets.QLabel(label), r, 0)
        grid.addWidget(widget, r, 1)
        if var is None:
            return
        grid.addWidget(self._origin(var), r + 1, 1)
        grid.setRowMinimumHeight(r + 2, 6)
        if var in self.window.locked:
            widget.setEnabled(False)
            widget.setToolTip("$%s is set in the environment: studio.env cannot change it" % var)
        self.fields[var] = (widget, value)

    def _origin(self, var):
        where = self.window.origins.get(var)
        if where == studio_env.ENVIRONMENT:
            text, tip = "environment (locked)", "Set before ArmorPaint started: it wins over studio.env"
        elif where == WINDOW:
            text, tip = "this window, not saved", "Applied here only: Save writes it to studio.env"
        elif where:
            text, tip = os.path.basename(where), where
        else:
            text, tip = "not set", "Default value"
        label = QtWidgets.QLabel("$%s · %s" % (var, text))
        label.setObjectName("caption")
        label.setToolTip(tip)
        return label

    @staticmethod
    def _combo(default_text, items, current, describe=None):
        """Menu : défaut (None), puis items [(texte, valeur)] ou None pour un séparateur ; current : valeur de la
        variable, ajoutée si le menu ne l'a pas (alias, rôle, inconnue)."""
        combo = QtWidgets.QComboBox()
        combo.setMinimumContentsLength(16)
        combo.setSizeAdjustPolicy(QtWidgets.QComboBox.AdjustToMinimumContentsLengthWithIcon)
        combo.addItem(default_text, None)
        for item in items:
            if item is None:
                combo.insertSeparator(combo.count())
            else:
                combo.addItem(item[0], item[1])
        if current:
            i = combo.findData(current)
            if i < 0:
                combo.addItem(describe(current) if describe else "%s (unknown)" % current, current)
                i = combo.count() - 1
            combo.setCurrentIndex(i)
        return combo

    def _space_row(self, grid, label, var, default_text, c):
        items = []
        if c.ready:
            items = [(c.describe(r), r) for r in c.roles()] + [None] + [(n, n) for _, n in c.spaces()]

        def describe(value):
            return c.describe(value) if c.ready and c.resolve(value) else "%s (unknown)" % value

        combo = self._combo(default_text, items, (os.environ.get(var) or "").strip(), describe)
        combo.setEnabled(c.ready)
        if not c.ready:
            combo.setToolTip(c.error or "")
        self._row(grid, label, combo, var, combo.currentData)

    def _host_section(self, c):
        host = self.window.state.get("ocio") or {}
        self._section("Color management", "Set in ArmorPaint: subzero converts the maps in the configuration that "
                                          "reads them, and shows thumbnails and colors like its viewport.")
        grid = self._grid()

        def value(text, tip=""):
            label = QtWidgets.QLabel(text or "—")
            label.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
            label.setToolTip(tip)
            return label

        origin = host.get("origin") or ""
        self._row(grid, "Configuration", value(host.get("name") or c.name,
                                               "%s%s" % (host.get("config") or "", " (%s)" % origin if origin else "")))
        if not host.get("enabled"):
            self._row(grid, "Status", value(host.get("status") or "off"))
        self._row(grid, "Display", value(host.get("display")))
        self._row(grid, "View", value(host.get("view")))
        self._row(grid, "Look", value(host.get("look") or "None"))
        self._row(grid, "Viewport", value("view transform on" if host.get("viewport") else "off: shown as rendered"))
        prefs = QtWidgets.QPushButton("Color Preferences...")
        prefs.clicked.connect(lambda: self.window.channel.request("show_prefs", {"tab": "color"}, lambda d, e: None))
        row = QtWidgets.QHBoxLayout()
        row.addWidget(prefs)
        row.addStretch(1)
        self.lay.addLayout(row)

    def _standalone_section(self, c):
        self._section("Color management", "Without ArmorPaint, the same variables as ArmorPaint: $OCIO gives the "
                                          "configuration; display, view and look show thumbnails and colors.")
        grid = self._grid()
        edit = QtWidgets.QLineEdit(os.environ.get("OCIO", ""))
        edit.setPlaceholderText(subzero_color.DEFAULT_CONFIG)
        browse = QtWidgets.QPushButton("Browse...")
        holder = QtWidgets.QWidget()
        row = QtWidgets.QHBoxLayout(holder)
        row.setContentsMargins(0, 0, 0, 0)
        row.addWidget(edit, 1)
        row.addWidget(browse)
        browse.clicked.connect(lambda: self._browse_config(edit))
        self._row(grid, "Configuration", holder, "OCIO", lambda: edit.text().strip() or None)
        if not c.ready:
            return
        cfg = c.config
        display = self._combo("Default (%s)" % cfg.getDefaultDisplay(), [(d, d) for d in c.displays()],
                              os.environ.get("STUDIO_OCIO_DISPLAY"))
        view = QtWidgets.QComboBox()

        def fill_views(keep):
            d = display.currentData()
            d = d if d in c.displays() else cfg.getDefaultDisplay()  # inconnu : remplacé par le défaut, comme Color
            view.clear()
            view.addItem("Default (%s)" % cfg.getDefaultView(d), None)
            for v in c.views(d):
                view.addItem(v, v)
            i = view.findData(keep) if keep else 0
            if i < 0:
                view.addItem("%s (unknown)" % keep, keep)
                i = view.count() - 1
            view.setCurrentIndex(i)

        fill_views(os.environ.get("STUDIO_OCIO_VIEW"))
        display.currentIndexChanged.connect(lambda _: fill_views(view.currentData()))
        look = self._combo("None", [(lk, lk) for lk in c.looks()], os.environ.get("STUDIO_OCIO_LOOK"))
        viewport = QtWidgets.QCheckBox("Show thumbnails and colors through the view transform")
        viewport.setChecked(subzero_color.env_flag(os.environ.get("STUDIO_OCIO_VIEWPORT"), True))
        self._row(grid, "Display", display, "STUDIO_OCIO_DISPLAY", display.currentData)
        self._row(grid, "View", view, "STUDIO_OCIO_VIEW", view.currentData)
        self._row(grid, "Look", look, "STUDIO_OCIO_LOOK", look.currentData)
        self._row(grid, "Viewport", viewport, "STUDIO_OCIO_VIEWPORT", lambda: None if viewport.isChecked() else "0")

    def _browse_config(self, edit):
        start = edit.text() if os.path.isfile(edit.text()) else os.path.expanduser("~")
        path, _ = QtWidgets.QFileDialog.getOpenFileName(self, "OCIO configuration", start,
                                                        "OCIO configurations (*.ocio);;All files (*)")
        if path:
            edit.setText(path)

    def _maps_section(self, c):
        src = c.source("base") if c.ready else None
        data = c.data_space() if c.ready else None
        self._section("Maps", "Color space of the textures sent to ArmorPaint. sbsrender writes color maps sRGB-encoded "
                              "and data maps raw; another space converts them after rendering (EXR keeps the precision).")
        grid = self._grid()
        self._space_row(grid, "Color maps", subzero_color.VAR_COLOR, "Default: as rendered (%s)" % (src or "sRGB"), c)
        self._space_row(grid, "Data maps", subzero_color.VAR_DATA, "Default: %s" % (data or "Raw"), c)
        group = studio_qt.Group("Per map", opened=any(os.environ.get(v) for _, v in MAP_VARS))
        holder = QtWidgets.QWidget()
        per_map = self._grid(holder)
        per_map.setColumnMinimumWidth(0, 110 - studio_qt.Group.INDENT)  # réglages alignés sur ceux du dessus
        for ch, var in MAP_VARS:
            kind = "color maps" if ch in subzero_color.COLOR_MAPS else "data maps"
            self._space_row(per_map, CHANNEL_LABEL[ch], var, "Same as %s" % kind, c)
        group.body.addWidget(holder)
        self.lay.addWidget(group)

    def _colors_section(self, c):
        self._section("Colors", "Space of the color pickers of the parameters; swatches show colors like the thumbnails.")
        grid = self._grid()
        picking = (c.resolve("color_picking") or c.source("base")) if c.ready else None
        self._space_row(grid, "Color picking", subzero_color.VAR_PICKING,
                        "Default: color_picking (%s)" % picking if picking else "Default: color_picking", c)

    # -- actions

    def desired(self):
        """{variable: valeur voulue, None pour le défaut} des réglages modifiables."""
        return {var: value() for var, (widget, value) in self.fields.items() if widget.isEnabled()}

    def _apply(self, save):
        error = self.window.apply_settings(self.desired(), save=save)
        if error:
            self.status.setText(error)
            self.status.setStyleSheet("color: %s;" % ERROR)
        else:
            target = self.window.env_file()
            self.status.setText("Saved to %s" % os.path.basename(target) if save else "Applied to this window")
            self.status.setStyleSheet("color: %s;" % SUCCESS)


class SubzeroWindow(QtWidgets.QWidget):
    def __init__(self, channel, mac, test=None):
        # Sans ArmorPaint (lot E9, subzero_app.py) : fenêtre ordinaire, qui exporte dans un dossier.
        self.standalone = bool(getattr(channel, "standalone", False))
        flags = QtCore.Qt.Window if self.standalone else QtCore.Qt.Window | QtCore.Qt.WindowStaysOnTopHint
        super().__init__(None, flags)
        self.channel = channel
        self.mac = mac
        self.test = test
        self.template, self.template_error = NAME_TEMPLATE, None
        if self.standalone:
            wanted = os.environ.get("SUBZERO_EXPORT_TEMPLATE") or NAME_TEMPLATE
            self.template_error = check_template(wanted)
            if self.template_error is None:
                self.template = wanted
            else:
                log(self.template_error)
        self.test_log = {}
        self.state = {}
        self.tools = None
        self.tools_error = None
        self.package = None  # {"sbsar", "description", "cook", "path"}
        self.graph = None
        self.values = {}
        self.entries = {}
        self.preset = None
        self.size = None  # log2, None : celle du projet
        self.fmt = subzero_render.default_format()  # format du rendu final (clé de subzero_render.FORMATS)
        self.final_seq = 0  # rendus finaux lancés : le nettoyage suit la réponse du dernier
        self.mapping = []  # [[sortie, canal, raison]]
        self.overrides = {}  # sortie -> canal choisi à la main ("" : aucun)
        self.material_dir = None  # dossier du matériau envoyé
        self.material_name = None
        self.base_dir = None  # dossier de base choisi pour un projet jamais enregistré
        self.pending = None  # rendu final en attente d'envoi (une requête create/update est en cours)
        self.request_busy = False
        self.dirty = False  # réglages changés depuis le dernier rendu final
        self.preview_dir = os.path.join(subzero_render.CACHE_DIR, "preview", str(os.getpid()))
        self.job = None  # dernier travail soumis
        self.color = subzero_color.Color(host={"enabled": False, "status": "waiting for ArmorPaint"})
        self.origins = {}  # variable de couleur -> fichier, studio_env.ENVIRONMENT ou WINDOW
        self.locked = set()  # variables de l'environnement, que studio.env ne change pas
        self.applied_spaces = {}  # {canal: espace} des textures du matériau envoyé, pour color_space_was
        self.previous_files = []  # noms des textures du matériau envoyé (recette), nettoyées si un rendu les remplace
        self.thumb_files = None  # ({canal: fichier}, {canal: espace}) des vignettes montrées
        self.settings = None  # SettingsDialog
        self.worker = Worker()
        self.worker.stepped.connect(self._on_step)
        self.worker.finished.connect(self._on_finished)
        self.soon = QtCore.QTimer(self, singleShot=True, interval=SOON_MS)
        self.soon.timeout.connect(self._commit)
        self.setWindowTitle("subzero")
        self.setAcceptDrops(True)
        self._build()
        self._update_buttons()
        viewer = (test or {}).get("viewer")
        self.viewer_button.setChecked(env_flag("SUBZERO_VIEWER", self.standalone) if viewer is None else bool(viewer))
        self.viewer.setVisible(self.viewer_button.isChecked())
        if self.browser is not None:
            shown = (test or {}).get("browser")
            self.browser_button.setChecked(self.browser.config.get("visible", True) if shown is None else bool(shown))
            self.browser.setVisible(self.browser_button.isChecked())
            if (test or {}).get("browser_add"):
                self.browser.add_folder(test["browser_add"])
        self._initial_geometry()
        self.viewer_button.toggled.connect(lambda on: self._toggle_panel(self.viewer, on))
        if self.browser is not None:
            self.browser_button.toggled.connect(lambda on: self._toggle_panel(self.browser, on))
        channel.events["raise"] = lambda m: self.to_front()
        channel.events["open"] = self._on_open_event
        channel.events["prefs"] = self._on_prefs_event
        channel.events["project"] = self._on_project_event
        channel.events["ocio"] = self._on_ocio_event
        channel.closed.connect(self._on_host_gone)
        self._status("Loading...")
        channel.request("state", None, self._on_state)

    # -- construction

    def _build(self):
        # Disposition du panneau Shader Settings de Painter : barre du haut (matériau, boutons de même largeur, nom,
        # vignettes), sections défilantes (MATERIAL, PARAMETERS, OUTPUTS), barre du bas (état, envoi). À gauche de
        # cette colonne, le viewer.
        outer = QtWidgets.QHBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)
        self.viewer = subzero_viewer.Viewer(CHANNEL_LABEL)
        self.viewer.mapChanged.connect(lambda ch: self._highlight_thumbs())
        self.viewer_keys = subzero_viewer.Keys(self, self.viewer)
        self.browser = subzero_browser.Browser() if self.standalone else None
        if self.browser is not None:
            self.browser.opened.connect(lambda path: self._open({"file": path}))
            outer.addWidget(self.browser)
        outer.addWidget(self.viewer)
        column = QtWidgets.QWidget()
        column.setMinimumWidth(440)
        outer.addWidget(column, 1)
        root = QtWidgets.QVBoxLayout(column)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        top = QtWidgets.QFrame()
        top.setObjectName("bar")
        head = QtWidgets.QVBoxLayout(top)
        head.setContentsMargins(10, 10, 10, 10)
        head.setSpacing(7)
        header = QtWidgets.QHBoxLayout()
        header.setSpacing(10)
        self.icon = QtWidgets.QLabel()
        self.icon.setFixedSize(48, 48)
        self.icon.setStyleSheet("background: #252525; border-radius: 3px;")
        titles = QtWidgets.QVBoxLayout()
        titles.setSpacing(2)
        self.title = QtWidgets.QLabel("No material")
        self.title.setObjectName("title")
        self.subtitle = muted("Open a .sbs or .sbsar file, or drop one here.")
        titles.addStretch(1)
        titles.addWidget(self.title)
        titles.addWidget(self.subtitle)
        titles.addStretch(1)
        header.addWidget(self.icon)
        header.addLayout(titles, 1)
        head.addLayout(header)

        buttons = QtWidgets.QHBoxLayout()
        buttons.setSpacing(6)
        self.open_button = QtWidgets.QPushButton("Open...")
        self.reload_button = QtWidgets.QPushButton("Reload")
        self.reload_button.setToolTip("Read the file again (after saving it in Designer)")
        self.settings_button = QtWidgets.QPushButton("Settings")
        self.settings_button.setToolTip("Color spaces of the maps and colors (OCIO)")
        self.viewer_button = QtWidgets.QPushButton("Viewer")
        self.viewer_button.setCheckable(True)
        self.viewer_button.setToolTip("Show the rendered maps (keys 1 to 9 switch maps)")
        self.browser_button = QtWidgets.QPushButton("Browser")
        self.browser_button.setCheckable(True)
        self.browser_button.setToolTip("Packages of the library folders ($SUBZERO_LIBRARY, and folders added with +)")
        self.browser_button.setVisible(self.standalone)
        for b in (self.open_button, self.reload_button, self.settings_button, self.viewer_button, self.browser_button):
            buttons.addWidget(b, 1)
        head.addLayout(buttons)
        self.open_button.clicked.connect(self._browse_package)
        self.reload_button.clicked.connect(self._reload)
        self.settings_button.clicked.connect(self._show_settings)

        form = QtWidgets.QGridLayout()
        form.setHorizontalSpacing(8)
        form.setVerticalSpacing(6)
        form.setColumnMinimumWidth(0, 120)
        form.setColumnStretch(1, 1)
        self.graph_label = QtWidgets.QLabel("Graph")
        self.graph_combo = QtWidgets.QComboBox()
        self.name = QtWidgets.QLineEdit()
        self.name.setPlaceholderText("Material name")
        form.addWidget(self.graph_label, 0, 0)
        form.addWidget(self.graph_combo, 0, 1)
        form.addWidget(QtWidgets.QLabel("Material name"), 1, 0)
        form.addWidget(self.name, 1, 1)
        head.addLayout(form)
        self.graph_combo.activated.connect(self._on_graph)

        self.banner = QtWidgets.QLabel()
        self.banner.setObjectName("banner")
        self.banner.setWordWrap(True)
        self.banner.hide()
        self.prefs_button = QtWidgets.QPushButton("Locate sbsrender..." if self.standalone else "Open Preferences")
        self.prefs_button.hide()
        if self.standalone:
            self.prefs_button.clicked.connect(self._pick_sbsrender)
        else:
            self.prefs_button.clicked.connect(lambda: self.channel.request("show_prefs", None, lambda d, e: None))
        banner_row = QtWidgets.QHBoxLayout()
        banner_row.addWidget(self.banner, 1)
        banner_row.addWidget(self.prefs_button, 0, QtCore.Qt.AlignVCenter)
        head.addLayout(banner_row)

        thumbs = QtWidgets.QHBoxLayout()
        thumbs.setSpacing(6)
        self.thumbs = {}
        for ch, label, _, _ in CHANNELS:
            box = QtWidgets.QVBoxLayout()
            box.setContentsMargins(0, 0, 0, 0)
            box.setSpacing(3)
            image = QtWidgets.QLabel()
            image.setFixedSize(THUMB, THUMB)
            image.setAlignment(QtCore.Qt.AlignCenter)
            image.setStyleSheet("background: #252525; border-radius: 2px;")
            image.installEventFilter(self)
            image.setProperty("channel", ch)
            caption = QtWidgets.QLabel(label)
            caption.setObjectName("caption")
            caption.setAlignment(QtCore.Qt.AlignCenter)
            box.addWidget(image)
            box.addWidget(caption)
            holder = QtWidgets.QWidget()
            holder.setLayout(box)
            holder.hide()
            thumbs.addWidget(holder)
            self.thumbs[ch] = (holder, image)
        thumbs.addStretch(1)
        head.addLayout(thumbs)
        root.addWidget(top)

        body = QtWidgets.QWidget()
        content = QtWidgets.QVBoxLayout(body)
        content.setContentsMargins(12, 10, 12, 14)
        content.setSpacing(10)

        content.addWidget(studio_qt.section_title("Material"))
        grid = QtWidgets.QGridLayout()
        grid.setHorizontalSpacing(8)
        grid.setVerticalSpacing(8)
        grid.setColumnMinimumWidth(0, 120)
        grid.setColumnStretch(1, 1)
        self.preset_combo = QtWidgets.QComboBox()
        self.seed = QtWidgets.QSpinBox()
        self.seed.setRange(0, 10 ** 6)
        self.seed.setKeyboardTracking(False)
        self.seed.setButtonSymbols(QtWidgets.QAbstractSpinBox.NoButtons)
        self.seed_button = QtWidgets.QPushButton("Random")
        self.size_combo = QtWidgets.QComboBox()
        self.format_combo = QtWidgets.QComboBox()
        for key, f in subzero_render.FORMATS.items():
            self.format_combo.addItem(f["label"], key)
        self.format_combo.setToolTip("File format of the %s (previews stay in TGA)" % (
            "exported textures" if self.standalone else "textures sent to ArmorPaint"))
        seed_row = QtWidgets.QHBoxLayout()
        seed_row.setSpacing(6)
        seed_row.addWidget(self.seed, 1)
        seed_row.addWidget(self.seed_button)
        self.seed_label = QtWidgets.QLabel("Random Seed")
        grid.addWidget(QtWidgets.QLabel("Preset"), 0, 0)
        grid.addWidget(self.preset_combo, 0, 1)
        grid.addWidget(self.seed_label, 1, 0)
        grid.addLayout(seed_row, 1, 1)
        size_row = QtWidgets.QHBoxLayout()
        size_row.setSpacing(8)
        size_row.addWidget(self.size_combo, 1)
        size_row.addSpacing(4)
        size_row.addWidget(QtWidgets.QLabel("Format"))
        size_row.addWidget(self.format_combo, 1)
        grid.addWidget(QtWidgets.QLabel("Size"), 2, 0)
        grid.addLayout(size_row, 2, 1)
        content.addLayout(grid)
        self.preset_combo.activated.connect(self._on_preset)
        self.seed.valueChanged.connect(lambda v: self._on_param("$randomseed", v, "now"))
        self.seed_button.clicked.connect(lambda: self.seed.setValue(int.from_bytes(os.urandom(3), "little") % 10 ** 6))
        self.size_combo.activated.connect(self._on_size)
        self.format_combo.activated.connect(self._on_format)

        content.addSpacing(4)
        content.addWidget(studio_qt.separator())
        content.addWidget(studio_qt.section_title("Parameters"))
        self.params = Params()
        self.params.changed.connect(self._on_param)
        content.addWidget(self.params)
        restore = QtWidgets.QHBoxLayout()
        restore.addStretch(1)
        self.restore_button = QtWidgets.QPushButton("Restore defaults")
        self.restore_button.setToolTip("Default value of every parameter (the random seed is kept)")
        restore.addWidget(self.restore_button)
        content.addLayout(restore)
        self.restore_button.clicked.connect(lambda: (self.preset_combo.setCurrentIndex(0), self._on_preset(0)))

        content.addSpacing(4)
        content.addWidget(studio_qt.separator())
        content.addWidget(studio_qt.section_title("Outputs"))
        self.outputs_group = studio_qt.Group("Channels", opened=False)
        self.outputs_head = self.outputs_group.head
        self.outputs = QtWidgets.QTreeWidget()
        self.outputs.setHeaderLabels(["Output", "Channel"])
        self.outputs.setRootIsDecorated(False)
        self.outputs.setMaximumHeight(210)
        self.outputs_group.body.addWidget(self.outputs)
        content.addWidget(self.outputs_group)
        content.addStretch(1)

        scroll = QtWidgets.QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        scroll.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
        scroll.setWidget(body)
        root.addWidget(scroll, 1)

        bottom = QtWidgets.QFrame()
        bottom.setObjectName("bar")
        actions = QtWidgets.QHBoxLayout(bottom)
        actions.setContentsMargins(10, 8, 10, 8)
        actions.setSpacing(8)
        self.status = ElidedLabel()
        self.live = QtWidgets.QCheckBox("Live")
        self.live.setChecked(env_flag("SUBZERO_LIVE", True))
        self.live.setToolTip("Export the textures again after each change" if self.standalone else
                             "Update the material in ArmorPaint after each change")
        self.duplicate_button = QtWidgets.QPushButton("Duplicate")
        self.duplicate_button.setToolTip("Export to a new folder" if self.standalone else
                                         "Send as a new material, in a new folder")
        self.reveal_button = QtWidgets.QPushButton("Show")
        self.reveal_button.setToolTip("Open the export folder")
        self.reveal_button.clicked.connect(self._reveal)
        self.send_button = QtWidgets.QPushButton("Export" if self.standalone else "Send to ArmorPaint")
        self.send_button.setObjectName("primary")
        self.send_button.setDefault(True)
        actions.addWidget(self.status, 1)
        actions.addWidget(self.live)
        actions.addWidget(self.reveal_button)
        actions.addWidget(self.duplicate_button)
        actions.addWidget(self.send_button)
        root.addWidget(bottom)
        self.live.toggled.connect(lambda on: (self._update_buttons(), on and self.dirty and self._commit()))
        self.send_button.clicked.connect(self._send)
        self.duplicate_button.clicked.connect(lambda: self._send(duplicate=True))

    def _status(self, text, color=None):
        self.status.setText(text)
        self.status.setStyleSheet("color: %s;" % (color or MUTED))

    def _update_buttons(self):
        ready = self.graph is not None and self.tools is not None and any(ch for _, ch, _ in self.mapping)
        sent = self.material_dir is not None
        self.send_button.setText("Update" if sent else "Export" if self.standalone else "Send to ArmorPaint")
        if self.standalone:
            self.send_button.setToolTip("Export again to %s" % self.material_dir if sent else
                                        "Render the textures at full size into a folder")
        self.reveal_button.setVisible(self.standalone and sent)
        self.send_button.setEnabled(ready and (not sent or self.dirty or not self.live.isChecked()))
        self.duplicate_button.setVisible(sent)
        self.duplicate_button.setEnabled(ready)
        self.reload_button.setEnabled(self.package is not None)
        self.restore_button.setEnabled(self.graph is not None)
        self.live.setVisible(sent)

    # -- état et évènements d'ArmorPaint

    def _on_state(self, data, error):
        if error:
            self._status("ArmorPaint: %s" % error.get("message"), ERROR)
            return
        self.state = data or {}
        self.test_log["state"] = self.state
        self._read_origins()
        self._sync_env()
        self._make_color()
        default = "Default (%d)" if self.standalone else "Project (%d)"
        sizes = [(default % self._project_res(), None)] + [(str(1 << n), n) for n in (9, 10, 11, 12, 13)]
        self.size_combo.clear()
        for label, n in sizes:
            self.size_combo.addItem(label, n)
        opening = (self.test or {}).get("open") or self.state.get("open")
        if self.template_error:
            self._status(self.template_error, ERROR)
        self._locate(then=lambda: self._open(opening) if opening else self._ready())

    def _project_res(self):
        res = self.state.get("resolution") or [2048, 2048]
        return int(res[0])

    def _locate(self, then=None):
        path = self.state.get("sbsrender", "")

        def job(track, step):
            return subzero_render.locate(path)

        self._pending_then = then
        self._submit(job, "locate")

    def _ready(self):
        t = self.test
        if t is not None and t.get("browser_open") and self.browser is not None and not t.get("_browsed"):
            t["_browsed"] = True
            self.browser.flush()
            path = self.browser.find(t["browser_open"])
            self.test_log["browser_open"] = path
            if path:
                self.browser.opened.emit(path)
                return
        if t is not None:
            self._run_test()

    def _on_open_event(self, message):
        self.to_front()
        target = {k: message[k] for k in ("file", "dir") if message.get(k)}
        if target:
            self._open(target)

    def _on_prefs_event(self, message):
        # Chemin changé dans les préférences : nouvelle détection, qui remplace celle en cours ; la suite de celle-ci
        # (ouverture au démarrage) passe avant le rendu du réglage courant.
        self.state["sbsrender"] = message.get("sbsrender", "")
        before = self._pending_then

        def then():
            if before is not None:
                before()
            elif self.graph is not None:
                self._commit()

        self._locate(then=then)

    def _on_project_event(self, message):
        # Autre projet : le matériau envoyé n'y est plus ; le prochain envoi en crée un.
        ocio = self.state.get("ocio")
        self.state.update(message.get("state") or {})
        self.material_dir = None
        self.material_name = None
        self.base_dir = None
        self.applied_spaces = {}
        self._update_buttons()
        if self.state.get("ocio") != ocio:  # l'état envoyé tient lieu d'évènement ocio
            self._color_changed()

    def _on_ocio_event(self, message):
        # Configuration, affichage du viewport ou import OCIO changés dans ArmorPaint.
        self.state["ocio"] = (message.get("state") or {}).get("ocio")
        self._color_changed()

    def _on_host_gone(self):
        log("ArmorPaint closed the connection")
        QtWidgets.QApplication.instance().quit()

    # -- couleur (lot E11)

    @property
    def host_mode(self):
        """Vrai si ArmorPaint donne sa gestion couleur (état avec "ocio") ; faux sans lui : OCIO et STUDIO_OCIO_*."""
        return "ocio" in self.state

    def _managed_vars(self):
        return SUBZERO_VARS if self.host_mode else COLOR_VARS

    def env_file(self):
        """studio.env où Save écrit : celui d'ArmorPaint, sinon celui de studio_env ; "" ou None sans fichier."""
        if "env" in self.state:
            return (self.state.get("env") or {}).get("file") or ""
        return studio_env.write_target()

    def _read_origins(self):
        env = self.state.get("env")
        if env is not None:
            origins = dict(env.get("origins") or {})
        else:
            origins = {n: studio_env.origin(n) for n in COLOR_VARS if n in os.environ}
        self.locked = {n for n, o in origins.items() if o == studio_env.ENVIRONMENT}
        self.origins = origins

    def _sync_env(self):
        """Variables de couleur relues dans studio.env : ArmorPaint ne l'a lu qu'à son démarrage, et un réglage
        enregistré depuis doit compter sans le relancer. Celles de l'environnement restent."""
        try:
            found = studio_env.defined()
        except OSError as e:
            log("studio.env: %s" % e)
            return
        for name in self._managed_vars():
            if name in self.locked:
                continue
            if name in found:
                os.environ[name], self.origins[name] = found[name]
            else:
                os.environ.pop(name, None)
                self.origins.pop(name, None)

    def _make_color(self):
        self.color = subzero_color.Color(host=self.state.get("ocio") if self.host_mode else None)
        c = self.color
        log("color: %s" % (c.error or "%s (%s, %s)" % (c.name, c.config_source, c.origin)))
        for w in c.warnings:
            log("color: %s" % w)
        self.params.set_color(c)
        self.viewer.set_color(c)
        self.settings_button.setToolTip("Color spaces of the maps and colors (OCIO)" + ("\n" + c.error if c.error else ""))

    def _source(self, channel):
        """Espace de ce qu'écrit sbsrender pour le canal, None sans OCIO."""
        return self.color.source(channel) if self.color.ready else None

    def plan(self):
        """{canal: (source, cible, erreur)} de tous les canaux ; {} sans OCIO."""
        if not self.color.ready:
            return {}
        return self.color.plan([(ch, CHANNEL_SUFFIX[ch]) for ch in CHANNEL_LABEL])

    def _color_key(self):
        linked = {ch for _, ch in self.linked()}
        return (self.color.config_source if self.color.ready else None,
                sorted((ch, dst) for ch, (_, dst, _) in self.plan().items() if ch in linked))

    def _color_changed(self):
        """Configuration ou réglages changés : vignettes et nuanciers revus ; le matériau envoyé est rendu à nouveau si
        l'espace d'une de ses cartes change (tout de suite avec Live, sinon au prochain Update)."""
        before = self._color_key()
        self._make_color()
        self._refresh_thumbs()
        if self.settings is not None and self.settings.isVisible():
            self.settings.reload()
        if self._color_key() != before and self.material_dir is not None:
            self.dirty = True
            self._update_buttons()
            if self.live.isChecked():
                self._commit()

    def _refresh_thumbs(self):
        if self.thumb_files is None:
            return
        files, spaces = self.thumb_files
        self._show_thumbs(load_thumbs({ch: p for ch, p in files.items() if os.path.isfile(p)}, self.color, spaces))

    def _show_settings(self):
        if self.settings is None:
            self.settings = SettingsDialog(self)
        else:
            self.settings.reload()
        self.settings.show()
        self.settings.raise_()
        self.settings.activateWindow()

    def apply_settings(self, desired, save=False):
        """desired : {variable: valeur, None pour le défaut}. Change les variables de la fenêtre (pas celles de
        l'environnement) et, avec save, les écrit dans studio.env. Rend un message d'erreur, None si tout va bien."""
        editable = {v: (x or None) for v, x in desired.items() if v not in self.locked}
        for var, value in editable.items():
            if value is None:
                os.environ.pop(var, None)
                self.origins.pop(var, None)
            elif os.environ.get(var) != value:
                os.environ[var] = value
                self.origins[var] = WINDOW
        error = None
        if save:
            target = self.env_file()
            if not target:
                error = "no studio.env to write: $STUDIO_ENV is empty"
            else:
                try:
                    written = studio_env.store(target, editable)
                    log("%s: %s" % (target, ", ".join("%s=%s" % (k, "" if v is None else v) for k, v in written.items())
                                    or "already up to date"))
                    self._sync_env()
                except (OSError, ValueError) as e:
                    error = "could not write %s: %s" % (target, e)
        self.test_log.setdefault("settings", []).append({"desired": desired, "save": save, "error": error})
        self._color_changed()
        return error

    def _color_log(self):
        c = self.color
        return {"ready": c.ready, "error": c.error, "config": c.config_source, "name": c.name, "origin": c.origin,
                "display": c.display, "view": c.view, "look": c.look, "viewport": c.viewport, "warnings": c.warnings,
                "plan": {ch: list(v) for ch, v in self.plan().items()},
                "picking": list(c.picking()) if c.ready else None, "origins": self.origins,
                "locked": sorted(self.locked), "file": self.env_file(),
                "env": {v: os.environ[v] for v in COLOR_VARS if v in os.environ},
                "applied": self.applied_spaces, "thumbs": self.thumb_files[1] if self.thumb_files else None,
                "swatches": {k: r.editor.shown for k, r in self.params.rows.items() if isinstance(r.editor, ColorEditor)}}

    # -- travaux

    def _submit(self, job, kind, **info):
        job.kind = kind
        job.info = info
        self.job = job
        self.worker.submit(job)

    # Un travail remplacé juste après avoir fini a déjà émis son résultat : seul celui du dernier soumis compte.
    def _on_step(self, job, data):
        if job is self.job and job.kind == "final":
            self._deliver(job, data)

    def _on_finished(self, job, result):
        if job is not self.job:
            return
        if isinstance(result, Exception):
            self._failed(job, result)
            return
        if job.kind == "locate":
            self.tools, self.tools_error = result, None
            log("sbsrender %s (%s)" % (result["version"], result["sbsrender"]))
            self._show_banner(None)
            then, self._pending_then = self._pending_then, None
            self._update_buttons()
            if then:
                then()
        elif job.kind == "open":
            self._opened(job, result)
        elif job.kind in ("preview", "final"):
            self.thumb_files = (result["files"], result["spaces"])
            self._show_thumbs(result["thumbs"])
            self._show_maps()
            if job.kind == "preview":
                self._status("Preview %d px in %.2f s" % (1 << job.info["size"], result["seconds"]))
            self.test_log.setdefault("renders", []).append({"kind": job.kind, "seconds": result["seconds"],
                                                           "files": result["files"], "spaces": result["spaces"],
                                                           "converted": result.get("converted")})
            self._test_next()

    def _failed(self, job, error):
        message = str(error)
        if job.kind == "locate":
            self.tools, self.tools_error = None, message
            self._show_banner(message)
            then, self._pending_then = self._pending_then, None
            self._update_buttons()
            if then:
                then()
            return
        if isinstance(error, subzero_render.Error) and "cancelled" in message:
            return
        self._status(message, ERROR)
        self.test_log.setdefault("errors", []).append(message)
        if job.kind == "open":
            self.test_log["open_error"] = message
        self._test_next(failed=True)

    def _show_banner(self, message):
        self.banner.setVisible(bool(message))
        self.prefs_button.setVisible(bool(message))
        if message:
            self.banner.setText(message)

    # -- paquet

    def _browse_package(self):
        start = os.path.dirname(self.package["path"]) if self.package else ""
        path, _ = QtWidgets.QFileDialog.getOpenFileName(self, "Open", start, "Packages (*.sbsar *.sbs)")
        if path:
            self._open({"file": path})

    def dragEnterEvent(self, event):
        if any(u.toLocalFile().lower().endswith((".sbs", ".sbsar")) for u in event.mimeData().urls()):
            event.acceptProposedAction()

    def dropEvent(self, event):
        for u in event.mimeData().urls():
            if u.toLocalFile().lower().endswith((".sbs", ".sbsar")):
                self._open({"file": u.toLocalFile()})
                break

    def _open(self, target, keep=None):
        """target : {"file"} (paquet) ou {"dir"} (matériau envoyé, avec sa recette). keep : réglages gardés (Reload)."""
        recipe, folder = None, None
        if target.get("dir"):
            folder = target["dir"]
            try:
                recipe = read_recipe(folder)
            except (OSError, ValueError) as e:
                self._status("No subzero recipe in %s: %s" % (folder, e), ERROR)
                self.test_log["open_error"] = str(e)
                self._test_next(failed=True)
                return
            path = recipe_package(recipe, folder)
        else:
            path = target["file"]
        tools = self.tools
        self._status("Opening %s..." % os.path.basename(path))

        def job(track, step):
            if not os.path.isfile(path):
                raise subzero_render.Error("file not found: %s" % path)
            if path.lower().endswith(".sbs") and tools is None:
                raise subzero_render.Error("a .sbs file is cooked by sbscooker: %s" % (self.tools_error or "sbsrender not found"))
            pkg = subzero_render.open_package(path, tools or {}, cancel=track) if tools else \
                {"sbsar": path, "description": subzero_meta.read_sbsar(path), "cook": None}
            pkg["path"] = path
            pkg["icons"] = read_icons(pkg["description"], pkg["sbsar"])
            return pkg

        self._submit(job, "open", recipe=recipe, folder=folder, keep=keep)

    def _opened(self, job, pkg):
        info = job.info
        recipe, keep = info.get("recipe"), info.get("keep")
        self.package = pkg
        graphs = subzero_render.graphs(pkg["description"])
        url = (recipe or {}).get("graph") or (keep or {}).get("graph")
        graph = subzero_render.find_graph(pkg["description"], url) if url else subzero_render.find_graph(pkg["description"])
        self.graph_combo.clear()
        for g in graphs:
            self.graph_combo.addItem(g.get("label") or g["pkgurl"], g["pkgurl"])
        self.graph_combo.setCurrentIndex(max(0, self.graph_combo.findData(graph["pkgurl"])))
        self.graph_combo.setVisible(len(graphs) > 1)
        self.graph_label.setVisible(len(graphs) > 1)
        if recipe is not None:
            self.material_dir = info["folder"]
            self.material_name = recipe.get("name")
            self.overrides = dict(recipe.get("overrides") or {})
            self._set_format(recipe.get("format") or "tga")  # v0 : TGA
            self._set_graph(graph, values=recipe.get("values") or {}, entries=recipe.get("entries") or {},
                            preset=recipe.get("preset"), size=recipe.get("size"))
            self.name.setText(recipe.get("name") or graph.get("label") or "")
            files = {ch: os.path.join(self.material_dir, f) for ch, f in (recipe.get("files") or {}).items()
                     if os.path.isfile(os.path.join(self.material_dir, f))}
            self.applied_spaces = recipe_spaces(recipe)
            self.previous_files = list((recipe.get("files") or {}).values())
            spaces = {ch: self.applied_spaces.get(ch) or self._source(ch) for ch in files}
            self.thumb_files = (files, spaces)
            self._show_thumbs(load_thumbs(files, self.color, spaces))
            self._show_maps()
            self.dirty = False
            self._status("Editing %s" % self.material_name)
        elif keep is not None:
            same = keep.get("graph") == graph["pkgurl"]
            self._set_graph(graph, values=keep["values"] if same else {}, entries=keep["entries"] if same else {},
                            preset=keep["preset"] if same else None, size=self.size)
            self._commit()
        else:
            self.material_dir = None
            self.material_name = None
            self.applied_spaces = {}
            self.overrides = {}
            self._set_format(subzero_render.default_format())
            self._set_graph(graph)
            self.name.setText(graph.get("label") or os.path.splitext(os.path.basename(pkg["path"]))[0])
            self._commit()
        cooked = " · cooked in %.2f s" % pkg["cook"]["seconds"] if pkg.get("cook") and not pkg["cook"].get("cached") else ""
        self.subtitle.setText("%s · %s%s" % (os.path.basename(pkg["path"]), graph.get("category") or graph.get("graphtype") or "graph", cooked))
        self.test_log["opened"] = {"path": pkg["path"], "graph": graph["pkgurl"], "cook": pkg.get("cook")}
        if self.browser is not None:
            self.browser.mark(pkg["path"])
        self._update_buttons()
        if recipe is not None or self.test is not None:
            self._test_next()

    def _set_graph(self, graph, values=None, entries=None, preset=None, size=None):
        self.graph = graph
        by_id = {i["identifier"]: i for i in graph["inputs"]}
        self.values = graph_values(graph, values)
        self.entries = {k: v for k, v in (entries or {}).items() if k in by_id}
        self.preset = preset
        self.size = size
        self.title.setText(graph.get("label") or graph["pkgurl"])
        self._set_icon(graph)
        self.preset_combo.clear()
        self.preset_combo.addItem("Default", None)
        for p in graph.get("presets") or []:
            self.preset_combo.addItem(p["label"], p["label"])
        self.preset_combo.setCurrentIndex(max(0, self.preset_combo.findData(preset)))
        has_seed = "$randomseed" in by_id
        self.seed.setVisible(has_seed)
        self.seed_button.setVisible(has_seed)
        self.seed_label.setVisible(has_seed)
        self.seed.blockSignals(True)
        self.seed.setValue(int(self.values.get("$randomseed", 0)))
        self.seed.blockSignals(False)
        self.size_combo.setCurrentIndex(max(0, self.size_combo.findData(size)))
        merged = dict(self.values)
        merged.update(self.entries)
        self.params.build(graph, merged)
        self._remap()

    def _set_icon(self, graph):
        data = ((self.package or {}).get("icons") or {}).get(graph["pkgurl"])
        image = QtGui.QImage.fromData(data) if data else QtGui.QImage()
        if not image.isNull():
            self.icon.setPixmap(QtGui.QPixmap.fromImage(image).scaled(48, 48, QtCore.Qt.KeepAspectRatio,
                                                                      QtCore.Qt.SmoothTransformation))
        else:
            self.icon.clear()

    def _reload(self):
        if self.package is None:
            return
        keep = {"graph": self.graph["pkgurl"], "values": dict(self.values), "entries": dict(self.entries),
                "preset": self.preset}
        self._open({"file": self.package["path"]}, keep=keep)

    # -- réglages

    def _on_graph(self, index):
        url = self.graph_combo.itemData(index)
        if self.package is None or url == self.graph["pkgurl"]:
            return
        self.overrides = {}
        self._set_graph(subzero_render.find_graph(self.package["description"], url))
        self.name.setText(self.graph.get("label") or "")
        self._changed()

    def _on_preset(self, index):
        label = self.preset_combo.itemData(index)
        preset = next((p for p in self.graph.get("presets") or [] if p["label"] == label), None)
        self.values = {k: (v if k == "$randomseed" else next(i["default"] for i in self.graph["inputs"]
                                                               if i["identifier"] == k)) for k, v in self.values.items()}
        if preset is not None:
            for k, v in (preset.get("values") or {}).items():
                if k in self.values:
                    self.values[k] = v
        self.preset = label
        if label and self.material_dir is None:
            self.name.setText(label)
        merged = dict(self.values)
        merged.update(self.entries)
        self.params.set_values(merged)
        self._changed()

    def _on_size(self, index):
        self.size = self.size_combo.itemData(index)
        self._changed()

    def _set_format(self, fmt):
        self.fmt = fmt if fmt in subzero_render.FORMATS else subzero_render.default_format()
        self.format_combo.setCurrentIndex(max(0, self.format_combo.findData(self.fmt)))

    def _on_format(self, index):
        fmt = self.format_combo.itemData(index)
        if fmt != self.fmt:
            self.fmt = fmt
            self._changed()

    def _on_param(self, ident, value, mode):
        by_id = {i["identifier"]: i for i in self.graph["inputs"]} if self.graph else {}
        if ident in by_id and by_id[ident]["type"] == "image":
            if value:
                self.entries[ident] = value
            else:
                self.entries.pop(ident, None)
        else:
            self.values[ident] = value
        merged = dict(self.values)
        merged.update(self.entries)
        self.params.refresh(merged)
        if mode == "drag":
            return
        self._remap()
        if mode == "soon":
            self.soon.start()
        else:
            self._changed()

    def _changed(self):
        self.soon.stop()
        self.dirty = True
        self._commit()

    def _remap(self):
        rows = apply_overrides(auto_mapping(self.graph, self.values), self.overrides)
        self.mapping = rows
        self.outputs.blockSignals(True)
        self.outputs.clear()
        for o, ch, why in rows:
            item = QtWidgets.QTreeWidgetItem([o.get("label") or o["identifier"], ""])
            item.setToolTip(0, "%s (%s)" % (o["identifier"], ", ".join(o.get("usages") or []) or "no usage"))
            self.outputs.addTopLevelItem(item)
            combo = QtWidgets.QComboBox()
            combo.addItem("—", "")
            for c, label, _, _ in CHANNELS:
                combo.addItem(label, c)
            combo.setCurrentIndex(max(0, combo.findData(ch or "")))
            if why and not ch:
                combo.setToolTip(why)
                item.setForeground(0, QtGui.QBrush(QtGui.QColor("#666666")))
            combo.activated.connect(lambda i, o=o["identifier"], c=combo: self._on_channel(o, c.itemData(i)))
            self.outputs.setItemWidget(item, 1, combo)
        self.outputs.blockSignals(False)
        linked = sum(1 for _, ch, _ in rows if ch)
        unlinked = len(rows) - linked
        self.outputs_head.setText("Channels (%d linked%s)" % (linked, ", %d not linked" % unlinked if unlinked else ""))
        self._update_buttons()

    def _on_channel(self, output, channel):
        for o, ch, _ in self.mapping:
            if ch == channel and channel and o["identifier"] != output:
                self.overrides[o["identifier"]] = ""  # un canal ne vient que d'une sortie
        self.overrides[output] = channel
        self._remap()
        self._changed()

    # -- rendu

    def linked(self):
        return [(o["identifier"], ch) for o, ch, _ in self.mapping if ch]

    def _commit(self):
        """Rendu du réglage courant : final dans le dossier du matériau (envoyé, Live), aperçu 512 sinon."""
        if self.graph is None or self.tools is None or not self.linked():
            return
        if self.material_dir is not None and self.live.isChecked():
            self._render_final(self.material_dir, "update")
        else:
            self._render_preview()

    def _render_preview(self):
        graph, tools, sbsar = self.graph, self.tools, self.package["sbsar"]
        values, entries = render_values(graph, self.values), dict(self.entries)
        outs = self.linked()
        out_dir = self.preview_dir
        color = self.color
        spaces = {ch: self._source(ch) for _, ch in outs}  # aperçu tel que sbsrender l'écrit
        # sans ArmorPaint, à la taille choisie (512 par défaut) : le viewer montre ce qui sera exporté
        size = self._final_size() if self.standalone else (PREVIEW_SIZE, PREVIEW_SIZE)
        self._status("Rendering preview...")

        def job(track, step):
            res = subzero_render.render(sbsar, graph, out_dir, tools, values=values, entries=entries,
                                        size=size, outputs=[o for o, _ in outs],
                                        names={o: ch for o, ch in outs}, cancel=track)
            files = {ch: res["files"][o] for o, ch in outs}
            return {"files": files, "spaces": spaces, "thumbs": load_thumbs(files, color, spaces),
                    "seconds": res["seconds"]}

        self._submit(job, "preview", size=size[0])

    def _final_size(self):
        if self.size is not None:
            return (self.size, self.size)
        res = self.state.get("resolution") or [2048, 2048]
        return (log2_size(res[0]), log2_size(res[1] if len(res) > 1 else res[0]))

    def _render_final(self, folder, action):
        graph, tools, sbsar = self.graph, self.tools, self.package["sbsar"]
        values, entries = dict(self.values), dict(self.entries)
        outs = self.linked()
        fmt = self.fmt
        size = self._final_size()
        color = self.color
        template = self.template
        self.final_seq += 1
        seq = self.final_seq
        self._status("Rendering %d px..." % (1 << size[0]))
        self.dirty = False
        self._update_buttons()

        def job(track, step):
            data = render_material(sbsar, graph, folder, tools, outs, values=values, entries=entries, size=size,
                                   fmt=fmt, color=color, template=template, cancel=track)
            data["seq"] = seq
            step(data)
            data = dict(data)
            data["thumbs"] = load_thumbs(data["files"], color, data["spaces"])
            return data

        self._submit(job, "final", folder=folder, action=action, outs=outs)

    def _deliver(self, job, data):
        """Rendu final fini : recette écrite, puis matériau créé ou mis à jour dans ArmorPaint."""
        folder, action = job.info["folder"], job.info["action"]
        try:
            self._write_recipe(folder, job.info["outs"], data)
        except OSError as e:
            self._status("Could not write %s: %s" % (RECIPE, e), ERROR)
            return
        args = {"dir": folder, "name": self.name.text().strip() or self.graph.get("label") or "Material"}
        request = (action, args, data)
        if self.request_busy:
            self.pending = request  # le plus récent seulement
            return
        self._request(request)

    def _request(self, request):
        action, args, data = request
        # Espaces du rendu précédent au moment de l'envoi : une requête en attente suit celle qui la précède.
        args["links"] = links(data["files"], data["spaces"], self.applied_spaces if action == "update" else None)
        self.request_busy = True
        t0 = time.time()

        def done(reply, error):
            self.request_busy = False
            took = time.time() - t0
            entry = {"action": action, "args": args, "reply": reply, "error": error, "render": data["seconds"],
                     "armorpaint": round(took, 3)}
            self.test_log.setdefault("requests", []).append(entry)
            if error:
                self._status("ArmorPaint: %s" % error.get("message"), ERROR)
            else:
                if action == "create":
                    self.material_dir = args["dir"]
                    self.material_name = (reply or {}).get("material") or args.get("name")
                self.applied_spaces = dict(data["spaces"])
                if self.pending is None and data.get("seq") == self.final_seq:
                    entry["deleted"] = self._delete_stale(args["dir"], data["files"], (reply or {}).get("kept") or [],
                                                          self.previous_files if action == "update" else [])
                    self.previous_files = [os.path.basename(p) for p in data["files"].values()]
                verb = {"create": "Sent", "update": "Updated"}.get(action, "Sent")
                notes = data.get("color_warnings") or []
                more = " \u00b7 " + notes[0] + (" (+%d)" % (len(notes) - 1) if len(notes) > 1 else "") if notes else ""
                if self.standalone:
                    self._status("%s %s: %d px in %.2f s%s" % (
                        "Exported" if action == "create" else "Updated", os.path.basename(args["dir"]),
                        1 << data["size"][0], data["seconds"], more), WARNING if notes else SUCCESS)
                    self.status.setToolTip("\n".join([args["dir"]] + notes))
                else:
                    self._status("%s %s: %d px in %.2f s, ArmorPaint %.2f s%s" % (
                        verb, self.material_name or "", 1 << data["size"][0], data["seconds"], took, more),
                        WARNING if notes else SUCCESS)
                    self.status.setToolTip("\n".join(notes))
            self._update_buttons()
            if self.pending is not None:
                request, self.pending = self.pending, None
                self._request(request)
            else:
                self._test_next()

        self.channel.request(action, args, done)

    def _delete_stale(self, folder, files, kept, previous=()):
        """Après la réponse au dernier rendu final : fichiers d'un rendu précédent (autre format, sortie retirée), dont
        ArmorPaint a retiré les textures ; ceux qu'un autre nœud utilise encore (kept) restent."""
        deleted = []
        for path in stale_files(folder, list(files.values()) + list(kept), self.template, previous):
            try:
                os.remove(path)
                deleted.append(os.path.basename(path))
            except OSError as e:
                log("could not delete %s: %s" % (path, e))
        if deleted:
            log("deleted %s" % ", ".join(deleted))
        return deleted

    def _write_recipe(self, folder, outs, data):
        write_recipe(folder, make_recipe(self.package["path"], folder, self.name.text().strip() or self.graph.get("label") or "",
                                         self.graph, self.values, self.entries, self.preset, self.size, outs,
                                         self.overrides, data, self.color, self.tools))

    def _show_thumbs(self, thumbs):
        linked = {ch for _, ch in self.linked()}
        for ch, (holder, image) in self.thumbs.items():
            holder.setVisible(ch in linked)
            if ch in thumbs:
                pix = QtGui.QPixmap.fromImage(thumbs[ch])
                pix.setDevicePixelRatio(2.0)
                image.setPixmap(pix)
            elif ch in linked:
                image.setPixmap(QtGui.QPixmap())
        self._highlight_thumbs()

    def _highlight_thumbs(self):
        """Vignette de la carte du viewer entourée ; clic sur une vignette : sa carte dans le viewer."""
        current = self.viewer.current if self.viewer.isVisible() else None
        for ch, (holder, image) in self.thumbs.items():
            image.setStyleSheet("background: #252525; border-radius: 2px; border: 1px solid %s;" % (
                PAINTER_ACCENT if ch == current else "#252525"))
            image.setCursor(QtCore.Qt.PointingHandCursor if current else QtCore.Qt.ArrowCursor)

    def _show_maps(self):
        files, spaces = self.thumb_files
        self.viewer.set_maps(files, spaces, [ch for ch, _, _, _ in CHANNELS])

    def eventFilter(self, obj, event):
        if event.type() == QtCore.QEvent.MouseButtonPress and obj.property("channel"):
            self.viewer.select(obj.property("channel"))
        return False

    # -- envoi

    def _base_folder(self):
        project = (self.state.get("project") or {}).get("file") or ""
        if project:
            return subzero_dir(project)
        if self.base_dir:
            return self.base_dir
        if self.test is not None and self.test.get("dir"):
            self.base_dir = self.test["dir"]
            return self.base_dir
        if os.environ.get("SUBZERO_EXPORT_DIR"):  # lot E9 : dossier de base sans projet enregistré
            self.base_dir = os.path.expanduser(os.environ["SUBZERO_EXPORT_DIR"])
            return self.base_dir
        path = QtWidgets.QFileDialog.getExistingDirectory(
            self, "Export folder (one subfolder per material)" if self.standalone else
            "Folder for the textures (the project is not saved yet)", os.path.expanduser("~"))
        if not path:
            return None
        self.base_dir = path
        return path

    def _new_folder(self):
        base = self._base_folder()
        if base is None:
            return None
        stem = slug(self.name.text().strip() or self.graph.get("label") or "Material")
        folder, n = os.path.join(base, stem), 2
        while os.path.exists(folder):
            folder = os.path.join(base, "%s%d" % (stem, n))
            n += 1
        return folder

    def _send(self, duplicate=False):
        if self.graph is None or self.tools is None:
            return
        if self.material_dir is not None and not duplicate:
            self._render_final(self.material_dir, "update")
            return
        folder = self._new_folder()
        if folder is None:
            return
        self._render_final(folder, "create")

    def _reveal(self):
        if self.material_dir:
            QtGui.QDesktopServices.openUrl(QtCore.QUrl.fromLocalFile(self.material_dir))

    def _pick_sbsrender(self):
        """Sans ArmorPaint : sbsrender choisi à la main, gardé dans studio.env (SUBZERO_SBSRENDER) s'il y en a un."""
        start = os.path.dirname(self.state.get("sbsrender") or "") or subzero_render.DESIGNER_WHERE
        path, _ = QtWidgets.QFileDialog.getOpenFileName(self, "sbsrender of Adobe Substance 3D Designer", start)
        if not path:
            return
        os.environ["SUBZERO_SBSRENDER"] = path
        target = studio_env.write_target()
        if target:
            try:
                studio_env.store(target, {"SUBZERO_SBSRENDER": path})
                log("%s: SUBZERO_SBSRENDER=%s" % (target, path))
            except (OSError, ValueError) as e:
                log("could not write %s: %s" % (target, e))
        self._on_prefs_event({"sbsrender": path})

    # -- fenêtre

    def _initial_geometry(self):
        """Colonne de COLUMN, le viewer carré de la hauteur de la fenêtre et le navigateur, dans la limite de l'écran :
        un écran trop étroit pour les trois garde le navigateur fermé."""
        screen = self.screen() or QtGui.QGuiApplication.primaryScreen()
        avail = screen.availableGeometry() if screen is not None else QtCore.QRect(0, 0, 1920, 1080)
        height = max(560, min(WINDOW_HEIGHT, avail.height() - 60))
        viewer = not self.viewer.isHidden()  # fenêtre pas encore montrée : isVisible() faux
        browser = self.browser is not None and not self.browser.isHidden()
        room = avail.width() - 40 - COLUMN
        if viewer and browser and room - subzero_browser.WIDTH < min(height, 640):
            self.browser_button.setChecked(False)
            self.browser.hide()
            browser = False
        if browser:
            room -= subzero_browser.WIDTH
        if viewer:
            height = max(480, min(height, room))
            self.viewer.setFixedWidth(height)
        self.resize(COLUMN + (height if viewer else 0) + (subzero_browser.WIDTH if browser else 0), height)

    def _toggle_panel(self, panel, on):
        """Viewer ou navigateur, ajouté à gauche : la fenêtre s'élargit de sa largeur, colonne inchangée."""
        if on == panel.isVisible():
            return
        geo = self.geometry()
        if on:
            side = self.height() if panel is self.viewer else panel.width()
            if panel is self.viewer:
                panel.setFixedWidth(side)
            panel.show()
            geo.setLeft(geo.left() - side)
        else:
            geo.setLeft(geo.left() + panel.width())
            panel.hide()
        if panel is self.browser:
            self.browser.remember_visible(on)
        screen = self.screen()
        if screen is not None:  # dans l'écran : décalée à droite si le bord gauche en sort
            avail = screen.availableGeometry()
            if geo.left() < avail.left():
                geo.moveLeft(avail.left())
        self.setGeometry(geo)
        self._highlight_thumbs()

    def resizeEvent(self, event):
        super().resizeEvent(event)
        if self.viewer.isVisible() and self.viewer.width() != self.height():
            self.viewer.setFixedWidth(self.height())

    def to_front(self):
        self.show()
        self.raise_()
        self.activateWindow()
        if self.mac is not None:
            self.mac.activate()

    def closeEvent(self, event):
        super().closeEvent(event)
        self.worker.stop()
        self.viewer.stop()
        if self.browser is not None:
            self.browser.stop()
        shutil.rmtree(self.preview_dir, ignore_errors=True)
        if self.mac is not None:
            self.mac.hide()  # macOS rend le premier plan à ArmorPaint
        QtWidgets.QApplication.instance().quit()

    # -- essai sans intervention

    def _run_test(self):
        """Réglages de l'essai, puis action ; la suite vient de _test_next, à la fin de chaque étape."""
        t = self.test
        if "settings" in t and not t.get("_settings"):
            t["_settings"] = True
            desired = dict(t["settings"])
            save = bool(desired.pop("save", False))
            self.test_log["settings_error"] = self.apply_settings(desired, save=save)
        if self.graph is None:
            self._finish_test()
            return
        if "graph" in t and t["graph"] != self.graph["pkgurl"]:
            self._set_graph(subzero_render.find_graph(self.package["description"], t["graph"]))
        if "preset" in t:
            self.preset_combo.setCurrentIndex(max(0, self.preset_combo.findData(t["preset"])))
            self._on_preset(self.preset_combo.currentIndex())
        for k, v in (t.get("values") or {}).items():
            self._on_param(k, v, "drag")
        merged = dict(self.values)
        merged.update(self.entries)
        self.params.set_values(merged)
        self.seed.blockSignals(True)
        self.seed.setValue(int(self.values.get("$randomseed", 0)))
        self.seed.blockSignals(False)
        self._remap()
        if "size" in t:
            self.size = t["size"]
            self.size_combo.setCurrentIndex(max(0, self.size_combo.findData(self.size)))
        if "format" in t:
            self._set_format(t["format"])
        if "live" in t:
            self.live.setChecked(bool(t["live"]))
        self.soon.stop()
        action = t.get("action", "create")
        t["_stage"] = "action"
        if action == "none":
            if not self.worker.busy:  # sinon à la fin du rendu en cours (aperçu des réglages de l'essai)
                self._finish_test()
        elif action == "duplicate":
            self._send(duplicate=True)
        elif action == "update" and self.material_dir is not None:
            self._render_final(self.material_dir, "update")
        else:
            self._send()

    def _test_viewer(self, t):
        """Carte, touches, tirettes et zoom de l'essai, puis état du viewer une fois son calcul fini."""
        v = self.viewer
        v.flush()
        if t.get("viewer_map"):
            v.select(t["viewer_map"])
        pressed = []
        for k in t.get("viewer_keys") or []:
            char = k.get("key") or ""
            code = ord(char.upper()) if len(char) == 1 else getattr(QtCore.Qt, "Key_" + char, 0)
            mods = QtCore.Qt.KeypadModifier if k.get("keypad") else QtCore.Qt.NoModifier
            event = QtGui.QKeyEvent(QtCore.QEvent.KeyPress, int(code), mods, int(k.get("scan", 0)), int(k.get("vk", 0)),
                                    0, char if len(char) == 1 else "")
            target = QtWidgets.QApplication.focusWidget() or self
            QtWidgets.QApplication.sendEvent(target, event)
            pressed.append({"key": char, "digit": subzero_viewer.key_digit(event), "current": v.current})
        for key in ("exposure", "gamma", "saturation"):
            if key in t:
                v.set_param(key, float(t[key]))
        zoom = t.get("zoom")
        if zoom == "fit":
            v.canvas.fit()
        elif zoom == "1:1":
            v.canvas.one_to_one()
        elif zoom is not None:
            v.canvas.zoom_to(float(zoom))
        v.flush()
        if t.get("viewer_hover"):
            v.canvas.hovered.emit(tuple(t["viewer_hover"]))
        self.test_log["viewer"] = dict(v.test_log(), keys=pressed, width=v.width(), height=self.height())

    def _test_next(self, failed=False):
        t = self.test
        if t is None:
            return
        stage = t.get("_stage")
        if stage is None:
            if failed or self.graph is None:
                self._finish_test()
            else:
                t["_stage"] = "setup"
                QtCore.QTimer.singleShot(0, self._run_test)
        elif stage == "action" and not self.worker.busy and not self.request_busy:
            self._finish_test()

    def _finish_test(self):
        t = self.test
        if t.get("_done"):
            return
        t["_done"] = True
        self.test_log.update({
            "graph": self.graph["pkgurl"] if self.graph else None,
            "mapping": [[o["identifier"], ch, why] for o, ch, why in self.mapping],
            "material_dir": self.material_dir,
            "material_name": self.material_name,
            "status": self.status.text(),
            "values": self.values,
            "shown": sorted(k for k, r in self.params.rows.items() if not r.box.isHidden()),
            "banner": self.banner.text() if not self.banner.isHidden() else "",
            "tools": self.tools,
            "color": self._color_log(),
        })
        if self.viewer.isVisible():
            self._test_viewer(t)
        if self.browser is not None:
            if t.get("browser_tab"):
                self.browser.tab_bar.setCurrentIndex(max(0, self.browser._tab_index(t["browser_tab"])))
                self.browser.flush()
            if "browser_search" in t:
                self.browser.search.setText(t["browser_search"])
            self.browser.flush()
            self.test_log["browser"] = self.browser.test_log()
        if t.get("screenshot"):
            QtWidgets.QApplication.processEvents()  # dispositions à jour (vignettes montrées juste avant)
            self.grab().save(t["screenshot"])
        if t.get("settings_screenshot"):
            self._show_settings()
            QtWidgets.QApplication.processEvents()
            self.settings.grab().save(t["settings_screenshot"])
            self.test_log["settings_panel"] = {"banner": self.settings.banner.text() if not self.settings.banner.isHidden()
                                               else "", "fields": sorted(self.settings.fields),
                                               "editable": sorted(self.settings.desired())}
        if t.get("result"):
            with open(t["result"], "w") as f:
                json.dump(self.test_log, f, indent=1, default=str)
        self.close()


def main():
    app, mac, channel = studio_qt.start("subzero", painter=True)
    test = None
    if os.environ.get("SUBZERO_TEST"):
        with open(os.environ["SUBZERO_TEST"]) as f:
            test = json.load(f)
    window = SubzeroWindow(channel, mac, test)
    window.to_front()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
