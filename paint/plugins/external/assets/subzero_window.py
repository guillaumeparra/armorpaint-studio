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
# les textures <Nom>_<canal>.tga (.exr pour normale et hauteur dans un projet en 16 ou 32 bits) et la recette
# subzero.json (paquet, graphe, valeurs, preset, taille, sorties, version de sbsrender). <Nom> est un seul mot
# (« ColorfulPlanks ») : les règles de nom d'ArmorPaint (couleur ou donnée, canal) ne voient alors que le canal, et
# les noms de fichiers, qui servent de noms d'assets, restent uniques dans le projet. Décalque (couleur de base avec un
# alpha utile) : couleur réécrite en RGB, alpha dans <Nom>_opacity.tga, car ArmorPaint multiplie la couleur par l'alpha.
#
# SUBZERO_TEST=<fichier.json> : essai sans intervention (.tests/subzero_app.sh, .tests/subzero_window_ui.py). Le
# fichier donne "open" ({"file"} ou {"dir"} : paquet, ou matériau déjà envoyé à rééditer ; absent : celui de l'état),
# "graph", "preset", "values" ({id: valeur}), "size" (log2, ou null pour celle du projet), "live", "dir" (dossier de
# base d'un projet jamais enregistré), "action" ("create", "update", "duplicate" ou "none"), "result" (fichier écrit
# à la fin) et "screenshot". La fenêtre les applique, agit, écrit le résultat, puis se ferme.

import datetime
import hashlib
import json
import math
import os
import posixpath
import re
import shutil
import sys
import threading
import time

from PySide6 import QtCore, QtGui, QtWidgets

import studio_qt
import subzero_7z
import subzero_meta
import subzero_render
from studio_qt import ERROR, MUTED, SUCCESS, WARNING, log, muted

RECIPE = "subzero.json"
PREVIEW_SIZE = 9  # 512
THUMB = 88
SOON_MS = 300  # réglage au clavier ou à la molette : rendu après ce délai sans nouveau changement

# Canaux d'ArmorPaint : socket de la sortie PBR (import_folder.c) et suffixe du fichier, reconnu par les règles de nom.
CHANNELS = [
    ("base", "Base Color", 0, "basecolor"),
    ("opacity", "Opacity", 1, "opacity"),
    ("occlusion", "Occlusion", 2, "occlusion"),
    ("roughness", "Roughness", 3, "roughness"),
    ("metallic", "Metallic", 4, "metallic"),
    ("normal", "Normal", 5, "normal"),
    ("height", "Height", 7, "height"),
]
CHANNEL_LABEL = {c[0]: c[1] for c in CHANNELS}
CHANNEL_SOCKET = {c[0]: c[2] for c in CHANNELS}
CHANNEL_SUFFIX = {c[0]: c[3] for c in CHANNELS}
DATA_EXR = ("normal", "height")  # en EXR half dans un projet en 16 ou 32 bits

# Usage (ou identifiant) d'une sortie -> canal.
USAGE_CHANNEL = {
    "basecolor": "base", "opacity": "opacity", "ambientocclusion": "occlusion", "roughness": "roughness",
    "metallic": "metallic", "normal": "normal", "height": "height",
}
NAME_CHANNEL = dict(USAGE_CHANNEL, albedo="base", base="base", ao="occlusion", occlusion="occlusion",
                    metalness="metallic", metal="metallic", rough="roughness", displacement="height", alpha="opacity")
# Sorties connues qu'ArmorPaint ne sait pas représenter : montrées, non liées.
UNSUPPORTED = {
    "emissive": "ArmorPaint only has an emission flag",
    "diffuse": "specular/glossiness workflow: base color is used",
    "specular": "specular/glossiness workflow", "glossiness": "specular/glossiness workflow",
    "specularlevel": "no specular level in ArmorPaint", "anisotropylevel": "no anisotropy in ArmorPaint",
    "anisotropyangle": "no anisotropy in ArmorPaint", "translucency": "no translucency in ArmorPaint",
    "scatteringcolor": "no subsurface color in ArmorPaint", "scatteringdistancescale": "no subsurface in ArmorPaint",
    "sheencolor": "no sheen in ArmorPaint", "sheenopacity": "no sheen in ArmorPaint",
    "sheenroughness": "no sheen in ArmorPaint", "absorptioncolor": "no absorption in ArmorPaint",
}

# Paramètres techniques, fixés d'office et masqués comme dans Painter : bascules channel_<canal> (le canal existe dans
# le projet) et format de la normale (OpenGL, celui d'ArmorPaint).
PAINTER_CHANNELS = {
    "diffuse", "basecolor", "specular", "glossiness", "roughness", "metallic", "height", "normal", "ambientocclusion",
    "specularlevel", "opacity", "translucency", "scatter", "scatteringcolor", "scatteringdistancescale",
    "anisotropylevel", "anisotropyangle", "redshift", "rayleigh", "absorptioncolor", "mask", "sheenopacity",
    "sheencolor", "sheenroughness", "emissive", "displacement", "ior",
}
PROJECT_CHANNELS = {"basecolor", "normal", "roughness", "metallic", "height", "ambientocclusion", "opacity", "emissive"}



