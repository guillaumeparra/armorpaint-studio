#!/usr/bin/env python3
# Banc du chantier E1 (subzero), hors de l'app : description des .sbsar (subzero_meta.py), cuisson et rendu
# (subzero_render.py). Lancé par .tests/subzero.sh, qui en donne les réglages. Aucun fichier Adobe dans le dépôt : les
# échantillons sont lus hors dépôt, et chaque partie est sautée (« skip ») si les siens sont absents.
#
#   SUBZERO_SBSRENDER   sbsrender (défaut : détection dans /Applications)
#   SUBZERO_SHELF       dossier des .sbsar de Painter (défaut : Contents de Painter dans /Applications)
#   SUBZERO_STUDY       dossier de sbsar_meta.py de l'étude (défaut : ~/Desktop/code/sbsar/recherche-live/l5)
#   SUBZERO_SAMPLES     dossier de colorful_planks.sbsar et colorful_planks/colorful_planks.sbs (~/Desktop/code/sbsar)
#   SUBZERO_RENDER_ALL  1 : les 107 matériaux de l'étagère en 2048 (≈ 4 min) ; sinon 20 tirés au sort
#   SUBZERO_CACHE       cache de cuisson (subzero.sh en donne un vide, dans le dossier de travail)

import collections
import json
import os
import random
import shutil
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "assets"))
import subzero_meta  # noqa: E402
import subzero_render  # noqa: E402

W = sys.argv[1]
FAILED = []
PAINTER = "/Applications/Adobe Substance 3D Painter/Adobe Substance 3D Painter.app/Contents"
SHELF = os.environ.get("SUBZERO_SHELF") or PAINTER
STUDY = os.environ.get("SUBZERO_STUDY") or os.path.expanduser("~/Desktop/code/sbsar/recherche-live/l5")
SAMPLES = os.environ.get("SUBZERO_SAMPLES") or os.path.expanduser("~/Desktop/code/sbsar")
PLANKS = os.path.join(SAMPLES, "colorful_planks.sbsar")
PLANKS_SBS = os.path.join(SAMPLES, "colorful_planks", "colorful_planks.sbs")
INFO_TYPES = {"float1": "FLOAT1", "float2": "FLOAT2", "float3": "FLOAT3", "float4": "FLOAT4", "int1": "INTEGER1",
              "int2": "INTEGER2", "int3": "INTEGER3", "int4": "INTEGER4", "image": "IMAGE", "string": "STRING",
              "font": "FONT"}


def check(name, cond, detail=""):
    print("%s %s%s" % ("ok  " if cond else "FAIL", name, "" if cond or not detail else " : %s" % detail))
    if not cond:
        FAILED.append(name)


def skip(name, why):
    print("skip %s (%s)" % (name, why))


def exr_pixel_types(path):
    # Types des canaux d'un EXR (attribut chlist) : 0 uint, 1 half, 2 float ; None si ce n'est pas un EXR.
    head = open(path, "rb").read(4096)
    at = head.find(b"channels\x00chlist\x00")
    if head[:4] != b"\x76\x2f\x31\x01" or at < 0:
        return None
    p, types = at + len(b"channels\x00chlist\x00") + 4, []
    while head[p] != 0:
        p = head.index(b"\x00", p) + 1
        types.append(int.from_bytes(head[p:p + 4], "little"))
        p += 16
    return types


def exr_compression(path):
    # Attribut compression d'un EXR : 0 aucune, 3 zip, 4 piz… ; None s'il manque.
    head = open(path, "rb").read(4096)
    at = head.find(b"compression\x00compression\x00")
    return head[at + len(b"compression\x00compression\x00") + 4] if at >= 0 else None


def shelf_files(root):
    return sorted(os.path.join(d, f) for d, _, fs in os.walk(root) if ".alg_meta" not in d for f in fs
                  if f.endswith(".sbsar"))


# ---- Outils ------------------------------------------------------------------------------------------------------------

try:
    TOOLS = subzero_render.locate(os.environ.get("SUBZERO_SBSRENDER", ""))
    print("sbsrender %s (%s)" % (TOOLS["version"], TOOLS["sbsrender"]))
except subzero_render.Error as e:
    TOOLS = None
    print("skip sbsrender : %s" % e)

