# Lot A6 : validation de la chaîne couleur contre des outils indépendants d'ArmorPaint. Des mires (rampe sRGB,
# ColorChecker sRGB, valeurs HDR > 1, ColorChecker ACEScg) passent par l'import, un calque et l'export studio ; les
# fichiers écrits sont comparés à oiiotool --colorconvert (OpenImageIO, son propre OCIO) sur les mêmes entrées. Le
# viewport (LUT de studio_ocio_view) est comparé à oiiotool --ociodisplay exact et à une LUT baked par ociobakelut.
# Nuke, s'il est là, relit l'export ACEScg (aller-retour) et refait la transformation d'affichage.
#
#   python ocio_validation.py make <dossier>          mires, maillage, preset, scripts ArmorPaint et Nuke
#   python ocio_validation.py check <dossier> [passe…] compare, écrit <dossier>/report.md ; code 1 hors tolérance
#   python ocio_validation.py upstream <dossier>       export upstream --export-textures exr16/exr32 contre png
#
# Les tolérances sont en tête de fichier ; report.md donne les écarts mesurés.

import os
import subprocess
import sys

import numpy as np
import OpenEXR
from PIL import Image

CONFIG = "ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5"  # configuration intégrée d'ArmorPaint (studio_ocio)
NUKE_CONFIG = "fn-nuke_studio-config-v3.0.0_aces-v2.0_ocio-v2.4"  # la même famille, livrée avec Nuke 17 (OCIO 2.4)
WORK = "Linear Rec.709 (sRGB)"  # espace de travail par défaut
DISPLAY, VIEW = "sRGB - Display", "ACES 2.0 - SDR 100 nits (Rec.709)"

# Textures du preset d'export : nom → espace (« linear » : l'espace de travail, voir README, Export).
EXPORTS = {"lin": "linear", "acescg": "ACEScg", "srgb": "sRGB - Texture", "p3": "Linear P3-D65"}

# ColorChecker 24 en sRGB 8 bits (valeurs BabelColor, D65), rangées de haut en bas.
CHECKER = [
    (115, 82, 68), (194, 150, 130), (98, 122, 157), (87, 108, 67), (133, 128, 177), (103, 189, 170),
    (214, 126, 44), (80, 91, 166), (193, 90, 99), (94, 60, 108), (157, 188, 64), (224, 163, 46),
    (56, 61, 150), (70, 148, 73), (175, 54, 60), (231, 199, 31), (187, 86, 149), (8, 133, 161),
    (243, 243, 242), (200, 200, 200), (160, 160, 160), (122, 122, 121), (85, 85, 85), (52, 52, 52),
]  # fmt: skip

# Mires : fichier, espace source (choisi par script avant l'import), grille (colonnes, rangées), taille d'un pas.
MIRES = {
    "ramp": ("ramp_srgb.png", "sRGB - Texture", (256, 1), 8),
    "checker": ("checker_srgb.png", "sRGB - Texture", (6, 4), 32),
    "hdr": ("hdr_lin.exr", WORK, (8, 4), 32),
    "aces": ("checker_acescg.exr", "ACEScg", (6, 4), 32),
    "gamut": ("gamut_acescg.exr", "ACEScg", (4, 2), 32),
}
LDR = ("ramp", "checker")  # sources 8 bits
# Couleurs ACEScg hors du gamut Rec.709 : leurs composantes négatives en Rec.709 linéaire ne tiennent pas dans un calque
# (couleur de base stockée en gamma 2.2, sans signe). Exactes seulement si l'espace de travail est ACEScg.
GAMUT = [(1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0), (0.02, 0.6, 0.9), (0.9, 0.7, 0.02), (0.6, 0.05, 0.7), (4.0, 0.2, 0.1), (0.5, 0.1, 0.05)]

# Passes ArmorPaint : profondeur des calques, espace de travail (STUDIO_OCIO_WORKING_SPACE pour la passe ACEScg).
RUNS = {"32": (32, WORK), "16": (16, WORK), "8": (8, WORK), "32aces": (32, "ACEScg")}

