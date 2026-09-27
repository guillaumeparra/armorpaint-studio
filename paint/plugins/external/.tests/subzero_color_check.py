#!/usr/bin/env python3
"""Gestion couleur de subzero (lot E11), hors de l'app et sans sbsrender. studio_env.py : lecture comparée à celle de
studio_env.c (env_test --print, compilé avec cc), valeurs réécrites par quote() relues à l'identique, update() et
store() sur deux fichiers. subzero_color.py : espace de chaque carte (défauts, variable de groupe, de carte, rôle,
alias, nom inconnu), espace de choix des couleurs, ArmorPaint (configuration, OCIO coupé) ou autonome (OCIO,
STUDIO_OCIO_*), transformations d'affichage ; conversion de fichiers EXR (half, float ; none, zips, zip ; 257x131)
comparée à PyOpenColorIO et relue par le lecteur OpenEXR officiel, formats 8 bits (PNG, TGA, TIFF, JPEG ; alpha
gardé), cas sans conversion (même espace, donnée, EXR gris) et compression illisible.

    <python avec PyOpenColorIO et numpy> .tests/subzero_color_check.py [dossier de travail]
    .ocio/venv a PyOpenColorIO, numpy et OpenEXR (lecteur de référence) ; PySide6 en plus pour les formats 8 bits et
    les images de la fenêtre (sinon : sautés).
"""
import os, shutil, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
EXT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(EXT, "assets"))
work = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp())
shutil.rmtree(work, ignore_errors=True)
os.makedirs(work)

import studio_env  # noqa: E402
import subzero_color  # noqa: E402
import subzero_exr  # noqa: E402

FAIL = []


def check(name, cond, detail=""):
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else "  (%s)" % (detail,)))
    if not cond:
        FAIL.append(name)


if subzero_color.MISSING is not None:
    print("PyOpenColorIO and numpy are needed: %s (%s)" % (subzero_color.MISSING, subzero_color.INSTALL_HINT))
    sys.exit(2)

import numpy as np  # noqa: E402
import PyOpenColorIO as OCIO  # noqa: E402

SRGB = "sRGB Encoded Rec.709 (sRGB)"
BUILTIN = "ocio://studio-config-latest"
for var in subzero_color.HOST_VARS + ("SUBZERO_OCIO_COLOR", "SUBZERO_OCIO_DATA", "SUBZERO_OCIO_PICKING"):
    os.environ.pop(var, None)

# ---- studio_env.py contre studio_env.c --------------------------------------------------------------------------------

os.environ["ENVX_REF"] = "ref"
tricky = ["", "simple", "with space", "it's", 'dq "x" $y \\ z', "~/not/expanded", "a#b", "a #b", "$HOME", SRGB,
          "ACES 2.0 - SDR 100 nits (Rec.709)", BUILTIN, "/path/with:colon", "tab\there", "trail ", "=eq", "back\\slash"]
lines = [
    "# comment", "", "ENVX_PLAIN=hello", "export ENVX_EXPORT=yes", "  ENVX_SPACES =  padded value   # comment",
    "ENVX_HASH=a#b", "ENVX_SQ='it is $HOME # not a comment'", 'ENVX_DQ="say \\"hi\\" $ENVX_REF \\$literal \\\\ back"',
    "ENVX_BRACE=${ENVX_REF}/x", "ENVX_TILDE=~/lib", "ENVX_TILDE_ALONE=~", "ENVX_NOT_TILDE=a~b", "ENVX_EMPTY=",
    'ENVX_EMPTYQ=""', "ENVX_CR=value\r", "ENVX_BAD='open", "not a line", "ENVX_DUP=first", "ENVX_DUP=second",
] + ["ENVX_Q%d=%s" % (i, studio_env.quote(v)) for i, v in enumerate(tricky)]
env_path = os.path.join(work, "syntax.env")
with open(env_path, "w", newline="") as f:
    f.write("\n".join(lines) + "\n")
