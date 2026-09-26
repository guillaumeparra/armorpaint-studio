#!/usr/bin/env python3
"""Fenêtre d'export (lot B6) contre un faux ArmorPaint : le protocole de studio_export_window.h, sans l'app. Projet
à 2 objets et 3 tuiles UDIM, export par tuile : liste groupée par map puis par tuile, fichiers décochés envoyés à
l'export, décomptes, notes et fichiers existants hors de la liste des problèmes.

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


MAPS = [("base_color", "exr", 16, "ACEScg"), ("roughness", "exr", 16, "raw"), ("height", "exr", 32, "raw")]
TILES = ["1001", "1002", "1003"]
STATE = {
    "project": {"name": "robot", "path": "/show/robot.v003.arm", "width": 2048, "height": 2048, "bits": 16,
                "objects": ["head", "body"], "tiles": TILES},
    "presets": [{"name": "basic", "description": "All maps", "valid": True, "shipped": True, "path": "/p/basic.json"}],
    "preset": "basic", "ocio": {"ready": True, "config": "studio", "status": "ok"},
    "color_spaces": ["ACEScg"], "color_space": "", "dir": "/out", "mode": 3, "padding": False,
}


def report(args):
    skip = {tuple(k) for k in args.get("skip", [])}
    files = []
    for tile in TILES:
        for name, fmt, bits, cs in MAPS:
            files.append({"path": "%s/robot_%s.%s.exr" % (args["dir"], name, tile), "texture": name, "variant": "",
                          "udim": tile, "format": fmt, "bits": bits, "channels": 3, "compression": "zip",
                          "color_space": cs, "size_estimate": 1 << 20, "exists": tile == "1001" and name == "base_color",
                          "skip": (name, "", tile) in skip})
    issues = [{"level": "note", "code": "height_8bit", "message": "height: in 8 bits"},
              {"level": "warning", "code": "exists", "message": "1 file already exists and will be replaced"},
              {"level": "warning", "code": "udim_gaps", "message": "missing UDIM tiles between 1001 and 1004: 1002"}]
    return {"ok": True, "preset": "basic", "destination": args["dir"], "files": files, "issues": issues,
            "total_size": sum(f["size_estimate"] for f in files if not f["skip"]), "mode": "per UDIM tile"}


def run(name, choices):
    result = os.path.join(work, name + ".result.json")
    choices = dict(choices, result=result)
    with open(os.path.join(work, name + ".test.json"), "w") as f:
        json.dump(choices, f)
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", STUDIO_EXPORT_WINDOW_TEST=os.path.join(work, name + ".test.json"))
    p = subprocess.Popen([sys.executable, SCRIPT], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env, text=True)
    exports = []
    for line in p.stdout:
        req = json.loads(line)
        op, args = req["op"], req.get("args", {})
        if op == "state":
            data = STATE
        else:
            data = {"ok": True, "report": report(args), "error": ""}
            if op == "export":
                exports.append(args)
        p.stdin.write(json.dumps({"id": req["id"], "ok": True, "data": data}) + "\n")
        p.stdin.flush()
        if op == "export":
            break
    p.stdin.close()
    p.wait(timeout=30)
    return json.load(open(result)), exports


r, exports = run("map", {"preset": "basic", "dir": "/out", "mode": 3, "skip": [["height", "", "1002"]]})
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

r, exports = run("set", {"preset": "basic", "dir": "/out", "mode": 3, "group": "set",
                         "skip": [["base_color", "", t] for t in TILES]})
check("groupé par tuile", [g.split()[0] for g in r["groups"]] == TILES, r["groups"])
check("chaque tuile : 2 of 3", all(g.endswith("(2 of 3)") for g in r["groups"]), r["groups"])
check("export sans base_color", sorted(map(tuple, exports[0]["skip"])) == [("base_color", "", t) for t in TILES], exports)

print("== OK" if not FAIL else "== FAIL")
sys.exit(1 if FAIL else 0)