# Tolérances. LDR : écart après la courbe sRGB, en /255 (la texture 8 bits importée est ré-encodée en gamma 2.2 sur
# 8 bits : un demi-pas de quantification). Flottant : écart relatif ; en 32 bits, l'aller-retour gamma 2.2 de la
# couleur de base sur le GPU (pow rapide de Metal, ~8e-5) et le processeur CPU optimisé d'OCIO, jusqu'à 4,7e-4 sur
# les négatifs des couleurs hors gamut passées par la courbe sRGB ; en 16 bits, le half du calque (2^-11 × 2,2 de la
# courbe). Viewport : précision de la LUT mesurée au lot A2 (README, Viewport).
# Espace de travail ACEScg : la texture 8 bits est quantifiée en primaires AP1 ; revenue en sRGB, l'erreur passe par
# la matrice AP1 → Rec.709 et la pente de la courbe sRGB près du noir (canal R du cyan du ColorChecker : ~5/255).
TOL_LDR = {"32": 1.0, "16": 1.0, "8": 1.0, "32aces": 6.0}
TOL_FLOAT = {32: 1e-3, 16: 2e-3}  # sans calque 8 bits : pas de flottant
TOL_VIEW_MAX, TOL_VIEW_GREY = 9.0, 0.3
TOL_NUKE_ROUNDTRIP = 1.0  # /255, source 8 bits relue par Nuke après export ACEScg
TOL_NUKE_DISPLAY = 0.5  # /255, ACES 2.0 par OCIO 2.4 + config v3 contre OCIO 2.5 + config v4 (même transformation)


# ── mires ────────────────────────────────────────────────────────────────────────────────────────────────────────


def grid(values, cols, rows, step):
    # values : cols × rows couleurs (rangée par rangée, de haut en bas) → image (rows·step, cols·step, 3)
    a = np.asarray(values, np.float32).reshape(rows, cols, 3)
    return np.repeat(np.repeat(a, step, 0), step, 1)


def hdr_values():
    # Rangée 1-2 : gris 0,18 × 2^k de -6 à +9 diaphs (0,0028 → 92). Rangées 3-4 : couleurs saturées et spéculaires.
    greys = [(0.18 * 2.0**k,) * 3 for k in range(-6, 10)]
    colors = [
        (4.0, 0.5, 0.1), (0.1, 2.0, 8.0), (16.0, 16.0, 0.5), (30.0, 2.0, 2.0), (0.2, 12.0, 0.3), (1.5, 1.2, 1.0),
        (64.0, 48.0, 40.0), (0.9, 0.05, 0.02), (0.02, 0.9, 0.05), (0.05, 0.02, 0.9), (2.5, 0.0, 2.5), (0.0, 3.0, 3.0),
        (0.5, 0.25, 0.125), (8.0, 4.0, 1.0), (1.0, 1.0, 1.0), (0.0, 0.0, 0.0),
    ]  # fmt: skip
    return greys + colors


def write_exr(path, img):
    img = np.ascontiguousarray(img, np.float32)
    channels = {c: np.ascontiguousarray(img[..., i]) for i, c in enumerate("RGB")}
    OpenEXR.File({"compression": OpenEXR.ZIP_COMPRESSION, "type": OpenEXR.scanlineimage}, channels).write(path)


def read_exr(path):
    with OpenEXR.File(path, separate_channels=True) as f:
        ch = f.channels()
        return np.stack([np.asarray(ch[c].pixels, np.float32) for c in "RGB"], -1)


def read_image(path):
    if path.endswith(".exr"):
        return read_exr(path)
    return np.asarray(Image.open(path).convert("RGB"), np.float32) / 255.0


def ocio_processor(src, dst=None, display=None, view=None):
    import PyOpenColorIO as OCIO

    cfg = OCIO.Config.CreateFromFile(CONFIG)
    if display:
        t = OCIO.DisplayViewTransform(src=src, display=display, view=view)
        return cfg.getProcessor(t).getDefaultCPUProcessor()
    return cfg.getProcessor(src, dst).getDefaultCPUProcessor()


