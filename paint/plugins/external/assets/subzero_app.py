#!/usr/bin/env python3
# subzero sans ArmorPaint (lot E9) : la fenêtre de subzero_window.py, qui exporte les textures dans un dossier au lieu
# de les envoyer, et des commandes sans fenêtre pour les scripts et les machines de rendu. Point d'entrée du paquet
# autonome (tools/release_subzero.py : bin/subzero, python -m subzero, rez, pip). Python 3.9+ ; PySide6 pour la
# fenêtre ; PyOpenColorIO et numpy pour la couleur (sans eux, textures telles que sbsrender les écrit).
#
#   subzero [paquet.sbsar | paquet.sbs | dossier d'un matériau]    fenêtre
#   subzero render <paquet | dossier> [options]                    rendu final, sans Qt ; résultat en JSON sur stdout
#   subzero info <paquet> [--graph URL]                            graphes, paramètres, presets et sorties, en JSON
#   subzero locate                                                  sbsrender et sbscooker trouvés, en JSON
#   subzero --version
#
# Un matériau exporté est un dossier <export>/<Nom>/ : textures <Nom>_<canal>.<ext> et recette subzero.json, la même
# qu'avec ArmorPaint. Rouvrir le dossier (fenêtre, ou `render` pour le refaire, par exemple à une autre taille) reprend
# ses réglages ; ArmorPaint Studio l'importe tel quel.
#
# Environnement : studio.env est chargé au démarrage comme ArmorPaint le fait ($STUDIO_ENV, sinon celui du dossier
# d'ArmorPaint ; studio_env.py), sans écraser les vraies variables. Puis :
#   SUBZERO_SBSRENDER         sbsrender (fichier, ou dossier de Designer) ; sinon la préférence subzero.sbsrender
#                             d'ArmorPaint Studio ($STUDIO_PREFS, sinon studio_prefs.json de son dossier), sinon Designer
#                             là où il s'installe, puis le PATH
#   SUBZERO_EXPORT_DIR        dossier des matériaux exportés (défaut : demandé une fois ; `render` : dossier courant)
#   SUBZERO_EXPORT_TEMPLATE   nom des textures, défaut {name}_{channel} ; champs {name}, {channel}, {size}
#   SUBZERO_NORMAL_FORMAT     opengl (défaut) ou directx
#   SUBZERO_SIZE              taille par défaut en pixels (fenêtre : 512, pour que le viewer suive ; `render` : 2048)
#   SUBZERO_VIEWER            viewer ouvert au démarrage de la fenêtre (1, défaut) ou non
#   SUBZERO_LIBRARY           dossiers du navigateur de paquets, séparés par ':' (un onglet chacun)
#   SUBZERO_CONFIG_DIR        réglages de la fenêtre (dossiers ajoutés au navigateur) ; défaut : dossier de
#                             configuration de l'utilisateur (subzero_browser.py)
#   SUBZERO_FORMAT, SUBZERO_EXR_COMPRESSION, SUBZERO_ENGINE, SUBZERO_CACHE, SUBZERO_LIVE    comme dans ArmorPaint
#   SUBZERO_OCIO_*, OCIO, STUDIO_OCIO_*                                                  couleur (panneau Settings)

__version__ = "0.1.0"

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import studio_env  # noqa: E402 — bibliothèque standard seule ; chargé avant les modules qui lisent l'environnement

COMMANDS = ("render", "info", "locate")
PREF_SBSRENDER = "subzero.sbsrender"  # subzero.c
PREFS_FILE = "studio_prefs.json"  # studio_prefs.c
DEFAULT_WIDTH = 2048  # subzero render
WINDOW_WIDTH = 512  # fenêtre : rendus rapides, que le viewer montre à mesure


class Usage(Exception):
    """Réglage ou argument invalide : message, code 2."""


def log(message):
    sys.stderr.write("subzero: %s\n" % message)
    sys.stderr.flush()


# ---- Réglages -----------------------------------------------------------------------------------------------------------


def prefs_files():
    explicit = os.environ.get("STUDIO_PREFS")
    return [explicit] if explicit else [os.path.join(studio_env.user_dir(), PREFS_FILE)]


def sbsrender_setting():
    """Chemin de sbsrender réglé : $SUBZERO_SBSRENDER, sinon la préférence d'ArmorPaint Studio ; "" pour la détection."""
    env = os.environ.get("SUBZERO_SBSRENDER")
    if env:
        return env
    for path in prefs_files():
        try:
            with open(path, encoding="utf-8") as f:
                value = json.load(f).get(PREF_SBSRENDER)
        except (OSError, ValueError, AttributeError):
            continue
        if isinstance(value, str) and value:
            return value
    return ""


