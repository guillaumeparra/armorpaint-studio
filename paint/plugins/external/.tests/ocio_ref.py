# Référence indépendante du banc OCIO (lot A1) : PyOpenColorIO, la roue officielle d'OpenColorIO, compilée ailleurs que
# notre shim. Écrit, pour une configuration, tout ce que ocio_test.c doit retrouver : listes, rôles, alias, attributs
# des espaces, file_rules, et le résultat de transformations sur une grille de pixels.
#
#   python ocio_ref.py <config ou URI> <sortie>
#   python ocio_ref.py --make-config <dossier>   écrit la configuration de test (fichier, LUT, looks, file_rules)
#
# Format de sortie : une ligne par fait, champs séparés par des tabulations, flottants en %.9g (relus exactement).

import sys

import PyOpenColorIO as OCIO

# RGBA : noirs, gris moyen, blanc, HDR, négatifs (hors gamut), petites valeurs. L'alpha, différent pour chaque pixel,
# vérifie qu'il n'est pas touché.
PIXELS = [
    (0.0, 0.0, 0.0, 1.0),
    (0.18, 0.18, 0.18, 0.5),
    (1.0, 1.0, 1.0, 0.25),
    (16.0, 8.0, 2.0, 0.75),
    (-0.1, 0.5, 2.0, 0.0),
    (0.001, 0.01, 0.1, 1.0),
    (0.9, 0.2, 0.05, 0.125),
    (100.0, 0.0, 0.5, 0.875),
]

# Espaces de la configuration intégrée à retrouver dans chaque configuration (studio_ocio_lib_identify_builtin).
BUILTINS = ["lin_rec709_srgb", "srgb_tx", "g22_rec709_tx", "lin_ap1_scene", "no_such_builtin"]

FILE_PATHS = ["robot_basecolor.png", "robot_nor.exr", "robot_rough.tif", "plate.dpx", "noext", "a/b/c_diffuse.jpg"]


def f(v):
    return "%.9g" % v


def pixels_line(px):
    return " ".join(f(c) for p in px for c in p)


def apply(proc):
    cpu = proc.getDefaultCPUProcessor()
    return [tuple(cpu.applyRGBA(list(p))) for p in PIXELS]


def make_config(d):
    # Une LUT 3D 2x2x2 (inversion des canaux) : vérifie la résolution d'un FileTransform relatif au dossier de la config.
    with open(d + "/invert.cube", "w") as out:
        out.write("LUT_3D_SIZE 2\n")
        for b in (0, 1):
            for g in (0, 1):
                for r in (0, 1):
                    out.write("%d %d %d\n" % (1 - r, 1 - g, 1 - b))
    with open(d + "/config.ocio", "w") as out:
        out.write("""ocio_profile_version: 2.1

name: studio_test
description: Configuration du banc de test OCIO d'ArmorPaint
search_path: .

roles:
  scene_linear: lin
  data: raw
  default: srgb_tex
  color_picking: srgb_tex

file_rules:
  - !<Rule> {name: normals, colorspace: raw, pattern: "*_nor*", extension: "*"}
  - !<Rule> {name: exr, colorspace: lin, pattern: "*", extension: exr}
  - !<Rule> {name: Default, colorspace: default}

displays:
  sRGB:
    - !<View> {name: Standard, colorspace: srgb_tex}
    - !<View> {name: Inverted, colorspace: inverted}
    - !<View> {name: Raw, colorspace: raw}

looks:
  - !<Look>
    name: warm
    process_space: lin
    transform: !<CDLTransform> {slope: [1.2, 1.0, 0.8], power: [1, 1, 1]}

colorspaces:
  - !<ColorSpace>
    name: lin
    aliases: [linear_alias, lin_srgb]
    family: Test/Linear
    encoding: scene-linear
    isdata: false

  - !<ColorSpace>
    name: srgb_tex
    family: Test/Display
    encoding: sdr-video
    from_scene_reference: !<ExponentWithLinearTransform> {gamma: 2.4, offset: 0.055, direction: inverse}

  - !<ColorSpace>
    name: inverted
    encoding: sdr-video
    from_scene_reference: !<GroupTransform>
      children:
        - !<ExponentWithLinearTransform> {gamma: 2.4, offset: 0.055, direction: inverse}
        - !<FileTransform> {src: invert.cube, interpolation: linear}

  - !<ColorSpace>
    name: raw
    family: Test/Utility
    encoding: data
    isdata: true

  - !<ColorSpace>
    name: hidden
    encoding: scene-linear

inactive_colorspaces: [hidden]
""")