names = sorted({l.split("=")[0].replace("export ", "").strip() for l in lines if "=" in l and not l.startswith("#")})
py = {}
for line in studio_env.read(env_path):
    py.setdefault(line.name, line.value)
check("quote() : chaque valeur relue à l'identique par studio_env.py",
      all(py.get("ENVX_Q%d" % i) == v for i, v in enumerate(tricky)),
      [(v, py.get("ENVX_Q%d" % i)) for i, v in enumerate(tricky) if py.get("ENVX_Q%d" % i) != v])
check("lecture Python : export, espaces, commentaire, guillemets, $VAR, ~, CRLF, ligne invalide sautée, doublon",
      py.get("ENVX_EXPORT") == "yes" and py.get("ENVX_SPACES") == "padded value" and py.get("ENVX_HASH") == "a#b"
      and py.get("ENVX_SQ") == "it is $HOME # not a comment" and py.get("ENVX_DQ") == 'say "hi" ref $literal \\ back'
      and py.get("ENVX_BRACE") == "ref/x" and py.get("ENVX_TILDE") == os.path.expanduser("~") + "/lib"
      and py.get("ENVX_NOT_TILDE") == "a~b" and py.get("ENVX_CR") == "value" and "ENVX_BAD" not in py
      and py.get("ENVX_DUP") == "first", py)
cc = shutil.which("cc")
if cc:
    exe = os.path.join(work, "env_test")
    build = subprocess.run([cc, "-std=c11", "-O1", "-D_DEFAULT_SOURCE", os.path.join(HERE, "env_test.c"),
                            os.path.join(EXT, "studio_env.c"), os.path.join(EXT, "studio_fs.c"), "-o", exe],
                           capture_output=True, text=True)
    if build.returncode != 0:
        check("env_test compilé", False, build.stderr[-2000:])
    else:
        out = subprocess.run([exe, "--print", env_path] + names, capture_output=True, text=True)
        c_values = {}
        for row in out.stdout.split("\n"):
            if row:
                name, eq, value = row.partition("=")
                if eq:
                    c_values[name] = value
        diff = {n: (py.get(n), c_values.get(n)) for n in names if py.get(n) != c_values.get(n)}
        check("studio_env.py lit comme studio_env.c (%d variables)" % len(names), out.returncode == 0 and not diff,
              diff or out.stderr)
else:
    print("skip lecteur C : cc absent")

# update() : remplacement (export gardé), doublons retirés, None retire, ajout sous l'en-tête, reste intact.
upd = os.path.join(work, "update.env")
with open(upd, "w") as f:
    f.write("# head\nexport A=1\nB=2\nA=3\n\nC=4\n")
studio_env.update(upd, {"A": "x y", "B": None, "D": "new"})
text = open(upd).read()
check("update() : export gardé, doublon et None retirés, ajout sous l'en-tête", text ==
      "# head\nexport A='x y'\n\nC=4\n\n# subzero settings\nD=new\n", repr(text))
fresh = os.path.join(work, "new", "dir", "studio.env")
studio_env.update(fresh, {"SUBZERO_OCIO_COLOR": "ACEScg"})
check("update() : fichier et dossiers créés", open(fresh).read() == "# subzero settings\nSUBZERO_OCIO_COLOR=ACEScg\n",
      open(fresh).read())

# store() : deux fichiers, le premier (celui de l'artiste) gagne ; "" masque une valeur du second (celui du studio).
mine, studio = os.path.join(work, "mine.env"), os.path.join(work, "studio.env")
with open(mine, "w") as f:
    f.write("SUBZERO_OCIO_PICKING=x\n")
with open(studio, "w") as f:
    f.write("SUBZERO_OCIO_DATA=Raw\nSUBZERO_OCIO_COLOR=ACEScg\n")
