# Banc du lot A4 (ocio_import_app.sh) : textures importées converties vers l'espace de travail. Écrit les images de
# test avec, à côté de chacune, ce que le décodeur d'ArmorPaint doit en tirer (<image>.npy, RGBA), puis compare les
# pixels relus de l'app (studio_texture_dump) à PyOpenColorIO.
#
#   python ocio_import_check.py make <dossier>
#   python ocio_import_check.py convert <dump> <image> <espace source>   pixels = PyOpenColorIO, puis encodage d'ArmorPaint
#   python ocio_import_check.py same <dump> <image>                       pixels intacts (donnée, espace de travail)
#   python ocio_import_check.py differ <dump> <image>                     pixels changés (au moins un canal)
# --opaque (convert, same) : seulement les pixels opaques de la source (le paquet d'assets du .arm les redessine en
# RGBA 8 bits avec mélange alpha).
#
# Références : 8 bits → v / 255, transformation de l'espace source vers « Linear Rec.709 (sRGB) », encodage gamma 2.2
# arrondi (round(x^(1/2.2) * 255), borné, NaN → 0) ; half et float → transformation en float32, valeurs non finies →
# 0, half bornés à ±65504. L'alpha n'est jamais touché. Tolérances : 1 pas en 8 bits (99 % exacts demandés), 1 ulp en
# half, 2e-6 relatif en float (le shim et la roue PyOpenColorIO ne sont pas compilés pareil).

import os
import struct
import sys

import numpy as np

WORKING = "Linear Rec.709 (sRGB)"
FORMATS = {0: np.uint8, 1: np.float16, 2: np.float32}  # GPU_TEXTURE_FORMAT_RGBA32, RGBA64, RGBA128


def config():
    import PyOpenColorIO as OCIO

    return OCIO.Config.CreateFromFile(os.environ.get("OCIO") or "ocio://studio-config-latest")


def save(path, decoded, write):
    write(path)
    np.save(path + ".npy", decoded)