def make(d):
    os.makedirs(d, exist_ok=True)
    ramp = grid([(i / 255.0,) * 3 for i in range(256)], 256, 1, 8)
    Image.fromarray(np.round(ramp * 255).astype(np.uint8)).save(f"{d}/ramp_srgb.png")
    checker = grid([tuple(c / 255.0 for c in p) for p in CHECKER], 6, 4, 32)
    Image.fromarray(np.round(checker * 255).astype(np.uint8)).save(f"{d}/checker_srgb.png")
    write_exr(f"{d}/hdr_lin.exr", grid(hdr_values(), 8, 4, 32))
    aces = np.ascontiguousarray(checker, np.float32)
    ocio_processor("sRGB - Texture", "ACEScg").applyRGB(aces)  # sur place
    write_exr(f"{d}/checker_acescg.exr", aces)
    write_exr(f"{d}/gamut_acescg.exr", grid(GAMUT, 4, 2, 32))

    # Plan d'UV 0..1 : pixel (x, y) de la texture exportée = même point de la mire (orientation vérifiée par le banc).
    with open(f"{d}/plane.obj", "w") as f:
        f.write("v -1 -1 0\nv 1 -1 0\nv 1 1 0\nv -1 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\n")
        f.write("f 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/4/1\n")

    os.makedirs(f"{d}/presets", exist_ok=True)
    tex = ", ".join(
        f'{{ "name": "{n}", "channels": ["base_r", "base_g", "base_b"], "color_space": "{cs}" }}' for n, cs in EXPORTS.items()
    )
    with open(f"{d}/presets/a6.json", "w") as f:
        f.write(f'{{ "version": 1, "defaults": {{ "format": "exr", "bits": 32 }}, "textures": [ {tex} ] }}\n')

    for run in RUNS:
        write_script(d, run)
    write_nuke(d)


def view_inputs():
    # Couleurs de l'espace de travail envoyées à la LUT du viewport : rampe, ColorChecker (sRGB → linéaire), HDR.
    to_lin = ocio_processor("sRGB - Texture", WORK)
    ramp = [to_lin.applyRGB([i / 255.0] * 3) for i in range(256)]
    checker = [to_lin.applyRGB([c / 255.0 for c in p]) for p in CHECKER]
    return [("grey", c) for c in ramp] + [("color", c) for c in checker] + [("hdr", list(c)) for c in hdr_values()]


def write_script(d, run):
    bits = RUNS[run][0]
    lines = ["int main() {", f'\tconsole_log(string("t: set_bits=%d bits=%d", studio_project_set_bits({bits}), studio_project_bits()));']
    for i, (name, (file, cs, _, _)) in enumerate(MIRES.items()):
        lines.append(f'\tstudio_texture_set_color_space("{d}/{file}", "{cs}");')
        lines.append(f'\tscript_import_asset("{d}/{file}", 0);')
        lines.append(f'\tconsole_log(string("t: {name} cs=[%s]", studio_texture_color_space("{d}/{file}")));')
    lines += [
        '\tslot_material_t *m = script_material_create("a6");',
        '\tui_node_t *tex = script_material_create_node_at("TEX_IMAGE", -400.0, 0.0);',
        '\tui_node_t *out = script_material_get_node("OUTPUT_MATERIAL_PBR");',
        "\tscript_material_connect(tex, 0, out, 0);",
        "\tscript_material_set(m);",
    ]
    for i, name in enumerate(MIRES):
        lines += [
            f"\tscript_material_set_button(tex, 0, {i}.0);",
            "\tscript_material_update();",
            "\tscript_fill_layer();",
            f'\tconsole_log(string("t: export {name}=%d", studio_export_run("{d}/out{run}/{name}", "a6", 0)));',
        ]
    if run == "32":  # la LUT ne dépend pas de la profondeur des calques
        for i, (_, c) in enumerate(view_inputs()):
            lines.append(f'\tconsole_log(string("v {i} %s", studio_ocio_view_apply_rgb({c[0]:.9g}, {c[1]:.9g}, {c[2]:.9g})));')
    lines += ["\treturn 0;", "}"]
    with open(f"{d}/a6_{run}.c", "w") as f:
        f.write("\n".join(lines) + "\n")


