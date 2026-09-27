#!/usr/bin/env python3
# Gestion couleur de subzero (lot E11) : configuration OCIO, espace de chaque carte, affichage des vignettes et des
# couleurs, choix des couleurs. PyOpenColorIO et numpy (pip install opencolorio numpy) ; sans eux, subzero marche comme
# en E6 (fichiers tels que sbsrender les écrit) et le panneau Settings dit quoi installer.
#
# sbsrender écrit les cartes couleur encodées sRGB (srgb_tx, vérifié en E6, même en EXR) et les cartes de donnée
# telles quelles. Sa gestion OCIO à lui (--ocio, --set-output-colorspace) ne sert pas : il prend toute sortie, même la
# rugosité, pour de l'espace « sRGB », nom qu'une config studio n'a pas, et convertit alors sans rien dire depuis
# scene_linear. subzero convertit donc lui-même, après le rendu, dans la configuration d'ArmorPaint (la même que
# celle qui lira les textures) ou, sans ArmorPaint, dans celle de $OCIO.
#
# Variables, dans l'ordre de résolution de l'espace d'une carte :
#   SUBZERO_OCIO_<CARTE>  une carte (BASECOLOR, OPACITY, OCCLUSION, ROUGHNESS, METALLIC, NORMAL, HEIGHT)
#   SUBZERO_OCIO_COLOR    cartes couleur (couleur de base) ; défaut : srgb_tx, ce qu'écrit sbsrender, sans conversion
#   SUBZERO_OCIO_DATA     cartes de donnée ; défaut : le rôle data (Raw), sans conversion
#   SUBZERO_OCIO_PICKING  espace des couleurs des paramètres dans la fenêtre ; défaut : le rôle color_picking
# Valeurs : nom, alias ou rôle de la configuration. Sans ArmorPaint (autonome, E9), la fenêtre lit aussi OCIO,
# STUDIO_OCIO_DISPLAY, STUDIO_OCIO_VIEW, STUDIO_OCIO_LOOK et STUDIO_OCIO_VIEWPORT, comme ArmorPaint ; avec lui, elle
# prend sa configuration et la transformation d'affichage de son viewport.

import os
import threading

try:
    import numpy as np
    import PyOpenColorIO as OCIO
    MISSING = None
except ImportError as e:  # la fenêtre marche sans : pas de conversion ni d'affichage OCIO
    np = OCIO = None
    MISSING = str(e)

import subzero_exr

DEFAULT_CONFIG = "ocio://studio-config-latest"  # comme ArmorPaint sans préférence ni $OCIO
SOURCE_COLOR = "srgb_tx"  # espace intégré de ce qu'écrit sbsrender pour les cartes couleur
COLOR_MAPS = {"base"}  # canaux d'ArmorPaint dont sbsrender encode les valeurs en sRGB ; les autres sont des données
VAR_COLOR, VAR_DATA, VAR_PICKING = "SUBZERO_OCIO_COLOR", "SUBZERO_OCIO_DATA", "SUBZERO_OCIO_PICKING"
HOST_VARS = ("OCIO", "STUDIO_OCIO_DISPLAY", "STUDIO_OCIO_VIEW", "STUDIO_OCIO_LOOK", "STUDIO_OCIO_VIEWPORT")
INSTALL_HINT = "pip install opencolorio numpy"


def map_var(suffix):
    """Variable d'une carte, par le suffixe de son fichier : basecolor -> SUBZERO_OCIO_BASECOLOR."""
    return "SUBZERO_OCIO_" + suffix.upper()


def env_flag(value, default=True):
    if value is None:
        return default
    return value.strip().lower() not in ("", "0", "false", "no", "off")


class Error(Exception):
    """Conversion impossible, message montré tel quel."""


