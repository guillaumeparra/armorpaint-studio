#!/usr/bin/env python3
"""subzero autonome (lot E9), sans ArmorPaint, avec le vrai sbsrender. Commandes : --version, locate, info, render
(preset, --set, taille, format, gabarit de noms, normale DirectX, rendu repris d'une recette, textures périmées
supprimées, erreurs en JSON). Fenêtre (offscreen) : taille 512 par défaut, viewer carré de la hauteur de la fenêtre,
touches 1 à 9 (pavé numérique, AZERTY par la position), exposition, gamma et saturation comparés à PyOpenColorIO,
valeurs lues, export puis mise à jour dans un dossier, $SUBZERO_VIEWER=0. Navigateur : onglets de $SUBZERO_LIBRARY,
dossier caché ignoré, icônes (.sbsar, .sbs), dossier ajouté gardé dans browser.json, paquet ouvert, recherche. Paquet :
tools/release_subzero.py (--check), archives reproductibles, contenu (aucun fichier d'Adobe), lanceur bin/subzero
(lien symbolique compris), roue installée dans un venv sans réseau, package.py et rezbuild.py de rez.

    <python avec PySide6, PyOpenColorIO et numpy> .tests/subzero_standalone.py [dossier de travail]
    SUBZERO_SBSRENDER=<sbsrender> (défaut : détection) ; SUBZERO_SAMPLES (défaut : ~/Desktop/code/sbsar).
"""
import base64, hashlib, json, os, shutil, subprocess, sys, tarfile, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
EXT = os.path.dirname(HERE)
ASSETS = os.path.join(EXT, "assets")
APP = os.path.join(ASSETS, "subzero_app.py")
PY = sys.executable
work = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp())
shutil.rmtree(work, ignore_errors=True)
os.makedirs(work)
SAMPLES = os.environ.get("SUBZERO_SAMPLES") or os.path.expanduser("~/Desktop/code/sbsar")
PLANKS = os.path.join(SAMPLES, "colorful_planks.sbsar")
SRGB = "sRGB Encoded Rec.709 (sRGB)"
FAIL = []

sys.path.insert(0, ASSETS)
import subzero_material as sm  # noqa: E402
import subzero_meta  # noqa: E402

ENV = dict(os.environ)
for var in list(ENV):  # réglages de la machine : sans effet sur le banc
    if var.startswith(("SUBZERO_", "STUDIO_OCIO_")) and var not in ("SUBZERO_SBSRENDER", "SUBZERO_SAMPLES") or var == "OCIO":
        ENV.pop(var)
ENV.update(STUDIO_ENV="", STUDIO_PREFS=os.path.join(work, "none.json"), SUBZERO_CACHE=os.path.join(work, "cache"),
           SUBZERO_CONFIG_DIR=os.path.join(work, "config"), PYTHONDONTWRITEBYTECODE="1")
SCREEN = os.path.join(work, "screen.json")
with open(SCREEN, "w") as f:
    json.dump({"screens": [{"name": "s", "x": 0, "y": 0, "width": 1920, "height": 1080, "logicalDpi": 96, "dpr": 1}]}, f)


def check(name, cond, detail=""):
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else "  (%s)" % (detail,)))
    if not cond:
        FAIL.append(name)


def cli(*args, env=None, timeout=300):
    """(code de sortie, JSON de stdout ou None, stderr)."""
    e = dict(ENV, **(env or {}))
    p = subprocess.run([PY, "-B", APP] + list(args), env=e, capture_output=True, text=True, timeout=timeout)
    try:
        out = json.loads(p.stdout)
    except ValueError:
        out = None
    return p.returncode, out, p.stderr


def window(name, test, env=None, args=(), timeout=120):
    """Fenêtre autonome en offscreen avec l'essai `test` ; résultat de l'essai ({} si absent)."""
    result = os.path.join(work, name + ".result.json")
    test = dict(test, result=result, screenshot=os.path.join(work, name + ".png"))
    path = os.path.join(work, name + ".test.json")
    with open(path, "w") as f:
        json.dump(test, f)
    e = dict(ENV, SUBZERO_TEST=path, QT_QPA_PLATFORM="offscreen:configfile=" + SCREEN, **(env or {}))
    with open(os.path.join(work, name + ".log"), "w") as log:
        code = subprocess.run([PY, "-B", APP] + list(args), env=e, stdout=log, stderr=subprocess.STDOUT,
                              timeout=timeout).returncode
    try:
        with open(result) as f:
            res = json.load(f)
    except (OSError, ValueError):
        res = {}
    res["_exit"] = code
    return res


