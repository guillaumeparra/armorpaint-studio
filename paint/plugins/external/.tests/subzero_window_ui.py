#!/usr/bin/env python3
"""Fenêtre subzero (chantier E2) contre un faux ArmorPaint : le protocole de subzero.h, sans l'app, avec le vrai
sbsrender. Règles pures (nom, canaux, paramètres techniques) ; envoi d'un .sbsar (dossier <projet>_subzero/<Nom>,
fichiers <Nom>_<canal>.tga, recette, liaisons) ; réédition depuis la recette et mise à jour ; duplication ; projet en
16 bits (normale et hauteur en EXR) ; décalque (alpha de la couleur de base en fichier d'opacité) ; preset ; .sbs cuit ;
projet jamais enregistré ; sbsrender introuvable.

    <python avec PySide6> .tests/subzero_window_ui.py [dossier de travail]
    SUBZERO_SBSRENDER=<sbsrender> (défaut : détection dans /Applications) ; SUBZERO_SAMPLES, SUBZERO_SHELF comme
    subzero_check.py.
"""
import glob, json, os, shutil, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "assets")
SCRIPT = os.path.join(ASSETS, "subzero_window.py")
work = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp())
os.makedirs(work, exist_ok=True)
os.environ["SUBZERO_CACHE"] = os.path.join(work, "cache")
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
sys.path.insert(0, ASSETS)
import subzero_meta  # noqa: E402
import subzero_render  # noqa: E402
import subzero_window as sw  # noqa: E402

FAIL = []
SAMPLES = os.environ.get("SUBZERO_SAMPLES") or os.path.expanduser("~/Desktop/code/sbsar")
PLANKS = os.path.join(SAMPLES, "colorful_planks.sbsar")
PLANKS_SBS = os.path.join(SAMPLES, "colorful_planks", "colorful_planks.sbs")
PAINTER = "/Applications/Adobe Substance 3D Painter/Adobe Substance 3D Painter.app/Contents"
SHELF = os.environ.get("SUBZERO_SHELF") or PAINTER
SBSRENDER = os.environ.get("SUBZERO_SBSRENDER", "")


def check(name, cond, detail=""):
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else "  (%s)" % detail))
    if not cond:
        FAIL.append(name)


def run(name, test, state, events=()):
    """Lance la fenêtre en offscreen avec l'essai `test`, répond comme ArmorPaint (et envoie `events` juste après
    l'état) ; rend (résultat, requêtes)."""
    test = dict(test, result=os.path.join(work, name + ".result.json"), screenshot=os.path.join(work, name + ".png"))
    path = os.path.join(work, name + ".test.json")
    with open(path, "w") as f:
        json.dump(test, f)
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", SUBZERO_TEST=path)
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
            data = {"material": args.get("name") or os.path.basename(args["dir"]), "reloaded": len(args["links"])}
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


# ---- Règles pures ----------------------------------------------------------------------------------------------------

check("nom d'une ligne", sw.slug("Colorful Planks") == "ColorfulPlanks" and sw.slug("wood_plain-02") == "WoodPlain02"
      and sw.slug("  ") == "Material", sw.slug("wood_plain-02"))
check("dossier commun aux versions du projet", sw.subzero_dir("/p/robot_v003.arm") == "/p/robot_subzero"
      and sw.subzero_dir("/p/robot.arm") == "/p/robot_subzero", sw.subzero_dir("/p/robot_v003.arm"))
normal_format = {"identifier": "normal_format", "type": "int1", "widget": "combobox", "usertag": {},
                 "items": [(0, "DirectX"), (1, "OpenGL")]}
check("format de normale : OpenGL imposé", sw.technical_value(normal_format) == 1)
check("bascules de canaux", sw.technical_value({"identifier": "channel_basecolor", "widget": "togglebutton"}) == 1
      and sw.technical_value({"identifier": "channel_specular", "widget": "togglebutton"}) == 0
      and sw.technical_value({"identifier": "channel_choice", "widget": "togglebutton"}) is None)
with open(os.path.join(work, "gray.tga"), "wb") as f:  # 2x1, 32 bits, alpha opaque
    f.write(bytes([0, 0, 2] + [0] * 9 + [2, 0, 1, 0, 32, 8]) + bytes([9, 9, 9, 255] * 2))
