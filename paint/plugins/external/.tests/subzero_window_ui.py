#!/usr/bin/env python3
"""Fenêtre subzero (chantier E2, formats E6, couleur E11) contre un faux ArmorPaint : le protocole de subzero.h, sans
l'app, avec le vrai sbsrender. Règles pures (nom, canaux, paramètres techniques, décalques TGA, EXR, PNG et TIFF, fichiers périmés) ;
envoi d'un .sbsar (dossier <projet>_subzero/<Nom>, fichiers <Nom>_<canal>.exr par défaut, recette, liaisons, espace
de couleur de la couleur de base) ; réédition et changement de format (anciens fichiers supprimés, sauf ceux
qu'ArmorPaint garde) ; recette v0 sans format (TGA) ; duplication ; $SUBZERO_FORMAT ; décalque (alpha de la couleur de
base en fichier d'opacité, dans chaque format) ; preset ; .sbs cuit ; projet jamais enregistré ; sbsrender introuvable.
Avec PyOpenColorIO et numpy (lot E11) : espaces des liaisons dans la configuration d'ArmorPaint, couleur de base
convertie en ACEScg et réglage enregistré dans studio.env, texture qui suit le réglage (color_space_was), variable de
l'environnement verrouillée, studio.env relu à l'ouverture, espace inconnu, évènement ocio, nuanciers, sans ArmorPaint
(OCIO, STUDIO_OCIO_*), OCIO coupé dans ArmorPaint, panneau Settings.

    <python avec PySide6> .tests/subzero_window_ui.py [dossier de travail]
    SUBZERO_SBSRENDER=<sbsrender> (défaut : détection dans /Applications) ; SUBZERO_SAMPLES, SUBZERO_SHELF comme
    subzero_check.py.
"""
import glob, json, os, shutil, struct, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "assets")
SCRIPT = os.path.join(ASSETS, "subzero_window.py")
work = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp())
os.makedirs(work, exist_ok=True)
os.environ["SUBZERO_CACHE"] = os.path.join(work, "cache")
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
os.environ.pop("SUBZERO_FORMAT", None)  # format par défaut : EXR half
os.environ["STUDIO_ENV"] = ""  # aucun studio.env, sauf pour les essais qui en donnent un
sys.path.insert(0, ASSETS)
import subzero_color  # noqa: E402
import subzero_exr  # noqa: E402
import subzero_meta  # noqa: E402
import subzero_render  # noqa: E402
import subzero_material as sm  # noqa: E402
import subzero_window as sw  # noqa: E402
from PySide6 import QtGui  # noqa: E402

for var in sm.COLOR_VARS:  # réglages de la machine : sans effet sur le banc
    os.environ.pop(var, None)
HAS_OCIO = subzero_color.MISSING is None
SRGB = "sRGB Encoded Rec.709 (sRGB)"  # srgb_tx dans la configuration intégrée (studio-config-latest)

qt = QtGui.QGuiApplication(["subzero_window_ui", "-platform", "offscreen"])  # QImage, greffons TIFF et JPEG

FAIL = []
SAMPLES = os.environ.get("SUBZERO_SAMPLES") or os.path.expanduser("~/Desktop/code/sbsar")
PLANKS = os.path.join(SAMPLES, "colorful_planks.sbsar")
PLANKS_SBS = os.path.join(SAMPLES, "colorful_planks", "colorful_planks.sbs")
PAINTER = "/Applications/Adobe Substance 3D Painter/Adobe Substance 3D Painter.app/Contents"
SHELF = os.environ.get("SUBZERO_SHELF") or PAINTER
SBSRENDER = os.environ.get("SUBZERO_SBSRENDER", "")


def check(name, cond, detail=""):
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else "  (%s)" % (detail,)))
    if not cond:
        FAIL.append(name)


def run(name, test, state, events=(), reply=None, env=None):
    """Lance la fenêtre en offscreen avec l'essai `test`, répond comme ArmorPaint (plus `reply` aux create et update ;
    envoie `events` juste après l'état) ; rend (résultat, requêtes)."""
    test = dict(test, result=os.path.join(work, name + ".result.json"), screenshot=os.path.join(work, name + ".png"))
    path = os.path.join(work, name + ".test.json")
    with open(path, "w") as f:
        json.dump(test, f)
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", SUBZERO_TEST=path, **(env or {}))
    p = subprocess.Popen([sys.executable, SCRIPT], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env, text=True)
    requests = []
    deadline = time.time() + 60
    watchdog = threading.Timer(120, p.kill)  # fenêtre bloquée : le banc échoue au lieu d'attendre
    watchdog.start()
    for line in p.stdout:
        req = json.loads(line)
        requests.append(req)
        op, args = req.get("op"), req.get("args") or {}
        if op == "state":
            data = state
        elif op in ("create", "update"):
            data = dict({"material": args.get("name") or os.path.basename(args["dir"]), "reloaded": len(args["links"]),
                         "removed": 0, "kept": []}, **(reply or {}))
        else:
            data = {}
        p.stdin.write(json.dumps({"id": req["id"], "ok": True, "data": data}) + "\n")
        if op == "state":
            for event in events:
                p.stdin.write(json.dumps(event) + "\n")
        p.stdin.flush()
        if time.time() > deadline:
            break
    p.stdin.close()
    try:
        p.wait(timeout=20)
    except subprocess.TimeoutExpired:
        p.kill()
    watchdog.cancel()
    result = {}
    if os.path.isfile(test["result"]):
        with open(test["result"]) as f:
            result = json.load(f)
    return result, requests