desired = {"SUBZERO_OCIO_DATA": "Raw", "SUBZERO_OCIO_COLOR": None, "SUBZERO_OCIO_PICKING": None,
           "SUBZERO_OCIO_NORMAL": "Raw"}
written = studio_env.store(mine, desired, paths=[mine, studio])
got = {k: v for k, (v, _) in studio_env.defined([mine, studio]).items()}
check("store() : rien pour une valeur déjà donnée, \"\" pour masquer celle du studio, ligne retirée pour le défaut",
      written == {"SUBZERO_OCIO_COLOR": "", "SUBZERO_OCIO_PICKING": None, "SUBZERO_OCIO_NORMAL": "Raw"}
      and got == {"SUBZERO_OCIO_COLOR": "", "SUBZERO_OCIO_DATA": "Raw", "SUBZERO_OCIO_NORMAL": "Raw"}, (written, got))
check("store() : une seconde fois, rien à écrire", studio_env.store(mine, desired, paths=[mine, studio]) == {})
color = subzero_color.Color(env=dict(os.environ, **{k: v for k, v in got.items()}))
check("valeur vide = défaut (masque celle du studio)", color.plan([("base", "basecolor")])["base"] == (SRGB, SRGB, None))

# ---- Espaces des cartes -----------------------------------------------------------------------------------------------

chans = [("base", "basecolor"), ("roughness", "roughness"), ("normal", "normal")]
c = subzero_color.Color(env={})
check("autonome sans réglage : configuration intégrée, aucune conversion", c.ready and c.origin == "default"
      and c.plan(chans) == {"base": (SRGB, SRGB, None), "roughness": ("Raw", "Raw", None), "normal": ("Raw", "Raw", None)},
      (c.error, c.plan(chans)))
check("rôles et alias : srgb_tx, scene_linear, data", c.resolve("srgb_tx") == SRGB and c.resolve("scene_linear") == "ACEScg"
      and c.resolve("data") == "Raw" and c.resolve("nope") is None)
check("encodages : ACEScg scene-linear, Raw data", c.encoding("ACEScg") == "scene-linear" and c.encoding("Raw") == "data")
check("libellé d'un rôle", c.describe("scene_linear") == "scene_linear → ACEScg" and c.describe("ACEScg") == "ACEScg")
c = subzero_color.Color(env={"SUBZERO_OCIO_COLOR": "scene_linear", "SUBZERO_OCIO_DATA": "Nope",
                             "SUBZERO_OCIO_ROUGHNESS": "data", "SUBZERO_OCIO_BASECOLOR": " "})
plan = c.plan(chans)
check("variable de groupe (rôle), de carte, vide = défaut, nom inconnu", plan == {
    "base": (SRGB, "ACEScg", None), "roughness": ("Raw", "Raw", None),
    "normal": ("Raw", "Raw", "unknown color space Nope ($SUBZERO_OCIO_DATA)")}, plan)
c = subzero_color.Color(env={"SUBZERO_OCIO_COLOR": "ACEScg", "SUBZERO_OCIO_BASECOLOR": "srgb_tx"})
check("variable de carte avant celle du groupe", c.plan(chans)["base"] == (SRGB, SRGB, None), c.plan(chans))
check("choix des couleurs : rôle color_picking par défaut, sinon $SUBZERO_OCIO_PICKING",
      subzero_color.Color(env={}).picking() == (SRGB, None)
      and subzero_color.Color(env={"SUBZERO_OCIO_PICKING": "ACEScg"}).picking() == ("ACEScg", None)
      and subzero_color.Color(env={"SUBZERO_OCIO_PICKING": "x"}).picking() == (
          SRGB, "unknown color space x ($SUBZERO_OCIO_PICKING)"))

host = {"enabled": True, "config": BUILTIN, "display": "sRGB - Display", "view": "Un-tone-mapped", "look": "",
        "viewport": True}
