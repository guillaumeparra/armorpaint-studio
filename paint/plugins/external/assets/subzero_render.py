#!/usr/bin/env python3
# Cuisson et rendu des .sbs/.sbsar pour subzero (chantier E1), par sbsrender et sbscooker de Designer. Bibliothèque
# standard seule (Python 3.8+), sans ArmorPaint : la fenêtre (subzero_window.py) s'en sert, les bancs aussi.
#
# Le chemin de sbsrender vient d'ArmorPaint (préférence subzero.sbsrender) ; vide, il est cherché là où Designer
# s'installe (DESIGNER_GLOBS : /Applications sous macOS, /opt/Adobe sous Linux), puis dans le PATH. sbscooker est
# cherché à côté ; sans lui, les .sbs sont refusés et les .sbsar marchent. La bibliothèque sbs:// du cuiseur est
# trouvée par lui-même, en ../Resources/packages.
#
# Linux (Rocky 9.4, lot E11) : même code. Moteur par défaut : celui que choisit sbsrender (vk, ogl3, sinon sse2 sur
# CPU) ; SUBZERO_ENGINE le force (sse2 sur une machine sans GPU). Cache en $XDG_CACHE_HOME/subzero (~/.cache/subzero).
#
# Mesures (M4, colorful_planks, mtl, 6 sorties) : coût fixe ≈ 0,6 s par processus (dyld, moteur), 512 en 0,68 s,
# 2048 en 1,17 s en TGA. `--memory-budget` est toujours donné : sans lui, sbsrender lance system_profiler pour lire la
# VRAM et l'attend 0,3 s.
#
# Formats (lot E6, FORMATS) : ceux qu'ArmorPaint lit, EXR half par défaut. Mesures en 2048, 9 sorties, poste chargé :
# TGA 1,3 s ; EXR half 1,6 s sans compression (2,5 s en zip) ; EXR float 1,7 s ; PNG 2,0 s en best_speed (4,7 s
# avec la compression par défaut) ; TIFF 2,3 s ; JPEG 1,6 s. L'EXR « 32f » n'est en float qu'avec
# --exr-format-use-float (sinon half), et la hauteur y a bien des valeurs qu'un half n'a pas. Les sorties grises sont
# écrites sur un canal (Y en EXR). ArmorPaint lit le PNG, le TIFF et le PSD en 8 bits même en 16 : pas de 16 bits
# entiers au menu. Écartés : PSD, BMP (rien de plus que TIFF et TGA), HDR (flottant sans alpha, moins précis que
# l'EXR half), DDS, WebP, WBMP, PSB (qu'ArmorPaint ne lit pas).
#
# sbsrender rend le code 0 sur une sortie introuvable (« Cannot find graph output ») ou un bitmap manquant (image
# noire) : chaque fichier attendu est vérifié, et les lignes [ERROR]/[WARNING] sont relevées. Il ne crée pas le
# dossier de sortie (code 47). Il échoue aussi, au hasard (≈ 1 fois sur 8 sur colorful_planks), quand un même appel
# écrit deux formats (TGA et EXR, ou TGA et TIFF) : « Result cannot be saved », code 47. Un format par appel ne
# l'a jamais fait (0 sur 47) : toutes les sorties d'un rendu sont dans le même format. Le TGA « 16 bits » n'existe
# pas : sbsrender l'écrit en 8 bits sans rien dire.
#
# Le rendu se fait dans un dossier temporaire à côté de la destination, puis les fichiers y sont renommés : ArmorPaint
# ne lit jamais un fichier à moitié écrit, et un rendu annulé ne touche à rien.
#
#   python3 subzero_render.py [--sbsrender CHEMIN] locate
#   python3 subzero_render.py [--sbsrender CHEMIN] cook fichier.sbs
#   python3 subzero_render.py [--sbsrender CHEMIN] render fichier.sbs[ar] --out DOSSIER [--graph URL] [--preset NOM]
#       [--size 11] [--set id=valeur]... [--entry id=image]... [--output id[=nom]]... [--format exr16] [--engine mtl]
# Résultat en JSON sur stdout.