def state(project="", bits=8, res=1024, sbsrender=None):
    return {"sbsrender": SBSRENDER if sbsrender is None else sbsrender, "project": {"file": project},
            "resolution": [res, res], "bits": bits}


def ops(requests, op):
    return [r for r in requests if r.get("op") == op]


def listing(folder):
    return sorted(f for f in os.listdir(folder) if f != sm.RECIPE)


def exr_channels(path):
    with subzero_exr.Image(path) as im:
        return [c for c, _ in im.channels]


def qt_image(path):
    return QtGui.QImageReader(path).read()


# ---- Règles pures ----------------------------------------------------------------------------------------------------

check("nom d'une ligne", sm.slug("Colorful Planks") == "ColorfulPlanks" and sm.slug("wood_plain-02") == "WoodPlain02"
      and sm.slug("  ") == "Material", sm.slug("wood_plain-02"))
flags = []
for value in (None, "", "0", "off", "No", "1", "yes"):
    if value is None:
        os.environ.pop("SUBZERO_TEST_FLAG", None)
    else:
        os.environ["SUBZERO_TEST_FLAG"] = value
    flags.append(sm.env_flag("SUBZERO_TEST_FLAG", True))
os.environ.pop("SUBZERO_TEST_FLAG", None)
check("variable booléenne (SUBZERO_LIVE) : absente = défaut ; vide, 0, off, no = faux", flags == [True, False, False, False,
      False, True, True], flags)
check("dossier commun aux versions du projet", sm.subzero_dir("/p/robot_v003.arm") == "/p/robot_subzero"
      and sm.subzero_dir("/p/robot.arm") == "/p/robot_subzero", sm.subzero_dir("/p/robot_v003.arm"))
normal_format = {"identifier": "normal_format", "type": "int1", "widget": "combobox", "usertag": {},
                 "items": [(0, "DirectX"), (1, "OpenGL")]}
check("format de normale : OpenGL imposé", sm.technical_value(normal_format) == 1)
check("bascules de canaux", sm.technical_value({"identifier": "channel_basecolor", "widget": "togglebutton"}) == 1
      and sm.technical_value({"identifier": "channel_specular", "widget": "togglebutton"}) == 0
      and sm.technical_value({"identifier": "channel_choice", "widget": "togglebutton"}) is None)
with open(os.path.join(work, "gray.tga"), "wb") as f:  # 2x1, 32 bits, alpha opaque
    f.write(bytes([0, 0, 2] + [0] * 9 + [2, 0, 1, 0, 32, 8]) + bytes([9, 9, 9, 255] * 2))
with open(os.path.join(work, "decal.tga"), "wb") as f:  # origine en haut
    f.write(bytes([0, 0, 2] + [0] * 9 + [2, 0, 1, 0, 32, 0x28]) + bytes([1, 2, 3, 255, 4, 5, 6, 0]))
gray = open(os.path.join(work, "gray.tga"), "rb").read()
check("alpha opaque : couleur laissée telle quelle", not sm.split_alpha(os.path.join(work, "gray.tga"),
                                                                        os.path.join(work, "gray_opacity.tga"))
      and open(os.path.join(work, "gray.tga"), "rb").read() == gray
      and not os.path.exists(os.path.join(work, "gray_opacity.tga")))
check("alpha de décalque : couleur en RGB, alpha en opacité grise, orientation et pied TGA 2.0 gardés",
      sm.split_alpha(os.path.join(work, "decal.tga"), os.path.join(work, "decal_opacity.tga"))
      and open(os.path.join(work, "decal.tga"), "rb").read() == bytes(
          [0, 0, 2] + [0] * 9 + [2, 0, 1, 0, 24, 0x20]) + bytes([1, 2, 3, 4, 5, 6]) + sm.TGA_FOOTER
      and open(os.path.join(work, "decal_opacity.tga"), "rb").read() == bytes(
          [0, 0, 3] + [0] * 9 + [2, 0, 1, 0, 8, 0x28]) + bytes([255, 0]) + sm.TGA_FOOTER)
check("TGA réécrit lu par Qt (pied TGA 2.0)", not qt_image(os.path.join(work, "decal.tga")).isNull()
      if hasattr(QtGui, "QImageReader") else True)
with open(os.path.join(work, "nofooter.tga"), "wb") as f:  # couleur de base réécrite avant E11 : sans pied, origine en bas
    f.write(bytes([0, 0, 2] + [0] * 9 + [2, 0, 2, 0, 24, 0]) + bytes([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]))
thumb = sw.read_tga(os.path.join(work, "nofooter.tga"))
check("vignette d'un TGA sans pied : lu, BGR vers RGB, origine en bas", not thumb.isNull()
      and thumb.pixelColor(0, 0).getRgb()[:3] == (9, 8, 7) and thumb.pixelColor(1, 1).getRgb()[:3] == (6, 5, 4),
      [thumb.pixelColor(x, y).getRgb()[:3] for y in (0, 1) for x in (0, 1)] if not thumb.isNull() else "null")