# ---- Règles pures (testées sans Qt) ------------------------------------------------------------------------------------


def slug(label):
    """Nom de dossier et préfixe des fichiers : un seul mot, « Colorful Planks » -> « ColorfulPlanks »."""
    words = re.findall(r"[A-Za-z0-9]+", label or "")
    s = "".join(w[:1].upper() + w[1:] for w in words)
    return s or "Material"


def subzero_dir(project_file):
    """Dossier des matériaux d'un projet : « /p/robot_v003.arm » -> « /p/robot_subzero », commun à ses versions."""
    stem = re.sub(r"\.arm$", "", os.path.basename(project_file), flags=re.I)
    stem = re.sub(r"[._-]v\d+$", "", stem, flags=re.I) or stem
    return os.path.join(os.path.dirname(project_file), stem + "_subzero")


def technical_value(inp):
    """Valeur imposée d'un paramètre technique, None pour un paramètre montré."""
    ident = inp["identifier"]
    if inp.get("widget") == "combobox" and (ident == "normal_format" or "$normal_map_format" in (inp.get("usertag") or {})):
        for value, label in inp.get("items") or []:
            if "opengl" in str(label).lower():
                return value
        return None
    if inp.get("widget") == "togglebutton" and ident.startswith("channel_") and ident[8:] in PAINTER_CHANNELS:
        return 1 if ident[8:] in PROJECT_CHANNELS else 0
    return None


def shown(inp):
    """Paramètre montré à l'artiste (hors système, techniques et types que sbsrender ne reçoit pas en valeur)."""
    return (not inp.get("system") and inp.get("widget") is not None and technical_value(inp) is None
            and inp["type"] not in ("string", "font"))


def env_values(graph, values):
    env = {i["identifier"]: i["default"] for i in graph["inputs"]}
    env.update(values)
    for i in graph["inputs"]:
        t = technical_value(i)
        if t is not None:
            env[i["identifier"]] = t
    return env


_compiled = {}


def condition(expr, env):
    f = _compiled.get(expr)
    if f is None:
        try:
            f = subzero_meta.compile_visibleif(expr)
        except Exception:  # noqa: BLE001 - condition illisible : visible, comme Painter
            f = lambda env: True  # noqa: E731
        _compiled[expr] = f
    try:
        return bool(f(env))
    except Exception:  # noqa: BLE001
        return True


def output_channel(output):
    """(canal, None) ou (None, raison : non représentable dans ArmorPaint, ou usage inconnu)."""
    for u in output.get("usages") or []:
        key = re.sub(r"[^a-z0-9]", "", u.lower())
        if key in USAGE_CHANNEL:
            return USAGE_CHANNEL[key], None
        if key in UNSUPPORTED:
            return None, UNSUPPORTED[key]
    key = re.sub(r"[^a-z0-9]", "", output["identifier"].lower())
    key = key[7:] if key.startswith("channel") and key != "channel" else key
    if key in NAME_CHANNEL:
        return NAME_CHANNEL[key], None
    return None, UNSUPPORTED.get(key) or "Unknown usage: choose a channel by hand"


def auto_mapping(graph, values):
    """[(sortie, canal ou None, raison)] des sorties image visibles, dans l'ordre du graphe ; un canal par sortie au
    plus, la première l'emporte. Sans couleur de base, la diffuse en tient lieu."""
    env = env_values(graph, values)
    rows, taken = [], set()
    for o in graph["outputs"]:
        if o["type"] != "image" or (o.get("visibleif") and not condition(o["visibleif"], env)):
            continue
        ch, why = output_channel(o)
        if ch in taken:
            ch, why = None, "%s already comes from another output" % CHANNEL_LABEL[ch]
        if ch:
            taken.add(ch)
        rows.append([o, ch, why])
    if "base" not in taken:
        for row in rows:
            if row[1] is None and "diffuse" in [u.lower() for u in row[0].get("usages") or []]:
                row[1], row[2] = "base", None
                break
    return rows


def render_values(graph, values):
    """Valeurs à passer à sbsrender : celles qui diffèrent du défaut, plus les paramètres techniques."""
    by_id = {i["identifier"]: i for i in graph["inputs"]}
    out = {}
    for k, v in values.items():
        i = by_id.get(k)
        if i is not None and i["type"] not in ("image", "string", "font") and v != i["default"]:
            out[k] = v
    for i in graph["inputs"]:
        t = technical_value(i)
        if t is not None and t != i["default"]:
            out[i["identifier"]] = t
    return out