def write_nuke(d):
    # Nuke : relit l'export ACEScg de la mire ColorChecker (Read en ACEScg, Write en sRGB - Texture, flottant) et
    # applique display/view aux entrées du viewport. Lancé par le banc : Nuke -t nuke_a6.py
    s = f'''import nuke
root = nuke.root()
root["colorManagement"].setValue("OCIO")
root["OCIO_config"].setValue("{NUKE_CONFIG}")
def write(node, path, colorspace):
    w = nuke.nodes.Write(file=path, file_type="exr", colorspace=colorspace)
    w["datatype"].setValue("32 bit float")
    w.setInput(0, node)
    nuke.execute(w, 1, 1)
read = nuke.nodes.Read(file="{d}/out32/checker/untitled_acescg.exr", colorspace="ACEScg")
write(read, "{d}/nuke/roundtrip_srgb.exr", "sRGB Encoded Rec.709 (sRGB)")
src = nuke.nodes.Read(file="{d}/view_in.exr", raw=True)
disp = nuke.nodes.OCIODisplay(display="{DISPLAY}", view="{VIEW}")
disp["colorspace"].setValue("{WORK}")
disp.setInput(0, src)
out = nuke.nodes.Write(file="{d}/nuke/view_out.exr", file_type="exr", raw=True)
out["datatype"].setValue("32 bit float")
out.setInput(0, disp)
nuke.execute(out, 1, 1)
print("nuke: OCIO", root["OCIO_config"].value())
'''
    os.makedirs(f"{d}/nuke", exist_ok=True)
    with open(f"{d}/nuke_a6.py", "w") as f:
        f.write(s)
    vin = np.asarray([c for _, c in view_inputs()], np.float32).reshape(1, -1, 3)
    write_exr(f"{d}/view_in.exr", vin)


# ── contrôle ─────────────────────────────────────────────────────────────────────────────────────────────────────


def oiiotool(*args):
    tool = os.path.join(os.path.dirname(sys.executable), "oiiotool")
    subprocess.run([tool, "--colorconfig", CONFIG, *args], check=True, capture_output=True)


def centers(img, cols, rows):
    # Moyenne du quart central de chaque case de la grille, rangée par rangée → (rows·cols, 3)
    h, w = img.shape[:2]
    out = []
    for j in range(rows):
        for i in range(cols):
            x0, x1 = int((i + 0.375) * w / cols), max(int((i + 0.625) * w / cols), int((i + 0.375) * w / cols) + 1)
            y0, y1 = int((j + 0.375) * h / rows), max(int((j + 0.625) * h / rows), int((j + 0.375) * h / rows) + 1)
            out.append(img[y0:y1, x0:x1].reshape(-1, 3).mean(0))
    return np.asarray(out)


def srgb_curve(v):
    v = np.clip(v, 0.0, 1.0)
    return np.where(v <= 0.0031308, 12.92 * v, 1.055 * np.power(v, 1 / 2.4) - 0.055)


def err_ldr(out, ref, encoded):
    # /255 après la courbe sRGB (une sortie déjà encodée est comparée telle quelle)
    if encoded:
        return np.abs(out - ref).max(-1) * 255
    return np.abs(srgb_curve(out) - srgb_curve(ref)).max(-1) * 255


def err_rel(out, ref):
    return (np.abs(out - ref) / np.maximum(np.abs(ref), 0.01)).max(-1)


class Report:
    def __init__(self):
        self.lines, self.failed = [], False

    def row(self, label, value, tol, unit):
        ok = bool(value <= tol)
        self.failed |= not ok
        self.lines.append(f"| {label} | {value:.3g} {unit} | ≤ {tol:g} | {'ok' if ok else '**hors tolérance**'} |")
        print(f"{'ok  ' if ok else 'FAIL'} {label}: {value:.3g} {unit} (≤ {tol:g})")

    def text(self, s):
        self.lines.append(s)