check("liaisons : couleur de chaque fichier vers son canal, couleur de base en srgb_tx",
      sm.links({"base": "b.exr", "opacity": "o.exr"}) == [
          {"file": "b.exr", "output": 0, "socket": 0, "color_space": "srgb_tx"},
          {"file": "o.exr", "output": 0, "socket": 1}], sm.links({"base": "b.exr", "opacity": "o.exr"}))
check("liaisons : espaces donnés, et ceux du rendu précédent (color_space_was)",
      sm.links({"base": "b.exr", "normal": "n.exr"}, {"base": "ACEScg", "normal": "Raw"}, {"base": "srgb_tx"}) == [
          {"file": "b.exr", "output": 0, "socket": 0, "color_space": "ACEScg", "color_space_was": "srgb_tx"},
          {"file": "n.exr", "output": 0, "socket": 5, "color_space": "Raw"}])
check("espaces d'une recette : notés (E11), couleur de base srgb_tx (E6), aucun (v0)",
      sm.recipe_spaces({"format": "exr16", "color": {"spaces": {"base": "ACEScg"}}}) == {"base": "ACEScg"}
      and sm.recipe_spaces({"format": "png"}) == {"base": "srgb_tx"} and sm.recipe_spaces({}) == {})

# Décalque EXR (A, B, G, R en half, comme sbsrender) : 2x1, le second pixel transparent.
half = lambda *v: struct.pack("<%de" % len(v), *v)
rgba = [("A", subzero_exr.HALF), ("B", subzero_exr.HALF), ("G", subzero_exr.HALF), ("R", subzero_exr.HALF)]
subzero_exr.write(os.path.join(work, "opaque.exr"), 2, 1, rgba, [half(1, 1, .3, .3, .2, .2, .1, .1)])
subzero_exr.write(os.path.join(work, "decal.exr"), 2, 1, rgba, [half(1, 0, .3, .6, .2, .5, .1, .4)])
opaque = open(os.path.join(work, "opaque.exr"), "rb").read()
check("EXR opaque : couleur laissée telle quelle", not sm.split_alpha(os.path.join(work, "opaque.exr"),
                                                                      os.path.join(work, "opaque_opacity.exr"))
      and open(os.path.join(work, "opaque.exr"), "rb").read() == opaque
      and not os.path.exists(os.path.join(work, "opaque_opacity.exr")))
ok = sm.split_alpha(os.path.join(work, "decal.exr"), os.path.join(work, "decal_opacity.exr"))
with subzero_exr.Image(os.path.join(work, "decal.exr")) as im:
    rgb = [c for c, _ in im.channels], [im.values(im.line(0), c) for c in "RGB"]
with subzero_exr.Image(os.path.join(work, "decal_opacity.exr")) as im:
    alpha = [(c, t) for c, t in im.channels], im.values(im.line(0), "Y")
as_half = lambda *v: struct.unpack("<%de" % len(v), half(*v))
check("décalque EXR : couleur en B G R, alpha en Y (half)", ok and rgb == (["B", "G", "R"], [
    as_half(.1, .4), as_half(.2, .5), as_half(.3, .6)])
      and alpha == ([("Y", subzero_exr.HALF)], (1.0, 0.0)), (rgb, alpha))
for ext in ("png", "tif"):
    image = QtGui.QImage(2, 1, QtGui.QImage.Format_ARGB32)
    image.setPixelColor(0, 0, QtGui.QColor(10, 20, 30, 255))
    image.setPixelColor(1, 0, QtGui.QColor(40, 50, 60, 0 if ext == "png" else 128))
    base, opacity = os.path.join(work, "decal." + ext), os.path.join(work, "decal_opacity." + ext)
    image.save(base)
    ok = sm.split_alpha(base, opacity)
    b, o = qt_image(base), qt_image(opacity)
    check("décalque %s : couleur sans alpha, alpha en gris" % ext.upper(), ok and not b.hasAlphaChannel()
          and b.pixelColor(0, 0).getRgb()[:3] == (10, 20, 30) and o.isGrayscale()
          and [o.pixelColor(x, 0).red() for x in (0, 1)] == [255, 0 if ext == "png" else 128],
          (ok, b.hasAlphaChannel(), o.isGrayscale(), [o.pixelColor(x, 0).red() for x in (0, 1)]))
    image.fill(QtGui.QColor(1, 2, 3, 255))
    image.save(base)
    check("%s opaque : laissé tel quel" % ext.upper(), not sm.split_alpha(base, opacity + "2")
          and not os.path.exists(opacity + "2"))
check("décalque JPEG : rien à séparer", not sm.split_alpha(os.path.join(work, "x.jpg"), os.path.join(work, "o.jpg")))

stale = os.path.join(work, "stale", "Wood")
shutil.rmtree(os.path.dirname(stale), ignore_errors=True)
os.makedirs(stale)
for n in ("Wood_basecolor.tga", "Wood_basecolor.exr", "Wood_normal.png", "Wood_opacity.tif", "Wood_normal.jpg",
          "Wood_custom.exr", "Other_basecolor.tga", "notes.txt", sm.RECIPE):
    open(os.path.join(stale, n), "w").close()
found = sorted(os.path.basename(p) for p in sm.stale_files(stale, [os.path.join(stale, "Wood_basecolor.exr"),
                                                                     os.path.join(stale, "Wood_normal.jpg")]))
check("fichiers périmés : <Nom>_<canal>.<ext> hors du rendu, rien d'autre", found == [
    "Wood_basecolor.tga", "Wood_normal.png", "Wood_opacity.tif"], found)