import glob
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

import subzero_meta

EXE = ".exe" if sys.platform == "win32" else ""
# Installations de Designer, la plus récente en dernier après tri.
DESIGNER_GLOBS = {
    "darwin": ["/Applications/Adobe Substance 3D Designer*/*.app/Contents/MacOS/sbsrender"],
    "win32": ["C:/Program Files/Adobe/Adobe Substance 3D Designer*/sbsrender.exe"],
}.get(sys.platform, ["/opt/Adobe/Adobe_Substance_3D_Designer*/sbsrender", "/opt/Adobe/Adobe Substance 3D Designer*/sbsrender"])
DESIGNER_WHERE = {"darwin": "/Applications", "win32": "C:/Program Files/Adobe"}.get(sys.platform, "/opt/Adobe")


def _cache_dir():
    if sys.platform == "darwin":
        return "~/Library/Caches/subzero"
    if sys.platform == "win32":
        return os.path.join(os.environ.get("LOCALAPPDATA") or "~", "subzero", "cache")
    return os.path.join(os.environ.get("XDG_CACHE_HOME") or "~/.cache", "subzero")


CACHE_DIR = os.path.expanduser(os.environ.get("SUBZERO_CACHE") or _cache_dir())
MEMORY_BUDGET_MB = 2000
# mtl (GPU, celui de Designer et de Painter sous macOS), neon (CPU, macOS), vk ou ogl3 (GPU, Linux), sse2 (CPU, Linux) ;
# "" : pas d'option --engine, sbsrender choisit.
ENGINE = os.environ.get("SUBZERO_ENGINE") or ("mtl" if sys.platform == "darwin" else "")

# Formats de sortie (lot E6), dans l'ordre du menu de la fenêtre. float : valeurs flottantes, que l'import OCIO
# d'ArmorPaint lirait comme de la lumière linéaire (la couleur de base est pourtant encodée sRGB, comme en 8 bits).
FORMATS = {
    "exr16": {"label": "EXR 16f", "ext": ".exr", "float": True, "alpha": True,
              "args": ["--output-format", "exr", "--output-bit-depth", "16f"]},
    "exr32": {"label": "EXR 32f", "ext": ".exr", "float": True, "alpha": True,
              "args": ["--output-format", "exr", "--output-bit-depth", "32f", "--exr-format-use-float"]},
    "png": {"label": "PNG 8", "ext": ".png", "float": False, "alpha": True,
            "args": ["--output-format", "png", "--output-bit-depth", "8", "--png-format-compression", "best_speed"]},
    "tga": {"label": "TGA 8", "ext": ".tga", "float": False, "alpha": True,
            "args": ["--output-format", "tga", "--output-bit-depth", "8"]},
    "tif": {"label": "TIFF 8", "ext": ".tif", "float": False, "alpha": True,
            "args": ["--output-format", "tif", "--output-bit-depth", "8"]},
    "jpg": {"label": "JPEG", "ext": ".jpg", "float": False, "alpha": False,
            "args": ["--output-format", "jpg", "--output-bit-depth", "8"]},
}
PREVIEW_FORMAT = "tga"  # aperçus 512 du cache : le plus rapide, et Qt le lit
EXR_COMPRESSIONS = ("none", "zip", "piz", "pxr24", "b44")  # celles de sbsrender ; la fenêtre ne lit que none et zip
EXR_COMPRESSION = os.environ.get("SUBZERO_EXR_COMPRESSION") or "none"  # none : le plus rapide à écrire et à lire


def default_format():
    """$SUBZERO_FORMAT s'il est au menu, sinon EXR half."""
    fmt = os.environ.get("SUBZERO_FORMAT") or ""
    return fmt if fmt in FORMATS else "exr16"