def rgbe(path, rgb):
    # Radiance sans RLE (lignes plates : stb_image les accepte si le premier pixel n'est pas 2 2 x<128).
    h, w, _ = rgb.shape
    out = bytearray(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n" % (h, w))
    decoded = np.zeros((h, w, 4), np.float32)
    decoded[..., 3] = 1.0
    for y in range(h):
        for x in range(w):
            v = float(max(rgb[y, x]))
            if v < 1e-32:
                out += bytes(4)
                continue
            m, e = np.frexp(v)
            scale = float(m) * 256.0 / v
            px = [int(c * scale) for c in rgb[y, x]]
            out += bytes(px + [int(e) + 128])
            decoded[y, x, :3] = [c * 2.0 ** (int(e) - 8) for c in px]  # stbi__hdr_convert : c * 2^(e - 136)
    with open(path, "wb") as f:
        f.write(out)
    np.save(path + ".npy", decoded)


def psd(path, composite, layers=()):
    # PSD RGB 8 bits, données brutes ; layers : [(nom, rgba)], le premier calque est la texture du fichier.
    h, w, _ = composite.shape
    out = bytearray(b"8BPS" + struct.pack(">H", 1) + bytes(6) + struct.pack(">HIIHH", 3, h, w, 8, 3))
    out += struct.pack(">II", 0, 0)  # mode de couleur, ressources
    if layers:
        records, data = bytearray(), bytearray()
        for name, rgba in layers:
            chans = [(-1, rgba[..., 3]), (0, rgba[..., 0]), (1, rgba[..., 1]), (2, rgba[..., 2])]
            records += struct.pack(">iiiiH", 0, 0, h, w, len(chans))
            for cid, plane in chans:
                records += struct.pack(">hI", cid, 2 + plane.size)
            n = name.encode()
            pascal = bytes([len(n)]) + n
            pascal += bytes((4 - len(pascal) % 4) % 4)
            extra = struct.pack(">II", 0, 0) + pascal  # masque, plages de fusion, nom
            records += b"8BIMnorm" + bytes([255, 0, 0, 0]) + struct.pack(">I", len(extra)) + extra
            for cid, plane in chans:
                data += struct.pack(">H", 0) + np.ascontiguousarray(plane, np.uint8).tobytes()
        info = struct.pack(">h", len(layers)) + records + data
        info += bytes(len(info) % 2)
        section = struct.pack(">I", len(info)) + info + struct.pack(">I", 0)
        out += struct.pack(">I", len(section)) + section
    else:
        out += struct.pack(">I", 0)
    out += struct.pack(">H", 0)
    for c in range(3):
        out += np.ascontiguousarray(composite[..., c], np.uint8).tobytes()
    with open(path, "wb") as f:
        f.write(out)


def make(d):
    import OpenEXR
    import tifffile
    from PIL import Image

    i = np.arange(256)
    ramp = np.zeros((4, 256, 4), np.uint8)
    ramp[0] = np.stack([i, 255 - i, (i * 37) % 256, 255 - i], -1)
    ramp[1] = np.stack([i, i, i, i], -1)
    ramp[2] = np.stack([(i * 91) % 256, (i * 53 + 7) % 256, (i * 13 + 100) % 256, (i * 29) % 256], -1)
    ramp[3] = np.stack([(i * 7) % 256, (255 - i * 3) % 256, (i * i) % 256, np.full(256, 255)], -1)
    for name in ("ramp.png", "ramp_copy.png", "browse.png", "move.png"):
        save(os.path.join(d, name), ramp, lambda p: Image.fromarray(ramp, "RGBA").save(p))
    rng = np.random.default_rng(7)
    noise = rng.integers(0, 256, (16, 16, 4), dtype=np.uint8)
    for name in ("wood_normal.png", "pre.png"):
        save(os.path.join(d, name), noise, lambda p: Image.fromarray(noise, "RGBA").save(p))

    # 8 bits RGB sans alpha : TIFF (io_tiff) et PSD à plat (io_psd), alpha à 255.
    rgb = ramp[:, :, :3].copy()
    opaque = np.concatenate([rgb, np.full((4, 256, 1), 255, np.uint8)], -1)
    save(os.path.join(d, "photo.tif"), opaque, lambda p: tifffile.imwrite(p, rgb, photometric="rgb", compression=None))
    save(os.path.join(d, "flat.psd"), opaque, lambda p: psd(p, rgb))
    # Deux calques : « base » est la texture du fichier, « top » une texture à part (fichier.top.png).
    top = ramp[:, ::-1].copy()
    psd(os.path.join(d, "layered.psd"), rgb, [("base", ramp), ("top", top)])
    np.save(os.path.join(d, "layered.psd.npy"), ramp)
    np.save(os.path.join(d, "layered.psd.top.png.npy"), top)

    # EXR : valeurs HDR, négatives, petites, non finies ; float et half. Règle EXR de la configuration → ACES2065-1.
    vals = np.array([0.0, 0.18, 1.0, 16.0, 100.0, -0.1, 1e-3, 0.5, 2.5, 7.0, 0.02, 60000.0, np.nan, np.inf, -np.inf, 0.75],
                    np.float32)
    plate = np.zeros((8, 16, 4), np.float32)
    for y in range(8):
        plate[y, :, 0] = np.roll(vals, y)
        plate[y, :, 1] = np.roll(vals, 3 * y + 1)
        plate[y, :, 2] = np.roll(vals, 5 * y + 2)
        plate[y, :, 3] = np.linspace(0.0, 1.0, 16, dtype=np.float32) * (y + 1) / 8
    plate[0, 0, 3] = 1.0

    def exr(p, a):
        header = {"compression": OpenEXR.ZIP_COMPRESSION, "type": OpenEXR.scanlineimage}
        with OpenEXR.File(header, {"RGBA": a}) as f:
            f.write(p)

    save(os.path.join(d, "plate.exr"), plate, lambda p: exr(p, plate))
    half = plate.astype(np.float16)
    save(os.path.join(d, "plate_half.exr"), half, lambda p: exr(p, half))

    # .hdr : décodé en half par ArmorPaint, défaut Linear Rec.709 (sRGB) = espace de travail : rien à convertir.
    sky = np.zeros((4, 8, 3), np.float32)
    sky[..., 0] = np.array([0.5, 2.0, 0.01, 10.0, 0.18, 1.0, 3.0, 0.3])[None, :] * np.array([1, 2, 0.5, 4])[:, None]
    sky[..., 1] = 0.25
    sky[..., 2] = np.linspace(0.05, 5.0, 32).reshape(4, 8)
    rgbe(os.path.join(d, "sky.hdr"), sky)


def dump(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:4] == b"STX1", path
    w, h, fmt = struct.unpack("<iii", data[4:16])
    return np.frombuffer(data[16:], FORMATS[fmt]).reshape(h, w, 4), fmt


def reference(image):
    return np.load(image + ".npy")


def encode_u8(x):
    x = np.nan_to_num(x.astype(np.float64), nan=0.0, posinf=1.0, neginf=0.0)
    return np.floor(np.clip(x, 0.0, 1.0) ** (1 / 2.2) * 255 + 0.5).astype(np.int64)


def expected(src, fmt, space):
    import PyOpenColorIO as OCIO  # noqa: F401

    cpu = config().getProcessor(space, WORKING).getDefaultCPUProcessor()
    px = src.astype(np.float32) / 255.0 if fmt == 0 else src.astype(np.float32)
    out = np.ascontiguousarray(px.copy())
    cpu.applyRGBA(out)
    out[..., 3] = px[..., 3]
    if fmt == 0:
        return encode_u8(out[..., :3])
    rgb = np.where(np.isfinite(out[..., :3]), out[..., :3], 0.0)
    if fmt == 1:
        return np.clip(rgb, -65504.0, 65504.0).astype(np.float16)
    return rgb.astype(np.float32)


def half_ulps(a, b):
    ia = a.astype(np.float16).view(np.int16).astype(np.int64)
    ib = b.astype(np.float16).view(np.int16).astype(np.int64)
    ia = np.where(ia < 0, -(ia & 0x7FFF), ia)  # ordre des flottants signés
    ib = np.where(ib < 0, -(ib & 0x7FFF), ib)
    return np.abs(ia - ib)


def main():
    cmd = sys.argv[1]
    if cmd == "make":
        make(sys.argv[2])
        return 0
    args = [a for a in sys.argv[2:] if a != "--opaque"]
    got, fmt = dump(args[0])
    src = reference(args[1])
    if "--opaque" in sys.argv:
        rows = np.all(src[..., 3] == (255 if src.dtype == np.uint8 else 1.0), axis=1)
        got, src = got[rows], src[rows]
    if got.shape != src.shape:
        print("shape %s, expected %s" % (got.shape, src.shape))
        return 1
    if src.dtype == np.uint8 and fmt != 0 or src.dtype != np.uint8 and fmt == 0:
        print("format %d for a %s image" % (fmt, src.dtype))
        return 1
    ref_alpha = src[..., 3].astype(np.float16) if fmt == 1 else src[..., 3]
    alpha_ok = np.array_equal(got[..., 3], ref_alpha, equal_nan=True)
    if cmd == "same":
        ref = src.astype(np.float16) if fmt == 1 else src
        same = np.array_equal(got, ref, equal_nan=True) or (fmt == 1 and half_ulps(got, ref).max() <= 1)
        print("identical" if same else "differ (max %.6g)" % np.nanmax(np.abs(got.astype(np.float64) - ref.astype(np.float64))))
        return 0 if same else 1
    if cmd == "differ":
        changed = not np.array_equal(got[..., :3], src[..., :3].astype(got.dtype), equal_nan=True)
        print("changed" if changed else "unchanged")
        return 0 if changed and alpha_ok else 1
    ref = expected(src, fmt, args[2])
    rgb = got[..., :3]
    if fmt == 0:
        err = np.abs(rgb.astype(np.int64) - ref)
        exact = float(np.mean(err == 0))
        ok = err.max() <= 1 and exact >= 0.99
        print("8-bit: max %d step(s), %.2f%% exact, alpha %s" % (err.max(), exact * 100, "intact" if alpha_ok else "CHANGED"))
    elif fmt == 1:
        ulps = half_ulps(rgb, ref)
        ok = ulps.max() <= 1
        print("half: max %d ulp, alpha %s" % (ulps.max(), "intact" if alpha_ok else "CHANGED"))
    else:
        err = np.abs(rgb.astype(np.float64) - ref.astype(np.float64))
        ok = bool(np.all(err <= 1e-7 + 2e-6 * np.abs(ref.astype(np.float64))))
        print("float: max error %.3g, alpha %s" % (err.max(), "intact" if alpha_ok else "CHANGED"))
    return 0 if ok and alpha_ok else 1


if __name__ == "__main__":
    sys.exit(main())
