#!/usr/bin/env python3
"""Fenêtre d'export (lot B6) contre un faux ArmorPaint : le protocole de studio_export_window.h, sans l'app. Projet
à 2 objets et 3 tuiles UDIM, export par tuile : liste groupée par map puis par tuile, fichiers décochés envoyés à
l'export, décomptes, notes et fichiers existants hors de la liste des problèmes ; tableau des maps (bits et résolution
par map, pour toutes, maps désactivées), sidecar du projet (choix remis à l'ouverture, preset enregistré).

    <python avec PySide6> .tests/export_window_ui.py [dossier de travail]
"""
import json, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "assets", "studio_export_window.py")
work = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp()
os.makedirs(work, exist_ok=True)
FAIL = []


def check(name, cond, detail=""):
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else "  (%s)" % detail))
    if not cond:
        FAIL.append(name)


BITS = {"exr": [16, 32], "png": [8, 16]}
BASIC = {"description": "All maps", "defaults": {"format": "exr"}, "textures": [
    {"name": "base_color", "channels": ["base_r", "base_g", "base_b"], "color_space": "ACEScg"},
    {"name": "roughness", "channels": ["rough"], "color_space": "raw"},
    {"name": "height", "channels": ["height"], "bits": 32, "color_space": "raw"}]}
TILES = ["1001", "1002", "1003"]
PROJECT = os.path.join(work, "robot_v003.arm")
SIDECAR = os.path.join(work, "robot.export_settings.json")
with open(os.path.join(work, "basic.json"), "w") as f:
    json.dump(BASIC, f)


def textures(preset):
    """Ce qu'ArmorPaint dit des textures d'un preset (put_textures), pour ce faux preset simplifié."""
    out = []
    for t in preset["textures"]:
        fmt = t.get("format", preset.get("defaults", {}).get("format", "png"))
        cs = t.get("color_space", "raw")
        out.append({"name": t["name"], "suffix": t["name"], "format": fmt, "bits": t.get("bits", 16 if fmt == "exr" else 8),
                    "bits_options": BITS[fmt], "resolution": t.get("resolution", 0), "channels": len(t["channels"]),
                    "color_space": cs, "color": cs != "raw"})
    return out


def info(name, preset):
    return {"name": name, "description": preset.get("description", ""), "valid": True, "error": "",
            "textures": textures(preset)}


def state(args):
    given = (args or {}).get("presets", [])
    return {
        "project": {"name": "robot", "path": PROJECT, "width": 2048, "height": 2048, "bits": 16,
                    "objects": ["head", "body"], "tiles": TILES},
        "presets": [dict(info("basic", BASIC), shipped=True, path=os.path.join(work, "basic.json"))],
        "project_presets": [info(p["name"], p["preset"]) for p in given],
        "preset": "basic", "preset_project": False, "overrides": None, "skip": [],
        "ocio": {"ready": True, "config": "studio", "status": "ok"},
        "color_spaces": ["ACEScg", "sRGB - Texture"], "color_space": "", "dir": "/out", "mode": 3, "padding": False,
    }


def report(args):
    skip = {tuple(k) for k in args.get("skip", [])}
    preset = args.get("preset_json") or BASIC
    overrides = args.get("overrides", {})
    files = []
    for tile in TILES:
        for t in textures(preset):
            o = overrides.get(t["name"], {})
            bits, res = o.get("bits", t["bits"]), o.get("resolution", t["resolution"]) or 2048
            files.append({"path": "%s/robot_%s.%s.exr" % (args["dir"], t["name"], tile), "texture": t["name"],
                          "variant": "", "udim": tile, "width": res, "height": res, "format": t["format"], "bits": bits,
                          "channels": t["channels"], "compression": "zip", "color_space": t["color_space"],
                          "size_estimate": res * res * t["channels"] * bits // 8,
                          "exists": tile == "1001" and t["name"] == "base_color",
                          "skip": (t["name"], "", tile) in skip or (t["name"], "*", "*") in skip})
    issues = [{"level": "note", "code": "height_8bit", "message": "height: in 8 bits"},
              {"level": "warning", "code": "exists", "message": "1 file already exists and will be replaced"},
              {"level": "warning", "code": "udim_gaps", "message": "missing UDIM tiles between 1001 and 1004: 1002"}]
    return {"ok": True, "preset": args["preset"], "destination": args["dir"], "files": files, "issues": issues,
            "total_size": sum(f["size_estimate"] for f in files if not f["skip"]), "mode": "per UDIM tile"}