class Color(object):
    """Configuration chargée et réglages. host : state["ocio"] d'ArmorPaint ({"enabled", "status", "config", "name",
    "display", "view", "look", "viewport"}) ; None sans ArmorPaint : tout vient de env. env : variables (défaut
    os.environ, lues à la création)."""

    def __init__(self, host=None, env=None):
        env = dict(os.environ if env is None else env)
        self.env = env
        self.host = host
        self.config = None
        self.error = None
        self.warnings = []
        self.display = self.view = self.look = None
        self.viewport = False
        self._procs = {}
        self._lock = threading.Lock()
        if host is not None:
            self.config_source = host.get("config") or ""
            self.origin = "ArmorPaint"
        else:
            self.config_source = env.get("OCIO") or DEFAULT_CONFIG
            self.origin = "$OCIO" if env.get("OCIO") else "default"
        if MISSING is not None:
            self.error = "PyOpenColorIO and numpy are needed for color management (%s): %s" % (MISSING, INSTALL_HINT)
            return
        if host is not None and not host.get("enabled"):
            self.error = "OCIO is off in ArmorPaint: %s" % (host.get("status") or "no configuration")
            return
        try:
            self.config = OCIO.Config.CreateFromFile(self.config_source)
        except Exception as e:  # noqa: BLE001 — OCIO lève ses propres types
            self.error = "OCIO configuration %s: %s" % (self.config_source, e)
            return
        self._builtin = None
        if host is not None:
            self.display, self.view, self.look = host.get("display") or None, host.get("view") or None, host.get("look") or None
            self.viewport = bool(host.get("viewport")) and bool(self.display and self.view)
        else:
            self._display_from_env()

    # -- configuration

    @property
    def ready(self):
        return self.config is not None

    @property
    def name(self):
        return self.config.getName() if self.config is not None else ""

    def version(self):
        return OCIO.__version__ if OCIO is not None else ""

    def _display_from_env(self):
        # Comme studio_ocio_view.c : dès que l'une des trois est définie, les trois viennent de l'environnement ; une
        # valeur inconnue est remplacée par le défaut.
        c = self.config
        display, view, look = (self.env.get(v) or None for v in HOST_VARS[1:4])
        if display is not None and display not in c.getDisplays():
            self.warnings.append("unknown display %s ($STUDIO_OCIO_DISPLAY)" % display)
            display = None
        self.display = display or c.getDefaultDisplay()
        if view is not None and view not in c.getViews(self.display):
            self.warnings.append("unknown view %s for %s ($STUDIO_OCIO_VIEW)" % (view, self.display))
            view = None
        self.view = view or c.getDefaultView(self.display)
        if look is not None and c.getLook(look) is None:
            self.warnings.append("unknown look %s ($STUDIO_OCIO_LOOK)" % look)
            look = None
        self.look = look
        self.viewport = env_flag(self.env.get("STUDIO_OCIO_VIEWPORT"), True) and bool(self.display and self.view)

    def resolve(self, name):
        """Nom canonique d'un espace (nom, alias ou rôle), None s'il est inconnu."""
        if self.config is None or not name:
            return None
        cs = self.config.getColorSpace(name)
        return cs.getName() if cs is not None else None

    def builtin(self, name):
        """Espace de la configuration équivalent à un espace de la configuration intégrée (srgb_tx…), None sinon."""
        if self.config is None:
            return None
        key = ("builtin", name)
        if key not in self._procs:
            try:
                if self._builtin is None:
                    self._builtin = OCIO.Config.CreateFromBuiltinConfig("studio-config-latest")
                self._procs[key] = OCIO.Config.IdentifyBuiltinColorSpace(self.config, self._builtin, name) or None
            except Exception:  # noqa: BLE001
                self._procs[key] = None
        return self._procs[key]

    def is_data(self, name):
        cs = self.config.getColorSpace(name) if self.config is not None and name else None
        return bool(cs is not None and cs.isData())

    def data_space(self):
        """Espace de donnée : le rôle data, sinon le premier espace isdata."""
        found = self.resolve("data")
        if found is not None:
            return found
        for n in self.config.getColorSpaceNames() if self.config is not None else []:
            if self.config.getColorSpace(n).isData():
                return n
        return None

    def spaces(self):
        """[(famille, nom)] des espaces actifs, dans l'ordre de la configuration."""
        if self.config is None:
            return []
        return [(self.config.getColorSpace(n).getFamily() or "", n) for n in self.config.getColorSpaceNames()]

    def roles(self):
        return sorted(self.config.getRoleNames()) if self.config is not None else []

    def displays(self):
        return list(self.config.getDisplays()) if self.config is not None else []

    def views(self, display):
        return list(self.config.getViews(display)) if self.config is not None and display else []

    def looks(self):
        return [lk.getName() for lk in self.config.getLooks()] if self.config is not None else []

    # -- cartes

    def source(self, channel):
        """Espace de ce qu'écrit sbsrender pour ce canal : srgb_tx de la configuration pour une carte couleur, l'espace
        de donnée sinon. None si la configuration n'en a pas."""
        return self.builtin(SOURCE_COLOR) if channel in COLOR_MAPS else self.data_space()

    def setting(self, channel, suffix):
        """(valeur demandée, variable qui la donne) pour la carte : SUBZERO_OCIO_<CARTE>, puis celle de son groupe ;
        (None, None) : le défaut."""
        for var in (map_var(suffix), VAR_COLOR if channel in COLOR_MAPS else VAR_DATA):
            value = (self.env.get(var) or "").strip()
            if value:
                return value, var
        return None, None

    def target(self, channel, suffix):
        """Espace du fichier livré : (nom canonique, erreur). Sans réglage, l'espace source (pas de conversion) ; un
        nom inconnu donne l'erreur et l'espace source."""
        src = self.source(channel)
        value, var = self.setting(channel, suffix)
        if value is None:
            return src, None
        found = self.resolve(value)
        if found is None:
            return src, "unknown color space %s ($%s)" % (value, var)
        return found, None

    def plan(self, channels):
        """{canal: (source, cible, erreur)} ; channels : [(canal, suffixe)]."""
        out = {}
        for ch, suffix in channels:
            dst, err = self.target(ch, suffix)
            out[ch] = (self.source(ch), dst, err)
        return out

    # -- transformations

    def processor(self, src, dst):
        """Processeur CPU src -> dst, None s'il ne change rien (même espace, donnée, pas de configuration). Lève Error
        si la configuration ne sait pas le construire."""
        if self.config is None or not src or not dst or src == dst:
            return None
        key = ("cs", src, dst)
        with self._lock:
            if key not in self._procs:
                try:
                    p = self.config.getProcessor(src, dst)
                except Exception as e:  # noqa: BLE001
                    raise Error("no transform from %s to %s: %s" % (src, dst, e))
                self._procs[key] = None if p.isNoOp() else p.getDefaultCPUProcessor()
            return self._procs[key]

    def display_processor(self, src):
        """Processeur CPU src -> affichage (display, view, look), None sans affichage OCIO, pour une donnée (montrée
        telle quelle, comme dans Painter) ou un espace inconnu."""
        if self.config is None or not self.viewport or not src or self.is_data(src):
            return None
        key = ("display", src, self.display, self.view, self.look)
        with self._lock:
            if key not in self._procs:
                try:
                    t = OCIO.DisplayViewTransform(src=src, display=self.display, view=self.view)
                    if self.look:  # comme studio_ocio_lib_proc_display : le look avant la vue, par le pipeline legacy
                        pipeline = OCIO.LegacyViewingPipeline()
                        pipeline.setDisplayViewTransform(t)
                        pipeline.setLooksOverrideEnabled(True)
                        pipeline.setLooksOverride(self.look)
                        p = pipeline.getProcessor(self.config)
                    else:
                        p = self.config.getProcessor(t)
                    self._procs[key] = None if p.isNoOp() else p.getDefaultCPUProcessor()
                except Exception:  # noqa: BLE001
                    self._procs[key] = None
            return self._procs[key]

    def view_processor(self, space, channel):
        """Processeur des vignettes et des nuanciers d'un canal dont les valeurs sont dans space : la transformation
        d'affichage ; sans elle, retour à ce qu'écrit sbsrender (sRGB) pour une carte couleur convertie. None : montrer
        les valeurs telles quelles (donnée, pas de configuration, espace inconnu)."""
        if self.config is None or channel not in COLOR_MAPS:
            return None
        space = self.resolve(space) or self.builtin(space) if space else None
        if space is None:
            return None
        if self.viewport:
            return self.display_processor(space)
        try:
            return self.processor(space, self.source(channel))
        except Error:
            return None

    def convert_rgb(self, rgb, src, dst):
        p = self.processor(src, dst)
        return list(rgb) if p is None else [float(x) for x in p.applyRGB([float(x) for x in rgb])]

    def view_rgb(self, rgb, space, channel="base"):
        """Couleur à l'écran (0-1) d'une valeur de l'espace space, comme les vignettes du canal."""
        p = self.view_processor(space, channel)
        return list(rgb) if p is None else [float(x) for x in p.applyRGB([float(x) for x in rgb])]

    def picking(self):
        """(espace des couleurs des paramètres, erreur) : $SUBZERO_OCIO_PICKING, sinon le rôle color_picking, sinon
        l'espace des valeurs du graphe."""
        graph = self.source("base")
        value = (self.env.get(VAR_PICKING) or "").strip()
        if value:
            found = self.resolve(value)
            return (found, None) if found else (graph, "unknown color space %s ($%s)" % (value, VAR_PICKING))
        return self.resolve("color_picking") or graph, None

    def encoding(self, name):
        """Encodage déclaré de l'espace (scene-linear, sdr-video, data…), "" s'il n'en a pas."""
        cs = self.config.getColorSpace(name) if self.config is not None and name else None
        return (cs.getEncoding() or "") if cs is not None else ""

    def describe(self, name):
        """Nom montré d'une valeur réglée : « rôle → espace » pour un rôle ou un alias."""
        found = self.resolve(name)
        return name if found is None or found == name else "%s \u2192 %s" % (name, found)

    # -- fichiers

    def convert_file(self, path, src, dst):
        """Réécrit l'image path de src vers dst (alpha gardé). Faux si rien n'est à faire. Lève Error."""
        p = self.processor(src, dst)  # Error si la configuration ne sait pas
        if p is None:
            return False
        ext = os.path.splitext(path)[1].lower()
        try:
            if ext == ".exr":
                return _convert_exr(path, p)
            return _convert_8bit(path, p, ext)
        except (OSError, subzero_exr.Error, ValueError) as e:
            raise Error("%s: color conversion %s -> %s failed: %s" % (os.path.basename(path), src, dst, e))



