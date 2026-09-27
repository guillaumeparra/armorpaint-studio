#!/usr/bin/env python3
# Règles d'un matériau subzero, sans Qt (lot E9) : canaux d'ArmorPaint, paramètres techniques, liaison des sorties aux
# canaux, noms des textures, recette subzero.json, rendu final d'un matériau (sbsrender, décalque, couleur OCIO). La
# fenêtre (subzero_window.py) et `subzero render` (subzero_app.py) s'en servent ; une machine de rendu sans
# bibliothèques Qt fait tourner le second. Python 3.9+, bibliothèque standard ; PyOpenColorIO et numpy pour la
# couleur (subzero_color.py), PySide6 pour le seul décalque en PNG ou en TIFF.

import datetime
import hashlib
import json
import math
import os
import re

import subzero_color
import subzero_exr
import subzero_meta
import subzero_render

RECIPE = "subzero.json"

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
COLOR_SPACES = {"base": "srgb_tx"}  # sans PyOpenColorIO (lot E6) : espace intégré de ce qu'écrit sbsrender

# Variables des réglages de couleur (lot E11) : celles de subzero, puis celles d'ArmorPaint (réglées ici sans lui).
MAP_VARS = [(ch, subzero_color.map_var(CHANNEL_SUFFIX[ch])) for ch in CHANNEL_LABEL]
SUBZERO_VARS = [subzero_color.VAR_COLOR, subzero_color.VAR_DATA] + [v for _, v in MAP_VARS] + [subzero_color.VAR_PICKING]
COLOR_VARS = SUBZERO_VARS + list(subzero_color.HOST_VARS)

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
# Format de la normale : OpenGL, celui d'ArmorPaint ; sans lui (lot E9), $SUBZERO_NORMAL_FORMAT peut demander DirectX.
NORMAL_FORMATS = ("opengl", "directx")
NORMAL_FORMAT = "opengl"
# Noms des textures : <Nom>_<canal>, que les règles de nom d'ArmorPaint reconnaissent ; sans lui (lot E9),
# $SUBZERO_EXPORT_TEMPLATE, avec {name} (nom du dossier), {channel} (suffixe du canal) et {size} (largeur).
NAME_TEMPLATE = "{name}_{channel}"
TEMPLATE_SIZES = [1 << n for n in range(4, 14)]



# ---- Règles pures (testées sans Qt) ------------------------------------------------------------------------------------


def env_flag(name, default):
    """Variable d'environnement booléenne (lot E5) : 0, false, no, off ou vide = faux ; absente = default."""
    value = os.environ.get(name)
    if value is None:
        return default
    return value.strip().lower() not in ("", "0", "false", "no", "off")


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
            if NORMAL_FORMAT in re.sub(r"[^a-z]", "", str(label).lower()):
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


def apply_overrides(rows, overrides):
    """Liaison choisie à la main ({sortie: canal, "" : aucun}) appliquée aux lignes d'auto_mapping."""
    for row in rows:
        o = row[0]["identifier"]
        if o in overrides:
            row[1] = overrides[o] or None
            row[2] = None if row[1] else "not linked"
    return rows


def graph_values(graph, values=None, preset=None):
    """Réglages d'un graphe : le défaut de chaque paramètre montré (et la graine), puis les valeurs du preset (son
    étiquette), puis values. Les clés que le graphe ne montre pas sont ignorées ; un preset inconnu lève ValueError."""
    by_id = {i["identifier"]: i for i in graph["inputs"]}
    out = {i["identifier"]: i["default"] for i in graph["inputs"] if shown(i) and i["type"] != "image"}
    if "$randomseed" in by_id:
        out["$randomseed"] = by_id["$randomseed"]["default"]
    if preset:
        found = next((p for p in graph.get("presets") or [] if p["label"] == preset), None)
        if found is None:
            raise ValueError("no preset %r in %s (%s)" % (preset, graph["pkgurl"], ", ".join(
                p["label"] for p in graph.get("presets") or []) or "no presets"))
        for k, v in (found.get("values") or {}).items():
            if k in out:
                out[k] = v
    for k, v in (values or {}).items():
        if k in out:
            out[k] = v
    return out


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
    (image_texture_node.c, sauf en JPEG), d'où des bords sombres sous l'opacité. La couleur est réécrite sans alpha ;
    l'alpha va dans opacity (niveaux de gris, même format) s'il est donné. Vrai si réécrit."""
    ext = os.path.splitext(base)[1].lower()
    if ext == ".tga":
        return split_alpha_tga(base, opacity)
    if ext == ".exr":
        return subzero_exr.split_alpha(base, opacity)
    if ext in (".png", ".tif", ".tiff"):
        return split_alpha_qt(base, opacity)
    return False


def split_alpha_tga(base, opacity=None):
    """split_alpha d'un TGA non compressé, ce qu'écrit sbsrender."""
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
            f.write(TGA_FOOTER)
        os.replace(tmp, path)

    if opacity:
        write(opacity, 3, 8, 0x08 | orient, alpha)  # comme les sorties grises de sbsrender
    write(base, 2, 24, orient, rgb)
    return True


def check_template(template):
    """Modèle de nom des textures : rend le message d'erreur, None s'il convient (un nom par canal, sans dossier)."""
    try:
        names = {texture_name(template, "Name", s, 2048) for s in CHANNEL_SUFFIX.values()}
    except (KeyError, IndexError, ValueError) as e:
        return "bad texture name template %r: %s (fields: {name}, {channel}, {size})" % (template, e)
    if len(names) != len(CHANNEL_SUFFIX):
        return "texture name template %r must contain {channel}" % template
    if any("/" in n or "\\" in n or n in ("", ".", "..") for n in names):
        return "texture name template %r must give file names, not paths" % template
    return None


def texture_name(template, name, suffix, width):
    """Nom sans extension d'une texture : « {name}_{channel} » -> « ColorfulPlanks_basecolor »."""
    return template.format(name=name, channel=suffix, size=width)