if not os.path.isfile(PLANKS):
    print("skip fenêtre : %s absent" % PLANKS)
    sys.exit(1 if FAIL else 0)
try:
    subzero_render.locate(SBSRENDER)
except subzero_render.Error as e:
    print("skip fenêtre : %s" % e)
    sys.exit(1 if FAIL else 0)

planks = subzero_render.find_graph(subzero_meta.read_sbsar(PLANKS))
mapping = {o["identifier"]: ch for o, ch, _ in sm.auto_mapping(planks, {})}
check("sorties de colorful_planks vers les canaux (diffuse, spéculaire, brillance masquées par channel_*)", mapping == {
    "basecolor": "base", "normal": "normal", "roughness": "roughness", "metallic": "metallic", "height": "height",
    "ambientocclusion": "occlusion"}, mapping)
shown_all = {o["identifier"]: (ch, why) for o, ch, why in sm.auto_mapping(dict(planks, outputs=[
    dict(o, visibleif=None) for o in planks["outputs"]]), {})}
check("sorties non représentables montrées sans canal, avec la raison", shown_all.get("glossiness", (1, ""))[0] is None
      and "glossiness" in shown_all["glossiness"][1] and shown_all.get("diffuse", (1,))[0] is None, shown_all)
shelf = sorted(glob.glob(os.path.join(SHELF, "Resources/starter_assets/materials/**/*.sbsar"), recursive=True))
bad = []
for path in shelf:
    g = subzero_render.find_graph(subzero_meta.read_sbsar(path))
    rows = sm.auto_mapping(g, {})
    chans = [ch for _, ch, _ in rows if ch]
    colour = any(u.lower() in ("basecolor", "diffuse") for o, _, _ in rows for u in o.get("usages") or [])
    if len(chans) != len(set(chans)) or any(not ch and not why for _, ch, why in rows) or colour != ("base" in chans):
        bad.append((os.path.basename(path), [(o["identifier"], ch, why) for o, ch, why in rows]))
if shelf:
    check("étagère (%d matériaux) : un canal par sortie au plus, sinon une raison ; couleur de base dès qu'une sortie "
          "en a l'usage" % len(shelf), not bad, bad[:3])
else:
    print("skip étagère : %s absent" % SHELF)
shown = [i["identifier"] for i in planks["inputs"] if sm.shown(i)]
check("paramètres montrés : ni système, ni canaux, ni format de normale",
      "wood_color" in shown and "age" in shown and "contrast" in shown and not any(
          s.startswith(("$", "channel_")) or s == "normal_format" for s in shown), shown)

# ---- Envoi -----------------------------------------------------------------------------------------------------------

project = os.path.join(work, "robot_v003.arm")
folder = os.path.join(work, "robot_subzero", "ColorfulPlanks")
shutil.rmtree(os.path.join(work, "robot_subzero"), ignore_errors=True)
t0 = time.time()
res, reqs = run("create", {"open": {"file": PLANKS}, "action": "create"}, state(project))
took = time.time() - t0
create = ops(reqs, "create")
check("un create", len(create) == 1, [r.get("op") for r in reqs])
if create:
    a = create[0]["args"]
    files = sorted(os.path.basename(l["file"]) for l in a["links"])
    check("dossier <projet>_subzero/<Nom>", a["dir"] == folder, a["dir"])
    check("nom du matériau", a.get("name") == "Colorful Planks", a.get("name"))
    check("6 textures nommées <Nom>_<canal>.exr (EXR half par défaut)", files == sorted(
        "ColorfulPlanks_%s.exr" % c for c in ("basecolor", "normal", "roughness", "metallic", "height", "occlusion")),
        files)
    check("sockets", sorted(l["socket"] for l in a["links"]) == [0, 2, 3, 4, 5, 7], a["links"])
    if HAS_OCIO:  # sans ArmorPaint : configuration par défaut, sans conversion
        check("espaces des liaisons : couleur de base %s, données Raw" % SRGB,
              [l.get("color_space") for l in a["links"] if l["socket"] == 0] == [SRGB]
              and all(l.get("color_space") == "Raw" for l in a["links"] if l["socket"] != 0), a["links"])
    else:
        check("couleur de base en srgb_tx, les autres au choix d'ArmorPaint (sans PyOpenColorIO)",
              [l.get("color_space") for l in a["links"] if l["socket"] == 0] == ["srgb_tx"]
              and not any("color_space" in l for l in a["links"] if l["socket"] != 0), a["links"])
    check("textures écrites à la résolution du projet", all(os.path.isfile(l["file"]) for l in a["links"]))
    with subzero_exr.Image(os.path.join(folder, "ColorfulPlanks_basecolor.exr")) as im:
        check("1024 px, half", (im.width, [t for _, t in im.channels]) == (1024, [1, 1, 1, 1]), (im.width, im.channels))
recipe = sm.read_recipe(folder) if os.path.isfile(os.path.join(folder, sm.RECIPE)) else {}
check("recette", recipe.get("graph") == planks["pkgurl"] and recipe.get("package", {}).get("sha256")
      and recipe.get("files", {}).get("base") == "ColorfulPlanks_basecolor.exr" and recipe.get("format") == "exr16",
      recipe)