def files_in(folder):
    return sorted(os.listdir(folder)) if os.path.isdir(folder) else []


# ---- Commandes -----------------------------------------------------------------------------------------------------------

p = subprocess.run([PY, "-B", APP, "--version"], env=ENV, capture_output=True, text=True)
version = p.stdout.strip().split()[-1] if p.stdout.strip() else ""
check("--version", p.returncode == 0 and p.stdout.startswith("subzero "), p.stdout + p.stderr)

code, out, err = cli("locate")
check("locate : sbsrender trouvé, avec sa version", code == 0 and out and out["ok"] and out["result"].get("version"),
      (code, out, err[-300:]))

code, out, err = cli("info", PLANKS)
info = (out or {}).get("result") or {}
graph = (info.get("graphs") or [{}])[0]
params = {p["identifier"]: p for p in graph.get("parameters") or []}
check("info : graphe, paramètres (défaut, plage), presets et sorties avec leur canal",
      code == 0 and graph.get("graph") == "pkg://colorful_planks"
      and len(params.get("wood_color", {}).get("default") or []) == 3
      and (params.get("age", {}).get("min"), params.get("age", {}).get("max")) == (0.0, 1.0)
      and "Aged Planks" in (graph.get("presets") or [])
      and any(o.get("channel") == "base" for o in graph.get("outputs") or []),
      (code, list(params)[:5], err[-300:]))

out_dir = os.path.join(work, "out")
code, out, err = cli("render", PLANKS, "-o", out_dir, "--preset", "Aged Planks", "--set", "age=0.2",
                     "--size", "256", "--name", "Planks CLI")
res = (out or {}).get("result") or {}
folder = res.get("folder") or ""
check("render : dossier <DIR>/<Nom>, six cartes EXR 256 px, recette",
      code == 0 and folder == os.path.join(out_dir, "PlanksCLI") and res.get("size") == [256, 256]
      and len(res.get("files") or {}) == 6 and all(p.endswith(".exr") and os.path.isfile(p) for p in res["files"].values())
      and os.path.isfile(os.path.join(folder, sm.RECIPE)), (code, res.get("folder"), res.get("size"), err[-400:]))
check("render : couleur de base sRGB, données Raw", (res.get("spaces") or {}).get("base") == SRGB
      and (res.get("spaces") or {}).get("roughness") == "Raw", res.get("spaces"))
recipe = sm.read_recipe(folder) if os.path.isfile(os.path.join(folder, sm.RECIPE)) else {}
check("render : preset et --set dans la recette", recipe.get("preset") == "Aged Planks"
      and abs(float(recipe.get("values", {}).get("age", -1)) - 0.2) < 1e-6, recipe.get("values", {}).get("age"))
opengl_normal = os.path.join(folder, "PlanksCLI_normal.exr")
g_opengl = None
if os.path.isfile(opengl_normal):
    import subzero_exr
    g_opengl = subzero_exr.read_planes(opengl_normal)[2]["G"].astype("float32")

code, out, err = cli("render", folder, "--format", "png", "--size", "128",
                     env={"SUBZERO_EXPORT_TEMPLATE": "{name}_{channel}_{size}", "SUBZERO_NORMAL_FORMAT": "directx"})
res2 = (out or {}).get("result") or {}
names = files_in(folder)
check("render d'un dossier : repris sur place, gabarit {size}, PNG, anciens EXR supprimés",
      code == 0 and res2.get("folder") == folder and "PlanksCLI_basecolor_128.png" in names
      and not any(n.endswith(".exr") for n in names), (code, names, err[-400:]))
if g_opengl is not None and "PlanksCLI_normal_128.png" in names:
    from PySide6 import QtGui
    img = QtGui.QImage(os.path.join(folder, "PlanksCLI_normal_128.png")).convertToFormat(QtGui.QImage.Format_RGB888)
    g_dx = [QtGui.QColor(img.pixel(x, 64)).green() / 255.0 for x in range(0, 128, 8)]
    g_gl = [float(g_opengl[128, 2 * x]) for x in range(0, 128, 8)]  # 256 -> 128 px
    mean = sum(abs(a + b - 1.0) for a, b in zip(g_dx, g_gl)) / len(g_dx)
    check("SUBZERO_NORMAL_FORMAT=directx : vert inversé", mean < 0.08, mean)

