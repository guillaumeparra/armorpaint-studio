# Vérification du viewport OCIO (lot A2, ocio_view_app.sh) : les couleurs sondées dans le compositeur d'ArmorPaint
# sont comparées à la transformation display/view/look exacte de PyOpenColorIO.
#
#   grep -E "^[pa] " run.log | python ocio_view_check.py
#
# Lignes lues : « p <étiquette> <couleur> r g b » (sortie du compositeur, GPU) et « a <étiquette> <couleur> r g b »
# (studio_ocio_view_apply_rgb : la même LUT interpolée sur le CPU). Entrée : chaque canal k/255, stocké en half dans
# la cible du compositeur. Écarts en 1/255 après écrêtage à [0, 1].
# Tolérances : la LUT contre OCIO exact, celles de ocio_view_test.c (gris 0.5, couleurs 18 : les couleurs saturées au
# bord du gamut ACES 2.0) ; GPU contre CPU 0.5 (sortie half, poids du filtrage bilinéaire matériel) ; sans OCIO, le
# tonemap filmique de Hejl d'ArmorPaint à 0.5 près, et l'identité en mode couleur de base.

import sys

import numpy as np
import PyOpenColorIO as OCIO

WORKING = "Linear Rec.709 (sRGB)"
ACES = ("sRGB - Display", "ACES 2.0 - SDR 100 nits (Rec.709)", None)
GAMUT = "ACES 1.3 Reference Gamut Compression"
# étiquette → (configuration, (display, view, look)) ; None : pas d'OCIO (filmique ou identité).
EXPECT = {
    "aces": ("ocio://studio-config-latest", ACES),
    "untonemapped": ("ocio://studio-config-latest", ("sRGB - Display", "Un-tone-mapped", None)),
    "look": ("ocio://studio-config-latest", ("sRGB - Display", "ACES 2.0 - SDR 100 nits (Rec.709)", GAMUT)),
    "p3": ("ocio://studio-config-latest", ("Display P3 - Display", "ACES 2.0 - SDR 100 nits (P3 D65)", None)),
    "cg": ("ocio://cg-config-latest", ACES),
    "filmic": None,
    "tst": None,
    "basecolor": "identity",
}
fails = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        fails.append(what)


def exact(config, display, view, look, rgb):
    cfg = OCIO.Config.CreateFromFile(config)
    if look is None:
        proc = cfg.getProcessor(WORKING, display, view, OCIO.TRANSFORM_DIR_FORWARD)
    else:  # comme le shim : pipeline legacy, look avant la vue
        dvt = OCIO.DisplayViewTransform(src=WORKING, display=display, view=view)
        pipeline = OCIO.LegacyViewingPipeline()
        pipeline.setDisplayViewTransform(dvt)
        pipeline.setLooksOverrideEnabled(True)
        pipeline.setLooksOverride(look)
        proc = pipeline.getProcessor(cfg)
    out = np.ascontiguousarray(rgb, dtype=np.float32).copy()
    proc.getDefaultCPUProcessor().applyRGB(out)
    return out


def filmic(rgb):
    x = np.maximum(rgb - 0.004, 0.0)
    return (x * (x * 6.2 + 0.5)) / (x * (x * 6.2 + 1.7) + 0.06)


def err255(a, b):
    return np.abs(np.clip(a, 0, 1) - np.clip(b, 0, 1)).max(axis=-1) * 255.0


probes, applied = {}, {}
for line in sys.stdin:
    f = line.split()
    if len(f) < 3:
        continue
    kind, label, color = f[0], f[1], int(f[2]) & 0xFFFFFFFF
    vals = [float(v) for v in f[3:6]] if len(f) >= 6 else None
    (probes if kind == "p" else applied).setdefault(label, []).append((color, vals))

for label, expect in EXPECT.items():
    rows = probes.get(label, [])
    if not rows or any(v is None for _, v in rows):
        check(False, "%s: probes missing or failed" % label)
        continue
    colors = np.array([c for c, _ in rows], dtype=np.int64)
    got = np.array([v for _, v in rows], dtype=np.float32)
    inp = np.stack([(colors >> 16) & 255, (colors >> 8) & 255, colors & 255], axis=-1) / 255.0
    inp = inp.astype(np.float16).astype(np.float32)  # cible RGBA64 du compositeur
    grey = (inp[:, 0] == inp[:, 1]) & (inp[:, 1] == inp[:, 2])
    if expect is None:
        e = err255(got, filmic(inp))
        check(e.max() <= 0.5, "%s: ArmorPaint filmic tonemap, max %.2f/255" % (label, e.max()))
        continue
    if expect == "identity":
        e = err255(got, inp)
        check(e.max() <= 0.5, "%s: unchanged (no tonemap in this mode), max %.2f/255" % (label, e.max()))
        continue
    config, (display, view, look) = expect
    ref = exact(config, display, view, look, inp)
    e = err255(got, ref)
    what = "%s: %s / %s%s" % (label, display, view, " / " + look if look else "")
    check(e[grey].max() <= 0.5 and e.max() <= 18.0,
          "%s = OCIO, greys max %.2f/255, colours max %.2f/255" % (what, e[grey].max(), e.max()))
    cpu = applied.get(label, [])
    if len(cpu) == len(rows) and all(v is not None for _, v in cpu):
        # apply_rgb reçoit k/255 en float, sans l'arrondi half de la cible : < 0.1/255 en sortie
        e = err255(got, np.array([v for _, v in cpu], dtype=np.float32))
        check(e.max() <= 0.5, "%s: GPU = CPU LUT, max %.2f/255" % (label, e.max()))
    else:
        check(False, "%s: CPU LUT values missing" % label)

sys.exit(1 if fails else 0)