c = subzero_color.Color(host=host, env={"OCIO": "/nope.ocio", "STUDIO_OCIO_VIEW": "Raw"})
check("ArmorPaint : sa configuration et son affichage, pas ceux de l'environnement", c.ready and c.origin == "ArmorPaint"
      and c.view == "Un-tone-mapped" and c.viewport, (c.error, c.view))
c = subzero_color.Color(host=dict(host, enabled=False, status="texture color spaces are disabled"))
check("ArmorPaint, OCIO coupé : pas de configuration, raison donnée", not c.ready and "disabled" in (c.error or ""),
      c.error)
c = subzero_color.Color(env={"STUDIO_OCIO_VIEW": "Un-tone-mapped", "STUDIO_OCIO_LOOK": "nope"})
check("autonome : vue de l'environnement, affichage par défaut, look inconnu retiré", c.display == "sRGB - Display"
      and c.view == "Un-tone-mapped" and c.look is None and c.viewport and any("nope" in w for w in c.warnings),
      (c.display, c.view, c.look, c.warnings))
check("autonome : STUDIO_OCIO_VIEWPORT=0", not subzero_color.Color(env={"STUDIO_OCIO_VIEWPORT": "0"}).viewport)
bad = subzero_color.Color(env={"OCIO": os.path.join(work, "missing.ocio")})
check("autonome : configuration illisible", not bad.ready and "missing.ocio" in (bad.error or "") and bad.origin == "$OCIO",
      bad.error)

c = subzero_color.Color(env={})
off = subzero_color.Color(env={"STUDIO_OCIO_VIEWPORT": "0"})
check("affichage : donnée telle quelle, couleur par la vue", c.view_processor("Raw", "normal") is None
      and c.view_processor("ACEScg", "roughness") is None and c.view_processor(SRGB, "base") is not None)
check("sans viewport : couleur convertie ramenée en sRGB, sRGB telle quelle, espace inconnu tel quel",
      off.view_processor("ACEScg", "base") is not None and off.view_processor(SRGB, "base") is None
      and off.view_processor("srgb_tx", "base") is None and off.view_processor("nope", "base") is None)
grey = c.view_rgb([0.5, 0.5, 0.5], SRGB)
back = off.view_rgb(off.convert_rgb([0.5, 0.5, 0.5], SRGB, "ACEScg"), "ACEScg")
check("vue ACES : gris 0,5 sRGB montré autrement (%.3f), aller-retour ACEScg sans vue : 0,5" % grey[0],
      abs(grey[0] - 0.5) > 0.02 and max(abs(x - 0.5) for x in back) < 1e-5, (grey, back))
look = subzero_color.Color(env={"STUDIO_OCIO_LOOK": "ACES 1.3 Reference Gamut Compression"})
check("look : processeur construit (pipeline legacy)", look.look and look.display_processor(SRGB) is not None)

# ---- Conversion de fichiers -------------------------------------------------------------------------------------------

try:
    import OpenEXR
except ImportError:
    OpenEXR = None