def main():
    if sys.argv[1] == "--make-config":
        make_config(sys.argv[2])
        return
    cfg = OCIO.Config.CreateFromFile(sys.argv[1])
    cfg.validate()
    out = []
    out.append("version\t" + OCIO.GetVersion())
    out.append("config_name\t" + cfg.getName())
    names = list(cfg.getColorSpaceNames(OCIO.SEARCH_REFERENCE_SPACE_ALL, OCIO.COLORSPACE_ACTIVE))
    for n in names:
        cs = cfg.getColorSpace(n)
        out.append("cs\t%s\t%d\t%s\t%s" % (n, 1 if cs.isData() else 0, cs.getEncoding(), cs.getFamily()))
        for a in cs.getAliases():
            out.append("resolve\t%s\t%s" % (a, n))
    for d in cfg.getDisplays():
        out.append("display\t" + d)
        out.append("default_view\t%s\t%s" % (d, cfg.getDefaultView(d)))
        for v in cfg.getViews(d):
            out.append("view\t%s\t%s" % (d, v))
    out.append("default_display\t" + cfg.getDefaultDisplay())
    for look in cfg.getLookNames():
        out.append("look\t" + look)
    for role, _ in cfg.getRoles():
        out.append("role\t%s" % role)
        out.append("resolve\t%s\t%s" % (role, cfg.getColorSpace(role).getName()))
    for p in FILE_PATHS:
        out.append("from_path\t%s\t%s" % (p, cfg.getColorSpaceFromFilepath(p)[0]))  # (espace, index de la règle)

    builtin = OCIO.Config.CreateFromFile("ocio://default")
    for b in BUILTINS:
        cs = cfg.getColorSpace(b)
        if cs is None:
            try:
                cs = cfg.getColorSpace(OCIO.Config.IdentifyBuiltinColorSpace(cfg, builtin, b))
            except OCIO.Exception:
                cs = None
        out.append("identify\t%s\t%s" % (b, cs.getName() if cs is not None else ""))

    out.append("pixels\t" + pixels_line(PIXELS))
    # Chaque espace vers et depuis scene_linear : chaque transformation de la configuration est parcourue.
    lin = cfg.getColorSpace("scene_linear").getName()
    for n in names:
        for src, dst in ((n, lin), (lin, n)):
            out.append("xform\t%s\t%s\t%s" % (src, dst, pixels_line(apply(cfg.getProcessor(src, dst)))))
    # Chaque display/view depuis scene_linear, aller et retour.
    for d in cfg.getDisplays():
        for v in cfg.getViews(d):
            for inv, direction in ((0, OCIO.TRANSFORM_DIR_FORWARD), (1, OCIO.TRANSFORM_DIR_INVERSE)):
                proc = cfg.getProcessor(lin, d, v, direction)
                out.append("display_xform\t%s\t%s\t%s\t%d\t\t%s" % (lin, d, v, inv, pixels_line(apply(proc))))
    # Avec un look : le pipeline « legacy » des applications hôtes, aller et retour.
    d = cfg.getDefaultDisplay()
    v = cfg.getDefaultView(d)
    for look in cfg.getLookNames():
        dvt = OCIO.DisplayViewTransform(src=lin, display=d, view=v)
        vp = OCIO.LegacyViewingPipeline()
        vp.setDisplayViewTransform(dvt)
        vp.setLooksOverrideEnabled(True)
        vp.setLooksOverride(look)
        fwd = vp.getProcessor(cfg)
        out.append("display_xform\t%s\t%s\t%s\t0\t%s\t%s" % (lin, d, v, look, pixels_line(apply(fwd))))
        group = fwd.createGroupTransform()
        group.setDirection(OCIO.TRANSFORM_DIR_INVERSE)
        inv = cfg.getProcessor(group)
        out.append("display_xform\t%s\t%s\t%s\t1\t%s\t%s" % (lin, d, v, look, pixels_line(apply(inv))))
    with open(sys.argv[2], "w") as fh:
        fh.write("\n".join(out) + "\n")


main()