def run(name, choices):
    result = os.path.join(work, name + ".result.json")
    choices = dict(choices, result=result)
    with open(os.path.join(work, name + ".test.json"), "w") as f:
        json.dump(choices, f)
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", STUDIO_EXPORT_WINDOW_TEST=os.path.join(work, name + ".test.json"))
    p = subprocess.Popen([sys.executable, SCRIPT], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env, text=True)
    requests = []
    for line in p.stdout:
        req = json.loads(line)
        op, args = req["op"], req.get("args", {})
        requests.append(req)
        if op == "state":
            data = state(args)
        elif op == "inspect":
            data = info(args["name"], args["preset"])
        else:
            data = {"ok": True, "report": report(args), "error": ""}
        p.stdin.write(json.dumps({"id": req["id"], "ok": True, "data": data}) + "\n")
        p.stdin.flush()
    p.wait(timeout=30)
    exports = [r["args"] for r in requests if r["op"] == "export"]
    return json.load(open(result)), exports, requests


def sidecar():
    return json.load(open(SIDECAR)) if os.path.exists(SIDECAR) else None


if os.path.exists(SIDECAR):
    os.remove(SIDECAR)
r, exports, _ = run("map", {"preset": "basic", "dir": "/out", "mode": 3, "skip": [["height", "", "1002"]]})
check("projet : objets et tuiles comptés", "2 objects" in r["project"] and "3 UDIM tiles" in r["project"], r["project"])
check("composition : 3 maps × 3 tuiles", r["composition"] == "3 maps × 3 UDIM tiles", r["composition"])
check("groupé par map, un groupe par texture", [g.split()[0] for g in r["groups"]] == ["base_color", "roughness", "height"],
      r["groups"])
check("groupe partiellement coché : 2 of 3", "height  (2 of 3)" in r["groups"], r["groups"])
check("fichier décoché envoyé à l'export", len(exports) == 1 and exports[0]["skip"] == [["height", "", "1002"]], exports)
check("8 fichiers cochés, 1 décoché", len(r["files"]) == 8 and r["skipped"] == ["robot_height.1002.exr"], r["skipped"])
check("message de fin : fichiers écrits seulement", "Exported 8 files" in r["summary"], r["summary"])
check("problèmes : ni note ni fichier existant, le reste oui",
      "udim" in r["issues"].lower() and "8 bits" not in r["issues"] and "already exists" not in r["issues"], r["issues"])

check("sidecar écrit à côté du projet, version retirée du nom", r["sidecar"] == SIDECAR and sidecar() is not None, r["sidecar"])
check("sidecar : fichier décoché de basic", (sidecar() or {}).get("export", {}).get("presets", {}).get("file:basic", {})
      .get("skip") == [["height", "", "1002"]], sidecar())

os.remove(SIDECAR)
r, exports, _ = run("set", {"preset": "basic", "dir": "/out", "mode": 3, "group": "set",
                            "skip": [["base_color", "", t] for t in TILES]})
check("groupé par tuile", [g.split()[0] for g in r["groups"]] == TILES, r["groups"])
check("chaque tuile : 2 of 3", all(g.endswith("(2 of 3)") for g in r["groups"]), r["groups"])
check("export sans base_color", sorted(map(tuple, exports[0]["skip"])) == [("base_color", "", t) for t in TILES], exports)