def split_alpha(base, opacity=None):
    """Couleur de base dont l'alpha n'est pas partout opaque (décalque) : ArmorPaint multiplie la couleur par l'alpha
    (image_texture_node.c), d'où des bords sombres sous l'opacité. La couleur est réécrite en RGB ; l'alpha va dans
    opacity (niveaux de gris) s'il est donné. Vrai si réécrit. TGA non compressé seulement, ce qu'écrit sbsrender."""
    try:
        with open(base, "rb") as f:
            data = f.read()
    except OSError:
        return False
    if len(data) < 18 or data[2] != 2 or data[16] != 32:
        return False
    start = 18 + data[0] + ((data[5] | data[6] << 8) * ((data[7] + 7) // 8) if data[1] else 0)
    n = (data[12] | data[13] << 8) * (data[14] | data[15] << 8)
    end = start + n * 4
    if len(data) < end:
        return False
    alpha = data[start + 3:end:4]
    if alpha.count(255) == n:
        return False
    rgb = bytearray(n * 3)
    for c in range(3):
        rgb[c::3] = data[start + c:end:4]
    orient = data[17] & 0x30  # origine en haut, de droite à gauche

    def write(path, kind, bpp, desc, body):
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(bytes([0, 0, kind, 0, 0, 0, 0, 0]) + data[8:16] + bytes([bpp, desc]))
            f.write(body)
        os.replace(tmp, path)

    if opacity:
        write(opacity, 3, 8, 0x08 | orient, alpha)  # comme les sorties grises de sbsrender
    write(base, 2, 24, orient, rgb)
    return True


def links(files):
    """Liaisons pour ArmorPaint : [{"file", "output" (0 couleur, 1 alpha), "socket"}]."""
    return [{"file": path, "output": 0, "socket": CHANNEL_SOCKET[ch]} for ch, path in files.items()]


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_recipe(folder):
    with open(os.path.join(folder, RECIPE), encoding="utf-8") as f:
        recipe = json.load(f)
    if not isinstance(recipe, dict) or recipe.get("tool") != "subzero":
        raise ValueError("not a subzero recipe")
    return recipe


def recipe_package(recipe, folder):
    """Chemin du paquet d'une recette : absolu s'il existe, sinon relatif au dossier (projet déplacé)."""
    pkg = recipe.get("package") or {}
    path = pkg.get("path") or ""
    if path and os.path.isfile(path):
        return path
    rel = pkg.get("relative") or ""
    if rel:
        candidate = os.path.normpath(os.path.join(folder, rel))
        if os.path.isfile(candidate):
            return candidate
    return path


def write_recipe(folder, recipe):
    path = os.path.join(folder, RECIPE)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(recipe, f, indent=1, sort_keys=True)
        f.write("\n")
    os.replace(tmp, path)


def log2_size(n):
    return max(4, min(13, int(round(math.log2(max(1, n))))))


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


def read_icons(desc, sbsar):
    """{pkgurl: PNG} des icônes de graphes, rangées à côté du XML de leur assemblage dans l'archive."""
    wanted = {g["pkgurl"]: posixpath.join(posixpath.dirname(a["member"]), g["icon"])
              for a in desc["assemblies"] for g in a["graphs"] if g.get("icon")}
    if not wanted:
        return {}
    try:
        members = subzero_7z.read_7z(sbsar)
    except (OSError, ValueError):
        return {}
    return {url: members[m] for url, m in wanted.items() if members.get(m)}


def read_gray_tga(path):
    """TGA en niveaux de gris non compressé (type 3, celui des sorties grises de sbsrender), que Qt ne lit pas."""
    with open(path, "rb") as f:
        head = f.read(18)
        if len(head) < 18 or head[2] != 3 or head[16] != 8:
            return QtGui.QImage()
        w, h = head[12] | head[13] << 8, head[14] | head[15] << 8
        f.seek(18 + head[0])
        data = f.read(w * h)
    if len(data) < w * h:
        return QtGui.QImage()
    image = QtGui.QImage(data, w, h, w, QtGui.QImage.Format_Grayscale8).copy()
    return image if head[17] & 0x20 else image.transformed(QtGui.QTransform().scale(1, -1))  # origine en bas à gauche


def load_thumbs(files):
    """{canal: QImage réduite} ; les EXR (que Qt ne lit pas) sont absents."""
    thumbs = {}
    for ch, path in files.items():
        if path.lower().endswith(".exr"):
            continue
        image = read_gray_tga(path) if path.lower().endswith(".tga") else QtGui.QImage()
        if image.isNull():
            image = QtGui.QImageReader(path).read()
        if not image.isNull():
            thumbs[ch] = image.scaled(THUMB * 2, THUMB * 2, QtCore.Qt.KeepAspectRatio, QtCore.Qt.SmoothTransformation)
    return thumbs


# ---- Éditeurs de paramètres -------------------------------------------------------------------------------------------


class Editor(QtWidgets.QWidget):
    """Widget d'un paramètre. changed(valeur, mode) : "drag" (curseur tenu), "soon" (clavier, molette), "now"."""

    changed = QtCore.Signal(object, str)

    def __init__(self, inp):
        super().__init__()
        self.inp = inp
        self.lay = QtWidgets.QVBoxLayout(self)
        self.lay.setContentsMargins(0, 0, 0, 0)
        self.lay.setSpacing(2)

    def set_value(self, value):
        raise NotImplementedError


class SliderEditor(Editor):
    """Curseur de 1 à 4 composantes, entier ou flottant ; angle en degrés à l'écran (tours pour sbsrender)."""

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
            row.setSpacing(6)
            if self.n > 1:
                tag = QtWidgets.QLabel(labels[k] if k < len(labels) else "XYZW"[k])
                tag.setObjectName("muted")
                tag.setFixedWidth(62)
                row.addWidget(tag)
            slider = QtWidgets.QSlider(QtCore.Qt.Horizontal)
            steps = int(b - a) if self.is_int else 1000
            slider.setRange(0, max(1, steps))
            if self.is_int:
                spin = QtWidgets.QSpinBox()
                spin.setRange(-10 ** 9 if not inp.get("clamp") else int(a), 10 ** 9 if not inp.get("clamp") else int(b))
            else:
                spin = QtWidgets.QDoubleSpinBox()
                step = float(inp.get("step") or 0.01) * self.scale
                spin.setDecimals(1 if angle else max(2, min(4, -int(math.floor(math.log10(step))) if step > 0 else 3)))
                spin.setSingleStep(step if step > 0 else 0.01)
                if inp.get("clamp"):
                    spin.setRange(a * self.scale, b * self.scale)
                else:
                    spin.setRange(-1e6, 1e6)
            spin.setKeyboardTracking(False)
            spin.setFixedWidth(84)
            spin.setButtonSymbols(QtWidgets.QAbstractSpinBox.NoButtons)
            row.addWidget(slider, 1)
            row.addWidget(spin)
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
    def __init__(self, inp):
        super().__init__(inp)
        labels = inp.get("button_labels") or ()
        text = labels[1] if len(labels) == 2 and tuple(labels) != ("False", "True") else ""
        self.box = QtWidgets.QCheckBox(text)
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


class ColorEditor(Editor):
    def __init__(self, inp):
        super().__init__(inp)
        self.alpha = inp["type"] == "float4"
        self.button = QtWidgets.QPushButton()
        self.button.setFixedHeight(24)
        self.lay.addWidget(self.button)
        self.button.clicked.connect(self._pick)
        self._value = inp["default"]

    def set_value(self, value):
        self._value = list(value)
        c = self._qcolor()
        self.button.setStyleSheet("QPushButton { background: %s; border: 1px solid #3a3a3a; border-radius: 4px; }" % c.name())
        self.button.setToolTip(" ".join("%.3f" % x for x in self._value))

    def _qcolor(self):
        v = [max(0.0, min(1.0, float(x))) for x in self._value] + [1.0]
        return QtGui.QColor.fromRgbF(v[0], v[1], v[2], v[3] if self.alpha else 1.0)

    def _pick(self):
        options = QtWidgets.QColorDialog.ShowAlphaChannel if self.alpha else QtWidgets.QColorDialog.ColorDialogOptions()
        c = QtWidgets.QColorDialog.getColor(self._qcolor(), self, self.inp.get("label") or "", options)
        if c.isValid():
            v = [round(c.redF(), 6), round(c.greenF(), 6), round(c.blueF(), 6)] + ([round(c.alphaF(), 6)] if self.alpha else [])
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


def make_editor(inp):
    widget, typ = inp.get("widget"), inp["type"]
    if typ == "image":
        return ImageEditor(inp)
    if widget == "togglebutton":
        return ToggleEditor(inp)
    if widget == "combobox":
        return ComboEditor(inp)
    if widget == "color" and typ in ("float3", "float4"):
        return ColorEditor(inp)
    if widget == "angle":
        return SliderEditor(inp, angle=True)
    return SliderEditor(inp)


class ParamRow(object):
    def __init__(self, inp, editor, label, reset):
        self.inp, self.editor, self.label, self.reset = inp, editor, label, reset


class Params(QtWidgets.QWidget):
    """Paramètres d'un graphe, groupés dans l'ordre du fichier, conditions de visibilité appliquées."""

    changed = QtCore.Signal(str, object, str)

    def __init__(self):
        super().__init__()
        self.lay = QtWidgets.QVBoxLayout(self)
        self.lay.setContentsMargins(0, 0, 0, 0)
        self.lay.setSpacing(6)
        self.rows = {}
        self.groups = []  # [(bouton, conteneur, [identifiants])]
        self.graph = None

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
        for g in order:
            box = QtWidgets.QWidget()
            grid = QtWidgets.QGridLayout(box)
            grid.setContentsMargins(0 if not g else 10, 2, 0, 4)
            grid.setHorizontalSpacing(10)
            grid.setVerticalSpacing(6)
            grid.setColumnStretch(1, 1)
            if g:
                head = QtWidgets.QToolButton()
                head.setText(" " + g.replace("/", " / "))
                head.setCheckable(True)
                head.setChecked(True)
                head.setArrowType(QtCore.Qt.DownArrow)
                head.setToolButtonStyle(QtCore.Qt.ToolButtonTextBesideIcon)
                head.setStyleSheet("QToolButton { border: none; font-weight: 600; color: %s; }" % MUTED)
                head.toggled.connect(lambda on, b=box, h=head: (b.setVisible(on), h.setArrowType(
                    QtCore.Qt.DownArrow if on else QtCore.Qt.RightArrow)))
                self.lay.addWidget(head)
            else:
                head = None
            for r, inp in enumerate(members[g]):
                label = QtWidgets.QLabel(inp.get("label") or inp["identifier"])
                label.setMinimumWidth(130)
                if inp.get("description"):
                    label.setToolTip(inp["description"])
                editor = make_editor(inp)
                editor.set_value(values.get(inp["identifier"], inp["default"]))
                reset = QtWidgets.QToolButton()
                reset.setText("↺")
                reset.setToolTip("Reset to default")
                reset.setAutoRaise(True)
                grid.addWidget(label, r, 0, QtCore.Qt.AlignTop)
                grid.addWidget(editor, r, 1)
                grid.addWidget(reset, r, 2, QtCore.Qt.AlignTop)
                ident = inp["identifier"]
                editor.changed.connect(lambda v, mode, ident=ident: self.changed.emit(ident, v, mode))
                reset.clicked.connect(lambda _=False, ident=ident: self._reset(ident))
                self.rows[ident] = ParamRow(inp, editor, label, reset)
            self.lay.addWidget(box)
            self.groups.append((head, box, [i["identifier"] for i in members[g]]))
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
            visible = not cond or condition(cond, env)
            row.label.setVisible(visible)
            row.editor.setVisible(visible)
            row.reset.setVisible(visible)
            row.reset.setEnabled(values.get(ident, row.inp["default"]) != row.inp["default"])
        for head, box, idents in self.groups:
            any_visible = any(not self.rows[i].label.isHidden() for i in idents)
            if head is not None:
                head.setVisible(any_visible)
                box.setVisible(any_visible and head.isChecked())
            else:
                box.setVisible(any_visible)


# ---- Fenêtre -------------------------------------------------------------------------------------------------------------


class SubzeroWindow(QtWidgets.QWidget):
    def __init__(self, channel, mac, test=None):
        super().__init__(None, QtCore.Qt.Window | QtCore.Qt.WindowStaysOnTopHint)
        self.channel = channel
        self.mac = mac
        self.test = test
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
        self.worker = Worker()
        self.worker.stepped.connect(self._on_step)
        self.worker.finished.connect(self._on_finished)
        self.soon = QtCore.QTimer(self, singleShot=True, interval=SOON_MS)
        self.soon.timeout.connect(self._commit)
        self.setWindowTitle("subzero")
        self.resize(560, 860)
        self.setAcceptDrops(True)
        self._build()
        self._update_buttons()
        channel.events["raise"] = lambda m: self.to_front()
        channel.events["open"] = self._on_open_event
        channel.events["prefs"] = self._on_prefs_event
        channel.events["project"] = self._on_project_event
        channel.closed.connect(self._on_host_gone)
        self._status("Loading...")
        channel.request("state", None, self._on_state)

    # -- construction

    def _build(self):
        root = QtWidgets.QVBoxLayout(self)
        root.setContentsMargins(16, 14, 16, 12)
        root.setSpacing(10)

        header = QtWidgets.QHBoxLayout()
        self.icon = QtWidgets.QLabel()
        self.icon.setFixedSize(48, 48)
        self.icon.setStyleSheet("background: #1b1b1b; border-radius: 4px;")
        titles = QtWidgets.QVBoxLayout()
        titles.setSpacing(1)
        self.title = QtWidgets.QLabel("No material")
        font = self.title.font()
        font.setPointSizeF(font.pointSizeF() * 1.35)
        font.setWeight(QtGui.QFont.DemiBold)
        self.title.setFont(font)
        self.subtitle = muted("Open a .sbs or .sbsar file, or drop one here.")
        titles.addWidget(self.title)
        titles.addWidget(self.subtitle)
        self.open_button = QtWidgets.QPushButton("Open...")
        self.reload_button = QtWidgets.QPushButton("Reload")
        self.reload_button.setToolTip("Read the file again (after saving it in Designer)")
        header.addWidget(self.icon)
        header.addLayout(titles, 1)
        header.addWidget(self.open_button)
        header.addWidget(self.reload_button)
        root.addLayout(header)
        self.open_button.clicked.connect(self._browse_package)
        self.reload_button.clicked.connect(self._reload)

        self.banner = QtWidgets.QLabel()
        self.banner.setWordWrap(True)
        self.banner.setStyleSheet("background: #3a2a1a; color: %s; border-radius: 4px; padding: 6px 8px;" % WARNING)
        self.banner.hide()
        self.prefs_button = QtWidgets.QPushButton("Open Preferences")
        self.prefs_button.hide()
        self.prefs_button.clicked.connect(lambda: self.channel.request("show_prefs", None, lambda d, e: None))
        banner_row = QtWidgets.QHBoxLayout()
        banner_row.addWidget(self.banner, 1)
        banner_row.addWidget(self.prefs_button)
        root.addLayout(banner_row)

        thumbs = QtWidgets.QHBoxLayout()
        thumbs.setSpacing(6)
        self.thumbs = {}
        for ch, label, _, _ in CHANNELS:
            box = QtWidgets.QVBoxLayout()
            box.setSpacing(2)
            image = QtWidgets.QLabel()
            image.setFixedSize(THUMB, THUMB)
            image.setAlignment(QtCore.Qt.AlignCenter)
            image.setStyleSheet("background: #1b1b1b; border-radius: 3px; color: #555555;")
            caption = QtWidgets.QLabel(label)
            caption.setObjectName("muted")
            caption.setAlignment(QtCore.Qt.AlignCenter)
            box.addWidget(image)
            box.addWidget(caption)
            holder = QtWidgets.QWidget()
            holder.setLayout(box)
            holder.hide()
            thumbs.addWidget(holder)
            self.thumbs[ch] = (holder, image)
        thumbs.addStretch(1)
        root.addLayout(thumbs)

        top = QtWidgets.QGridLayout()
        top.setHorizontalSpacing(10)
        top.setVerticalSpacing(6)
        self.graph_combo = QtWidgets.QComboBox()
        self.preset_combo = QtWidgets.QComboBox()
        self.seed = QtWidgets.QSpinBox()
        self.seed.setRange(0, 10 ** 6)
        self.seed.setKeyboardTracking(False)
        self.seed_button = QtWidgets.QPushButton("Random")
        self.size_combo = QtWidgets.QComboBox()
        seed_row = QtWidgets.QHBoxLayout()
        seed_row.addWidget(self.seed, 1)
        seed_row.addWidget(self.seed_button)
        self.graph_label = QtWidgets.QLabel("Graph")
        self.seed_label = QtWidgets.QLabel("Random Seed")
        top.addWidget(self.graph_label, 0, 0)
        top.addWidget(self.graph_combo, 0, 1)
        top.addWidget(QtWidgets.QLabel("Preset"), 1, 0)
        top.addWidget(self.preset_combo, 1, 1)
        top.addWidget(self.seed_label, 2, 0)
        top.addLayout(seed_row, 2, 1)
        top.addWidget(QtWidgets.QLabel("Size"), 3, 0)
        top.addWidget(self.size_combo, 3, 1)
        top.setColumnStretch(1, 1)
        root.addLayout(top)
        self.graph_combo.activated.connect(self._on_graph)
        self.preset_combo.activated.connect(self._on_preset)
        self.seed.valueChanged.connect(lambda v: self._on_param("$randomseed", v, "now"))
        self.seed_button.clicked.connect(lambda: self.seed.setValue(int.from_bytes(os.urandom(3), "little") % 10 ** 6))
        self.size_combo.activated.connect(self._on_size)

        self.params = Params()
        self.params.changed.connect(self._on_param)
        scroll = QtWidgets.QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        scroll.setWidget(self.params)
        root.addWidget(scroll, 1)

        self.outputs_head = QtWidgets.QToolButton()
        self.outputs_head.setCheckable(True)
        self.outputs_head.setArrowType(QtCore.Qt.RightArrow)
        self.outputs_head.setToolButtonStyle(QtCore.Qt.ToolButtonTextBesideIcon)
        self.outputs_head.setStyleSheet("QToolButton { border: none; font-weight: 600; color: %s; }" % MUTED)
        self.outputs = QtWidgets.QTreeWidget()
        self.outputs.setHeaderLabels(["Output", "Channel"])
        self.outputs.setRootIsDecorated(False)
        self.outputs.setMaximumHeight(170)
        self.outputs.hide()
        self.outputs_head.toggled.connect(lambda on: (self.outputs.setVisible(on), self.outputs_head.setArrowType(
            QtCore.Qt.DownArrow if on else QtCore.Qt.RightArrow)))
        root.addWidget(self.outputs_head)
        root.addWidget(self.outputs)

        bottom = QtWidgets.QHBoxLayout()
        self.name = QtWidgets.QLineEdit()
        self.name.setPlaceholderText("Material name")
        self.live = QtWidgets.QCheckBox("Live")
        self.live.setChecked(True)
        self.live.setToolTip("Update the material in ArmorPaint after each change")
        bottom.addWidget(QtWidgets.QLabel("Material"))
        bottom.addWidget(self.name, 1)
        bottom.addWidget(self.live)
        root.addLayout(bottom)
        self.live.toggled.connect(lambda on: (self._update_buttons(), on and self.dirty and self._commit()))

        actions = QtWidgets.QHBoxLayout()
        self.status = muted()
        self.duplicate_button = QtWidgets.QPushButton("Duplicate")
        self.duplicate_button.setToolTip("Send as a new material, in a new folder")
        self.send_button = QtWidgets.QPushButton("Send to ArmorPaint")
        self.send_button.setObjectName("primary")
        self.send_button.setDefault(True)
        actions.addWidget(self.status, 1)
        actions.addWidget(self.duplicate_button)
        actions.addWidget(self.send_button)
        root.addLayout(actions)
        self.send_button.clicked.connect(self._send)
        self.duplicate_button.clicked.connect(lambda: self._send(duplicate=True))

    def _status(self, text, color=None):
        self.status.setText(text)
        self.status.setStyleSheet("color: %s;" % (color or MUTED))

    def _update_buttons(self):
        ready = self.graph is not None and self.tools is not None and any(ch for _, ch, _ in self.mapping)
        sent = self.material_dir is not None
        self.send_button.setText("Update" if sent else "Send to ArmorPaint")
        self.send_button.setEnabled(ready and (not sent or self.dirty or not self.live.isChecked()))
        self.duplicate_button.setVisible(sent)
        self.duplicate_button.setEnabled(ready)
        self.reload_button.setEnabled(self.package is not None)
        self.live.setVisible(sent)

    # -- état et évènements d'ArmorPaint

    def _on_state(self, data, error):
        if error:
            self._status("ArmorPaint: %s" % error.get("message"), ERROR)
            return
        self.state = data or {}
        self.test_log["state"] = self.state
        sizes = [("Project (%d)" % self._project_res(), None)] + [(str(1 << n), n) for n in (9, 10, 11, 12)]
        self.size_combo.clear()
        for label, n in sizes:
            self.size_combo.addItem(label, n)
        opening = (self.test or {}).get("open") or self.state.get("open")
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
        if self.test is not None:
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
        self.state.update(message.get("state") or {})
        self.material_dir = None
        self.material_name = None
        self.base_dir = None
        self._update_buttons()

    def _on_host_gone(self):
        log("ArmorPaint closed the connection")
        QtWidgets.QApplication.instance().quit()

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
            self._show_thumbs(result["thumbs"])
            if job.kind == "preview":
                self._status("Preview %d px in %.2f s" % (1 << PREVIEW_SIZE, result["seconds"]))
            self.test_log.setdefault("renders", []).append({"kind": job.kind, "seconds": result["seconds"],
                                                           "files": result["files"]})
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
            self._set_graph(graph, values=recipe.get("values") or {}, entries=recipe.get("entries") or {},
                            preset=recipe.get("preset"), size=recipe.get("size"))
            self.name.setText(recipe.get("name") or graph.get("label") or "")
            files = {ch: os.path.join(self.material_dir, f) for ch, f in (recipe.get("files") or {}).items()
                     if os.path.isfile(os.path.join(self.material_dir, f))}
            self._show_thumbs(load_thumbs(files))
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
            self.overrides = {}
            self._set_graph(graph)
            self.name.setText(graph.get("label") or os.path.splitext(os.path.basename(pkg["path"]))[0])
            self._commit()
        cooked = " · cooked in %.2f s" % pkg["cook"]["seconds"] if pkg.get("cook") and not pkg["cook"].get("cached") else ""
        self.subtitle.setText("%s · %s%s" % (os.path.basename(pkg["path"]), graph.get("category") or graph.get("graphtype") or "graph", cooked))
        self.test_log["opened"] = {"path": pkg["path"], "graph": graph["pkgurl"], "cook": pkg.get("cook")}
        self._update_buttons()
        if recipe is not None or self.test is not None:
            self._test_next()

    def _set_graph(self, graph, values=None, entries=None, preset=None, size=None):
        self.graph = graph
        by_id = {i["identifier"]: i for i in graph["inputs"]}
        self.values = {i["identifier"]: i["default"] for i in graph["inputs"] if shown(i) and i["type"] != "image"}
        if "$randomseed" in by_id:
            self.values["$randomseed"] = by_id["$randomseed"]["default"]
        for k, v in (values or {}).items():
            if k in self.values:
                self.values[k] = v
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
        rows = auto_mapping(self.graph, self.values)
        for row in rows:
            o = row[0]["identifier"]
            if o in self.overrides:
                row[1] = self.overrides[o] or None
                row[2] = None if row[1] else "not linked"
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
        self.outputs_head.setText(" Outputs (%d linked%s)" % (linked, ", %d not linked" % unlinked if unlinked else ""))
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
        self._status("Rendering preview...")

        def job(track, step):
            res = subzero_render.render(sbsar, graph, out_dir, tools, values=values, entries=entries,
                                        size=(PREVIEW_SIZE, PREVIEW_SIZE), outputs=[o for o, _ in outs],
                                        names={o: ch for o, ch in outs}, cancel=track)
            files = {ch: res["files"][o] for o, ch in outs}
            return {"files": files, "thumbs": load_thumbs(files), "seconds": res["seconds"]}

        self._submit(job, "preview")

    def _final_size(self):
        if self.size is not None:
            return (self.size, self.size)
        res = self.state.get("resolution") or [2048, 2048]
        return (log2_size(res[0]), log2_size(res[1] if len(res) > 1 else res[0]))

    def _render_final(self, folder, action):
        graph, tools, sbsar = self.graph, self.tools, self.package["sbsar"]
        values, entries = render_values(graph, self.values), dict(self.entries)
        outs = self.linked()
        prefix = os.path.basename(folder.rstrip("/"))
        names = {o: "%s_%s" % (prefix, CHANNEL_SUFFIX[ch]) for o, ch in outs}
        exr = [o for o, ch in outs if ch in DATA_EXR] if int(self.state.get("bits", 8)) > 8 else []
        size = self._final_size()
        self._status("Rendering %d px..." % (1 << size[0]))
        self.dirty = False
        self._update_buttons()

        def job(track, step):
            os.makedirs(folder, exist_ok=True)
            res = subzero_render.render(sbsar, graph, folder, tools, values=values, entries=entries, size=size,
                                        outputs=[o for o, _ in outs], names=names, exr=exr, cancel=track)
            files = {ch: res["files"][o] for o, ch in outs}
            if "base" in files:  # décalque : alpha de la couleur vers l'opacité, faute de sortie opacité
                opacity = os.path.join(folder, "%s_%s.tga" % (prefix, CHANNEL_SUFFIX["opacity"]))
                if split_alpha(files["base"], None if "opacity" in files else opacity) and "opacity" not in files:
                    files["opacity"] = opacity
                elif "opacity" not in files and os.path.isfile(opacity):
                    os.remove(opacity)  # d'un rendu précédent
            data = {"files": files, "seconds": res["seconds"], "size": size, "warnings": res["warnings"]}
            step(data)
            data = dict(data)
            data["thumbs"] = load_thumbs(files)
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
        args = {"dir": folder, "name": self.name.text().strip() or self.graph.get("label") or "Material",
                "links": links(data["files"])}
        request = (action, args, data)
        if self.request_busy:
            self.pending = request  # le plus récent seulement
            return
        self._request(request)

    def _request(self, request):
        action, args, data = request
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
                verb = {"create": "Sent", "update": "Updated"}.get(action, "Sent")
                self._status("%s %s: %d px in %.2f s, ArmorPaint %.2f s" % (
                    verb, self.material_name or "", 1 << data["size"][0], data["seconds"], took), SUCCESS)
            self._update_buttons()
            if self.pending is not None:
                request, self.pending = self.pending, None
                self._request(request)
            else:
                self._test_next()

        self.channel.request(action, args, done)

    def _write_recipe(self, folder, outs, data):
        pkg_path = self.package["path"]
        recipe = {
            "tool": "subzero",
            "version": 1,
            "name": self.name.text().strip() or self.graph.get("label") or "",
            "package": {"path": pkg_path, "relative": os.path.relpath(pkg_path, folder), "sha256": file_sha256(pkg_path)},
            "graph": self.graph["pkgurl"],
            "preset": self.preset,
            "values": {k: v for k, v in self.values.items()
                       if v != next((i["default"] for i in self.graph["inputs"] if i["identifier"] == k), None)},
            "entries": dict(self.entries),
            "size": self.size,
            "outputs": {o: ch for o, ch in outs},
            "overrides": dict(self.overrides),
            "files": {ch: os.path.basename(p) for ch, p in data["files"].items()},
            "sbsrender": self.tools.get("version") if self.tools else None,
            "rendered": {"size": [1 << data["size"][0], 1 << data["size"][1]], "seconds": data["seconds"],
                         "date": datetime.datetime.now().isoformat(timespec="seconds")},
        }
        write_recipe(folder, recipe)

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
                image.setText("EXR" if ch in DATA_EXR and int(self.state.get("bits", 8)) > 8 else "")

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
        path = QtWidgets.QFileDialog.getExistingDirectory(
            self, "Folder for the textures (the project is not saved yet)", os.path.expanduser("~"))
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

    # -- fenêtre

    def to_front(self):
        self.show()
        self.raise_()
        self.activateWindow()
        if self.mac is not None:
            self.mac.activate()

    def closeEvent(self, event):
        super().closeEvent(event)
        self.worker.stop()
        shutil.rmtree(self.preview_dir, ignore_errors=True)
        if self.mac is not None:
            self.mac.hide()  # macOS rend le premier plan à ArmorPaint
        QtWidgets.QApplication.instance().quit()

    # -- essai sans intervention

    def _run_test(self):
        """Réglages de l'essai, puis action ; la suite vient de _test_next, à la fin de chaque étape."""
        t = self.test
        if self.graph is None:
            self._finish_test()
            return
        if "graph" in t and t["graph"] != self.graph["pkgurl"]:
            self._set_graph(subzero_render.find_graph(self.package["description"], t["graph"]))
        if "preset" in t:
            self._on_preset(max(0, self.preset_combo.findData(t["preset"])))
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
        if "live" in t:
            self.live.setChecked(bool(t["live"]))
        self.soon.stop()
        action = t.get("action", "create")
        t["_stage"] = "action"
        if action == "none":
            self._finish_test()
        elif action == "duplicate":
            self._send(duplicate=True)
        elif action == "update" and self.material_dir is not None:
            self._render_final(self.material_dir, "update")
        else:
            self._send()

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
            "shown": sorted(k for k, r in self.params.rows.items() if not r.label.isHidden()),
            "banner": self.banner.text() if not self.banner.isHidden() else "",
            "tools": self.tools,
        })
        if t.get("screenshot"):
            self.grab().save(t["screenshot"])
        if t.get("result"):
            with open(t["result"], "w") as f:
                json.dump(self.test_log, f, indent=1, default=str)
        self.close()


def main():
    app, mac, channel = studio_qt.start("subzero")
    test = None
    if os.environ.get("SUBZERO_TEST"):
        with open(os.environ["SUBZERO_TEST"]) as f:
            test = json.load(f)
    window = SubzeroWindow(channel, mac, test)
    window.to_front()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
