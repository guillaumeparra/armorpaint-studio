#!/usr/bin/env python3
# Viewer de la fenêtre subzero : panneau carré à gauche des réglages (sa largeur suit la hauteur de la fenêtre), qui
# montre les cartes rendues (aperçu ou textures exportées), une à la fois. Touches 1 à 9 (pavé numérique, rangée du
# haut, AZERTY compris) : carte n parmi celles liées, dans l'ordre des vignettes ; clic sur une vignette aussi.
# Molette : zoom autour du curseur ; glisser (bouton gauche ou milieu) : déplacer ; double clic, F et 0 : ajuster et
# 1:1 (un pixel de l'image par pixel de l'écran) ; + et - : zoom. Au pied, valeurs du fichier sous le curseur.
#
# Couleur, comme le viewer de Nuke : trois tirettes au-dessus de l'image, exposition (diaphragmes) et saturation dans
# scene_linear, gamma après la transformation d'affichage (setLinearCC et setDisplayCC d'OCIO). Une carte couleur passe
# de son espace à scene_linear, puis par display/view/look de subzero_color (ou retourne à son espace sans affichage
# OCIO) ; une carte de donnée est montrée telle quelle, tirettes comprises. Sans OCIO, décodage et encodage sRGB ; sans
# numpy, image telle quelle et tirettes inactives.
#
# Calcul en arrière-plan, le plus récent seulement : d'abord une réduction d'au plus PROXY pixels de côté, puis l'image
# entière. OCIO relâche le GIL : ses transformations sont réparties sur plusieurs fils (ACES 2.0 : 1,1 s pour 2048² sur
# un seul, 0,2 s sur huit). Les cartes lues restent en mémoire dans la limite de CACHE_BYTES.

import math
import os
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

from PySide6 import QtCore, QtGui, QtWidgets

import subzero_color
import subzero_exr
from studio_qt import ERROR, MUTED, log

try:
    import numpy as np
except ImportError:
    np = None

PROXY = 1024  # côté maximal du premier calcul
CACHE_BYTES = 768 << 20  # cartes lues gardées (tableaux source et linéaire)
ZOOM_MIN, ZOOM_MAX = 1.0 / 64, 64.0
WHEEL_STEP = 1.2  # facteur d'un cran de molette
SMOOTH_BELOW = 2.0  # zoom à partir duquel les pixels se voient (sans lissage)
REC709_LUMA = (0.2126, 0.7152, 0.0722)
# Touches de la rangée du haut par position (AZERTY : & é " ' ( - è _ ç sans majuscule) : code virtuel macOS
# (kVK_ANSI_1…9), code de touche X11 (evdev + 8 : 10…18) et code de balayage Windows (2…10).
MAC_DIGITS = {0x12: 1, 0x13: 2, 0x14: 3, 0x15: 4, 0x17: 5, 0x16: 6, 0x1A: 7, 0x1C: 8, 0x19: 9}
MAC_ZERO = 0x1D
TEXT_INPUTS = (QtWidgets.QLineEdit, QtWidgets.QAbstractSpinBox, QtWidgets.QTextEdit, QtWidgets.QPlainTextEdit)
SLIDERS = (  # (clé, libellé, défaut, minimum et maximum de la case, de la tirette, pas, infobulle)
    ("exposure", "Exposure", 0.0, (-20.0, 20.0), (-6.0, 6.0), 0.1, "Exposure in stops, in scene linear (before the view)"),
    ("gamma", "Gamma", 1.0, (0.05, 8.0), (0.25, 4.0), 0.05, "Gamma after the display transform"),
    ("saturation", "Saturation", 1.0, (0.0, 8.0), (0.0, 2.0), 0.05, "Saturation in scene linear (luma kept)"),
)
DEFAULTS = {k: d for k, _, d, _, _, _, _ in SLIDERS}

_pool = None


def pool():
    global _pool
    if _pool is None:
        _pool = ThreadPoolExecutor(max(2, min(8, os.cpu_count() or 2)), thread_name_prefix="subzero-viewer")
    return _pool


# ---- Lecture ------------------------------------------------------------------------------------------------------------


def read_tga(path):
    """TGA non compressé : gris 8 bits (type 3, sorties grises de sbsrender, que Qt ne lit pas), couleur 24 ou 32 bits
    (type 2), même sans le pied TGA 2.0 (couleur de base réécrite par split_alpha avant le lot E11). QImage nulle
    sinon."""
    with open(path, "rb") as f:
        head = f.read(18)
        if len(head) < 18 or head[1] or (head[2], head[16]) not in ((3, 8), (2, 24), (2, 32)):
            return QtGui.QImage()
        w, h, n = head[12] | head[13] << 8, head[14] | head[15] << 8, head[16] // 8
        f.seek(18 + head[0])
        data = f.read(w * h * n)
    if len(data) < w * h * n:
        return QtGui.QImage()
    fmt = {1: QtGui.QImage.Format_Grayscale8, 3: QtGui.QImage.Format_BGR888, 4: QtGui.QImage.Format_ARGB32}[n]
    image = QtGui.QImage(data, w, h, w * n, fmt).copy()  # ARGB32 : octets B, G, R, A en petit-boutiste, comme le TGA
    flip = QtGui.QTransform().scale(-1 if head[17] & 0x10 else 1, 1 if head[17] & 0x20 else -1)  # origine en bas
    return image if flip.isIdentity() else image.transformed(flip)


