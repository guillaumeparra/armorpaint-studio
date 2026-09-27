#!/usr/bin/env python3
# OpenEXR minimal pour subzero (lot E6), bibliothèque standard seule. Qt ne lit pas l'EXR : la fenêtre s'en sert pour
# les vignettes des rendus finaux et pour séparer l'alpha des décalques. Scanlines, une seule partie, canaux non
# sous-échantillonnés ; half, float ou uint. Lecture sans compression, ZIPS ou ZIP ; les autres (RLE, PIZ, PXR24,
# B44…) lèvent Error. Écriture sans compression, ZIPS ou ZIP. read_planes et write_planes (lot E11, conversion OCIO
# des cartes) demandent numpy, importé à leur appel seulement. sbsrender écrit les couleurs en A, B, G, R et les gris en Y, avec les
# valeurs de ses sorties 8 bits (couleur de base encodée sRGB, normale en 0-1) : une vignette les prend telles quelles.

import itertools
import os
import struct
import zlib

MAGIC = b"\x76\x2f\x31\x01"
UINT, HALF, FLOAT = 0, 1, 2
PIXEL_SIZE = {UINT: 4, HALF: 2, FLOAT: 4}
PIXEL_CODE = {UINT: "I", HALF: "e", FLOAT: "f"}
NONE, ZIPS, ZIP = 0, 2, 3
CHUNK_LINES = {NONE: 1, ZIPS: 1, ZIP: 16}
COMPRESSION_NAMES = ["none", "rle", "zips", "zip", "piz", "pxr24", "b44", "b44a", "dwaa", "dwab", "htj2k256", "htj2k32"]


class Error(Exception):
    pass