def check_exports(d, run, rep):
    bits, work = RUNS[run]
    rep.text(f"\n### Export, calques {bits} bits, espace de travail {work}\n\n| Mire → texture | Écart max | Tolérance | |\n|---|---|---|---|")
    os.makedirs(f"{d}/ref", exist_ok=True)
    gamut = None
    for name, (file, cs, (cols, rows), _) in MIRES.items():
        if bits == 8 and name not in LDR:
            continue  # calques 8 bits : pas de valeurs > 1 ni de flottants
        for tex, dst in EXPORTS.items():
            dst = work if dst == "linear" else dst
            ref_path = f"{d}/ref/{name}_{dst.replace(' ', '_')}.exr"
            if not os.path.exists(ref_path):
                oiiotool(f"{d}/{file}", "--ch", "R,G,B", "-d", "float", "--colorconvert", cs, dst, "-o", ref_path)
            out_path = f"{d}/out{run}/{name}/untitled_{tex}.exr"
            if not os.path.exists(out_path):
                rep.row(f"{name} → {tex} ({dst}) : fichier absent", float("inf"), 0, "")
                continue
            out, ref = centers(read_exr(out_path), cols, rows), centers(read_exr(ref_path), cols, rows)
            if name == "gamut" and work != "ACEScg":
                if tex == "acescg":  # limite connue, mesurée : composantes négatives perdues dans le calque
                    gamut = (out, ref)
                continue
            if name in LDR:
                rep.row(f"{name} → {tex} ({dst})", err_ldr(out, ref, dst == "sRGB - Texture").max(), TOL_LDR[run], "/255")
            else:
                rep.row(f"{name} → {tex} ({dst})", err_rel(out, ref).max(), TOL_FLOAT[bits], "rel.")
    if gamut is not None:
        out, ref = gamut
        rep.text(
            f"\nLimite (non comptée) : couleurs ACEScg hors du gamut Rec.709, relues en ACEScg ; rouge ACEScg (1, 0, 0) → "
            f"({out[0][0]:.3f}, {out[0][1]:.3f}, {out[0][2]:.3f}), écart relatif max {err_rel(out, ref).max():.3g}. Espace de travail ACEScg pour ces textures."
        )
        print(f"info gamut hors Rec.709, espace de travail {work} : écart relatif max {err_rel(out, ref).max():.3g} (limite connue)")


def check_view(d, rep):
    rep.text("\n### Viewport : LUT de studio_ocio_view contre OCIO exact\n\n| Entrées | Écart max | Tolérance | |\n|---|---|---|---|")
    got = {}
    with open(f"{d}/app32.log", errors="replace") as f:
        for line in f:
            p = line.split()
            if len(p) == 5 and p[0] == "v":
                got[int(p[1])] = [float(x) for x in p[2:]]
    kinds = [k for k, _ in view_inputs()]
    if len(got) != len(kinds):
        rep.row("viewport : sorties de studio_ocio_view_apply_rgb", float("inf"), 0, "")
        return
    lut = np.asarray([got[i] for i in range(len(kinds))])
    oiiotool(f"{d}/view_in.exr", "--iscolorspace", WORK, "--ociodisplay", DISPLAY, VIEW, "-o", f"{d}/ref/view_exact.exr")
    exact = read_exr(f"{d}/ref/view_exact.exr").reshape(-1, 3)

    # LUT d'affichage baked par ociobakelut comme en production : 65³ en entrée ACEScct (log, couvre le HDR ; un
    # shaper ACEScct est refusé, la matrice AP1 mélange les canaux), appliquée par OCIO (FileTransform) après conversion.
    bake = os.path.join(os.path.dirname(sys.executable), "ociobakelut")
    cube = f"{d}/ref/view_bake.cube"
    subprocess.run([bake, "--iconfig", CONFIG, "--inputspace", "ACEScct", "--displayview", DISPLAY, VIEW, "--format", "resolve_cube",
                    "--cubesize", "65", cube], check=True, capture_output=True)  # fmt: skip
    oiiotool(f"{d}/view_in.exr", "--colorconvert", WORK, "ACEScct", "--ociofiletransform", cube, "-o", f"{d}/ref/view_bake.exr")
    baked = read_exr(f"{d}/ref/view_bake.exr").reshape(-1, 3)

    k = np.asarray(kinds)
    e_lut, e_bake = np.abs(lut - exact).max(-1) * 255, np.abs(baked - exact).max(-1) * 255
    for kind, label in (("grey", "rampe de gris (256)"), ("color", "ColorChecker (24)"), ("hdr", "HDR, 0,0028 → 92 (32)")):
        tol = TOL_VIEW_GREY if kind == "grey" else TOL_VIEW_MAX
        rep.row(f"viewport, {label}", e_lut[k == kind].max(), tol, "/255")
    rep.text(
        f"\nLUT ociobakelut 65³ (entrée ACEScct) sur les mêmes entrées, pour comparaison : gris {e_bake[k == 'grey'].max():.3g}/255, "
        f"ColorChecker {e_bake[k == 'color'].max():.3g}/255, HDR {e_bake[k == 'hdr'].max():.3g}/255 ; LUT du viewport : gris "
        f"{e_lut[k == 'grey'].max():.3g}/255, ColorChecker {e_lut[k == 'color'].max():.3g}/255, HDR {e_lut[k == 'hdr'].max():.3g}/255."
    )
    return exact