def read_qimage(path):
    """QImage 8 bits d'une carte (sans numpy) ; nulle si elle ne se lit pas."""
    lower = path.lower()
    if lower.endswith(".exr"):
        try:
            w, h, kind, data = subzero_exr.thumbnail(path, 1 << 16)
        except (OSError, subzero_exr.Error):
            return QtGui.QImage()
        if kind == "gray":
            return QtGui.QImage(data, w, h, w, QtGui.QImage.Format_Grayscale8).copy()
        return QtGui.QImage(data, w, h, w * 3, QtGui.QImage.Format_RGB888).copy()
    image = read_tga(path) if lower.endswith(".tga") else QtGui.QImage()
    return image if not image.isNull() else QtGui.QImageReader(path).read()


def read_pixels(path):
    """Valeurs du fichier : tableau numpy (hauteur, largeur, 1 ou 3) du type du fichier (float16/32 pour un EXR,
    uint8 ou uint16 sinon) et diviseur qui ramène à 0-1. Lève OSError ou ValueError."""
    lower = path.lower()
    if lower.endswith(".exr"):
        try:
            w, h, planes, _, _ = subzero_exr.read_planes(path)
        except subzero_exr.Error as e:
            raise ValueError(str(e))
        color = [planes[c] for c in ("R", "G", "B") if c in planes]
        if len(color) == 3:
            return np.stack(color, axis=-1), 1.0
        gray = planes.get("Y", color[0] if color else None)
        if gray is None:
            raise ValueError("no Y or R, G, B channels")
        return gray[..., None], 1.0
    if lower.endswith(".tga"):
        with open(path, "rb") as f:
            data = f.read()
        if len(data) >= 18 and not data[1] and (data[2], data[16]) in ((3, 8), (2, 24), (2, 32)):
            w, h, n = data[12] | data[13] << 8, data[14] | data[15] << 8, data[16] // 8
            px = np.frombuffer(data, np.uint8, count=w * h * n, offset=18 + data[0]).reshape(h, w, n)
            px = px[..., :1] if n == 1 else px[..., 2::-1]  # BGR(A) -> RGB
            if data[17] & 0x10:
                px = px[:, ::-1]
            if not data[17] & 0x20:
                px = px[::-1]
            return np.ascontiguousarray(px), 255.0
    image = QtGui.QImageReader(path).read()
    if image.isNull():
        raise ValueError("unreadable image")
    deep = image.depth() > 32 or image.format() == QtGui.QImage.Format_Grayscale16
    gray = image.isGrayscale() and not image.hasAlphaChannel()
    fmt = (QtGui.QImage.Format_Grayscale16 if gray else QtGui.QImage.Format_RGBX64) if deep else \
        (QtGui.QImage.Format_Grayscale8 if gray else QtGui.QImage.Format_RGBX8888)
    image = image.convertToFormat(fmt)
    w, h = image.width(), image.height()
    dtype, n = (np.uint16 if deep else np.uint8), (1 if gray else 4)
    size = 2 if deep else 1
    rows = np.frombuffer(image.constBits(), np.uint8, count=image.sizeInBytes()).reshape(h, image.bytesPerLine())
    px = rows[:, :w * n * size].copy().view(dtype).reshape(h, w, n)
    return (px if gray else np.ascontiguousarray(px[..., :3])), (65535.0 if deep else 255.0)


# ---- Couleur ------------------------------------------------------------------------------------------------------------


def srgb_decode(x):
    return np.where(x <= 0.04045, x / 12.92, np.power((np.maximum(x, 0.04045) + 0.055) / 1.055, 2.4)).astype(np.float32)


def srgb_encode(x):
    x = np.maximum(x, 0.0)
    return np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(np.maximum(x, 0.0031308), 1.0 / 2.4) - 0.055).astype(np.float32)