def default_width(fallback=DEFAULT_WIDTH):
    text = (os.environ.get("SUBZERO_SIZE") or "").strip()
    if not text:
        return fallback
    return parse_width(text, "$SUBZERO_SIZE")


def parse_width(text, what="--size"):
    """Largeur en pixels, une puissance de deux de 16 à 8192 -> elle-même."""
    try:
        width = int(text)
    except ValueError:
        width = 0
    if width < 16 or width > 8192 or width & (width - 1):
        raise Usage("%s: %s is not a power of two from 16 to 8192" % (what, text))
    return width


def configure():
    """Réglages globaux des règles, lus dans l'environnement ; lève Usage s'ils sont invalides."""
    import subzero_material
    fmt = (os.environ.get("SUBZERO_NORMAL_FORMAT") or "opengl").strip().lower()
    if fmt not in subzero_material.NORMAL_FORMATS:
        raise Usage("$SUBZERO_NORMAL_FORMAT: %s is not %s" % (fmt, " or ".join(subzero_material.NORMAL_FORMATS)))
    subzero_material.NORMAL_FORMAT = fmt
    default_width()


def template():
    import subzero_material
    value = os.environ.get("SUBZERO_EXPORT_TEMPLATE") or subzero_material.NAME_TEMPLATE
    error = subzero_material.check_template(value)
    if error:
        raise Usage("$SUBZERO_EXPORT_TEMPLATE: %s" % error)
    return value


def open_target(path):
    """Argument de la fenêtre ou de render -> {"file"} (paquet) ou {"dir"} (matériau exporté)."""
    path = os.path.abspath(os.path.expanduser(path))
    return {"dir": path} if os.path.isdir(path) else {"file": path}


def standalone_state(target=None, width=None):
    """État que la fenêtre demande à ArmorPaint, sans lui : ni "ocio" (couleur réglée par l'environnement) ni "env"
    (studio.env de studio_env), pas de projet. width : taille « Default » de la fenêtre."""
    width = width or default_width(WINDOW_WIDTH)
    state = {"sbsrender": sbsrender_setting(), "project": {"file": ""}, "resolution": [width, width], "bits": 8,
             "standalone": {"version": __version__}}
    if target:
        state["open"] = target
    return state


# ---- Fenêtre ------------------------------------------------------------------------------------------------------------


def run_window(argv):
    ap = argparse.ArgumentParser(prog="subzero", description="subzero %s: .sbs/.sbsar materials rendered by sbsrender. "
                                 "Commands: render, info, locate (subzero <command> --help)." % __version__)
    ap.add_argument("path", nargs="?", help="a .sbsar or .sbs package, or the folder of an exported material")
    ap.add_argument("--version", action="version", version="subzero %s" % __version__)
    a = ap.parse_args(argv)
    configure()
    import studio_qt
    import subzero_window
    app = studio_qt.start_standalone("subzero", painter=True)
    test = None
    if os.environ.get("SUBZERO_TEST"):
        with open(os.environ["SUBZERO_TEST"]) as f:
            test = json.load(f)
    host = subzero_window.LocalHost(standalone_state(open_target(a.path) if a.path else None))
    window = subzero_window.SubzeroWindow(host, None, test)
    window.to_front()
    return app.exec()


# ---- Commandes ----------------------------------------------------------------------------------------------------------


def locate_tools(a):
    import subzero_render
    return subzero_render.locate(a.sbsrender if a.sbsrender is not None else sbsrender_setting())


def cmd_locate(a):
    return locate_tools(a)


def cmd_info(a):
    import subzero_material as M
    import subzero_render
    tools = locate_tools(a) if a.package.lower().endswith(".sbs") else {}
    pkg = subzero_render.open_package(a.package, tools)
    graphs = [subzero_render.find_graph(pkg["description"], a.graph)] if a.graph else subzero_render.graphs(pkg["description"])
    out = []
    for g in graphs:
        values = M.graph_values(g)
        params = []
        for i in g["inputs"]:
            if i["identifier"] not in values and not (i["type"] == "image" and M.shown(i)):
                continue
            p = {k: i.get(k) for k in ("identifier", "label", "type", "default", "group", "widget", "description")}
            for k in ("min", "max", "items"):
                if k in i:
                    p[k] = i[k]
            params.append(p)
        outputs = []
        for o, ch, why in M.auto_mapping(g, values):
            outputs.append({"identifier": o["identifier"], "label": o.get("label"), "usages": o.get("usages") or [],
                            "channel": ch, "why": why})
        out.append({"graph": g["pkgurl"], "label": g.get("label"), "type": g.get("graphtype"),
                    "presets": [p["label"] for p in g.get("presets") or []], "parameters": params, "outputs": outputs})
    return {"package": os.path.abspath(a.package), "cook": pkg.get("cook"), "graphs": out}