cfg = OCIO.Config.CreateFromFile(BUILTIN)
proc = cfg.getProcessor(SRGB, "ACEScg").getDefaultCPUProcessor()
c = subzero_color.Color(env={})
rng = np.random.default_rng(7)
W, H = 257, 131
for kind, name in ((subzero_exr.HALF, "half"), (subzero_exr.FLOAT, "float")):
    for compression in (subzero_exr.NONE, subzero_exr.ZIPS, subzero_exr.ZIP):
        cname = subzero_exr.COMPRESSION_NAMES[compression]
        path = os.path.join(work, "base_%s_%s.exr" % (name, cname))
        dtype = subzero_exr.NUMPY_TYPE[kind]
        pl = {ch: rng.random((H, W)).astype(dtype) for ch in ("R", "G", "B")}
        pl["A"] = np.where(rng.random((H, W)) > 0.5, 1.0, 0.25).astype(dtype)
        subzero_exr.write_planes(path, pl, {ch: kind for ch in pl}, compression)
        ok = c.convert_file(path, SRGB, "ACEScg")
        w, h, got, types, comp = subzero_exr.read_planes(path)
        rgb = np.stack([pl[ch].astype(np.float32) for ch in "RGB"], -1).reshape(-1, 3).copy()
        proc.applyRGB(rgb.reshape(-1))
        want = rgb.astype(dtype).astype(np.float32)
        have = np.stack([got[ch].astype(np.float32) for ch in "RGB"], -1).reshape(-1, 3)
        err = float(np.max(np.abs(have - want)))
        check("EXR %s %s : converti comme PyOpenColorIO (écart %.1e), alpha, types et compression gardés" % (name, cname, err),
              ok and err == 0.0 and np.array_equal(got["A"], pl["A"]) and set(types.values()) == {kind}
              and comp == compression and (w, h) == (W, H), (ok, err, comp))
        if OpenEXR is not None:
            with OpenEXR.File(path, separate_channels=True) as f:
                ref = {ch: np.asarray(f.channels()[ch].pixels) for ch in "RGBA"}
            check("EXR %s %s : relu par OpenEXR" % (name, cname),
                  all(np.array_equal(ref[ch].astype(np.float32), got[ch].astype(np.float32)) for ch in "RGBA"))
if OpenEXR is None:
    print("skip lecteur OpenEXR : module absent (.ocio/venv l'a)")

gray = os.path.join(work, "gray.exr")
subzero_exr.write_planes(gray, {"Y": np.full((4, 4), 0.5, np.float16)}, {"Y": subzero_exr.HALF})
before = open(gray, "rb").read()
check("EXR gris : pas une carte couleur, laissé tel quel", not c.convert_file(gray, SRGB, "ACEScg")
      and open(gray, "rb").read() == before)
same = os.path.join(work, "base_half_none.exr")
before = open(same, "rb").read()
check("sans conversion : même espace, alias du même, depuis une donnée", not c.convert_file(same, SRGB, SRGB)
      and not c.convert_file(same, "ACEScg", "ACEScg") and not c.convert_file(same, "Raw", "ACEScg")
      and open(same, "rb").read() == before)
piz = os.path.join(work, "piz.exr")
data = bytearray(open(same, "rb").read())
at = data.find(b"compression\0compression\0") + len(b"compression\0compression\0") + 4
data[at] = 4  # PIZ, que subzero_exr ne lit pas
open(piz, "wb").write(bytes(data))
try:
    c.convert_file(piz, SRGB, "ACEScg")
    check("EXR PIZ : erreur", False)
except subzero_color.Error as e:
    check("EXR PIZ : subzero_color.Error nommant le fichier", "piz.exr" in str(e), e)

try:
    from PySide6 import QtGui
except ImportError:
    QtGui = None
if QtGui is None:
    print("skip formats 8 bits et images de la fenêtre : PySide6 absent")