code, out, err = cli("render", folder)
names = files_in(folder)
check("render suivant, réglages par défaut : textures du rendu précédent (gabarit {size}) supprimées",
      code == 0 and "PlanksCLI_basecolor.png" in names and not any("_128" in n for n in names), (code, names))

p = subprocess.run([PY, "-S", "-B", APP, "render", PLANKS, "-o", out_dir, "--name", "NoQt", "--size", "128", "--format",
                    "png"], env=ENV, capture_output=True, text=True, timeout=300)  # -S : ni PySide6, ni OCIO, ni numpy
try:
    res = json.loads(p.stdout)["result"]
except (ValueError, KeyError, TypeError):
    res = {}
check("render sans PySide6 (python -S) : PNG rendus, décalque non vérifié et signalé", p.returncode == 0
      and len(res.get("files") or {}) == 6 and any("PySide6" in w for w in res.get("warnings") or []),
      (p.returncode, res.get("warnings"), p.stderr[-300:]))

code, out, err = cli("render", PLANKS, "-o", out_dir, "--size", "300")
check("--size 300 : code 2, erreur en JSON", code == 2 and out is not None and not out["ok"] and "300" in out["error"],
      (code, out))
code, out, err = cli("render", PLANKS, "-o", out_dir, env={"SUBZERO_SBSRENDER": "/nowhere/sbsrender"})
check("sbsrender introuvable : code 1, erreur en JSON", code == 1 and out is not None and not out["ok"], (code, out))

# ---- Fenêtre et viewer --------------------------------------------------------------------------------------------------

keys = [{"key": "3"}, {"key": "\"", "vk": 0x14}, {"key": "2", "keypad": True}, {"key": "9"}]
res = window("viewer", {"open": {"file": PLANKS}, "preset": "Redwood Planks", "action": "none", "viewer": True,
                        "browser": False, "viewer_keys": keys, "zoom": "1:1", "viewer_hover": [40, 60]})
v = res.get("viewer") or {}
check("fenêtre : taille Default 512, aperçu à 512", res.get("_exit") == 0 and (res.get("state") or {}).get("resolution")
      == [512, 512] and res.get("status", "").startswith("Preview 512 px"), (res.get("_exit"), res.get("status")))
check("viewer carré de la hauteur de la fenêtre", v.get("visible") and v.get("width") == v.get("height") > 400,
      (v.get("width"), v.get("height")))
pressed = [(k["digit"], k["current"]) for k in v.get("keys") or []]
order = v.get("order") or []
check("touches : 3, 3 par la position (AZERTY), 2 du pavé numérique ; 9 sans carte ne change rien",
      2 < len(order) < 9 and pressed == [(3, order[2]), (3, order[2]), (2, order[1]), (9, order[1])], (pressed, order))
check("viewer : carte de donnée montrée telle quelle, à 1:1, valeurs lues",
      v.get("map") == order[1] and v.get("look") == "raw" and v.get("zoom") == 1.0 and v.get("readout", "").startswith("40, 60"),
      {k: v.get(k) for k in ("map", "look", "zoom", "readout")})

try:
    import PyOpenColorIO as OCIO
except ImportError:
    OCIO = None