def links(files, spaces=None, was=None):
    """Liaisons pour ArmorPaint : [{"file", "output" (0 couleur, 1 alpha), "socket", "color_space", "color_space_was"}].
    spaces : {canal: espace du fichier} (défaut COLOR_SPACES) ; was : ceux du rendu précédent envoyé."""
    spaces = COLOR_SPACES if spaces is None else spaces
    out = []
    for ch, path in files.items():
        link = {"file": path, "output": 0, "socket": CHANNEL_SOCKET[ch]}
        if spaces.get(ch):
            link["color_space"] = spaces[ch]
            if (was or {}).get(ch):
                link["color_space_was"] = was[ch]
        out.append(link)
    return out


def recipe_spaces(recipe):
    """Espaces des textures d'une recette : ceux qu'elle note (E11), la couleur de base en srgb_tx pour une recette
    du lot E6 (avec format), aucun pour la v0."""
    color = recipe.get("color")
    if isinstance(color, dict) and isinstance(color.get("spaces"), dict):
        return dict(color["spaces"])
    return dict(COLOR_SPACES) if recipe.get("format") else {}


def convert_maps(color, files, fmt):
    """Cartes rendues passées dans l'espace réglé (subzero_color.Color, thread du rendu) ; rend ({canal: espace du
    fichier}, [avertissements], [canaux réécrits]). Sans OCIO : fichiers tels quels, espaces de COLOR_SPACES (lot E6).
    Une conversion ratée laisse le fichier tel que sbsrender l'a écrit, dans son espace source."""
    if color is None or not color.ready:
        return {ch: cs for ch, cs in COLOR_SPACES.items() if ch in files}, [], []
    spaces, notes, converted = {}, [], []
    plan = color.plan([(ch, CHANNEL_SUFFIX[ch]) for ch in files])
    for ch, path in files.items():
        src, dst, err = plan[ch]
        if err:
            notes.append(err)
        if not src:
            notes.append("%s: no %s in the OCIO configuration, left as rendered" % (
                CHANNEL_LABEL[ch], "sRGB texture space" if ch in subzero_color.COLOR_MAPS else "data space"))
            if ch in COLOR_SPACES:
                spaces[ch] = COLOR_SPACES[ch]
            continue
        try:
            if color.convert_file(path, src, dst):
                converted.append(ch)
        except subzero_color.Error as e:
            notes.append(str(e))
            spaces[ch] = src
            continue
        spaces[ch] = dst
        if dst != src and not subzero_render.FORMATS[fmt]["float"] and color.encoding(dst) == "scene-linear":
            notes.append("%s in %s (scene-linear) as 8-bit %s: banding, prefer EXR" % (
                CHANNEL_LABEL[ch], dst, subzero_render.FORMATS[fmt]["label"]))
    return spaces, notes, converted


def stale_files(folder, keep, template=NAME_TEMPLATE, previous=()):
    """Textures d'un rendu précédent du dossier (<Nom>_<canal>.<ext>, un autre format, une autre taille ou une sortie
    retirée), et celles de la recette précédente (previous : noms de fichiers, un autre modèle de nom), qui ne sont pas
    dans keep (chemins)."""
    prefix = os.path.basename(folder.rstrip("/"))
    exts = sorted({f["ext"] for f in subzero_render.FORMATS.values()})
    keep = {os.path.normpath(k) for k in keep}
    names = {texture_name(template, prefix, suffix, w) + ext for suffix in CHANNEL_SUFFIX.values()
             for w in TEMPLATE_SIZES for ext in exts}
    names.update(os.path.basename(n) for n in previous if n and os.path.basename(n) not in (RECIPE, "", ".", ".."))
    return [os.path.join(folder, n) for n in sorted(names)
            if os.path.isfile(os.path.join(folder, n)) and os.path.normpath(os.path.join(folder, n)) not in keep]


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