def format_args(fmt):
    if fmt not in FORMATS:
        raise Error("unknown output format %s (%s)" % (fmt, ", ".join(FORMATS)))
    args = list(FORMATS[fmt]["args"])
    if FORMATS[fmt]["ext"] == ".exr":
        if EXR_COMPRESSION not in EXR_COMPRESSIONS:
            raise Error("unknown EXR compression %s in $SUBZERO_EXR_COMPRESSION (%s)" % (
                EXR_COMPRESSION, ", ".join(EXR_COMPRESSIONS)))
        args += ["--exr-format-compression", EXR_COMPRESSION]
    return args


def _hint():
    """Ce qu'il faut régler quand sbsrender manque : la variable d'environnement si elle est définie (lot E5), sinon
    la préférence."""
    env = os.environ.get("SUBZERO_SBSRENDER")
    if env:
        return "check $SUBZERO_SBSRENDER (%s), which overrides Preferences > subzero" % env
    return "set its path in Preferences > subzero"

LOG_LINE = re.compile(r"^\[(ERROR|WARNING)\]\[[^\]]*\](.*)$")


class Error(Exception):
    """Échec lisible, montré tel quel à l'artiste."""


# ---- Outils ------------------------------------------------------------------------------------------------------------

_versions = {}


def _version(sbsrender):
    # « version 16.0.6 (build 11611 commit 0x8ed35910) » -> « 16.0.6 (build 11611) » ; lue une fois par binaire.
    st = os.stat(sbsrender)
    key = (sbsrender, st.st_mtime, st.st_size)
    if key not in _versions:
        try:
            p = subprocess.run([sbsrender, "--version"], capture_output=True, text=True, timeout=20)
        except (OSError, subprocess.SubprocessError) as e:
            raise Error("%s does not run (%s): %s" % (sbsrender, e, _hint()))
        m = re.search(r"version\s+(\S+)(?:\s+\(build\s+(\d+))?", p.stdout + p.stderr)
        if p.returncode != 0 or m is None:
            raise Error("%s is not sbsrender (no version): %s" % (sbsrender, _hint()))
        _versions[key] = m.group(1) + (" (build %s)" % m.group(2) if m.group(2) else "")
    return _versions[key]


def locate(path=""):
    """Outils à partir du chemin de la préférence (vide : recherche de Designer, puis PATH). Rend {"sbsrender",
    "sbscooker" (None si absent), "version", "detected"} ou lève Error avec un message qui dit quoi régler."""
    path = os.path.expanduser((path or "").strip())
    detected = not path
    if detected:
        found = sorted(f for pattern in DESIGNER_GLOBS for f in glob.glob(pattern)) or \
            [f for f in [shutil.which("sbsrender")] if f]
        if not found:
            raise Error("sbsrender not found (no Adobe Substance 3D Designer in %s, none in the PATH): %s" % (
                DESIGNER_WHERE, _hint()))
        path = found[-1]
    if os.path.isdir(path):  # dossier donné : l'app de Designer, son Contents, ou le dossier de sbsrender
        for sub in ("Contents/MacOS/sbsrender", "MacOS/sbsrender", "sbsrender" + EXE):
            if os.path.isfile(os.path.join(path, sub)):
                path = os.path.join(path, sub)
                break
    if not os.path.isfile(path):
        raise Error("sbsrender not found at %s: %s" % (path, _hint()))
    if not os.access(path, os.X_OK):
        raise Error("%s is not executable: %s" % (path, _hint()))
    cooker = os.path.join(os.path.dirname(path), "sbscooker" + EXE)
    return {"sbsrender": path, "sbscooker": cooker if os.access(cooker, os.X_OK) else None, "version": _version(path),
            "detected": detected}


def _log_lines(*texts):
    errors, warnings = [], []
    for text in texts:
        for line in (text or "").splitlines():
            m = LOG_LINE.match(line.strip())
            if m:
                (errors if m.group(1) == "ERROR" else warnings).append(m.group(2).strip())
    return errors, warnings


def _spawn(argv, cancel):
    try:
        proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                start_new_session=True, cwd=tempfile.gettempdir())
    except OSError as e:
        raise Error("%s does not start (%s): %s" % (argv[0], e, _hint()))
    if cancel is not None:
        cancel(proc)
    return proc