# Maps : bits et résolution pour toutes, une map désactivée.
os.remove(SIDECAR)
r, exports, _ = run("maps", {"preset": "basic", "dir": "/out", "mode": 3, "bits_all": 32, "resolution_all": 1024,
                             "disabled": ["roughness"], "overrides": {"height": {"bits": 16}}})
maps = {m["name"]: m for m in r["maps"]}
check("maps : une ligne par texture du preset", list(maps) == ["base_color", "roughness", "height"], r["maps"])
check("maps : bits pour toutes (32), résolution pour toutes (1024)",
      all(m["bits"] == 32 and m["resolution"] == 1024 for m in r["maps"]), r["maps"])
check("maps : map désactivée", not maps["roughness"]["enabled"] and maps["roughness"]["size"] == "", r["maps"])
check("export : réglages différents du preset seulement",
      exports and exports[0]["overrides"] == {"base_color": {"bits": 32, "resolution": 1024},
                                              "roughness": {"bits": 32, "resolution": 1024},
                                              "height": {"resolution": 1024}}, exports and exports[0]["overrides"])
check("export : la map désactivée entière", exports and ["roughness", "*", "*"] in exports[0]["skip"], exports)
check("message de fin : 6 fichiers (2 maps × 3 tuiles)", "Exported 6 files" in r["summary"], r["summary"])
check("liste des fichiers : sans la map désactivée", r["files_shown"] and not any("roughness" in f for f in r["files"]),
      r["files"])

# Réouverture : tout est remis depuis le sidecar, sans rien choisir.
r, exports, _ = run("restore", {})
restored = r["restored"]
check("réouverture : preset, dossier et mode remis", restored["key"] == "file:basic" and restored["dir"] == "/out" and
      restored["mode"] == 3, restored)
check("réouverture : bits, résolution et map désactivée remis",
      restored["overrides"].get("base_color") == {"bits": 32, "resolution": 1024} and
      ["roughness", "*", "*"] in restored["skip"], restored)

# Preset du projet : les réglages en cours deviennent un preset du sidecar, utilisé tout de suite.
r, exports, requests = run("save", {"color_space": "sRGB - Texture", "save_preset": "robot_hero"})
inspect = [q["args"] for q in requests if q["op"] == "inspect"]
saved = (sidecar() or {}).get("presets", {}).get("robot_hero")
check("preset enregistré : vérifié par ArmorPaint, dans le sidecar", len(inspect) == 1 and saved is not None, inspect)
tex = {t["name"]: t for t in (saved or {}).get("textures", [])}
check("preset enregistré : sans la map désactivée, bits, résolution et espace dedans",
      list(tex) == ["base_color", "height"] and tex["base_color"].get("bits") == 32 and
      tex["base_color"].get("resolution") == 1024 and tex["base_color"].get("color_space") == "sRGB - Texture" and
      tex["height"].get("color_space") == "raw" and tex["height"].get("bits") == 32, saved)
check("export avec le preset du projet, sans réglage par-dessus",
      exports and exports[0]["preset"] == "robot_hero" and exports[0]["preset_json"] == saved and
      exports[0]["overrides"] == {} and exports[0]["color_space"] == "", exports)
r, exports, requests = run("reopen", {})
check("réouverture : presets du projet relus par ArmorPaint",
      requests[0]["op"] == "state" and requests[1]["op"] == "state" and
      [p["name"] for p in requests[1]["args"]["presets"]] == ["robot_hero"], [q["op"] for q in requests])
check("réouverture : preset du projet choisi, en tête du menu",
      r["restored"]["key"] == "project:robot_hero" and r["presets"][0] == "Robot Hero  ·  project", r["presets"])
check("nom refusé : celui d'un preset studio",
      run("clash", {"save_preset": "basic", "no_export": True})[0].get("errors", [""])[0].startswith("A studio preset"))

print("== OK" if not FAIL else "== FAIL")
sys.exit(1 if FAIL else 0)