if OCIO is not None:
    cfg = OCIO.Config.CreateFromFile("ocio://studio-config-latest")
    display = cfg.getDefaultDisplay()
    to_lin = cfg.getProcessor(SRGB, "ACEScg").getDefaultCPUProcessor()
    show = cfg.getProcessor(OCIO.DisplayViewTransform(src="ACEScg", display=display,
                                                      view=cfg.getDefaultView(display))).getDefaultCPUProcessor()

    def expected(value, exposure=0.0, gamma=1.0, saturation=1.0):
        lin = [c * 2.0 ** exposure for c in to_lin.applyRGB(list(value))]
        luma = sum(a * b for a, b in zip(lin, cfg.getDefaultLumaCoefs()))  # luma de la configuration, comme le viewer
        lin = [luma + saturation * (c - luma) for c in lin]
        return [round(min(1.0, max(0.0, c)) ** (1.0 / gamma) * 255.0) for c in show.applyRGB(lin)]

    for label, grade in (("exposition +1", {"exposure": 1.0}), ("gamma 2", {"gamma": 2.0}),
                         ("saturation 0", {"saturation": 0.0})):
        res = window("grade", dict({"open": {"file": PLANKS}, "action": "none", "viewer": True, "browser": False,
                                    "viewer_map": "base", "viewer_hover": [100, 100]}, **grade))
        v = res.get("viewer") or {}
        want = expected(v.get("value") or [0, 0, 0], **grade) if v.get("value") else None
        got = v.get("shown")
        check("viewer, %s : couleur montrée = PyOpenColorIO (ACEScg, %s)" % (label, display),
              want is not None and got is not None and max(abs(a - b) for a, b in zip(want, got)) <= 2
              and "ACEScg" in v.get("look", ""), (got, want, v.get("look")))
else:
    print("--   sans PyOpenColorIO : viewer comparé seulement sans OCIO")

export = os.path.join(work, "export")
res = window("export", {"open": {"file": PLANKS}, "preset": "Fresh Planks", "action": "create", "viewer": True,
                        "browser": False, "viewer_map": "base"}, env={"SUBZERO_EXPORT_DIR": export})
mat = res.get("material_dir") or ""
v = res.get("viewer") or {}
check("export : <SUBZERO_EXPORT_DIR>/<Nom>, EXR et recette", res.get("_exit") == 0
      and mat == os.path.join(export, "FreshPlanks") and "FreshPlanks_basecolor.exr" in files_in(mat)
      and sm.RECIPE in files_in(mat), (mat, files_in(mat), res.get("errors")))
check("viewer : textures exportées (EXR half)", v.get("file", "").startswith(mat) and v.get("file", "").endswith(".exr")
      and v.get("size") == [512, 512], (v.get("file"), v.get("size")))
res = window("update", {"format": "png", "action": "update", "viewer": True, "browser": False}, args=[mat],
             env={"SUBZERO_EXPORT_DIR": export})
names = files_in(mat)
check("réédition du dossier : PNG, EXR supprimés, état Updated", res.get("_exit") == 0 and "FreshPlanks_basecolor.png"
      in names and not any(n.endswith(".exr") for n in names) and res.get("status", "").startswith("Updated"),
      (names, res.get("status")))
res = window("noviewer", {"open": {"file": PLANKS}, "action": "none", "browser": False}, env={"SUBZERO_VIEWER": "0"})
check("SUBZERO_VIEWER=0 : viewer fermé", res.get("_exit") == 0 and "viewer" not in res and res.get("opened"),
      (res.get("_exit"), sorted(res)))

# ---- Navigateur ---------------------------------------------------------------------------------------------------------

lib1, lib2 = os.path.join(work, "lib1"), os.path.join(work, "lib2")
for d in (os.path.join(lib1, "wood"), os.path.join(lib1, ".hidden"), lib2):
    os.makedirs(d)
for dst in ("wood/colorful_planks.sbsar", "wood/old_planks.sbsar", ".hidden/secret.sbsar", "metal_plates.sbsar"):
    shutil.copy(PLANKS, os.path.join(lib1, dst))
shutil.copy(PLANKS, os.path.join(lib2, "colorful_planks.sbsar"))
png = subzero_meta.package_icon(PLANKS)
with open(os.path.join(lib1, "stone_wall.sbs"), "w") as f:
    f.write('<?xml version="1.0"?><package><content><graph><attributes><icon><datalength v="%d"/><strdata v="%s"/>'
            '</icon></attributes></graph></content></package>' % (len(png), base64.b64encode(png).decode()))
with open(os.path.join(lib1, "no_icon.sbs"), "w") as f:
    f.write('<?xml version="1.0"?><package/>')
library = {"SUBZERO_LIBRARY": os.pathsep.join([lib1, os.path.join(work, "missing")])}
res = window("browser", {"browser": True, "browser_add": lib2, "browser_open": "colorful_planks", "action": "none",
                         "viewer": True}, env=library)
