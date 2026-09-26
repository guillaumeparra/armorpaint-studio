# Vérification de l'export OCIO (lot A3, ocio_export_app.sh) : chaque texture exportée est comparée à la même
# transformation faite par PyOpenColorIO sur la texture « src », les valeurs d'ArmorPaint telles quelles.
#
#   python ocio_export_check.py <dossier d'export> <base des fichiers>
#
# Références : la couleur stockée (gamma 2.2) décodée comme le fait l'export (v^2.2, 0 sous 0), transformée de
# « Linear Rec.709 (sRGB) » vers l'espace de la texture. Tolérances : pow en float32 de numpy contre powf, puis la
# transformation : 2e-6 relatif en float ; 1 pas de quantification en 8 et 16 bits ; 1 ulp de half en half.

import json
import os
import sys

import numpy as np
import OpenEXR
import PyOpenColorIO as OCIO
import tifffile
from PIL import Image

d, base = sys.argv[1], sys.argv[2]
# Celle de l'app : $OCIO, sinon la configuration ACES intégrée.
cfg = OCIO.Config.CreateFromFile(os.environ.get("OCIO") or "ocio://studio-config-latest")
WORKING = "Linear Rec.709 (sRGB)"
fails = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        fails.append(what)


def exr(name):
    with OpenEXR.File(os.path.join(d, "%s_%s.exr" % (base, name)), separate_channels=True) as f:
        return {k: np.asarray(v.pixels, dtype=np.float32) for k, v in f.channels().items()}


def rgb(ch):
    return np.stack([ch["R"], ch["G"], ch["B"]], axis=-1)


def transform(pixels, dst):
    out = np.ascontiguousarray(pixels, dtype=np.float32).copy()
    cpu = cfg.getProcessor(WORKING, dst).getDefaultCPUProcessor()
    cpu.applyRGB(out)
    return out


def close(a, b, rel=2e-6, abs_=1e-7):
    err = np.abs(a.astype(np.float64) - b.astype(np.float64))
    return bool(np.all(err <= abs_ + rel * np.abs(b.astype(np.float64)))), float(err.max())


def quantized(a, ref, levels):
    q = np.floor(np.clip(ref, 0.0, 1.0) * levels + 0.5)
    return int(np.abs(a.astype(np.int64) - q.astype(np.int64)).max())


src = exr("src")
stored = rgb(src)
rough = src["A"]
lin = np.where(stored > 0, np.power(stored, np.float32(2.2), dtype=np.float32), np.float32(0))
distinct = len(np.unique(stored.reshape(-1, 3), axis=0))
check(stored.std() > 0.05 and distinct > 1000, "source varied: %d distinct colours" % distinct)
check(rough.std() > 0.01, "roughness varied")

ok, e = close(rgb(exr("acescg")), transform(lin, "ACEScg"))
check(ok, "ACEScg (EXR 32) = PyOpenColorIO, max err %.3g" % e)

c = exr("acescct")
ok, e = close(rgb(c), transform(lin, "ACEScct"))
check(ok, "ACEScct (EXR 32, RGBA) = PyOpenColorIO, max err %.3g" % e)
opac = exr("opac")
opac = opac.get("Y", opac.get("R"))
check(opac.std() > 0.01, "opacity varied")
check(np.array_equal(c["A"], opac), "ACEScct: alpha = opacity, untouched")

h = rgb(exr("half"))
ref = transform(lin, "ACEScg").astype(np.float16).astype(np.float32)
ok, e = close(h, ref, rel=1e-3)
check(ok, "lin_ap1_scene (alias, EXR half) = PyOpenColorIO, max err %.3g" % e)

t = tifffile.imread(os.path.join(d, "%s_srgb16.tif" % base))
m = quantized(t, transform(lin, "sRGB - Texture"), 65535)
check(t.dtype == np.uint16 and m <= 1, "sRGB - Texture (TIFF 16) = PyOpenColorIO, max %d code(s)" % m)

p = np.asarray(Image.open(os.path.join(d, "%s_srgb8.png" % base)).convert("RGB"))
m = quantized(p, transform(lin, "sRGB - Texture"), 255)
check(m <= 1, "srgb_tx (alias, PNG 8) = PyOpenColorIO, max %d code(s)" % m)

g = exr("grey")
y = g.get("Y", g.get("R"))
grey = transform(np.repeat(rough[..., None], 3, axis=-1), "ACEScg")[..., 0]
ok, e = close(y, grey)
check(ok, "1 channel (rough -> ACEScg) = R of the grey, max err %.3g" % e)

r = exr("rawdata")
check(np.array_equal(r.get("Y", r.get("R")), rough), "Raw (data) = values as stored")

legacy = rgb(exr("legacy"))
ok, e = close(legacy, lin)
check(ok, "linear keyword = v^2.2, max err %.3g" % e)
check(np.array_equal(rgb(exr("working")), legacy), "working space by name = linear keyword, bit for bit")

m = json.load(open(os.path.join(d, "%s.export.json" % base)))
check(m["ocio"]["config"] == cfg.getName() and m["ocio"]["working_space"] == WORKING and m["ocio"]["version"] == OCIO.GetVersion(),
      "manifest: OCIO version, config, working space")
files = {f["texture"]: f for f in m["files"]}
check(files["half"]["color_space"] == "lin_ap1_scene" and files["half"]["ocio"]["colorspace"] == "ACEScg",
      "manifest: name as written and canonical name")
check(files["acescg"]["ocio"]["from"] == WORKING and files["acescg"]["ocio"]["transform"]
      == cfg.getProcessor(WORKING, "ACEScg").getCacheID(), "manifest: transform = OCIO cache id")
check(files["rawdata"]["ocio"] == {"colorspace": "Raw", "data": True}, "manifest: data colorspace")
check("ocio" not in files["legacy"] and files["legacy"]["color_space"] == "linear", "manifest: keyword unchanged")
check(any("ocio_data_as_color" in w or "data channels converted" in w for w in m["warnings"]),
      "manifest: warning for data converted to a colour space")

sys.exit(1 if fails else 0)