else:
    app = QtGui.QGuiApplication(["subzero_color_check", "-platform", "offscreen"])
    src = (rng.random((37, 53, 4)) * 255).astype(np.uint8)
    src[..., 3] = np.where(rng.random((37, 53)) > 0.5, 255, 40)
    expect = (src[..., :3].astype(np.float32) / 255.0).reshape(-1, 3).copy()
    proc.applyRGB(expect.reshape(-1))
    expect = (np.clip(expect, 0, 1) * 255 + 0.5).astype(np.uint8).reshape(37, 53, 3)
    # Écart jugé sur les pixels opaques : libtiff rend l'alpha prémultiplié (précision perdue à la lecture, pas à la
    # conversion ; split_alpha retire l'alpha d'une couleur de base avant) ; JPEG : écart moyen, compression avec perte.
    opaque = src[..., 3] == 255
    for ext, alpha, tol in ((".png", True, 1), (".png", False, 1), (".tga", True, 1), (".tga", False, 1),
                            (".tif", True, 1), (".tif", False, 1)):
        path = os.path.join(work, "base_%s%s" % ("a" if alpha else "o", ext))
        image = subzero_color._rgba_qimage(src if alpha else np.dstack([src[..., :3], np.full((37, 53), 255, np.uint8)]),
                                           QtGui.QImage.Format_RGBA8888)
        if not alpha:
            image = image.convertToFormat(QtGui.QImage.Format_RGB888)
        if ext == ".tga":
            subzero_color._write_tga(path, subzero_color._qimage_rgba(image), alpha)
        else:
            image.save(path, None, 100 if ext == ".jpg" else -1)
        ok = c.convert_file(path, SRGB, "ACEScg")
        back = QtGui.QImage(path)
        out = subzero_color._qimage_rgba(back)
        delta = np.abs(out[..., :3].astype(int) - expect.astype(int))
        mask = opaque if alpha else np.ones_like(opaque)
        err = float(delta[mask].mean()) if ext == ".jpg" else int(delta[mask].max())
        if ext == ".tga":
            kept = open(path, "rb").read(18)[16] == (32 if alpha else 24) and \
                np.array_equal(out[..., 3], src[..., 3] if alpha else np.full((37, 53), 255, np.uint8))
        else:
            kept = np.array_equal(out[..., 3], src[..., 3]) if alpha else not back.hasAlphaChannel()
        check("%s %s : converti (écart %s %g/255), alpha %s" % (ext[1:].upper(), "RGBA" if alpha else "RGB",
                                                              "moyen" if ext == ".jpg" else "max", err,
                                                              "gardé" if alpha else "absent"),
              ok and back.size() == image.size() and err <= tol and kept, (ok, err, kept))
    # JPEG : dégradé (le bruit est le pire cas d'une compression avec perte), attendu calculé sur l'image relue.
    yy, xx = np.mgrid[0:64, 0:96]
    smooth = np.dstack([xx * 255 // 95, yy * 255 // 63, (xx + yy) * 255 // 158, np.full(xx.shape, 255)]).astype(np.uint8)
    path = os.path.join(work, "base.jpg")
    subzero_color._rgba_qimage(smooth, QtGui.QImage.Format_RGBA8888).convertToFormat(QtGui.QImage.Format_RGB888).save(
        path, None, 100)
    before = (subzero_color._qimage_rgba(QtGui.QImage(path))[..., :3].astype(np.float32) / 255.0).reshape(-1, 3).copy()
    proc.applyRGB(before.reshape(-1))
    ok = c.convert_file(path, SRGB, "ACEScg")
    back = QtGui.QImage(path)
    err = float(np.abs(subzero_color._qimage_rgba(back)[..., :3].reshape(-1, 3).astype(np.float32)
                       - np.clip(before, 0, 1) * 255).mean())
    check("JPEG : converti (écart moyen %.2f/255, qualité 95), sans alpha" % err, ok and err <= 1.5
          and not back.hasAlphaChannel(), (ok, err))
    img = subzero_color._rgba_qimage(src, QtGui.QImage.Format_RGBA8888)
    shown = subzero_color._qimage_rgba(subzero_color.apply_image(img, c.view_processor(SRGB, "base")))
    check("vignette 8 bits par la vue : alpha gardé, couleurs changées", np.array_equal(shown[..., 3], src[..., 3])
          and not np.array_equal(shown[..., :3], src[..., :3]))
    values = [0.0, 0.5, 1.0, 0.25, 0.75, 2.0]
    flat = subzero_color._qimage_rgba(subzero_color.floats_image(values, 2, 1, None))
    check("vignette flottante sans transformation : 8 bits bornés", flat[0, :, :3].reshape(-1).tolist() == [
        0, 128, 255, 64, 191, 255], flat[0, :, :3].reshape(-1).tolist())

print("== " + ("FAIL: " + ", ".join(FAIL) if FAIL else "OK"))
sys.exit(1 if FAIL else 0)