b = res.get("browser") or {}
tabs = {t["path"]: t for t in b.get("tabs") or []}
check("navigateur : un onglet par dossier de SUBZERO_LIBRARY, puis le dossier ajouté",
      [t["path"] for t in b.get("tabs") or []] == [lib1, os.path.join(work, "missing"), lib2]
      and not tabs[lib1]["removable"] and tabs[lib2]["removable"], b.get("tabs"))
check("navigateur : paquet ouvert depuis l'onglet ajouté", res.get("browser_open") == os.path.join(lib2, "colorful_planks.sbsar")
      and (res.get("opened") or {}).get("path") == os.path.join(lib2, "colorful_planks.sbsar"), res.get("browser_open"))
with open(os.path.join(work, "config", "browser.json")) as f:
    config = json.load(f)
check("navigateur : dossier ajouté gardé dans browser.json", config.get("folders") == [lib2] and config.get("tab") == lib2,
      config)
res = window("browser2", {"browser": True, "action": "none", "viewer": False, "browser_tab": lib1,
                          "browser_search": "planks"}, env=library)
b = res.get("browser") or {}
tabs = {t["path"]: t for t in b.get("tabs") or []}
check("navigateur relancé : onglet ajouté restauré", lib2 in tabs, list(tabs))
check("navigateur : sous-dossiers parcourus, dossier caché ignoré", tabs.get(lib1, {}).get("count") == 5,
      tabs.get(lib1))
check("navigateur : recherche", b.get("items") == ["colorful_planks", "old_planks"] and b.get("count") == "2 of 5 packages",
      (b.get("items"), b.get("count")))
check("navigateur : icônes des .sbsar et d'un .sbs, aucune pour un .sbs qui n'en a pas",
      {"metal_plates.sbsar", "old_planks.sbsar", "stone_wall.sbs"} <= set(b.get("icons") or [])
      and "no_icon.sbs" not in (b.get("icons") or []), b.get("icons"))
check("navigateur : fenêtre sans viewer = colonne + navigateur", res.get("_exit") == 0 and "viewer" not in res, sorted(res))

# ---- Paquet -------------------------------------------------------------------------------------------------------------

rel, rel2 = os.path.join(work, "rel"), os.path.join(work, "rel2")
p = subprocess.run([PY, os.path.join(EXT, "tools", "release_subzero.py"), "--out", rel, "--check"], capture_output=True,
                   text=True, env=ENV)
check("release_subzero.py --check", p.returncode == 0 and "check:" in p.stdout, p.stdout + p.stderr)
subprocess.run([PY, os.path.join(EXT, "tools", "release_subzero.py"), "--out", rel2], capture_output=True, env=ENV)


def sums(folder):
    try:
        return open(os.path.join(folder, "SHA256SUMS")).read()
    except OSError:
        return ""


check("archives reproductibles (SHA256SUMS identiques)", sums(rel) and sums(rel) == sums(rel2), (sums(rel), sums(rel2)))
tarball = os.path.join(rel, "subzero-%s.tar.gz" % version)
wheel = os.path.join(rel, "subzero-%s-py3-none-any.whl" % version)
members = tarfile.open(tarball).getnames() if os.path.isfile(tarball) else []
top = "subzero-%s/" % version
needed = ["bin/subzero", "setup.sh", "package.py", "rezbuild.py", "pyproject.toml", "requirements.txt", "LICENSE.md",
          "README.md", "share/studio.env.example", "python/subzero/__main__.py", "python/subzero/subzero_viewer.py",
          "python/subzero/subzero_browser.py", "python/subzero/subzero_window.py"]
check("archive : lanceur, modules, rez, pip, README", all(top + n in members for n in needed),
      [n for n in needed if top + n not in members])
check("archive : aucun fichier d'Adobe", not any(m.lower().endswith((".sbsar", ".sbs", ".sbsasm")) or "sbsrender" in
                                                os.path.basename(m) for m in members), members)
unpacked = os.path.join(work, "unpacked")
os.makedirs(unpacked)
if members:
    with tarfile.open(tarball) as t:
        t.extractall(unpacked)
launcher = os.path.join(unpacked, top, "bin", "subzero")
link = os.path.join(work, "subzero_link")
if os.path.isfile(launcher):
    os.symlink(launcher, link)