with open(os.path.join(work, "decal.tga"), "wb") as f:  # origine en haut
    f.write(bytes([0, 0, 2] + [0] * 9 + [2, 0, 1, 0, 32, 0x28]) + bytes([1, 2, 3, 255, 4, 5, 6, 0]))
gray = open(os.path.join(work, "gray.tga"), "rb").read()
check("alpha opaque : couleur laissée telle quelle", not sw.split_alpha(os.path.join(work, "gray.tga"),
                                                                        os.path.join(work, "gray_opacity.tga"))
      and open(os.path.join(work, "gray.tga"), "rb").read() == gray
      and not os.path.exists(os.path.join(work, "gray_opacity.tga")))
check("alpha de décalque : couleur en RGB, alpha en opacité grise, orientation gardée",
      sw.split_alpha(os.path.join(work, "decal.tga"), os.path.join(work, "decal_opacity.tga"))
      and open(os.path.join(work, "decal.tga"), "rb").read() == bytes(
          [0, 0, 2] + [0] * 9 + [2, 0, 1, 0, 24, 0x20]) + bytes([1, 2, 3, 4, 5, 6])
      and open(os.path.join(work, "decal_opacity.tga"), "rb").read() == bytes(
          [0, 0, 3] + [0] * 9 + [2, 0, 1, 0, 8, 0x28]) + bytes([255, 0]))
check("liaisons : couleur de chaque fichier vers son canal", sw.links({"base": "b.tga", "opacity": "o.tga"}) == [
    {"file": "b.tga", "output": 0, "socket": 0}, {"file": "o.tga", "output": 0, "socket": 1}])

if not os.path.isfile(PLANKS):
    print("skip fenêtre : %s absent" % PLANKS)
    sys.exit(1 if FAIL else 0)
try:
    subzero_render.locate(SBSRENDER)
except subzero_render.Error as e:
    print("skip fenêtre : %s" % e)
    sys.exit(1 if FAIL else 0)

planks = subzero_render.find_graph(subzero_meta.read_sbsar(PLANKS))
mapping = {o["identifier"]: ch for o, ch, _ in sw.auto_mapping(planks, {})}
check("sorties de colorful_planks vers les canaux (diffuse, spéculaire, brillance masquées par channel_*)", mapping == {
    "basecolor": "base", "normal": "normal", "roughness": "roughness", "metallic": "metallic", "height": "height",
    "ambientocclusion": "occlusion"}, mapping)
shown_all = {o["identifier"]: (ch, why) for o, ch, why in sw.auto_mapping(dict(planks, outputs=[
    dict(o, visibleif=None) for o in planks["outputs"]]), {})}
check("sorties non représentables montrées sans canal, avec la raison", shown_all.get("glossiness", (1, ""))[0] is None
      and "glossiness" in shown_all["glossiness"][1] and shown_all.get("diffuse", (1,))[0] is None, shown_all)
shelf = sorted(glob.glob(os.path.join(SHELF, "Resources/starter_assets/materials/**/*.sbsar"), recursive=True))
bad = []
for path in shelf:
    g = subzero_render.find_graph(subzero_meta.read_sbsar(path))
    rows = sw.auto_mapping(g, {})
    chans = [ch for _, ch, _ in rows if ch]
    colour = any(u.lower() in ("basecolor", "diffuse") for o, _, _ in rows for u in o.get("usages") or [])
    if len(chans) != len(set(chans)) or any(not ch and not why for _, ch, why in rows) or colour != ("base" in chans):
        bad.append((os.path.basename(path), [(o["identifier"], ch, why) for o, ch, why in rows]))
if shelf:
    check("étagère (%d matériaux) : un canal par sortie au plus, sinon une raison ; couleur de base dès qu'une sortie "
          "en a l'usage" % len(shelf), not bad, bad[:3])
else:
    print("skip étagère : %s absent" % SHELF)
shown = [i["identifier"] for i in planks["inputs"] if sw.shown(i)]
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
    check("6 textures nommées <Nom>_<canal>.tga", files == sorted("ColorfulPlanks_%s.tga" % c for c in (
        "basecolor", "normal", "roughness", "metallic", "height", "occlusion")), files)
    check("sockets", sorted(l["socket"] for l in a["links"]) == [0, 2, 3, 4, 5, 7], a["links"])
    check("textures écrites à la résolution du projet", all(os.path.isfile(l["file"]) for l in a["links"]))
    head = open(os.path.join(folder, "ColorfulPlanks_basecolor.tga"), "rb").read(18)
    check("1024 px", head[12] | head[13] << 8 == 1024, head[12] | head[13] << 8)