def apply_proc(proc, rgb, cancel=None):
    """Processeur OCIO appliqué sur place à un tableau (h, w, 3) float32 contigu, par bandes de lignes en parallèle.
    Faux si cancel() l'a interrompu."""
    h = rgb.shape[0]
    bands = max(1, min(h, (os.cpu_count() or 2) * 2))
    edges = [h * i // bands for i in range(bands + 1)]

    def run(i):
        if cancel is not None and cancel():
            return
        proc.applyRGB(rgb[edges[i]:edges[i + 1]].reshape(-1))

    list(pool().map(run, range(bands)))
    return not (cancel is not None and cancel())


class Look(object):
    """Chaîne d'affichage d'une carte : to_linear (espace du fichier -> scene_linear), display (scene_linear ->
    écran), luma ; describe pour les essais."""

    def __init__(self, color, channel, space):
        self.channel = channel
        self.to_linear = self.display = None  # processeur OCIO, "srgb" ou None
        self.luma = REC709_LUMA
        self.describe = "raw"
        c = color if color is not None and color.ready else None
        is_color = channel in subzero_color.COLOR_MAPS
        if c is not None:
            try:
                self.luma = tuple(float(v) for v in c.config.getDefaultLumaCoefs())
            except Exception:  # noqa: BLE001
                pass
            name = c.resolve(space) or c.builtin(space) if space else None
            if not is_color or name is None or c.is_data(name):
                return
            linear = c.resolve("scene_linear")
            try:
                if linear is None:  # pas de rôle scene_linear : l'affichage du canal, tirettes sur les valeurs encodées
                    self.display = c.view_processor(name, channel)
                    self.describe = "%s -> view" % name
                    return
                self.to_linear = c.processor(name, linear)
                self.display = c.display_processor(linear) if c.viewport else c.processor(linear, c.source(channel))
            except subzero_color.Error as e:
                log("viewer: %s" % e)
                self.to_linear = self.display = None
                return
            if c.viewport:
                shown = "%s / %s" % (c.display, c.view) + (" / %s" % c.look if c.look else "")
            else:
                shown = c.source(channel)
            self.describe = "%s -> %s -> %s" % (name, linear, shown)
        elif is_color:
            self.to_linear = self.display = "srgb"
            self.describe = "sRGB -> linear -> sRGB"


def linearize(pixels, scale, look, cancel=None):
    """Valeurs du fichier -> float32 (h, w, 3) contigu, dans scene_linear pour une carte couleur."""
    rgb = pixels.astype(np.float32)
    if scale != 1.0:
        rgb *= np.float32(1.0 / scale)
    if rgb.shape[2] == 1:
        rgb = np.repeat(rgb, 3, axis=2)
    rgb = np.ascontiguousarray(rgb)
    if look.to_linear == "srgb":
        rgb = srgb_decode(rgb)
    elif look.to_linear is not None:
        apply_proc(look.to_linear, rgb, cancel)
    return rgb


def grade(linear, look, params, cancel=None):
    """Image 8 bits RGB (h, w, 3) : exposition et saturation dans scene_linear, affichage, gamma. None si annulé."""
    exposure, gamma, saturation = params["exposure"], params["gamma"], params["saturation"]
    x = linear * np.float32(2.0 ** exposure) if exposure else linear.copy()
    if saturation != 1.0:
        luma = x @ np.asarray(look.luma, np.float32)
        x -= luma[..., None]
        x *= np.float32(saturation)
        x += luma[..., None]
    if cancel is not None and cancel():
        return None
    if look.display == "srgb":
        x = srgb_encode(x)
    elif look.display is not None and not apply_proc(look.display, x, cancel):
        return None
    if gamma != 1.0:
        np.power(np.maximum(x, 0.0, out=x), np.float32(1.0 / gamma), out=x)
    return (np.clip(x, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


def rgb_qimage(rgb):
    h, w = rgb.shape[:2]
    data = np.ascontiguousarray(rgb)
    return QtGui.QImage(data.data, w, h, data.strides[0], QtGui.QImage.Format_RGB888).copy()


# ---- Calcul en arrière-plan ------------------------------------------------------------------------------------------


class Source(object):
    """Carte lue : valeurs du fichier (pixels, scale), linéaires (linear) ou QImage sans numpy (image)."""

    def __init__(self, key, look):
        self.key = key
        self.look = look
        self.pixels = self.linear = self.image = None
        self.scale = 1.0
        self.width = self.height = 0
        self.error = None

    @property
    def nbytes(self):
        return sum(a.nbytes for a in (self.pixels, self.linear) if a is not None) + \
            (self.image.sizeInBytes() if self.image is not None else 0)


class Pipeline(QtCore.QObject):
    """Un fil : le travail le plus récent seulement ; un travail remplacé s'arrête à son prochain point de contrôle.
    done(résultat) arrive dans le fil de l'interface, pour chaque étape (réduction, puis image entière)."""

    done = QtCore.Signal(object)

    def __init__(self):
        super().__init__()
        self._cond = threading.Condition()
        self._job = None
        self._gen = 0
        self.busy = False
        self._quit = False
        self._thread = threading.Thread(target=self._loop, name="subzero-viewer", daemon=True)
        self._thread.start()

    def submit(self, job):
        with self._cond:
            self._gen += 1
            self._job = (self._gen, job)
            self.busy = True
            self._cond.notify()

    def stop(self):
        with self._cond:
            self._quit = True
            self._gen += 1
            self._cond.notify()

    def wait(self, timeout=30.0):
        """Attend la fin du travail en cours (essais) ; les résultats arrivent ensuite par la boucle Qt."""
        with self._cond:
            self._cond.wait_for(lambda: not self.busy, timeout)

    def _loop(self):
        while True:
            with self._cond:
                self._cond.wait_for(lambda: self._job is not None or self._quit)
                if self._quit:
                    return
                gen, job = self._job
                self._job = None

            def cancel(gen=gen):
                return gen != self._gen

            try:
                job(cancel, lambda result: cancel() or self.done.emit(result))
            except Exception as e:  # noqa: BLE001 — carte illisible : message dans le viewer
                if not cancel():
                    self.done.emit({"error": "%s: %s" % (type(e).__name__, e)})
            with self._cond:
                if self._job is None:
                    self.busy = False
                    self._cond.notify_all()


# ---- Image --------------------------------------------------------------------------------------------------------------


class Canvas(QtWidgets.QWidget):
    """Image zoomable. zoom : pixels de l'écran (physiques) par pixel de l'image ; pan : centre de l'image par rapport
    au centre du canevas, en pixels logiques. fitted : l'image suit la taille du canevas."""

    zoomed = QtCore.Signal(float)
    hovered = QtCore.Signal(object)  # (x, y) en pixels de l'image, ou None
    toggled = QtCore.Signal()  # double clic

    def __init__(self):
        super().__init__()
        self.setMouseTracking(True)
        self.setMinimumSize(160, 160)
        self.setFocusPolicy(QtCore.Qt.ClickFocus)
        self.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Expanding)
        self.pixmap = None
        self.image = None  # QImage montrée (réduction ou image entière)
        self.size_px = None  # (largeur, hauteur) de l'image source
        self.zoom = 1.0
        self.pan = QtCore.QPointF()
        self.fitted = True
        self.message = ""
        self._drag = None

    # -- état

    def dpr(self):
        return self.devicePixelRatioF() or 1.0

    def set_image(self, image, size):
        """image : QImage (réduction ou image entière) de la source de taille size (largeur, hauteur)."""
        old = self.size_px
        self.image = image if image is not None and not image.isNull() else None
        self.pixmap = QtGui.QPixmap.fromImage(image) if self.image is not None else None
        self.size_px = size
        if size is not None and old != size:
            if self.fitted or old is None:
                self.fit()
            else:  # autre taille (512 -> 2048) : même taille à l'écran, même centre
                self.set_zoom(self.zoom * old[0] / size[0], emit=True)
        self.update()

    def set_message(self, text):
        self.message = text
        self.update()

    def fit_zoom(self):
        if not self.size_px:
            return 1.0
        margin = 24
        w, h = max(1, self.width() - margin), max(1, self.height() - margin)
        return min(w / self.size_px[0], h / self.size_px[1]) * self.dpr()

    def fit(self):
        self.fitted = True
        self.pan = QtCore.QPointF()
        self.set_zoom(self.fit_zoom(), emit=True)

    def one_to_one(self, at=None):
        self.zoom_to(1.0, at)

    def set_zoom(self, zoom, emit=False):
        self.zoom = min(ZOOM_MAX, max(ZOOM_MIN, zoom))
        self.update()
        if emit:
            self.zoomed.emit(self.zoom)

    def zoom_to(self, zoom, at=None):
        """Zoom autour du point at (coordonnées du canevas ; défaut : le centre), qui reste sous le curseur."""
        zoom = min(ZOOM_MAX, max(ZOOM_MIN, zoom))
        if at is None:
            at = QtCore.QPointF(self.width() / 2.0, self.height() / 2.0)
        center = QtCore.QPointF(self.width() / 2.0, self.height() / 2.0)
        k = zoom / self.zoom
        self.pan = at - center - (at - center - self.pan) * k
        self.fitted = False
        self.set_zoom(zoom, emit=True)

    # -- géométrie

    def image_rect(self):
        """Rectangle de l'image dans le canevas (pixels logiques)."""
        s = self.zoom / self.dpr()
        w, h = self.size_px[0] * s, self.size_px[1] * s
        cx, cy = self.width() / 2.0 + self.pan.x(), self.height() / 2.0 + self.pan.y()
        return QtCore.QRectF(cx - w / 2.0, cy - h / 2.0, w, h)

    def to_image(self, pos):
        if not self.size_px:
            return None
        r = self.image_rect()
        s = self.zoom / self.dpr()
        x, y = int(math.floor((pos.x() - r.left()) / s)), int(math.floor((pos.y() - r.top()) / s))
        if 0 <= x < self.size_px[0] and 0 <= y < self.size_px[1]:
            return x, y
        return None

    # -- dessin

    def paintEvent(self, event):
        p = QtGui.QPainter(self)
        p.fillRect(self.rect(), QtGui.QColor("#1d1d1d"))
        if self.pixmap is not None and self.size_px:
            r = self.image_rect()
            visible = r.intersected(QtCore.QRectF(self.rect()))
            if not visible.isEmpty():
                # seule la partie visible, dans les pixels de la réduction ou de l'image entière
                kx, ky = self.pixmap.width() / r.width(), self.pixmap.height() / r.height()
                src = QtCore.QRectF((visible.left() - r.left()) * kx, (visible.top() - r.top()) * ky,
                                    visible.width() * kx, visible.height() * ky)
                p.setRenderHint(QtGui.QPainter.SmoothPixmapTransform, self.zoom < SMOOTH_BELOW)
                p.drawPixmap(visible, self.pixmap, src)
            p.setPen(QtGui.QPen(QtGui.QColor("#4a4a4a"), 1))
            p.setBrush(QtCore.Qt.NoBrush)
            p.drawRect(r.adjusted(-0.5, -0.5, 0.5, 0.5))
        if self.message:
            p.setPen(QtGui.QColor(ERROR if self.message.startswith("!") else MUTED))
            p.drawText(self.rect().adjusted(16, 16, -16, -16), QtCore.Qt.AlignCenter | QtCore.Qt.TextWordWrap,
                       self.message.lstrip("!"))
        p.end()

    def resizeEvent(self, event):
        super().resizeEvent(event)
        if self.fitted:
            self.set_zoom(self.fit_zoom(), emit=True)

    # -- souris

    def wheelEvent(self, event):
        delta = event.angleDelta().y() or event.angleDelta().x()
        if not delta or not self.size_px:
            return
        self.zoom_to(self.zoom * WHEEL_STEP ** (delta / 120.0), event.position())

    def mousePressEvent(self, event):
        if event.button() in (QtCore.Qt.LeftButton, QtCore.Qt.MiddleButton):
            self._drag = (event.position(), QtCore.QPointF(self.pan))
            self.setCursor(QtCore.Qt.ClosedHandCursor)

    def mouseMoveEvent(self, event):
        if self._drag is not None:
            start, pan = self._drag
            self.pan = pan + event.position() - start
            self.fitted = False
            self.update()
        self.hovered.emit(self.to_image(event.position()))

    def mouseReleaseEvent(self, event):
        self._drag = None
        self.unsetCursor()

    def mouseDoubleClickEvent(self, event):
        if event.button() == QtCore.Qt.LeftButton and self.size_px:
            if self.fitted:
                self.one_to_one(event.position())
            else:
                self.fit()
            self.toggled.emit()

    def leaveEvent(self, event):
        self.hovered.emit(None)


# ---- Panneau ------------------------------------------------------------------------------------------------------------


class Elided(QtWidgets.QLabel):
    """Libellé d'une ligne, coupé par « … » quand la place manque."""

    def __init__(self):
        super().__init__()
        self.setObjectName("caption")
        self.full = ""
        self.setSizePolicy(QtWidgets.QSizePolicy.Ignored, QtWidgets.QSizePolicy.Preferred)

    def setText(self, text):
        self.full = text
        self._elide()

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self._elide()

    def _elide(self):
        super().setText(self.fontMetrics().elidedText(self.full, QtCore.Qt.ElideMiddle, max(0, self.width())))


class Slider(QtWidgets.QWidget):
    """Tirette à la Nuke : libellé (clic : valeur par défaut), case, glissière."""

    changed = QtCore.Signal(str, float, bool)  # clé, valeur, glissement en cours

    def __init__(self, key, label, default, box_range, slide_range, step, tip):
        super().__init__()
        self.key, self.default = key, default
        self.log = key == "gamma"  # gamma : glissière logarithmique, 1 au milieu
        lo, hi = slide_range
        self.lo, self.hi = (math.log2(lo), math.log2(hi)) if self.log else (lo, hi)
        row = QtWidgets.QHBoxLayout(self)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(4)
        self.label = QtWidgets.QToolButton()
        self.label.setObjectName("reset")
        self.label.setText(label)
        self.label.setToolTip("%s\nClick: %g" % (tip, default))
        self.label.setCursor(QtCore.Qt.PointingHandCursor)
        self.box = QtWidgets.QDoubleSpinBox()
        self.box.setObjectName("value")
        self.box.setRange(*box_range)
        self.box.setDecimals(2)
        self.box.setSingleStep(step)
        self.box.setKeyboardTracking(False)
        self.box.setButtonSymbols(QtWidgets.QAbstractSpinBox.NoButtons)
        self.box.setFixedWidth(46)
        self.box.setAlignment(QtCore.Qt.AlignRight)
        self.slider = QtWidgets.QSlider(QtCore.Qt.Horizontal)
        self.slider.setRange(0, 1000)
        self.slider.setMinimumWidth(40)
        row.addWidget(self.label)
        row.addWidget(self.box)
        row.addWidget(self.slider, 1)
        self.set_value(default)
        self.label.clicked.connect(lambda: self._emit(self.default, False))
        self.box.valueChanged.connect(lambda v: self._emit(v, False, box=False))
        self.slider.valueChanged.connect(lambda pos: self._emit(self._from_pos(pos), self.slider.isSliderDown(), slider=False))
        self.slider.sliderReleased.connect(lambda: self._emit(self.value(), False))

    def _to_pos(self, v):
        x = math.log2(max(v, 1e-6)) if self.log else v
        return int(round((min(self.hi, max(self.lo, x)) - self.lo) / (self.hi - self.lo) * 1000))

    def _from_pos(self, pos):
        x = self.lo + pos / 1000.0 * (self.hi - self.lo)
        return round(2.0 ** x if self.log else x, 3)

    def value(self):
        return self.box.value()

    def set_value(self, v):
        for w, val in ((self.box, v), (self.slider, self._to_pos(v))):
            w.blockSignals(True)
            w.setValue(val)
            w.blockSignals(False)
        font = self.label.font()
        font.setBold(abs(v - self.default) > 1e-6)  # réglage actif : libellé en gras, comme les tirettes de Nuke
        self.label.setFont(font)

    def _emit(self, v, dragging, box=True, slider=True):
        if box:
            self.box.blockSignals(True)
            self.box.setValue(v)
            self.box.blockSignals(False)
        if slider:
            self.slider.blockSignals(True)
            self.slider.setValue(self._to_pos(v))
            self.slider.blockSignals(False)
        self.set_value(self.box.value())
        self.changed.emit(self.key, self.box.value(), dragging)


class Viewer(QtWidgets.QFrame):
    """Panneau du viewer. set_maps() à chaque rendu ; select() / select_index() choisissent la carte."""

    mapChanged = QtCore.Signal(str)

    def __init__(self, labels):
        super().__init__()
        self.setObjectName("viewer")
        self.labels = labels  # {canal: libellé}
        self.color = None
        self.files, self.spaces, self.order = {}, {}, []
        self.current = None
        self.params = dict(DEFAULTS)
        self.sources = {}  # clé -> Source, du plus ancien au plus récent
        self.shown = None  # (clé de la source, paramètres, complet) de l'image montrée
        self.hover = None
        self.last = {}  # dernier résultat, pour les essais
        self.pipeline = Pipeline()
        self.pipeline.done.connect(self._on_done)

        col = QtWidgets.QVBoxLayout(self)
        col.setContentsMargins(0, 0, 0, 0)
        col.setSpacing(0)
        top = QtWidgets.QFrame()
        top.setObjectName("bar")
        head = QtWidgets.QVBoxLayout(top)
        head.setContentsMargins(10, 8, 10, 8)
        head.setSpacing(6)
        row = QtWidgets.QHBoxLayout()
        row.setSpacing(6)
        self.map_combo = QtWidgets.QComboBox()
        self.map_combo.setToolTip("Map shown (keys 1 to 9, or click a thumbnail)")
        self.map_combo.setMinimumWidth(140)
        self.zoom_label = QtWidgets.QLabel("100%")
        self.zoom_label.setObjectName("muted")
        self.zoom_label.setMinimumWidth(44)
        self.zoom_label.setAlignment(QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)
        self.fit_button = QtWidgets.QPushButton("Fit")
        self.fit_button.setToolTip("Fit the image to the viewer (F, or double-click)")
        self.one_button = QtWidgets.QPushButton("1:1")
        self.one_button.setToolTip("One image pixel per screen pixel (0, or double-click)")
        row.addWidget(self.map_combo, 1)
        row.addWidget(self.zoom_label)
        row.addWidget(self.fit_button)
        row.addWidget(self.one_button)
        head.addLayout(row)
        sliders = QtWidgets.QHBoxLayout()
        sliders.setSpacing(12)
        self.sliders = {}
        for spec in SLIDERS:
            s = Slider(*spec)
            s.changed.connect(self._on_slider)
            sliders.addWidget(s, 1)
            self.sliders[spec[0]] = s
        head.addLayout(sliders)
        col.addWidget(top)
        self.canvas = Canvas()
        col.addWidget(self.canvas, 1)
        foot = QtWidgets.QFrame()
        foot.setObjectName("bar")
        bottom = QtWidgets.QHBoxLayout(foot)
        bottom.setContentsMargins(10, 5, 10, 5)
        self.info = Elided()
        self.readout = QtWidgets.QLabel()
        self.readout.setObjectName("caption")
        self.readout.setAlignment(QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)
        bottom.addWidget(self.info, 1)
        bottom.addWidget(self.readout)
        col.addWidget(foot)

        self.map_combo.activated.connect(lambda i: self.select(self.map_combo.itemData(i)))
        self.fit_button.clicked.connect(self.canvas.fit)
        self.one_button.clicked.connect(lambda: self.canvas.one_to_one())
        self.canvas.zoomed.connect(lambda z: self.zoom_label.setText("%d%%" % round(z * 100)))
        self.canvas.hovered.connect(self._on_hover)
        if np is None:
            for s in self.sliders.values():
                s.setEnabled(False)
                s.setToolTip("numpy is needed for exposure, gamma and saturation")
        self._show_empty()

    # -- cartes

    def set_color(self, color):
        """Configuration ou affichage changés : chaque carte est relue avec la nouvelle chaîne."""
        self.color = color
        self.sources.clear()
        self.shown = None
        self._refresh()

    def set_maps(self, files, spaces, order):
        """files : {canal: fichier} du dernier rendu ; spaces : {canal: espace} ; order : canaux, dans l'ordre des
        touches."""
        self.files = {ch: p for ch, p in files.items() if os.path.isfile(p)}
        self.spaces = dict(spaces or {})
        self.order = [ch for ch in order if ch in self.files]
        self.map_combo.blockSignals(True)
        self.map_combo.clear()
        for i, ch in enumerate(self.order):
            self.map_combo.addItem("%d  %s" % (i + 1, self.labels.get(ch, ch)), ch)
        self.map_combo.blockSignals(False)
        if self.current not in self.order:
            self.current = self.order[0] if self.order else None
        self._sync_combo()
        self.mapChanged.emit(self.current or "")
        self._refresh()

    def select(self, channel):
        if channel not in self.order or channel == self.current:
            return
        self.current = channel
        self._sync_combo()
        self.mapChanged.emit(channel)
        self._refresh()

    def select_index(self, n):
        """Carte n (1 à 9) parmi celles liées ; faux s'il n'y en a pas autant."""
        if 1 <= n <= len(self.order):
            self.select(self.order[n - 1])
            return True
        return False

    def set_param(self, key, value):
        self.sliders[key].set_value(value)
        self._on_slider(key, self.sliders[key].value(), False)

    def _sync_combo(self):
        self.map_combo.setCurrentIndex(max(0, self.map_combo.findData(self.current)))

    def _key(self, channel):
        path = self.files[channel]
        try:
            st = os.stat(path)
            stamp = (st.st_mtime_ns, st.st_size)
        except OSError:
            stamp = None
        return (path, stamp, self.spaces.get(channel), channel)

    def _show_empty(self):
        self.canvas.set_image(None, None)
        self.canvas.set_message("Rendered maps show here.\nKeys 1 to 9 switch maps.")
        self.info.setText("")
        self.readout.setText("")

    # -- calcul

    def _on_slider(self, key, value, dragging):
        self.params[key] = value
        self._refresh(dragging=dragging)

    def _refresh(self, dragging=False):
        """Image de la carte courante avec les réglages courants : tout de suite si elle est déjà calculée, sinon en
        arrière-plan (réduction d'abord ; seulement elle pendant un glissement)."""
        if not self.isVisible():
            return
        if self.current is None:
            self._show_empty()
            return
        key = self._key(self.current)
        params = dict(self.params)
        if self.shown is not None and self.shown[0] == key and self.shown[1] == params and self.shown[2]:
            return
        source = self.sources.get(key)
        if source is None:
            if self.canvas.pixmap is None or (self.shown and self.shown[0][3] != self.current):
                self.canvas.set_message("Loading...")
        look = source.look if source is not None else Look(self.color, self.current, self.spaces.get(self.current))
        full = not dragging
        t0 = time.time()

        def job(cancel, emit):
            src = source or self._load(key, look, cancel)
            if src is None:
                return
            if src.error or src.linear is None:
                emit({"key": key, "params": params, "source": src, "image": src.image, "full": True})
                return
            h, w = src.linear.shape[:2]
            step = max(1, -(-max(w, h) // PROXY))
            if step > 1:
                small = grade(np.ascontiguousarray(src.linear[::step, ::step]), look, params, cancel)
                if small is None:
                    return
                emit({"key": key, "params": params, "source": src, "image": rgb_qimage(small), "full": False,
                      "seconds": time.time() - t0})
                if not full:
                    return
            rgb = grade(src.linear, look, params, cancel)
            if rgb is not None:
                emit({"key": key, "params": params, "source": src, "image": rgb_qimage(rgb), "full": True,
                      "seconds": time.time() - t0})

        self.pipeline.submit(job)

    def _load(self, key, look, cancel):
        """Lecture dans le fil du calcul."""
        path = key[0]
        src = Source(key, look)
        if np is None:
            src.image = read_qimage(path)
            if src.image.isNull():
                src.error = "cannot read %s" % os.path.basename(path)
            else:
                src.width, src.height = src.image.width(), src.image.height()
            return src
        try:
            src.pixels, src.scale = read_pixels(path)
        except (OSError, ValueError) as e:
            src.error = "%s: %s" % (os.path.basename(path), e)
            return src
        src.height, src.width = src.pixels.shape[:2]
        src.linear = linearize(src.pixels, src.scale, look, cancel)
        return None if cancel() else src

    def _on_done(self, result):
        if "error" in result:
            self.canvas.set_message("!" + result["error"])
            self.last = {"error": result["error"]}
            return
        src, key = result["source"], result["key"]
        source_cached = key in self.sources
        if key not in self.sources:
            self.sources[key] = src
            self._trim()
        if self.current is None or key != self._key(self.current):
            return  # carte changée entre-temps
        if src.error:
            self.canvas.set_image(None, None)
            self.canvas.set_message("!" + src.error)
            self.last = {"error": src.error}
            return
        self.canvas.set_message("")
        self.canvas.set_image(result["image"], (src.width, src.height))
        self.shown = (key, result["params"], result["full"])
        if src.pixels is None:
            depth = "8-bit"
        elif src.pixels.dtype.kind == "f":
            depth = "half float" if src.pixels.dtype.itemsize == 2 else "float"
        else:
            depth = "%d-bit" % (8 * src.pixels.dtype.itemsize)
        self.info.setText("%s · %d × %d · %s · %s" % (
            os.path.basename(key[0]), src.width, src.height, depth, src.look.describe))
        self.info.setToolTip("%s\n%s" % (key[0], src.look.describe))
        self.last = {"map": key[3], "file": key[0], "size": [src.width, src.height], "full": result["full"],
                     "look": src.look.describe, "params": result["params"],
                     "image": [result["image"].width(), result["image"].height()],
                     "seconds": round(result.get("seconds", 0.0), 3), "cached": source_cached}
        self._on_hover(self.hover)

    def _trim(self):
        current = self._key(self.current) if self.current else None
        total = sum(s.nbytes for s in self.sources.values())
        for k in list(self.sources):
            if total <= CACHE_BYTES:
                break
            if k != current:
                total -= self.sources.pop(k).nbytes

    def _on_hover(self, at):
        self.hover = at
        src = self.sources.get(self.shown[0]) if self.shown else None
        if at is None or src is None or src.pixels is None:
            self.readout.setText("")
            return
        x, y = at
        if not (0 <= x < src.width and 0 <= y < src.height):
            self.readout.setText("")
            return
        v = src.pixels[y, x].astype(np.float64) / src.scale
        values = "  ".join("%.4f" % c for c in v) if len(v) > 1 else "%.4f" % v[0]
        self.readout.setText("%d, %d   %s" % (x, y, values))
        image = self.canvas.image
        if image is not None:  # essais : couleur montrée à ce pixel
            c = image.pixelColor(x * image.width() // src.width, y * image.height() // src.height)
            self.last["shown"] = [c.red(), c.green(), c.blue()]
            self.last["value"] = [float(a) for a in v]

    # -- fenêtre

    def showEvent(self, event):
        super().showEvent(event)
        self._refresh()

    def flush(self):
        """Essais : attend le calcul en cours et livre son résultat."""
        self._refresh()
        self.pipeline.wait()
        QtWidgets.QApplication.processEvents()

    def stop(self):
        self.pipeline.stop()

    def test_log(self):
        return dict(self.last, visible=self.isVisible(), current=self.current, order=list(self.order),
                    zoom=round(self.canvas.zoom, 4), fitted=self.canvas.fitted, sliders=dict(self.params),
                    readout=self.readout.text(), cached_maps=len(self.sources))


# ---- Touches ------------------------------------------------------------------------------------------------------------


def key_digit(event):
    """Chiffre 0 à 9 d'une touche : pavé numérique, rangée du haut par sa position (AZERTY), sinon le caractère ;
    None pour une autre touche."""
    key = event.key()
    if event.modifiers() & QtCore.Qt.KeypadModifier and QtCore.Qt.Key_0 <= key <= QtCore.Qt.Key_9:
        return key - QtCore.Qt.Key_0
    if sys.platform == "darwin":
        vk = event.nativeVirtualKey()
        if vk in MAC_DIGITS:
            return MAC_DIGITS[vk]
        if vk == MAC_ZERO:
            return 0
    elif sys.platform == "win32":
        code = event.nativeScanCode()
        if 2 <= code <= 11:
            return (code - 1) % 10
    elif QtGui.QGuiApplication.platformName() in ("xcb", "wayland"):
        code = event.nativeScanCode()
        if 10 <= code <= 19:
            return (code - 9) % 10
    if QtCore.Qt.Key_0 <= key <= QtCore.Qt.Key_9:
        return key - QtCore.Qt.Key_0
    return None


class Keys(QtCore.QObject):
    """Filtre de l'application : touches du viewer dans la fenêtre, hors champs de saisie ; les autres fenêtres
    (Settings, menus déroulants, dialogues) gardent les leurs."""

    def __init__(self, window, viewer):
        super().__init__(window)
        self.window, self.viewer = window, viewer
        QtWidgets.QApplication.instance().installEventFilter(self)

    def eventFilter(self, obj, event):
        if event.type() != QtCore.QEvent.KeyPress or not self.viewer.isVisible():
            return False
        if not isinstance(obj, QtWidgets.QWidget) or obj.window() is not self.window:
            return False
        if isinstance(QtWidgets.QApplication.focusWidget(), TEXT_INPUTS):
            return False
        return self.handle(event)

    def handle(self, event):
        mods = event.modifiers() & ~(QtCore.Qt.KeypadModifier | QtCore.Qt.ShiftModifier)
        if mods:
            return False
        digit = key_digit(event)
        if digit == 0:
            self.viewer.canvas.one_to_one()
            return True
        if digit is not None:
            return self.viewer.select_index(digit)
        key = event.key()
        if key == QtCore.Qt.Key_F:
            self.viewer.canvas.fit()
        elif key in (QtCore.Qt.Key_Plus, QtCore.Qt.Key_Equal):
            self.viewer.canvas.zoom_to(self.viewer.canvas.zoom * 2.0)
        elif key == QtCore.Qt.Key_Minus:
            self.viewer.canvas.zoom_to(self.viewer.canvas.zoom / 2.0)
        else:
            return False
        return True