def _run_all(argvs, cancel=None):
    """Lance chaque argv en même temps, chacun dans son propre groupe de processus ; cancel(proc) reçoit chaque
    processus pour pouvoir le tuer. Rend [(code, stdout, stderr)] et la durée totale. Code None : tué."""
    t0 = time.time()
    procs = []
    try:
        for argv in argvs:
            procs.append(_spawn(argv, cancel))
    except Error:
        for p in procs:
            kill(p)
        raise
    results = []
    for proc in procs:
        # Deux flux lus par threads (communicate) : pas d'interblocage si un processus remplit stderr.
        out, err = proc.communicate()
        code = proc.returncode if proc.returncode >= 0 else None
        results.append((code, out.decode(errors="replace"), err.decode(errors="replace")))
    return results, time.time() - t0


def _run(argv, cancel=None):
    """Un seul processus : (code, stdout, stderr, secondes)."""
    (result,), seconds = _run_all([argv], cancel)
    return result + (seconds,)


def kill(proc):
    """Tue le processus et ses descendants (groupe créé par _run)."""
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass


# ---- Cuisson -----------------------------------------------------------------------------------------------------------


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def cook(sbs, tools, cancel=None):
    """.sbs -> .sbsar cuit dans le cache, refait seulement si le .sbs ou le cuiseur ont changé (les dépendances du .sbs
    ne sont pas suivies en v0 : « Reload » recuit). Rend {"sbsar", "cached", "seconds"}."""
    if tools.get("sbscooker") is None:
        raise Error("sbscooker not found next to %s: .sbs files cannot be opened, .sbsar files can" % tools["sbsrender"])
    sbs = os.path.abspath(sbs)
    if not os.path.isfile(sbs):
        raise Error("file not found: %s" % sbs)
    st = os.stat(tools["sbscooker"])
    key = hashlib.sha256(("%s\n%s\n%s\n%s" % (_sha256(sbs), tools["sbscooker"], st.st_size, st.st_mtime)).encode())
    stem = os.path.splitext(os.path.basename(sbs))[0]
    folder = os.path.join(CACHE_DIR, "cook", "%s-%s" % (stem, key.hexdigest()[:16]))
    sbsar = os.path.join(folder, stem + ".sbsar")
    if os.path.isfile(sbsar):
        return {"sbsar": sbsar, "cached": True, "seconds": 0.0}
    os.makedirs(os.path.dirname(folder), exist_ok=True)
    work = tempfile.mkdtemp(prefix=".cook-", dir=os.path.dirname(folder))
    try:
        code, out, err, seconds = _run([tools["sbscooker"], "--inputs", sbs, "--output-path", work, "--output-name", stem],
                                       cancel=cancel)
        errors, _ = _log_lines(out, err)
        made = os.path.join(work, stem + ".sbsar")
        if code != 0 or not os.path.isfile(made):
            if code is None:
                raise Error("cooking cancelled")
            raise Error("cooking %s failed: %s" % (os.path.basename(sbs), "; ".join(errors) or "exit code %s" % code))
        try:
            os.rename(work, folder)
        except OSError:  # une autre cuisson du même .sbs a fini avant
            if not os.path.isfile(sbsar):
                raise
        return {"sbsar": sbsar, "cached": False, "seconds": seconds}
    finally:
        shutil.rmtree(work, ignore_errors=True)


def open_package(path, tools, cancel=None):
    """.sbs ou .sbsar -> {"sbsar", "description", "cook"} ; un .sbs est cuit d'abord, puis lu comme un .sbsar."""
    path = os.path.abspath(path)
    ext = os.path.splitext(path)[1].lower()
    cooked = None
    if ext == ".sbs":
        cooked = cook(path, tools, cancel)
        sbsar = cooked["sbsar"]
    elif ext == ".sbsar":
        sbsar = path
    else:
        raise Error("not a .sbs or .sbsar file: %s" % path)
    if not os.path.isfile(sbsar):
        raise Error("file not found: %s" % sbsar)
    try:
        desc = subzero_meta.read_sbsar(sbsar)
    except Exception as e:  # archive ou XML illisible
        raise Error("cannot read %s: %s" % (os.path.basename(sbsar), e))
    return {"sbsar": sbsar, "description": desc, "cook": cooked}