def parse_assignments(items, what):
    out = []
    for kv in items or []:
        k, sep, v = kv.partition("=")
        if not sep or not k:
            raise Usage("%s %s: expected ID=VALUE" % (what, kv))
        out.append((k, v))
    return out


def cmd_render(a):
    import subzero_color
    import subzero_material as M
    import subzero_meta
    import subzero_render
    names_template = template()
    tools = locate_tools(a)
    recipe, folder = None, None
    if os.path.isdir(a.source):
        folder = os.path.abspath(a.source)
        try:
            recipe = M.read_recipe(folder)
        except (OSError, ValueError) as e:
            raise Usage("%s: no subzero recipe (%s)" % (folder, e))
        package = M.recipe_package(recipe, folder)
    else:
        package = a.source
    package = os.path.abspath(package)
    pkg = subzero_render.open_package(package, tools)
    graph = subzero_render.find_graph(pkg["description"], a.graph or (recipe or {}).get("graph"))
    by_id = {i["identifier"]: i for i in graph["inputs"]}
    kept = recipe if recipe is not None and recipe.get("graph") == graph["pkgurl"] else {}

    # Réglages : ceux de la recette, ou le preset demandé (comme le menu de la fenêtre : défauts puis preset, graine
    # gardée), puis --set.
    if a.preset is not None:
        seed = {k: v for k, v in (kept.get("values") or {}).items() if k == "$randomseed"}
        values, preset = M.graph_values(graph, seed, a.preset or None), a.preset or None
    else:
        values, preset = M.graph_values(graph, kept.get("values")), kept.get("preset")
    for k, text in parse_assignments(a.set, "--set"):
        if k not in by_id or by_id[k]["type"] == "image":
            raise Usage("--set %s: no such parameter in %s%s" % (k, graph["pkgurl"],
                                                                  " (an image input: use --entry)" if k in by_id else ""))
        values[k] = subzero_meta.parse_value(by_id[k]["type"], text)
    entries = {k: v for k, v in (kept.get("entries") or {}).items() if k in by_id}
    for k, path in parse_assignments(a.entry, "--entry"):
        if by_id.get(k, {}).get("type") != "image":
            raise Usage("--entry %s: no image input with this identifier in %s" % (k, graph["pkgurl"]))
        entries[k] = os.path.abspath(path)
    overrides = dict(kept.get("overrides") or {})
    outputs = {o["identifier"] for o in graph["outputs"]}
    for o, ch in parse_assignments(a.map, "--map"):
        ch = "" if ch.lower() in ("", "none") else ch
        if o not in outputs or (ch and ch not in M.CHANNEL_LABEL):
            raise Usage("--map %s=%s: output of %s = one of %s, or none" % (o, ch, graph["pkgurl"], ", ".join(M.CHANNEL_LABEL)))
        for other, c in list(overrides.items()):
            if c == ch and ch and other != o:
                overrides[other] = ""  # un canal ne vient que d'une sortie
        overrides[o] = ch
    rows = M.apply_overrides(M.auto_mapping(graph, values), overrides)
    outs = [(o["identifier"], ch) for o, ch, _ in rows if ch]
    if not outs:
        raise Usage("no output of %s is linked to a channel (--map OUTPUT=CHANNEL)" % graph["pkgurl"])

    if a.size:
        size = M.log2_size(parse_width(a.size))
        saved_size = size
    else:
        saved_size = kept.get("size") if recipe is not None else None
        size = saved_size if saved_size is not None else M.log2_size(default_width())
    fmt = a.format or (recipe or {}).get("format") or ("tga" if recipe is not None else subzero_render.default_format())
    name = a.name or (recipe or {}).get("name") or graph.get("label") or os.path.splitext(os.path.basename(package))[0]
    if folder is None or a.out_dir:
        base = a.out_dir or os.environ.get("SUBZERO_EXPORT_DIR") or os.getcwd()
        folder = os.path.join(os.path.abspath(os.path.expanduser(base)), M.slug(name))

    color = subzero_color.Color()
    if color.error and any(v in os.environ for v in M.COLOR_VARS):
        log("color: %s" % color.error)
    data = M.render_material(pkg["sbsar"], graph, folder, tools, outs, values=values, entries=entries,
                             size=(size, size), fmt=fmt, color=color, template=names_template)
    M.write_recipe(folder, M.make_recipe(package, folder, name, graph, values, entries, preset, saved_size, outs,
                                         overrides, data, color, tools))
    deleted = []
    previous = (recipe or {}).get("files") or {} if folder == os.path.abspath(a.source) else {}
    for path in M.stale_files(folder, list(data["files"].values()), names_template, previous.values()):
        os.remove(path)
        deleted.append(os.path.basename(path))
    log("%s: %d textures, %d px %s in %.2f s" % (folder, len(data["files"]), 1 << size,
                                                  subzero_render.FORMATS[fmt]["label"], data["seconds"]))
    return {"folder": folder, "name": name, "package": package, "graph": graph["pkgurl"], "preset": preset,
            "files": data["files"], "spaces": data["spaces"], "size": [1 << size, 1 << size], "format": fmt,
            "seconds": data["seconds"], "warnings": data["warnings"], "color_warnings": data["color_warnings"],
            "color": {"ready": color.ready, "config": color.config_source if color.ready else None,
                      "error": color.error},
            "deleted": deleted, "cook": pkg.get("cook")}