renders = res.get("renders") or []
final = [r for r in res.get("requests") or [] if r["action"] == "create"]
print("     %.2f s en tout, rendu final %.2f s" % (took, final[0]["render"] if final else -1))
check("rendu final en moins de 2,5 s", final and final[0]["render"] < 2.5, final and final[0]["render"])

# ---- Réédition et mise à jour -----------------------------------------------------------------------------------------

normal = os.path.join(folder, "ColorfulPlanks_normal.exr")
res, reqs = run("update", {"open": {"dir": folder}, "values": {"age": 0.9, "$randomseed": 7}, "format": "png",
                           "action": "update"}, state(project), reply={"removed": 5, "kept": [normal]})
update = ops(reqs, "update")
check("un update sur le même dossier", len(update) == 1 and update[0]["args"]["dir"] == folder and not ops(reqs, "create"),
      [r.get("op") for r in reqs])
recipe = sm.read_recipe(folder)
check("recette mise à jour", recipe["values"].get("age") == 0.9 and recipe["values"].get("$randomseed") == 7
      and recipe.get("format") == "png", recipe)
check("réédition : nom gardé", res.get("material_name") == "Colorful Planks", res.get("material_name"))
pngs = ["ColorfulPlanks_%s.png" % c for c in ("basecolor", "height", "metallic", "normal", "occlusion", "roughness")]
final = [r for r in res.get("requests") or [] if r["action"] == "update"]
check("passage en PNG : liaisons en .png", update and sorted(os.path.basename(l["file"]) for l in update[0]["args"]["links"])
      == pngs, update and update[0]["args"]["links"])
check("passage en PNG : anciens EXR supprimés après la réponse, sauf celui qu'ArmorPaint garde (kept)",
      listing(folder) == sorted(pngs + ["ColorfulPlanks_normal.exr"])
      and final and len(final[-1].get("deleted") or []) == 5, (listing(folder), final and final[-1].get("deleted")))
image = qt_image(os.path.join(folder, "ColorfulPlanks_basecolor.png"))
check("PNG 8 bits à la résolution du projet", image.width() == 1024 and image.depth() in (24, 32), (image.width(),
                                                                                                     image.depth()))

del recipe["format"]  # recette v0 (lots E0-E4) : TGA
sm.write_recipe(folder, recipe)
res, reqs = run("v0", {"open": {"dir": folder}, "size": 8, "action": "update"}, state(project))
update = ops(reqs, "update")
tgas = [p.replace(".png", ".tga") for p in pngs]
check("recette sans format (v0) : rééditée en TGA, PNG et EXR gardé supprimés",
      update and listing(folder) == tgas and sm.read_recipe(folder).get("format") == "tga", listing(folder))

res, reqs = run("duplicate", {"open": {"dir": folder}, "action": "duplicate"}, state(project))
create = ops(reqs, "create")
check("duplication dans ColorfulPlanks2, au format de la recette", len(create) == 1
      and create[0]["args"]["dir"] == folder + "2"
      and os.path.isfile(os.path.join(folder + "2", "ColorfulPlanks2_basecolor.tga")), create and create[0]["args"]["dir"])

# ---- $SUBZERO_FORMAT, EXR 32f, preset --------------------------------------------------------------------------------

deep = os.path.join(work, "deep.arm")
shutil.rmtree(os.path.join(work, "deep_subzero"), ignore_errors=True)
res, reqs = run("tif", {"open": {"file": PLANKS}, "size": 8, "action": "create"}, state(deep, bits=16),
                env={"SUBZERO_FORMAT": "tif"})
create = ops(reqs, "create")
exts = {os.path.splitext(l["file"])[1] for l in create[0]["args"]["links"]} if create else set()
check("$SUBZERO_FORMAT=tif : paquet neuf rendu en TIFF", exts == {".tif"}, exts)
res, reqs = run("exr32", {"open": {"file": PLANKS}, "size": 8, "format": "exr32", "action": "create"}, state(deep))
create = ops(reqs, "create")
base = next((l["file"] for l in create[0]["args"]["links"] if l["socket"] == 0), "") if create else ""
check("EXR 32f : float", base.endswith(".exr") and subzero_exr.Image(base).channels[0][1] == subzero_exr.FLOAT, base)

presets = [p["label"] for p in planks.get("presets") or []]
if presets:
    res, reqs = run("preset", {"open": {"file": PLANKS}, "preset": presets[-1], "size": 8, "action": "create"},
                    state(deep))
    create = ops(reqs, "create")
    check("preset : nom du matériau", create and create[0]["args"]["name"] == presets[-1], create and create[0]["args"])

# ---- Décalque ---------------------------------------------------------------------------------------------------------