def check_nuke(d, rep, exact):
    if not os.path.exists(f"{d}/nuke/roundtrip_srgb.exr"):
        rep.text("\n### Nuke\n\nNon lancé (Nuke absent ou sans licence) : aller-retour et affichage non vérifiés par Nuke.")
        print("skip Nuke")
        return
    rep.text("\n### Nuke 17 (OCIO 2.4, " + NUKE_CONFIG + ")\n\n| Contrôle | Écart max | Tolérance | |\n|---|---|---|---|")
    back = centers(read_exr(f"{d}/nuke/roundtrip_srgb.exr"), 6, 4)
    src = np.asarray(CHECKER, np.float32) / 255.0
    rep.row("aller-retour : ColorChecker sRGB → ArmorPaint → export ACEScg → Nuke → sRGB", (np.abs(back - src).max(-1) * 255).max(), TOL_NUKE_ROUNDTRIP, "/255")
    view = read_exr(f"{d}/nuke/view_out.exr").reshape(-1, 3)
    rep.row("affichage ACES 2.0 de Nuke contre oiiotool (toutes les entrées)", (np.abs(view - exact).max(-1) * 255).max(), TOL_NUKE_DISPLAY, "/255")


def check(d, runs):
    rep = Report()
    rep.text(f"## Validation OCIO (lot A6)\n\nConfiguration : `{CONFIG}`, affichage « {DISPLAY} » / « {VIEW} ».")
    for run in runs:
        check_exports(d, run, rep)
    exact = check_view(d, rep)
    if exact is not None:
        check_nuke(d, rep, exact)
    with open(f"{d}/report.md", "w") as f:
        f.write("\n".join(rep.lines) + "\n")
    return 1 if rep.failed else 0


def check_upstream(d):
    # Patch metal-target-format : l'export upstream en EXR aplatit les calques dans des images RGBA64/RGBA128 avec des
    # pipelines compilés en RGBA32 (valeurs aberrantes sur Metal sans le patch). Référence : l'export PNG, 8 bits.
    failed = False
    for t in ("base", "nor", "occ", "rough", "metal"):
        png = np.asarray(Image.open(f"{d}/up_png/untitled_{t}.png").convert("RGB"), np.float32) / 255.0
        for bits in (16, 32):
            path = f"{d}/up_exr{bits}/untitled_{t}.exr"
            if not os.path.exists(path):
                print(f"FAIL export upstream exr{bits} {t} : fichier absent")
                failed = True
                continue
            with OpenEXR.File(path, separate_channels=True) as f:
                ch = f.channels()
                exr = np.stack([np.asarray(ch[c].pixels, np.float32) for c in ("RGB" if "R" in ch else "YYY")], -1)
            e = np.abs(exr - png).max() * 255 if np.isfinite(exr).all() else float("inf")
            ok = e <= 1.0
            failed |= not ok
            print(f"{'ok  ' if ok else 'FAIL'} export upstream exr{bits} {t} = png : {e:.3g}/255")
    return 1 if failed else 0


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "make":
        make(sys.argv[2])
    elif len(sys.argv) >= 3 and sys.argv[1] == "check":
        sys.exit(check(sys.argv[2], sys.argv[3:] or list(RUNS)))
    elif len(sys.argv) >= 3 and sys.argv[1] == "upstream":
        sys.exit(check_upstream(sys.argv[2]))
    else:
        print("usage: ocio_validation.py make|check|upstream <dossier> [passe…]")
        sys.exit(2)