for bad, expect in (("/nope/sbsrender", "not found at /nope/sbsrender"), ("/bin/ls", "is not sbsrender"),
                    (W, "not found at")):
    try:
        subzero_render.locate(bad)
        check("chemin faux refusé : %s" % bad, False, "accepté")
    except subzero_render.Error as e:
        check("chemin faux refusé : %s" % bad, expect in str(e) and "Preferences > subzero" in str(e), str(e))
if TOOLS is not None:
    folder = os.path.dirname(os.path.dirname(TOOLS["sbsrender"]))  # Contents (ou la copie isolée)
    check("dossier accepté à la place du binaire", subzero_render.locate(folder)["sbsrender"] == TOOLS["sbsrender"])

# ---- Description : étagère de Painter, contre l'étude et contre sbsrender info ------------------------------------------

files = shelf_files(SHELF) if os.path.isdir(SHELF) else []
if not files:
    skip("description de l'étagère", "pas de .sbsar dans %s" % SHELF)
else:
    t = time.time()
    descs, bad = {}, []
    for p in files:
        try:
            descs[p] = subzero_meta.read_sbsar(p)
        except Exception as e:  # noqa: BLE001 (tout échec de lecture compte)
            bad.append("%s: %s" % (p, e))
    check("%d .sbsar lus (%.1f s)" % (len(files), time.time() - t), not bad, "; ".join(bad[:3]))
    stats, visfail = collections.Counter(), []
    for p, d in descs.items():
        for g in subzero_render.graphs(d):
            stats["graphes"] += 1
            for item in g["inputs"] + g["outputs"]:
                if item["visibleif"]:
                    try:
                        subzero_meta.compile_visibleif(item["visibleif"])
                        stats["visibleif"] += 1
                    except ValueError as e:
                        visfail.append("%s %s" % (os.path.basename(p), e))
            try:
                subzero_meta.visible_inputs(g, {})
            except Exception as e:  # noqa: BLE001
                visfail.append("%s eval %s" % (os.path.basename(p), e))
    check("visibleif compilés et évalués (%d graphes, %d conditions)" % (stats["graphes"], stats["visibleif"]),
          not visfail, "; ".join(visfail[:3]))
    study = os.path.join(STUDY, "sbsar_meta.py")
    if not os.path.isfile(study):
        skip("identique à l'étude", "pas de %s" % study)
    else:
        # L'étude tourne dans son propre processus : ses modules (sevenz, sbsar_meta) ne se mêlent pas aux nôtres.
        code = ("import json, sys; sys.path.insert(0, %r); import sbsar_meta as m\n"
                "for p in sys.stdin.read().splitlines(): print(json.dumps(m.read_sbsar(p), sort_keys=True))") % STUDY
        proc = subprocess.run([sys.executable, "-c", code], input="\n".join(descs), capture_output=True, text=True,
                              env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
        theirs = proc.stdout.splitlines()
        diff = [p for p, line in zip(descs, theirs) if json.dumps(descs[p], sort_keys=True) != line]
        check("%d descriptions identiques à celles de l'étude" % len(descs),
              proc.returncode == 0 and len(theirs) == len(descs) and not diff,
              proc.stderr[-300:] or "; ".join(os.path.basename(p) for p in diff[:5]))
    if TOOLS is None:
        skip("contre sbsrender info", "pas de sbsrender")
    else:
        random.seed(1)
        sample = random.sample(sorted(descs), min(40, len(descs)))
        diff = []
        for p in sample:
            out = subprocess.run([TOOLS["sbsrender"], "info", p], capture_output=True, text=True, cwd="/tmp").stdout
            info, cur = collections.defaultdict(list), None
            for line in out.splitlines():
                w = line.split()
                if not w:
                    continue
                if w[0] == "GRAPH-URL":
                    cur = w[1]
                elif w[0] == "INPUT":
                    info[cur].append(("in", w[1], w[2]))
                elif w[0] == "OUTPUT":
                    info[cur].append(("out", w[1]))
            mine = {g["pkgurl"]: [("in", i["identifier"], INFO_TYPES[i["type"]]) for i in g["inputs"]]
                    + [("out", o["identifier"]) for o in g["outputs"]] for g in subzero_render.graphs(descs[p])}
            if dict(info) != mine:
                diff.append(os.path.basename(p))
        check("%d .sbsar décrits comme par sbsrender info" % len(sample), not diff, ", ".join(diff[:5]))

# ---- Rendu ------------------------------------------------------------------------------------------------------------


def render_package(path, size=(11, 11), outputs=None, out=None, **kw):
    pkg = subzero_render.open_package(path, TOOLS)
    g = subzero_render.find_graph(pkg["description"])
    return pkg, g, subzero_render.render(pkg["sbsar"], g, out or os.path.join(W, "render"), TOOLS, size=size,
                                         outputs=outputs, **kw)


if TOOLS is None:
    skip("rendus", "pas de sbsrender")
else:
    materials = [p for p in files if "/starter_assets/materials/" in p]
    if not materials:
        skip("matériaux de l'étagère", "pas de starter_assets/materials dans %s" % SHELF)
    else:
        if os.environ.get("SUBZERO_RENDER_ALL") != "1":
            random.seed(2)
            materials = sorted(random.sample(materials, min(20, len(materials))))
        errors, t, count = [], time.time(), 0
        for p in materials:
            d = subzero_meta.read_sbsar(p)
            g = subzero_render.find_graph(d)
            outs = [o["identifier"] for o in g["outputs"] if o["type"] == "image"]
            dest = os.path.join(W, "shelf", os.path.splitext(os.path.basename(p))[0])
            try:
                res = subzero_render.render(p, g, dest, TOOLS, size=(11, 11), outputs=outs)
                count += len(res["files"])
            except subzero_render.Error as e:
                errors.append("%s: %s" % (os.path.basename(p), e))
        check("%d matériaux de l'étagère rendus en 2048 (%d fichiers, %.0f s)" % (len(materials), count, time.time() - t),
              not errors, "; ".join(errors[:3]))

    wood = os.path.join(SHELF, "Resources/starter_assets/materials/Wood/wood_plain.sbsar")
    if not os.path.isfile(wood):
        skip("wood_plain", "absent")
    else:
        dest = os.path.join(W, "wood_plain")
        try:
            render_package(wood, outputs=["basecolor", "ambientocclusion"], out=dest)
            check("wood_plain : sortie ambientocclusion introuvable signalée", False, "rendu accepté")
        except subzero_render.Error as e:
            check("wood_plain : sortie ambientocclusion introuvable signalée",
                  "ambientocclusion not found" in str(e) and not os.path.exists(dest), str(e))

    if not os.path.isfile(PLANKS):
        skip("colorful_planks.sbsar", "absent de %s" % SAMPLES)
    else:
        dest = os.path.join(W, "planks")
        six = ["basecolor", "normal", "roughness", "metallic", "height", "ambientocclusion"]
        _, g, res = render_package(PLANKS, outputs=six, out=dest, names={"basecolor": "base"}, fmt="exr16")
        names = sorted(os.listdir(dest))
        check("colorful_planks : 6 sorties en EXR, renommées, en un seul appel (un format par appel)",
              names == ["ambientocclusion.exr", "base.exr", "height.exr", "metallic.exr", "normal.exr", "roughness.exr"]
              and len(res["argv"]) == 1, "%s, %d" % (names, len(res["argv"])))
        check("colorful_planks : EXR half, couleur en A B G R, gris en Y",
              exr_pixel_types(os.path.join(dest, "base.exr")) == [1, 1, 1, 1]
              and exr_pixel_types(os.path.join(dest, "height.exr")) == [1])
        comp = exr_compression(os.path.join(dest, "height.exr"))
        check("colorful_planks : EXR non compressé par défaut (1,6 s au lieu de 2,5 s en zip)", comp == 0, comp)
        # Chaque format du menu, en 256 : le bon fichier, reconnu à sa signature.
        magic = {".exr": (b"\x76\x2f\x31\x01",), ".png": (b"\x89PNG",), ".tif": (b"II*\x00", b"MM\x00*"),
                 ".jpg": (b"\xff\xd8",), ".tga": (b"",)}
        bad = []
        for fmt, f in subzero_render.FORMATS.items():
            out = os.path.join(W, "formats", fmt)
            try:
                files = render_package(PLANKS, size=(8, 8), outputs=["basecolor", "height"], out=out, fmt=fmt)[2]["files"]
                for path in files.values():
                    head = open(path, "rb").read(18)
                    if not path.endswith(f["ext"]) or not any(head.startswith(m) for m in magic[f["ext"]]) \
                            or (f["ext"] == ".tga" and head[2] not in (2, 3)):
                        bad.append("%s: %s" % (fmt, os.path.basename(path)))
            except subzero_render.Error as e:
                bad.append("%s: %s" % (fmt, e))
        check("formats du menu (%s) rendus" % ", ".join(subzero_render.FORMATS), not bad, "; ".join(bad))
        types = exr_pixel_types(os.path.join(W, "formats", "exr32", "height.exr"))
        check("EXR 32f : vraiment en float (--exr-format-use-float)", types == [2], types)
        # La couleur de base d'un EXR a les valeurs du TGA, encodées sRGB (d'où l'espace srgb_tx imposé à l'import).
        import subzero_exr  # noqa: E402
        with subzero_exr.Image(os.path.join(W, "formats", "exr16", "basecolor.exr")) as im:
            tga = open(os.path.join(W, "formats", "tga", "basecolor.tga"), "rb").read()
            start, bpp, top = 18 + tga[0], tga[16] // 8, bool(tga[17] & 0x20)  # BGR(A) non compressé
            gaps = []
            for y in range(0, im.height, 3):
                row = y if top else im.height - 1 - y
                red = im.values(im.line(y), "R")
                for x in range(0, im.width, 3):
                    gaps.append(abs(red[x] * 255 - tga[start + (row * im.width + x) * bpp + 2]))
        # Deux rendus ne sont pas identiques au bit près (jusqu'à 20 codes sur quelques pixels) : on compte.
        close = sum(g < 1.0 for g in gaps) / float(len(gaps))
        check("couleur de base EXR = TGA/255 (encodée sRGB) sur 98 % des pixels au moins, à un code près",
              close >= 0.98, "%.1f %%" % (100 * close))
        # $SUBZERO_EXR_COMPRESSION=zip : la vignette de la fenêtre décode le zip comme le non compressé.
        subzero_render.EXR_COMPRESSION = "zip"
        try:
            zipped = render_package(PLANKS, size=(8, 8), outputs=["basecolor", "height"], out=os.path.join(W, "zip"),
                                    fmt="exr16")[2]["files"]
        finally:
            subzero_render.EXR_COMPRESSION = "none"
        comps = [exr_compression(zipped[out]) for out in ("basecolor", "height")]
        exrmetrics = shutil.which("exrmetrics")
        if not exrmetrics:
            check("EXR en zip (sbsrender) : vignette décodée", comps == [3, 3]
                  and subzero_exr.thumbnail(zipped["basecolor"], 64)[2] == "rgb", comps)
            skip("EXR en zip décodé comme par OpenEXR", "pas d'exrmetrics")
        else:
            # Référence : le même fichier décompressé par OpenEXR (deux rendus ne sont pas identiques au bit près).
            diff = []
            for out in ("basecolor", "height"):
                ref = os.path.join(W, "zip", out + ".none.exr")
                subprocess.run([exrmetrics, "--convert", "-z", "none", zipped[out], "-o", ref],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
                with subzero_exr.Image(zipped[out]) as a, subzero_exr.Image(ref) as b:
                    if [a.line(y) for y in range(a.height)] != [b.line(y) for y in range(b.height)]:
                        diff.append(out)
            check("EXR en zip (sbsrender) décodé par subzero_exr comme par OpenEXR (exrmetrics), au bit près",
                  not diff and comps == [3, 3], "%s, %s" % (diff, comps))
        subzero_render.EXR_COMPRESSION = "piz"
        try:
            subzero_render.format_args("exr16")
            piz = None
        except subzero_render.Error as e:
            piz = str(e)
        subzero_render.EXR_COMPRESSION = "fast"
        try:
            subzero_render.format_args("exr16")
            bad_comp = None
        except subzero_render.Error as e:
            bad_comp = str(e)
        finally:
            subzero_render.EXR_COMPRESSION = "none"
        check("compression EXR inconnue refusée, piz acceptée", piz is None and bad_comp and "fast" in bad_comp, bad_comp)
        prev = os.environ.get("SUBZERO_FORMAT")
        os.environ["SUBZERO_FORMAT"] = "png"
        png = subzero_render.default_format()
        os.environ["SUBZERO_FORMAT"] = "bmp"
        unknown = subzero_render.default_format()
        if prev is None:
            del os.environ["SUBZERO_FORMAT"]
        else:
            os.environ["SUBZERO_FORMAT"] = prev
        check("$SUBZERO_FORMAT : pris s'il est au menu, sinon EXR half", (png, unknown) == ("png", "exr16"), "%s, %s" % (png, unknown))
        # Valeurs, preset et graine : le rendu change ; une valeur hors graphe est refusée avant le lancement.
        before = open(os.path.join(dest, "base.exr"), "rb").read()
        render_package(PLANKS, outputs=["basecolor"], out=dest, names={"basecolor": "base"}, fmt="exr16",
                       values={"wood_color": [0.9, 0.1, 0.1], "$randomseed": 7})
        check("colorful_planks : valeur et graine appliquées", open(os.path.join(dest, "base.exr"), "rb").read() != before)
        try:
            render_package(PLANKS, outputs=["basecolor"], out=dest, values={"nope": 1})
            check("entrée inconnue refusée", False, "acceptée")
        except subzero_render.Error as e:
            check("entrée inconnue refusée", "unknown input nope" in str(e), str(e))
        presets = [p["label"] for p in g["presets"]]
        if presets:
            res = render_package(PLANKS, size=(9, 9), outputs=["basecolor"], out=os.path.join(W, "preset"),
                                 preset=presets[0])[2]
            check("preset « %s » rendu" % presets[0], "--use-preset" in res["argv"][0])
        # Aperçu 512 : médiane de 3, seulement signalée (dépend de la charge du poste).
        ts = []
        for _ in range(3):
            ts.append(render_package(PLANKS, size=(9, 9), outputs=six, out=os.path.join(W, "preview"))[2]["seconds"])
        print("info aperçu 512, 6 sorties : %.2f s (médiane de 3)" % sorted(ts)[1])
        # Annulation : le processus est tué pendant le rendu, et la destination reste telle quelle.
        holder = {}
        timer = threading.Timer(0.4, lambda: subzero_render.kill(holder["proc"]))
        timer.start()
        t = time.time()
        try:
            render_package(PLANKS, size=(12, 12), outputs=six, out=dest, fmt="exr16",
                           cancel=lambda p: holder.update(proc=p))
            check("annulation", False, "rendu terminé")
        except subzero_render.Error as e:
            check("annulation : rendu tué en %.2f s, destination intacte" % (time.time() - t),
                  "cancelled" in str(e) and sorted(os.listdir(dest)) == names, str(e))
        timer.cancel()

    if not os.path.isfile(PLANKS_SBS):
        skip("colorful_planks.sbs", "absent de %s" % SAMPLES)
    elif TOOLS["sbscooker"] is None:
        skip("colorful_planks.sbs", "pas de sbscooker à côté de sbsrender")
    else:
        pkg, g, res = render_package(PLANKS_SBS, outputs=["basecolor", "normal"], out=os.path.join(W, "cooked"))
        check("colorful_planks.sbs cuit (%.2f s) puis rendu (%.2f s)" % (pkg["cook"]["seconds"], res["seconds"]),
              not pkg["cook"]["cached"] and len(res["files"]) == 2)
        again = subzero_render.open_package(PLANKS_SBS, TOOLS)
        check("deuxième ouverture du .sbs : cuisson reprise du cache", again["cook"]["cached"]
              and again["sbsar"] == pkg["sbsar"])
        broken = os.path.join(W, "broken.sbs")
        with open(broken, "w") as f:
            f.write("<package><notasbs/>")
        try:
            subzero_render.open_package(broken, TOOLS)
            check(".sbs illisible refusé", False, "accepté")
        except subzero_render.Error as e:
            check(".sbs illisible refusé", "cooking broken.sbs failed" in str(e), str(e))

print("== %s" % ("OK" if not FAILED else "ÉCHEC : " + ", ".join(FAILED)))
sys.exit(1 if FAILED else 0)