# ---- Rendu -------------------------------------------------------------------------------------------------------------


def graphs(desc):
    return [g for a in desc["assemblies"] for g in a["graphs"]]


def find_graph(desc, url=None):
    """Graphe demandé ; sinon le seul graphe matériau, sinon le premier."""
    gs = graphs(desc)
    if not gs:
        raise Error("the package has no graph")
    if url:
        for g in gs:
            if g["pkgurl"] == url:
                return g
        raise Error("graph %s not found in the package" % url)
    materials = [g for g in gs if g["graphtype"] == "material"]
    return materials[0] if len(materials) == 1 else gs[0]


def _report(stdout):
    # Rapport JSON de sbsrender, à la fin de stdout après ses lignes de log : [{"identifier", "outputs": [{"identifier",
    # "value": fichier}...]}].
    start = stdout.rfind("\n[\n")
    start = start + 1 if start >= 0 else (0 if stdout.startswith("[\n") else -1)
    if start < 0:
        return []
    try:
        report = json.loads(stdout[start:])
    except ValueError:
        return []
    return report if isinstance(report, list) else []


def render_args(sbsar, graph, out_dir, values=None, entries=None, preset=None, size=None, outputs=None,
                fmt=PREVIEW_FORMAT, engine=ENGINE):
    """Arguments de sbsrender. values : {id: valeur} (unités du moteur) ; entries : {id: image} ; size : (log2 x,
    log2 y) ; outputs : identifiants à rendre ; fmt : clé de FORMATS, un seul format par appel."""
    by_id = {i["identifier"]: i for i in graph["inputs"]}
    args = ["render", sbsar, "--input-graph", graph["pkgurl"]] + (["--engine", engine] if engine else []) + [
        "--memory-budget", str(MEMORY_BUDGET_MB), "--output-path", out_dir, "--output-name", "{outputNodeName}"] + \
        format_args(fmt)
    if preset:
        args += ["--use-preset", preset]
    if size is not None:
        args += ["--set-value", "$outputsize@%d,%d" % tuple(size)]
    for k, v in (values or {}).items():
        i = by_id.get(k)
        if i is None:
            raise Error("unknown input %s in graph %s" % (k, graph["pkgurl"]))
        if i["type"] in ("image", "string", "font"):
            raise Error("input %s is not numeric" % k)
        args += ["--set-value", "%s@%s" % (k, subzero_meta.format_value(i["type"], v))]
    for k, path in (entries or {}).items():
        if by_id.get(k, {}).get("type") != "image":
            raise Error("unknown image input %s in graph %s" % (k, graph["pkgurl"]))
        args += ["--set-entry", "%s@%s" % (k, path)]
    for o in outputs or []:
        args += ["--input-graph-output", o]
    return args