decal = glob.glob(os.path.join(SHELF, "Resources/starter_assets/materials/**/footprints.sbsar"), recursive=True)
if decal:
    for fmt in ("exr16", "png", "tga", "jpg"):
        res, reqs = run("decal_" + fmt, {"open": {"file": decal[0]}, "size": 8, "format": fmt, "action": "create"},
                        state(os.path.join(work, "decal_%s.arm" % fmt)))
        create = ops(reqs, "create")
        by_socket = {l["socket"]: l for l in create[0]["args"]["links"]} if create else {}
        base, opacity = by_socket.get(0, {}).get("file", ""), by_socket.get(1, {}).get("file", "")
        ext = subzero_render.FORMATS[fmt]["ext"]
        if fmt == "jpg":
            check("décalque JPEG : pas d'alpha, pas d'opacité", base.endswith(ext) and not opacity, by_socket)
            continue
        if fmt == "exr16":
            color, varied = bool(base) and exr_channels(base) == ["B", "G", "R"], False
            if opacity:
                with subzero_exr.Image(opacity) as im:
                    varied = im.has("Y") and len(im.channels) == 1 \
                        and len(set(im.values(im.line(im.height // 2), "Y"))) > 1
        elif fmt == "tga":
            color = open(base, "rb").read(18)[16] == 24 if base else False
            varied = opacity and open(opacity, "rb").read(18)[16] == 8 \
                and len(set(open(opacity, "rb").read()[18:18 + 256 * 256])) > 1
        else:
            b, o = qt_image(base), qt_image(opacity)
            color = not b.isNull() and not b.hasAlphaChannel()
            varied = not o.isNull() and o.isGrayscale() and len({o.pixelColor(x, 128).red() for x in range(256)}) > 1
        check("décalque %s : alpha de la couleur de base dans <Nom>_opacity%s, vers l'opacité ; couleur sans alpha"
              % (subzero_render.FORMATS[fmt]["label"], ext), opacity.endswith("_opacity" + ext) and color and varied
              and all(l["output"] == 0 for l in by_socket.values()), by_socket)
else:
    print("skip décalque : footprints.sbsar absent de %s" % SHELF)

# ---- Couleur (lot E11) -----------------------------------------------------------------------------------------------


def host(enabled=True, viewport=True, look=""):
    return {"enabled": enabled, "status": "ok" if enabled else "OCIO is off", "config": "ocio://studio-config-latest",
            "name": "studio-config-v4.0.0_aces-v2.0_ocio-v2.5", "origin": "default", "version": "2.5.0",
            "working_space": "ACEScg", "display": "sRGB - Display", "view": "ACES 2.0 - SDR 100 nits (Rec.709)",
            "look": look, "viewport": viewport}


def host_state(project, env_file="", origins=None, ocio=None, **kw):
    return dict(state(project, **kw), ocio=ocio or host(), env={"file": env_file, "origins": origins or {}})


def by_socket(req):
    return {l["socket"]: l for l in req["args"]["links"]}


def planes(path):
    import numpy as np
    w, h, p, _, _ = subzero_exr.read_planes(path)
    return np.stack([p[c].astype(np.float32) for c in ("R", "G", "B")], -1).reshape(-1, 3)


if not HAS_OCIO:
    print("skip couleur (E11) : %s (%s)" % (subzero_color.MISSING, subzero_color.INSTALL_HINT))
else:
    import numpy as np
    import PyOpenColorIO as OCIO
    e11 = os.path.join(work, "e11")
    shutil.rmtree(e11, ignore_errors=True)
    os.makedirs(e11)
    env_file = os.path.join(e11, "studio.env")
    with open(env_file, "w") as f:
        f.write("# studio\nexport SUBZERO_EXR_COMPRESSION=none\n")

    # Défaut, dans la configuration d'ArmorPaint : aucune conversion, espaces nommés comme dans la configuration.
    res, reqs = run("ocio_default", {"open": {"file": PLANKS}, "size": 8, "action": "create"},
                    host_state(os.path.join(e11, "plain.arm"), env_file), env={"STUDIO_ENV": env_file})
    create = ops(reqs, "create")
    links = by_socket(create[0]) if create else {}
    check("E11 défaut : couleur de base %s, données Raw, sans color_space_was" % SRGB,
          links.get(0, {}).get("color_space") == SRGB and all(l.get("color_space") == "Raw" for k, l in links.items() if k)
          and not any("color_space_was" in l for l in links.values()), links)
    plain = res.get("color") or {}
    check("E11 défaut : plan sans conversion, configuration d'ArmorPaint", plain.get("origin") == "ArmorPaint"
          and plain.get("plan", {}).get("base") == [SRGB, SRGB, None] and plain.get("plan", {}).get("normal")
          == ["Raw", "Raw", None], plain)

    # Couleur de base en ACEScg, enregistré dans studio.env (autre ligne gardée).
    res, reqs = run("ocio_acescg", {"open": {"file": PLANKS}, "size": 8, "action": "create",
                                    "settings": {"SUBZERO_OCIO_COLOR": "ACEScg", "save": True}},
                    host_state(os.path.join(e11, "aces.arm"), env_file), env={"STUDIO_ENV": env_file})
    create = ops(reqs, "create")
    links = by_socket(create[0]) if create else {}
    text = open(env_file).read()
    check("E11 ACEScg : réglage écrit dans studio.env, le reste gardé", "SUBZERO_OCIO_COLOR=ACEScg" in text
          and "export SUBZERO_EXR_COMPRESSION=none" in text and not res.get("settings_error"), (text, res.get("settings_error")))
    check("E11 ACEScg : liaison de la couleur de base en ACEScg", links.get(0, {}).get("color_space") == "ACEScg", links)
    aces_dir = create[0]["args"]["dir"] if create else ""
    plain_dir = os.path.join(e11, "plain_subzero", "ColorfulPlanks")
    try:
        cfg = OCIO.Config.CreateFromFile("ocio://studio-config-latest")
        proc = cfg.getProcessor(SRGB, "ACEScg").getDefaultCPUProcessor()
        src = planes(os.path.join(plain_dir, "ColorfulPlanks_basecolor.exr"))
        want = src.copy()
        proc.applyRGB(want.reshape(-1))
        got = planes(os.path.join(aces_dir, "ColorfulPlanks_basecolor.exr"))
        # Deux rendus de sbsrender (Metal) diffèrent de quelques pixels d'un pas de half : l'écart est jugé sur le
        # reste. Précision : half, arrondi une fois de plus.
        rel = np.max(np.abs(got - want) / np.maximum(np.abs(want), 1e-2), axis=1)
        off = int(np.count_nonzero(rel > 2e-3))
        check("E11 ACEScg : pixels = PyOpenColorIO srgb_tx -> ACEScg (%d pixels sur %d à plus de 2e-3 relatif)"
              % (off, len(rel)), off <= len(rel) // 1000, float(np.max(rel)))
        check("E11 ACEScg : la conversion change bien les pixels", float(np.max(np.abs(got - src))) > 0.01)
    except (OSError, subzero_exr.Error, KeyError) as e:
        check("E11 ACEScg : fichiers lus", False, e)
    final = [r for r in res.get("renders") or [] if r["kind"] == "final"]
    check("E11 ACEScg : seule la couleur de base réécrite, les cartes de donnée telles que sbsrender les écrit",
          final and final[-1].get("converted") == ["base"], final and final[-1].get("converted"))
    recipe = sm.read_recipe(aces_dir) if aces_dir else {}
    check("E11 ACEScg : recette avec configuration et espaces", (recipe.get("color") or {}).get("spaces", {}).get("base")
          == "ACEScg" and recipe["color"].get("config") == "ocio://studio-config-latest", recipe.get("color"))
    check("E11 ACEScg : vignette vue depuis ACEScg", ((res.get("color") or {}).get("thumbs") or {}).get("base") == "ACEScg",
          res.get("color"))

    # Réédition : ArmorPaint a démarré avant l'enregistrement (son environnement ne l'a pas) ; la fenêtre relit
    # studio.env, puis le réglage retiré ramène la couleur de base en sRGB, que la texture suit (color_space_was).
    res, reqs = run("ocio_follow", {"open": {"dir": aces_dir}, "action": "update",
                                    "settings": {"SUBZERO_OCIO_COLOR": None, "save": True}},
                    host_state(os.path.join(e11, "aces.arm"), env_file), env={"STUDIO_ENV": env_file})
    first = (res.get("settings") or [{}])[0]
    updates = ops(reqs, "update")
    last = by_socket(updates[-1]) if updates else {}
    check("E11 suivi : studio.env relu à l'ouverture (ACEScg), puis réglage retiré du fichier",
          "SUBZERO_OCIO_COLOR" not in open(env_file).read() and first.get("desired") == {"SUBZERO_OCIO_COLOR": None},
          open(env_file).read())
    check("E11 suivi : couleur de base de nouveau en %s, avec color_space_was ACEScg" % SRGB,
          last.get(0, {}).get("color_space") == SRGB and any(
              by_socket(u).get(0, {}).get("color_space_was") == "ACEScg" for u in updates), [u["args"]["links"] for u in updates])
    check("E11 suivi : recette revenue en sRGB", sm.read_recipe(aces_dir).get("color", {}).get("spaces", {}).get("base")
          == SRGB)

    # Variable de l'environnement : verrouillée ; studio.env relu (variable venue d'un fichier qui ne l'a plus :
    # retirée) ; espace inconnu signalé ; panneau Settings.
    with open(env_file, "a") as f:
        f.write("SUBZERO_OCIO_DATA=data\n")
    before = open(env_file).read()
    res, reqs = run("ocio_locked", {"action": "none", "settings_screenshot": os.path.join(work, "settings_host.png"),
                                    "settings": {"SUBZERO_OCIO_COLOR": None, "SUBZERO_OCIO_NORMAL": "NoSuchSpace",
                                                 "save": False}},
                    host_state("", env_file, origins={"SUBZERO_OCIO_COLOR": "environment", "SUBZERO_OCIO_PICKING": env_file}),
                    env={"STUDIO_ENV": env_file, "SUBZERO_OCIO_COLOR": "scene_linear", "SUBZERO_OCIO_PICKING": "ACEScg"})
    c = res.get("color") or {}
    plan = c.get("plan") or {}
    check("E11 verrou : variable de l'environnement gardée (scene_linear -> ACEScg)", plan.get("base") == [SRGB, "ACEScg", None]
          and c.get("locked") == ["SUBZERO_OCIO_COLOR"] and open(env_file).read() == before, c)
    check("E11 relecture : variable d'un fichier qui ne l'a plus retirée, celle du fichier prise",
          "SUBZERO_OCIO_PICKING" not in (c.get("env") or {}) and c.get("origins", {}).get("SUBZERO_OCIO_DATA") == env_file
          and plan.get("roughness") == ["Raw", "Raw", None], c)
    check("E11 espace inconnu : erreur, carte laissée en Raw", plan.get("normal") == [
        "Raw", "Raw", "unknown color space NoSuchSpace ($SUBZERO_OCIO_NORMAL)"], plan.get("normal"))
    panel = res.get("settings_panel") or {}
    check("E11 panneau (ArmorPaint) : variables de subzero, verrouillée non modifiable, erreur au bandeau",
          set(panel.get("fields") or []) == set(sm.SUBZERO_VARS) and "SUBZERO_OCIO_COLOR" not in panel.get("editable", [])
          and "NoSuchSpace" in panel.get("banner", ""), panel)

    # Nuanciers : par la transformation d'affichage du viewport ; évènement ocio (viewport coupé) : tels quels.
    colors = [i for i in planks["inputs"] if i.get("widget") == "color" and sm.shown(i)]
    res_on, _ = run("swatch_on", {"open": {"file": PLANKS}, "action": "none"}, host_state(""))
    res_off, _ = run("swatch_off", {"open": {"file": PLANKS}, "action": "none"}, host_state(""),
                     events=[{"event": "ocio", "state": {"ocio": host(viewport=False)}}])
    check("E11 évènement ocio : viewport coupé", (res_on.get("color") or {}).get("viewport") is True
          and (res_off.get("color") or {}).get("viewport") is False)
    if colors:
        ident = colors[0]["identifier"]
        raw = QtGui.QColor.fromRgbF(*[max(0.0, min(1.0, float(x))) for x in colors[0]["default"][:3]]).name()
        on, off = (r.get("color", {}).get("swatches", {}).get(ident) for r in (res_on, res_off))
        check("E11 nuancier %s : vue ACES avec le viewport (%s), valeur telle quelle sans (%s)" % (ident, on, off),
              off == raw and on and on != raw, (on, off, raw))
    else:
        print("skip nuanciers : pas de couleur dans colorful_planks")

    # Sans ArmorPaint : OCIO et STUDIO_OCIO_* de l'environnement, verrouillées ; pas de studio.env.
    res, reqs = run("standalone", {"action": "none", "settings_screenshot": os.path.join(work, "settings_alone.png"),
                                   "settings": {"SUBZERO_OCIO_DATA": "Raw", "save": True}}, state(""),
                    env={"STUDIO_OCIO_DISPLAY": "Nope", "STUDIO_OCIO_LOOK": "ACES 1.3 Reference Gamut Compression"})
    c = res.get("color") or {}
    panel = res.get("settings_panel") or {}
    check("E11 autonome : affichage inconnu remplacé par le défaut, look pris", c.get("origin") == "default"
          and c.get("display") == "sRGB - Display" and c.get("look") == "ACES 1.3 Reference Gamut Compression"
          and any("Nope" in w for w in c.get("warnings") or []), c)
    check("E11 autonome : $STUDIO_ENV vide, rien à enregistrer", "STUDIO_ENV is empty" in (res.get("settings_error") or ""),
          res.get("settings_error"))
    check("E11 autonome : panneau avec OCIO et STUDIO_OCIO_*, celles de l'environnement verrouillées",
          set(panel.get("fields") or []) == set(sm.COLOR_VARS) and "STUDIO_OCIO_DISPLAY" not in panel.get("editable", [])
          and "OCIO" in panel.get("editable", []), panel)

    # OCIO coupé dans ArmorPaint : comme sans PyOpenColorIO (lot E6).
    res, reqs = run("ocio_off", {"open": {"file": PLANKS}, "size": 8, "action": "create"},
                    host_state(os.path.join(e11, "off.arm"), ocio=host(enabled=False)))
    create = ops(reqs, "create")
    links = by_socket(create[0]) if create else {}
    check("E11 OCIO coupé : couleur de base srgb_tx, rien d'autre", links.get(0, {}).get("color_space") == "srgb_tx"
          and not any("color_space" in l for k, l in links.items() if k), links)

# ---- .sbs, projet jamais enregistré, sbsrender introuvable -----------------------------------------------------------

if os.path.isfile(PLANKS_SBS):
    base = os.path.join(work, "unsaved")
    shutil.rmtree(base, ignore_errors=True)
    res, reqs = run("sbs", {"open": {"file": PLANKS_SBS}, "size": 8, "dir": base, "action": "create"}, state(""))
    create = ops(reqs, "create")
    check(".sbs cuit puis envoyé, projet jamais enregistré : dossier choisi",
          create and create[0]["args"]["dir"] == os.path.join(base, "ColorfulPlanks"), res.get("errors"))
else:
    print("skip .sbs : %s absent" % PLANKS_SBS)

res, reqs = run("missing", {"open": {"file": PLANKS}, "action": "none"}, state(project, sbsrender="/nowhere/sbsrender"))
check("sbsrender introuvable : bandeau vers les préférences", "Preferences > subzero" in res.get("banner", "")
      and not ops(reqs, "create"), res.get("banner"))
check("sbsrender introuvable : paramètres montrés quand même", "wood_color" in res.get("shown", []), res.get("shown"))
res, reqs = run("prefs", {"open": {"file": PLANKS}, "size": 8, "action": "create"},
                state(os.path.join(work, "prefs.arm"), sbsrender="/nowhere/sbsrender"),
                events=[{"event": "prefs", "sbsrender": SBSRENDER}])
check("chemin corrigé dans les préférences pendant la détection : paquet ouvert, pas de bandeau, matériau envoyé",
      res.get("opened") and not res.get("banner") and len(ops(reqs, "create")) == 1,
      {k: res.get(k) for k in ("opened", "banner", "errors")})

print("== " + ("FAIL: " + ", ".join(FAIL) if FAIL else "OK"))
sys.exit(1 if FAIL else 0)
