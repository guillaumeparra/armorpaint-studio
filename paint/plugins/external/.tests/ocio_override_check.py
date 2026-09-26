# Vérification de l'espace imposé (lot B6, ocio_export_app.sh) : out_ov (studio_export_set_color_space) doit être
# identique, au bit près, à out_ref (preset mixref, qui déclare lui-même Linear P3-D65 pour les couleurs), données
# comprises ; out_cli (STUDIO_EXPORT_COLOR_SPACE, --export-textures exr32) à out_cliref (mixref, même commande : exr32
# aplatit les calques en 32 bits). Manifestes : espace imposé et espace par texture.
#
#   python ocio_override_check.py <dossier de travail>

import json
import os
import sys

import numpy as np
import OpenEXR
import tifffile

W = sys.argv[1]
SPACE = "Linear P3-D65"
fails = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        fails.append(what)


def load(d, name):
    path = os.path.join(W, d, "p_%s.exr" % name)
    if not os.path.exists(path):
        return tifffile.imread(os.path.join(W, d, "p_%s.tif" % name))
    with OpenEXR.File(path, separate_channels=True) as f:
        return np.stack([np.asarray(v.pixels, dtype=np.float32) for _, v in sorted(f.channels().items())])


for name in ["base", "cg", "tex", "rough", "data"]:
    ref = load("out_ref", name)
    check(np.array_equal(load("out_ov", name), ref), "%s: script override = preset declaring %s" % (name, SPACE))
    check(np.array_equal(load("out_cli", name), load("out_cliref", name)),
          "%s: STUDIO_EXPORT_COLOR_SPACE = preset declaring %s (command line)" % (name, SPACE))
# Garde-fou : la conversion change bien les valeurs (base en P3 contre data, les mêmes canaux en Raw).
check(not np.array_equal(load("out_ov", "base"), load("out_ov", "data")), "base converted, data raw")

for d, want in [("out_ov", SPACE), ("out_cli", SPACE), ("out_ref", None), ("out_cliref", None)]:
    m = json.load(open(os.path.join(W, d, "p.export.json")))
    check(m["preset"].get("color_space_override", "missing") == want, "%s manifest: color_space_override %s" % (d, want))
m = json.load(open(os.path.join(W, "out_ov", "p.export.json")))
spaces = {f["texture"]: f["color_space"] for f in m["files"]}
want = {"base": SPACE, "cg": SPACE, "tex": SPACE, "rough": "raw", "data": "Raw"}
check(spaces == want, "manifest colour spaces: colour textures overridden, data kept (%s)" % spaces)

sys.exit(1 if fails else 0)