for label, exe in (("bin/subzero", launcher), ("bin/subzero par un lien symbolique", link)):
    p = subprocess.run([exe, "--version"], capture_output=True, text=True, env=dict(ENV, SUBZERO_PYTHON=PY)) \
        if os.path.exists(exe) else None
    check("archive : %s --version" % label, p is not None and p.returncode == 0 and p.stdout.strip() == "subzero " + version,
          p and (p.stdout, p.stderr))
fake = os.path.join(work, "rezbin")
os.makedirs(fake)
with open(os.path.join(fake, "python3"), "w") as f:  # Python d'une résolution rez, qui se signale
    f.write('#!/bin/sh\ntouch "%s"\nexec "%s" "$@"\n' % (os.path.join(work, "rez_python_used"), PY))
os.chmod(os.path.join(fake, "python3"), 0o755)
p = subprocess.run([launcher, "--version"], capture_output=True, text=True, env=dict(
    ENV, REZ_USED_RESOLVE="python subzero", STUDIO_PYTHON=PY, PATH=fake + os.pathsep + ENV.get("PATH", ""))) \
    if os.path.isfile(launcher) else None
check("archive : dans un environnement rez, bin/subzero prend le Python de la résolution (pas $STUDIO_PYTHON)",
      p is not None and p.returncode == 0 and os.path.isfile(os.path.join(work, "rez_python_used")),
      p and (p.stdout, p.stderr))
p = subprocess.run([launcher, "info", PLANKS], capture_output=True, text=True, env=dict(ENV, SUBZERO_PYTHON=PY)) \
    if os.path.isfile(launcher) else None
check("archive : bin/subzero info", p is not None and p.returncode == 0 and json.loads(p.stdout)["ok"],
      p and p.stderr[-300:])

venv = os.path.join(work, "venv")
subprocess.run([PY, "-m", "venv", "--system-site-packages", venv], check=False)
vpy = os.path.join(venv, "bin", "python")
p = subprocess.run([vpy, "-m", "pip", "install", "-q", "--no-index", "--no-deps", wheel], capture_output=True, text=True)
check("roue : pip install sans réseau", p.returncode == 0, p.stderr[-400:])
exe = os.path.join(venv, "bin", "subzero")
p = subprocess.run([exe, "--version"], capture_output=True, text=True, env=ENV) if os.path.isfile(exe) else None
check("roue : commande subzero (console_scripts)", p is not None and p.stdout.strip() == "subzero " + version,
      p and (p.stdout, p.stderr))
p = subprocess.run([vpy, "-m", "subzero", "locate"], capture_output=True, text=True, env=ENV, cwd=work)
check("roue : python -m subzero locate", p.returncode == 0 and json.loads(p.stdout or "{}").get("ok"), p.stderr[-300:])
p = subprocess.run([vpy, "-m", "pip", "show", "subzero"], capture_output=True, text=True)
check("roue : métadonnées (version, extras)", "Version: %s" % version in p.stdout, p.stdout)
with open(os.path.join(unpacked, top, "package.py")) as f:
    pkg = {}
    exec(compile(f.read(), "package.py", "exec"), pkg)
check("rez : package.py (nom, version, outil, commandes)", pkg.get("name") == "subzero" and pkg.get("version") == version
      and pkg.get("tools") == ["subzero"] and callable(pkg.get("commands")) and "rezbuild.py" in pkg.get("build_command", ""),
      {k: pkg.get(k) for k in ("name", "version", "tools", "build_command")})
install = os.path.join(work, "rez_install")
p = subprocess.run([PY, os.path.join(unpacked, top, "rezbuild.py"), "install"], capture_output=True, text=True,
                   env=dict(ENV, REZ_BUILD_SOURCE_PATH=os.path.join(unpacked, top), REZ_BUILD_INSTALL_PATH=install,
                            REZ_BUILD_INSTALL="1"), cwd=os.path.join(work))
check("rez : rezbuild.py copie le paquet dans REZ_BUILD_INSTALL_PATH", p.returncode == 0
      and os.access(os.path.join(install, "bin", "subzero"), os.X_OK)
      and os.path.isfile(os.path.join(install, "python", "subzero", "subzero_viewer.py")), (p.stdout, p.stderr[-300:]))

print("== " + ("FAIL: " + ", ".join(FAIL) if FAIL else "OK"))
sys.exit(1 if FAIL else 0)