# ---- Images de la fenêtre -----------------------------------------------------------------------------------------------


def apply_image(image, proc):
    """QImage passée par proc (view_processor), alpha gardé ; image rendue telle quelle si proc vaut None."""
    if proc is None or image.isNull():
        return image
    from PySide6 import QtGui
    rgba = _qimage_rgba(image)
    flat = (rgba.astype(np.float32) / 255.0).reshape(-1)
    proc.applyRGBA(flat)
    out = _to_u8(flat).reshape(rgba.shape)
    out[..., 3] = rgba[..., 3]
    return _rgba_qimage(out, QtGui.QImage.Format_RGBA8888)


def floats_image(values, width, height, proc):
    """QImage RGB 8 bits de valeurs R, G, B flottantes (vignette d'un EXR) passées par proc : sans arrondi 8 bits
    avant la transformation, les sombres d'un EXR linéaire ne sont pas en escalier."""
    from PySide6 import QtGui
    rgb = np.asarray(values, np.float32).reshape(-1)
    if proc is not None:
        proc.applyRGB(rgb)
    return _rgba_qimage(_to_u8(rgb).reshape(height, width, 3), QtGui.QImage.Format_RGB888)


# ---- Conversion des fichiers --------------------------------------------------------------------------------------------


def _to_u8(values):
    return (np.clip(values, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


def _convert_exr(path, proc):
    width, height, planes, types, compression = subzero_exr.read_planes(path)
    if not all(c in planes for c in ("R", "G", "B")):
        return False  # gris : une carte couleur de sbsrender est en R, G, B
    rgba = np.empty((height, width, 4), np.float32)
    for i, c in enumerate(("R", "G", "B")):
        rgba[..., i] = planes[c]
    rgba[..., 3] = planes["A"] if "A" in planes else 1.0
    flat = rgba.reshape(-1)
    proc.applyRGBA(flat)
    for i, c in enumerate(("R", "G", "B")):
        planes[c] = rgba[..., i]
    subzero_exr.write_planes(path, planes, types, compression)
    return True


def _qimage_rgba(image):
    from PySide6 import QtGui
    img = image.convertToFormat(QtGui.QImage.Format_RGBA8888)
    w, h = img.width(), img.height()
    rows = np.frombuffer(img.constBits(), np.uint8, count=img.sizeInBytes()).reshape(h, img.bytesPerLine())
    return rows[:, :w * 4].reshape(h, w, 4).copy()


def _rgba_qimage(rgba, fmt):
    from PySide6 import QtGui
    h, w = rgba.shape[:2]
    data = np.ascontiguousarray(rgba)
    return QtGui.QImage(data.data, w, h, data.strides[0], fmt).copy()


def _convert_8bit(path, proc, ext):
    from PySide6 import QtGui
    if ext == ".tga":
        read = _read_tga(path)
        if read is None:
            return False
        rgba, alpha = read
    else:
        image = QtGui.QImage(path)
        if image.isNull():
            raise Error("cannot read the image")
        if image.isGrayscale() and not image.hasAlphaChannel():
            return False
        alpha = image.hasAlphaChannel()
        rgba = _qimage_rgba(image)
    flat = (rgba.astype(np.float32) / 255.0).reshape(-1)
    proc.applyRGBA(flat)
    out = _to_u8(flat).reshape(rgba.shape)
    if alpha:
        out[..., 3] = rgba[..., 3]  # l'alpha ne passe pas par l'arrondi
    if ext == ".tga":
        _write_tga(path, out, alpha)
        return True
    img = _rgba_qimage(out, QtGui.QImage.Format_RGBA8888)
    if not alpha:
        img = img.convertToFormat(QtGui.QImage.Format_RGB888)
    tmp = path + ".tmp" + ext
    fmt = {".png": "PNG", ".jpg": "JPG", ".jpeg": "JPG", ".tif": "TIFF", ".tiff": "TIFF"}.get(ext)
    if fmt is None:
        raise Error("format %s not supported" % ext)
    if not img.save(tmp, fmt, 95 if fmt == "JPG" else -1):
        raise Error("cannot write the image")
    os.replace(tmp, path)
    return True


def _read_tga(path):
    """(RGBA uint8 (h, w, 4), alpha) d'un TGA non compressé 24 ou 32 bits, avec ou sans pied TGA 2.0 ; None pour un
    gris (type 3). Lève Error pour un autre type."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 18:
        raise Error("not a TGA file")
    kind, bpp, desc = data[2], data[16], data[17]
    if kind == 3:
        return None
    if kind != 2 or data[1] or bpp not in (24, 32):
        raise Error("TGA type %d, %d bits not supported" % (kind, bpp))
    w, h, n = data[12] | data[13] << 8, data[14] | data[15] << 8, bpp // 8
    start = 18 + data[0]
    if len(data) < start + w * h * n:
        raise Error("truncated TGA file")
    px = np.frombuffer(data, np.uint8, count=w * h * n, offset=start).reshape(h, w, n)
    rgba = np.empty((h, w, 4), np.uint8)
    rgba[..., :3] = px[..., 2::-1]
    rgba[..., 3] = px[..., 3] if n == 4 else 255
    if desc & 0x10:  # de droite à gauche
        rgba = rgba[:, ::-1]
    if not desc & 0x20:  # origine en bas
        rgba = rgba[::-1]
    return np.ascontiguousarray(rgba), n == 4


TGA_FOOTER = bytes(8) + b"TRUEVISION-XFILE.\0"  # pied TGA 2.0, qu'écrit sbsrender : Qt refuse un TGA sans lui


def _write_tga(path, rgba, alpha):
    """TGA non compressé, origine en bas, pied TGA 2.0 (comme sbsrender) : 32 bits avec alpha, 24 sinon."""
    h, w = rgba.shape[:2]
    bgra = rgba[::-1, :, [2, 1, 0, 3]] if alpha else rgba[::-1, :, [2, 1, 0]]
    bpp = 32 if alpha else 24
    header = bytes([0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, w & 255, w >> 8, h & 255, h >> 8, bpp, 8 if alpha else 0])
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(header)
        f.write(np.ascontiguousarray(bgra).tobytes())
        f.write(TGA_FOOTER)
    os.replace(tmp, path)