def run_command(argv):
    ap = argparse.ArgumentParser(prog="subzero")
    ap.add_argument("--sbsrender", default=None, help="sbsrender to use (default: $SUBZERO_SBSRENDER, the ArmorPaint "
                    "Studio preference, then detection)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("locate", help="sbsrender and sbscooker found")
    i = sub.add_parser("info", help="graphs, parameters, presets and outputs of a package")
    i.add_argument("package")
    i.add_argument("--graph", help="graph URL (pkg://...), default: every graph")
    r = sub.add_parser("render", help="render a material into <out-dir>/<Name>/ (textures and subzero.json)")
    r.add_argument("source", help="a .sbsar or .sbs package, or the folder of an exported material (rendered again "
                   "in place, unless --out-dir)")
    r.add_argument("-o", "--out-dir", help="folder of the materials (default: $SUBZERO_EXPORT_DIR, else the current "
                   "folder)")
    r.add_argument("--name", help="material name, which names its folder and textures (default: the graph label)")
    r.add_argument("--graph", help="graph URL (pkg://...), default: the only material graph, else the first")
    r.add_argument("--preset", help="preset label ('' for the defaults)")
    r.add_argument("--set", action="append", metavar="ID=VALUE", help="parameter value (x,y,z for vectors)")
    r.add_argument("--entry", action="append", metavar="ID=IMAGE", help="image input")
    r.add_argument("--map", action="append", metavar="OUTPUT=CHANNEL", help="link an output to a channel (%s), or "
                   "none" % ", ".join(["base", "opacity", "occlusion", "roughness", "metallic", "normal", "height"]))
    r.add_argument("--size", help="width in pixels, a power of two (default: $SUBZERO_SIZE, else 2048)")
    r.add_argument("--format", help="exr16 (default: $SUBZERO_FORMAT), exr32, png, tga, tif or jpg")
    a = ap.parse_args(argv)
    import subzero_render
    try:
        configure()
        if getattr(a, "format", None) and a.format not in subzero_render.FORMATS:
            raise Usage("--format %s: one of %s" % (a.format, ", ".join(subzero_render.FORMATS)))
        result = {"locate": cmd_locate, "info": cmd_info, "render": cmd_render}[a.cmd](a)
    except (Usage, subzero_render.Error, OSError, ValueError) as e:
        log(str(e))
        print(json.dumps({"ok": False, "error": str(e)}, ensure_ascii=False))
        return 2 if isinstance(e, Usage) else 1
    print(json.dumps({"ok": True, "result": result}, ensure_ascii=False, indent=1, default=str))
    return 0


def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    studio_env.load()
    try:
        if argv and argv[0] in COMMANDS or argv[:1] == ["--sbsrender"]:
            return run_command(argv)
        return run_window(argv)
    except Usage as e:
        log(str(e))
        return 2


if __name__ == "__main__":
    sys.exit(main())