recipe = sw.read_recipe(folder) if os.path.isfile(os.path.join(folder, sw.RECIPE)) else {}
check("recette", recipe.get("graph") == planks["pkgurl"] and recipe.get("package", {}).get("sha256")
      and recipe.get("files", {}).get("base") == "ColorfulPlanks_basecolor.tga", recipe)
renders = res.get("renders") or []
final = [r for r in res.get("requests") or [] if r["action"] == "create"]
print("     %.2f s en tout, rendu final %.2f s" % (took, final[0]["render"] if final else -1))
check("rendu final en moins de 2,5 s", final and final[0]["render"] < 2.5, final and final[0]["render"])

# ---- Réédition et mise à jour -----------------------------------------------------------------------------------------

res, reqs = run("update", {"open": {"dir": folder}, "values": {"age": 0.9, "$randomseed": 7}, "action": "update"},
                state(project))
update = ops(reqs, "update")
check("un update sur le même dossier", len(update) == 1 and update[0]["args"]["dir"] == folder and not ops(reqs, "create"),
      [r.get("op") for r in reqs])
recipe = sw.read_recipe(folder)
check("recette mise à jour", recipe["values"].get("age") == 0.9 and recipe["values"].get("$randomseed") == 7,
      recipe["values"])
check("réédition : nom gardé", res.get("material_name") == "Colorful Planks", res.get("material_name"))

res, reqs = run("duplicate", {"open": {"dir": folder}, "action": "duplicate"}, state(project))
create = ops(reqs, "create")
check("duplication dans ColorfulPlanks2", len(create) == 1 and create[0]["args"]["dir"] == folder + "2"
      and os.path.isfile(os.path.join(folder + "2", "ColorfulPlanks2_basecolor.tga")),
      create and create[0]["args"]["dir"])

# ---- Projet en 16 bits, preset --------------------------------------------------------------------------------------

deep = os.path.join(work, "deep.arm")
shutil.rmtree(os.path.join(work, "deep_subzero"), ignore_errors=True)
res, reqs = run("deep", {"open": {"file": PLANKS}, "size": 9, "action": "create"}, state(deep, bits=16))
create = ops(reqs, "create")
exts = {os.path.basename(l["file"]).split("_")[1] for l in create[0]["args"]["links"]} if create else set()
check("16 bits : normale et hauteur en EXR", exts == {"basecolor.tga", "normal.exr", "roughness.tga", "metallic.tga",
                                                    "height.exr", "occlusion.tga"}, exts)

presets = [p["label"] for p in planks.get("presets") or []]
if presets:
    res, reqs = run("preset", {"open": {"file": PLANKS}, "preset": presets[-1], "size": 8, "action": "create"},
                    state(deep))
    create = ops(reqs, "create")
    check("preset : nom du matériau", create and create[0]["args"]["name"] == presets[-1], create and create[0]["args"])

# ---- Décalque ---------------------------------------------------------------------------------------------------------

decal = glob.glob(os.path.join(SHELF, "Resources/starter_assets/materials/**/footprints.sbsar"), recursive=True)
if decal:
    res, reqs = run("decal", {"open": {"file": decal[0]}, "size": 8, "action": "create"}, state(deep))
    create = ops(reqs, "create")
    by_socket = {l["socket"]: l for l in create[0]["args"]["links"]} if create else {}
    base, opacity = by_socket.get(0, {}).get("file", ""), by_socket.get(1, {}).get("file", "")
    check("décalque : alpha de la couleur de base dans <Nom>_opacity.tga, vers l'opacité ; couleur en RGB",
          opacity.endswith("_opacity.tga") and all(l["output"] == 0 for l in by_socket.values())
          and open(base, "rb").read(18)[16] == 24 and open(opacity, "rb").read(18)[16] == 8
          and len(set(open(opacity, "rb").read()[18:18 + 256 * 256])) > 1, create and create[0]["args"]["links"])
else:
    print("skip décalque : footprints.sbsar absent de %s" % SHELF)

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