def render_material(sbsar, graph, folder, tools, outs, values=None, entries=None, size=(11, 11), fmt="exr16",
                    color=None, template=NAME_TEMPLATE, cancel=None):
    """Rendu final d'un matériau dans folder (créé) : sorties outs [(sortie, canal)] nommées par template, décalque
    séparé, cartes passées dans leurs espaces OCIO (color). Sans Qt : la fenêtre (thread du rendu) et `subzero render`
    s'en servent. Rend {"files": {canal: chemin}, "seconds", "size", "format", "warnings", "spaces", "color_warnings",
    "converted"}."""
    prefix = os.path.basename(folder.rstrip("/"))
    width = 1 << size[0]
    names = {o: texture_name(template, prefix, CHANNEL_SUFFIX[ch], width) for o, ch in outs}
    os.makedirs(folder, exist_ok=True)
    res = subzero_render.render(sbsar, graph, folder, tools, values=render_values(graph, values or {}),
                                entries=dict(entries or {}), size=size, outputs=[o for o, _ in outs], names=names,
                                fmt=fmt, cancel=cancel)
    files = {ch: res["files"][o] for o, ch in outs}
    warnings = list(res["warnings"])
    if "base" in files and subzero_render.FORMATS[fmt]["alpha"]:  # décalque : alpha de la couleur vers l'opacité,
        # faute de sortie opacité
        opacity = os.path.join(folder, texture_name(template, prefix, CHANNEL_SUFFIX["opacity"], width)
                               + subzero_render.FORMATS[fmt]["ext"])
        try:
            split = split_alpha(files["base"], None if "opacity" in files else opacity)
        except ImportError:  # `subzero render` sans PySide6 : PNG et TIFF restent tels que sbsrender les écrit
            split = None
            warnings.append("decal check skipped: PNG and TIFF need PySide6 (a base color with transparency keeps "
                            "its alpha)")
        if split and "opacity" not in files:
            files["opacity"] = opacity
        elif split is not None and "opacity" not in files and os.path.isfile(opacity):
            os.remove(opacity)  # d'un rendu précédent
    spaces, notes, converted = convert_maps(color, files, fmt)
    return {"files": files, "seconds": res["seconds"], "size": size, "format": fmt, "warnings": warnings,
            "spaces": spaces, "color_warnings": notes, "converted": converted}


def make_recipe(package_path, folder, name, graph, values, entries, preset, size, outs, overrides, data, color, tools):
    """Recette subzero.json d'un rendu final (data : résultat de render_material). values : réglages de l'artiste,
    seuls ceux qui diffèrent du défaut sont gardés ; size : log2 choisi, None pour celui du projet."""
    defaults = {i["identifier"]: i["default"] for i in graph["inputs"]}
    ready = color is not None and color.ready
    return {
        "tool": "subzero",
        "version": 1,
        "name": name,
        "package": {"path": package_path, "relative": os.path.relpath(package_path, folder),
                    "sha256": file_sha256(package_path)},
        "graph": graph["pkgurl"],
        "preset": preset,
        "values": {k: v for k, v in values.items() if v != defaults.get(k)},
        "entries": dict(entries),
        "size": size,
        "format": data["format"],
        "outputs": {o: ch for o, ch in outs},
        "overrides": dict(overrides),
        "files": {ch: os.path.basename(p) for ch, p in data["files"].items()},
        "color": {"config": color.config_source if ready else "", "name": color.name if color is not None else "",
                  "spaces": dict(data["spaces"])},
        "sbsrender": tools.get("version") if tools else None,
        "rendered": {"size": [1 << data["size"][0], 1 << data["size"][1]], "seconds": data["seconds"],
                     "date": datetime.datetime.now().isoformat(timespec="seconds")},
    }


TGA_FOOTER = bytes(8) + b"TRUEVISION-XFILE.\0"  # pied TGA 2.0, qu'écrit sbsrender : Qt refuse un TGA sans lui


def split_alpha_qt(base, opacity=None):
    """split_alpha d'un PNG ou d'un TIFF 8 bits, par Qt (thread du rendu), importé ici : `subzero render` n'en a
    besoin que pour un décalque en PNG ou en TIFF."""
    from PySide6 import QtGui
    image = QtGui.QImageReader(base).read()
    if image.isNull() or not image.hasAlphaChannel():
        return False
    alpha = image.convertToFormat(QtGui.QImage.Format_Alpha8)
    w, h, bpl = alpha.width(), alpha.height(), alpha.bytesPerLine()
    bits = bytes(alpha.constBits())
    if all(bits[y * bpl:y * bpl + w].count(255) == w for y in range(h)):
        return False
    fmt = "tif" if base.lower().endswith((".tif", ".tiff")) else "png"

    def save(img, path):
        tmp = path + ".tmp"
        if not img.save(tmp, fmt):
            raise OSError("could not write %s" % path)
        os.replace(tmp, path)

    if opacity:
        save(QtGui.QImage(bits, w, h, bpl, QtGui.QImage.Format_Grayscale8).copy(), opacity)
    save(image.convertToFormat(QtGui.QImage.Format_RGB888), base)
    return True