def render(sbsar, graph, out_dir, tools, values=None, entries=None, preset=None, size=None, outputs=None, names=None,
           fmt=PREVIEW_FORMAT, engine=ENGINE, cancel=None):
    """Rend le graphe dans out_dir, au format fmt (clé de FORMATS). names : {sortie: nom de fichier sans extension}
    (défaut : l'identifiant). Les fichiers déjà là sont remplacés seulement si tout le rendu a réussi. Rend {"files":
    {sortie: chemin}, "warnings", "seconds", "argv"} ; lève Error si une sortie manque, si sbsrender échoue, ou sur
    annulation."""
    ids = [o["identifier"] for o in graph["outputs"] if o["type"] == "image"]
    wanted = list(outputs) if outputs is not None else ids
    missing = [o for o in wanted if o not in ids]
    if missing:  # comme wood_plain : le moteur rendrait 0 sans rien écrire
        raise Error("output %s not found in graph %s" % (", ".join(missing), graph["pkgurl"]))
    for e, path in (entries or {}).items():
        if not os.path.isfile(path):
            raise Error("image for input %s not found: %s" % (e, path))
    os.makedirs(out_dir, exist_ok=True)
    work = tempfile.mkdtemp(prefix=".render-", dir=out_dir)
    try:
        argvs = [[tools["sbsrender"]] + render_args(sbsar, graph, work, values, entries, preset, size, wanted, fmt,
                                                    engine)]
        results, seconds = _run_all(argvs, cancel=cancel)
        errors, warnings, written = [], [], {}
        for code, out, err in results:
            e, w = _log_lines(out, err)
            errors += e
            warnings += w
            if code is None:
                raise Error("render cancelled")
            if code != 0:
                raise Error("sbsrender failed (exit code %d): %s" % (code, "; ".join(e) or err.strip()[-500:]))
            for g in _report(out):
                for o in g.get("outputs", []):
                    if isinstance(o.get("value"), str):
                        written[o.get("identifier")] = o["value"]
        files = {}
        for o in wanted:
            path = written.get(o) or os.path.join(work, o + FORMATS[fmt]["ext"])
            if not os.path.isfile(path) or os.path.getsize(path) == 0:
                raise Error("sbsrender wrote no file for output %s%s" % (o, ": " + "; ".join(errors + warnings)
                                                                           if errors or warnings else ""))
            files[o] = path
        if errors:
            raise Error("sbsrender reported: %s" % "; ".join(errors))
        final = {}
        for o, path in files.items():
            dest = os.path.join(out_dir, (names or {}).get(o, o) + os.path.splitext(path)[1])
            os.replace(path, dest)
            final[o] = dest
        return {"files": final, "warnings": warnings, "seconds": round(seconds, 3), "argv": argvs}
    finally:
        shutil.rmtree(work, ignore_errors=True)


# ---- Ligne de commande -------------------------------------------------------------------------------------------------


def _main(argv):
    import argparse
    ap = argparse.ArgumentParser(description="subzero: cook and render .sbs/.sbsar files with sbsrender")
    ap.add_argument("--sbsrender", default="")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("locate")
    c = sub.add_parser("cook")
    c.add_argument("sbs")
    r = sub.add_parser("render")
    r.add_argument("package")
    r.add_argument("--out", required=True)
    r.add_argument("--graph")
    r.add_argument("--preset")
    r.add_argument("--size", type=int, help="log2 of the output size (11 = 2048)")
    r.add_argument("--set", action="append", default=[], metavar="ID=VALUE")
    r.add_argument("--entry", action="append", default=[], metavar="ID=IMAGE")
    r.add_argument("--output", action="append", metavar="ID[=NAME]")
    r.add_argument("--format", default=None, choices=list(FORMATS), help="default: $SUBZERO_FORMAT, else exr16")
    r.add_argument("--engine", default=ENGINE)
    a = ap.parse_args(argv)
    try:
        tools = locate(a.sbsrender)
        if a.cmd == "locate":
            result = tools
        elif a.cmd == "cook":
            result = cook(a.sbs, tools)
        else:
            pkg = open_package(a.package, tools)
            graph = find_graph(pkg["description"], a.graph)
            types = {i["identifier"]: i["type"] for i in graph["inputs"]}
            values = {}
            for kv in a.set:
                k, _, v = kv.partition("=")
                values[k] = subzero_meta.parse_value(types.get(k, "float1"), v)
            entries = dict(kv.partition("=")[::2] for kv in a.entry)
            outputs = names = None
            if a.output:
                outputs = [o.partition("=")[0] for o in a.output]
                names = {o.partition("=")[0]: o.partition("=")[2] for o in a.output if "=" in o}
            size = (a.size, a.size) if a.size else None
            result = render(pkg["sbsar"], graph, a.out, tools, values, entries, a.preset, size, outputs, names,
                            a.format or default_format(), a.engine)
            result["cook"] = pkg["cook"]
        print(json.dumps({"ok": True, "result": result}, ensure_ascii=False))
        return 0
    except Error as e:
        print(json.dumps({"ok": False, "error": str(e)}, ensure_ascii=False))
        return 1


if __name__ == "__main__":
    sys.exit(_main(sys.argv[1:]))