class Image(object):
    """EXR ouvert : en-tête lu, table des chunks ; les lignes se lisent à la demande (line)."""

    def __init__(self, path):
        self.path = path
        self._f = open(path, "rb")
        try:
            self._read_header()
        except (struct.error, ValueError, IndexError, UnicodeDecodeError) as e:
            self._f.close()
            raise Error("%s: bad EXR header (%s)" % (os.path.basename(path), e))
        except Error:
            self._f.close()
            raise
        self._cache = (None, None)

    def close(self):
        self._f.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _read_header(self):
        data = self._f.read(1 << 16)
        name = os.path.basename(self.path)
        if data[:4] != MAGIC:
            raise Error("%s: not an OpenEXR file" % name)
        flags = struct.unpack_from("<I", data, 4)[0]
        if flags & 0x1a00:  # tuilé (0x200), profond (0x800), multipart (0x1000)
            raise Error("%s: tiled, deep or multipart EXR not supported" % name)
        pos, attrs = 8, {}
        while True:
            end = data.index(b"\0", pos)
            key = data[pos:end].decode("ascii")
            pos = end + 1
            if not key:
                break
            end = data.index(b"\0", pos)
            kind = data[pos:end].decode("ascii")
            size = struct.unpack_from("<i", data, end + 1)[0]
            pos = end + 5
            if pos + size > len(data):
                raise Error("%s: header too large" % name)
            attrs[key] = (kind, data[pos:pos + size])
            pos += size
        channels, raw, p = [], attrs["channels"][1], 0
        while raw[p] != 0:
            end = raw.index(b"\0", p)
            ptype, _, xs, ys = struct.unpack_from("<iB3xii", raw, end + 1)
            if ptype not in PIXEL_SIZE or xs != 1 or ys != 1:
                raise Error("%s: channel %s: unsupported type or sampling" % (name, raw[p:end].decode()))
            channels.append((raw[p:end].decode("utf-8"), ptype))
            p = end + 17
        x0, y0, x1, y1 = struct.unpack("<4i", attrs["dataWindow"][1])
        self.compression = attrs["compression"][1][0]
        if self.compression not in CHUNK_LINES:
            label = COMPRESSION_NAMES[self.compression] if self.compression < len(COMPRESSION_NAMES) else "?"
            raise Error("%s: %s compression not supported (none, zips, zip)" % (name, label))
        self.channels = channels  # [(nom, type)], dans l'ordre du fichier (alphabétique)
        self.width, self.height = x1 - x0 + 1, y1 - y0 + 1
        self.y0 = y0
        self.line_bytes = sum(self.width * PIXEL_SIZE[t] for _, t in channels)
        self.lines = CHUNK_LINES[self.compression]
        count = (self.height + self.lines - 1) // self.lines
        self._f.seek(pos)
        table = self._f.read(8 * count)
        if len(table) < 8 * count:
            raise Error("%s: truncated offset table" % name)
        self.offsets = struct.unpack("<%dQ" % count, table)
        self._channel_at = {}
        off = 0
        for c, t in channels:
            self._channel_at[c] = (off, t)
            off += self.width * PIXEL_SIZE[t]

    def chunk(self, index):
        """Octets décompressés du chunk (lignes à la suite, canaux à plat dans chaque ligne)."""
        if self._cache[0] == index:
            return self._cache[1]
        self._f.seek(self.offsets[index])
        y, size = struct.unpack("<ii", self._f.read(8))
        data = self._f.read(size)
        lines = min(self.lines, self.height - index * self.lines)
        expected = lines * self.line_bytes
        if len(data) < size or y != self.y0 + index * self.lines:
            raise Error("%s: bad chunk %d" % (os.path.basename(self.path), index))
        if self.compression != NONE and size < expected:
            data = _unzip(data, expected, self.path)
        if len(data) != expected:
            raise Error("%s: chunk %d: %d bytes, %d expected" % (os.path.basename(self.path), index, len(data), expected))
        self._cache = (index, data)
        return data

    def line(self, y):
        """Octets de la ligne y (0 : en haut), canaux à plat."""
        data = self.chunk(y // self.lines)
        start = (y % self.lines) * self.line_bytes
        return data[start:start + self.line_bytes]

    def channel_bytes(self, line, name):
        off, t = self._channel_at[name]
        return line[off:off + self.width * PIXEL_SIZE[t]]

    def values(self, line, name, step=1):
        """Valeurs du canal name dans une ligne (octets de line), une sur step."""
        off, t = self._channel_at[name]
        return struct.unpack_from("<%d%s" % (self.width, PIXEL_CODE[t]), line, off)[::step]

    def has(self, name):
        return name in self._channel_at


def _unzip(data, expected, path):
    """ZIP/ZIPS d'OpenEXR : zlib, puis prédicteur (t[i] = t[i-1] + d[i] - 128) et octets entrelacés."""
    try:
        d = zlib.decompress(data)
    except zlib.error as e:
        raise Error("%s: bad zip chunk (%s)" % (os.path.basename(path), e))
    n = len(d)
    if n != expected:
        return d
    # t[i] = d[0] + Σ(d[1..i]) - 128·i : la somme cumulée mod 256, et 128 de plus (bit 7 inversé) aux rangs impairs.
    acc = bytes(map((255).__and__, itertools.accumulate(d)))
    mask = int.from_bytes(b"\x00\x80" * (n // 2) + b"\x00" * (n % 2), "little")
    t = (int.from_bytes(acc, "little") ^ mask).to_bytes(n, "little")
    out = bytearray(n)
    half = (n + 1) // 2
    out[0::2] = t[:half]
    out[1::2] = t[half:]
    return bytes(out)


def _header(width, height, channels, compression):
    def attr(name, kind, value):
        return name.encode() + b"\0" + kind.encode() + b"\0" + struct.pack("<i", len(value)) + value

    chlist = b"".join(n.encode("utf-8") + b"\0" + struct.pack("<iB3xii", t, 0, 1, 1) for n, t in channels) + b"\0"
    window = struct.pack("<4i", 0, 0, width - 1, height - 1)
    return MAGIC + struct.pack("<I", 2) + b"".join([
        attr("channels", "chlist", chlist),
        attr("compression", "compression", bytes([compression])),
        attr("dataWindow", "box2i", window),
        attr("displayWindow", "box2i", window),
        attr("lineOrder", "lineOrder", bytes([0])),
        attr("pixelAspectRatio", "float", struct.pack("<f", 1.0)),
        attr("screenWindowCenter", "v2f", struct.pack("<2f", 0.0, 0.0)),
        attr("screenWindowWidth", "float", struct.pack("<f", 1.0)),
    ]) + b"\0"


def _write_chunks(path, header, chunks):
    """chunks : [(y du premier rang, octets déjà compressés)] dans l'ordre. Fichier temporaire, puis renommé."""
    table, pos = [], len(header) + 8 * len(chunks)
    for _, data in chunks:
        table.append(pos)
        pos += 8 + len(data)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(header)
        f.write(struct.pack("<%dQ" % len(table), *table))
        for y, data in chunks:
            f.write(struct.pack("<ii", y, len(data)))
            f.write(data)
    os.replace(tmp, path)


def write(path, width, height, channels, lines):
    """EXR sans compression. channels : [(nom, type)] dans l'ordre alphabétique ; lines : une ligne par y (du haut),
    canaux à plat dans cet ordre. Écrit dans un fichier temporaire, puis renommé."""
    channels = sorted(channels)
    line_bytes = sum(width * PIXEL_SIZE[t] for _, t in channels)
    chunks = []
    for y, line in enumerate(lines):
        if len(line) != line_bytes:
            raise Error("line %d: %d bytes, %d expected" % (y, len(line), line_bytes))
        chunks.append((y, bytes(line)))
    _write_chunks(path, _header(width, height, channels, NONE), chunks)


# ---- Plans numpy (lot E11) ------------------------------------------------------------------------------------------

NUMPY_TYPE = {UINT: "<u4", HALF: "<f2", FLOAT: "<f4"}


def read_planes(path):
    """Image entière : (largeur, hauteur, {canal: tableau numpy (hauteur, largeur) du type du fichier}, {canal: type},
    compression)."""
    import numpy as np
    with Image(path) as im:
        data = b"".join(im.chunk(i) for i in range(len(im.offsets)))
        rows = np.frombuffer(data, np.uint8).reshape(im.height, im.line_bytes)
        planes = {}
        for name, t in im.channels:
            off = im._channel_at[name][0]
            size = im.width * PIXEL_SIZE[t]
            planes[name] = np.ascontiguousarray(rows[:, off:off + size]).view(NUMPY_TYPE[t])
        return im.width, im.height, planes, dict(im.channels), im.compression


def _zip(raw, level=4):
    """Inverse de _unzip : octets pairs puis impairs, différences + 128, zlib. Rend raw tel quel si la compression
    n'y gagne rien (OpenEXR le relit à sa taille)."""
    import numpy as np
    t = np.frombuffer(raw, np.uint8)
    inter = np.concatenate([t[0::2], t[1::2]])
    d = np.empty_like(inter)
    if len(inter):
        d[0] = inter[0]
        d[1:] = (inter[1:].astype(np.int16) - inter[:-1].astype(np.int16) + 128).astype(np.uint8)
    packed = zlib.compress(d.tobytes(), level)
    return packed if len(packed) < len(raw) else raw


def write_planes(path, planes, types, compression=NONE):
    """EXR depuis des plans numpy (hauteur, largeur) : {canal: tableau}, {canal: type} ; valeurs converties au type.
    compression : NONE, ZIPS ou ZIP."""
    import numpy as np
    if compression not in CHUNK_LINES:
        raise Error("%s: cannot write %s compression" % (os.path.basename(path), COMPRESSION_NAMES[compression]))
    channels = sorted((n, types[n]) for n in planes)
    height, width = next(iter(planes.values())).shape
    rows = np.concatenate([np.ascontiguousarray(planes[n], dtype=NUMPY_TYPE[t]).view(np.uint8).reshape(height, -1)
                           for n, t in channels], axis=1)
    step = CHUNK_LINES[compression]
    chunks = []
    for y in range(0, height, step):
        raw = rows[y:y + step].tobytes()
        chunks.append((y, raw if compression == NONE else _zip(raw)))
    _write_chunks(path, _header(width, height, channels, compression), chunks)


def _to8(values):
    return bytes(0 if not v > 0.0 else 255 if v >= 1.0 else int(v * 255.0 + 0.5) for v in values)


def thumbnail(path, size, floats=False):
    """Vignette (largeur, hauteur, "gray" ou "rgb", octets 8 bits) d'au plus size pixels de côté, un pixel sur n.
    floats : une vignette "rgb" rend ses valeurs telles quelles (liste de flottants R, G, B), pour une transformation
    de couleur (subzero_color)."""
    with Image(path) as im:
        step = max(1, -(-max(im.width, im.height) // size))
        color = [c for c in ("R", "G", "B") if im.has(c)]
        gray = "Y" if im.has("Y") else color[0] if len(color) < 3 and color else None
        if gray is None and len(color) < 3:
            raise Error("%s: no Y or R, G, B channels" % os.path.basename(path))
        rows = []
        for y in range(0, im.height, step):
            line = im.line(y)
            if gray is not None:
                rows.append(_to8(im.values(line, gray, step)))
            elif floats:
                r, g, b = (im.values(line, c, step) for c in ("R", "G", "B"))
                row = [0.0] * (len(r) * 3)
                row[0::3], row[1::3], row[2::3] = r, g, b
                rows.append(row)
            else:
                r, g, b = (_to8(im.values(line, c, step)) for c in ("R", "G", "B"))
                row = bytearray(len(r) * 3)
                row[0::3], row[1::3], row[2::3] = r, g, b
                rows.append(bytes(row))
        width = len(range(0, im.width, step))
        if gray is None and floats:
            return width, len(rows), "rgb", [v for row in rows for v in row]
        return width, len(rows), "gray" if gray is not None else "rgb", b"".join(rows)


def split_alpha(base, opacity=None):
    """Couleur de base EXR (A, B, G, R) dont l'alpha n'est pas partout opaque : réécrite en B, G, R sans compression ;
    l'alpha va dans opacity (canal Y) s'il est donné. Vrai si réécrite."""
    try:
        im = Image(base)
    except (OSError, Error):
        return False
    with im:
        if not (im.has("A") and im.has("R") and im.has("G") and im.has("B")):
            return False
        types = dict(im.channels)
        one = struct.pack("<" + PIXEL_CODE[types["A"]], 1) * im.width
        lines = [im.line(y) for y in range(im.height)]
        alphas = [im.channel_bytes(line, "A") for line in lines]
        if all(a == one for a in alphas) or all(min(im.values(line, "A")) >= 1.0 - 0.5 / 255 for line in lines):
            return False
        rgb = [(c, types[c]) for c in ("B", "G", "R")]
        width, height = im.width, im.height
        if opacity:
            write(opacity, width, height, [("Y", types["A"])], alphas)
        write(base, width, height, rgb, (b"".join(im.channel_bytes(line, c) for c, _ in rgb) for line in lines))
    return True
