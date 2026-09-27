#!/usr/bin/env python3
# Lecteur 7z minimal pour subzero (chantier E0), bibliothèque standard seule (Python 3.8+). Repris tel quel de
# l'étude (recherche-live/l5/sevenz.py).
#
# Suffisant pour les .sbsar : en-tête codé (LZMA), dossiers à un seul codeur LZMA / LZMA2 / copie (ID 00, ou ID vide :
# cas de l'icône des .sbsar, que bsdtar refuse), sous-flux. Pas de chiffrement, pas de BCJ, pas de dossiers
# multi-codeurs.
#
#   python3 subzero_7z.py fichier.sbsar    # liste des membres et leur taille

import lzma
import struct

SIG = b"7z\xbc\xaf\x27\x1c"

K_END, K_HEADER, K_ARCHIVE_PROPS, K_ADD_STREAMS, K_MAIN_STREAMS, K_FILES_INFO = 0, 1, 2, 3, 4, 5
K_PACK_INFO, K_UNPACK_INFO, K_SUBSTREAMS_INFO, K_SIZE, K_CRC, K_FOLDER = 6, 7, 8, 9, 10, 11
K_CODERS_UNPACK_SIZE, K_NUM_UNPACK_STREAM, K_EMPTY_STREAM, K_EMPTY_FILE = 12, 13, 14, 15
K_NAME, K_CTIME, K_ATIME, K_MTIME, K_WIN_ATTR, K_COMMENT, K_ENCODED_HEADER = 17, 18, 19, 20, 21, 22, 23
K_START_POS, K_DUMMY = 24, 25


class Reader:
    def __init__(self, b):
        self.b = b
        self.p = 0

    def byte(self):
        v = self.b[self.p]
        self.p += 1
        return v

    def bytes(self, n):
        v = self.b[self.p:self.p + n]
        self.p += n
        return v

    def num(self):
        first = self.byte()
        mask = 0x80
        value = 0
        for i in range(8):
            if first & mask == 0:
                high = first & (mask - 1)
                value |= high << (8 * i)
                return value
            value |= self.byte() << (8 * i)
            mask >>= 1
        return value

    def u32(self):
        return struct.unpack("<I", self.bytes(4))[0]

    def u64(self):
        return struct.unpack("<Q", self.bytes(8))[0]

    def bits(self, n):
        out, mask, cur = [], 0, 0
        for _ in range(n):
            if mask == 0:
                cur = self.byte()
                mask = 0x80
            out.append(bool(cur & mask))
            mask >>= 1
        return out

    def all_or_bits(self, n):
        if self.byte():
            return [True] * n
        return self.bits(n)


def _read_pack_info(r):
    pack_pos = r.num()
    n = r.num()
    sizes = []
    while True:
        t = r.num()
        if t == K_END:
            break
        if t == K_SIZE:
            sizes = [r.num() for _ in range(n)]
        elif t == K_CRC:
            defined = r.all_or_bits(n)
            for d in defined:
                if d:
                    r.u32()
        else:
            raise ValueError("pack info: id %d" % t)
    return pack_pos, sizes


def _read_folder(r):
    ncoders = r.num()
    coders = []
    total_out = 0
    for _ in range(ncoders):
        flag = r.byte()
        id_size = flag & 0x0F
        cid = r.bytes(id_size)
        nin, nout = 1, 1
        if flag & 0x10:
            nin, nout = r.num(), r.num()
        props = b""
        if flag & 0x20:
            props = r.bytes(r.num())
        coders.append({"id": cid, "props": props, "in": nin, "out": nout})
        total_out += nout
    for _ in range(total_out - 1):  # bind pairs
        r.num(); r.num()
    total_in = sum(c["in"] for c in coders)
    npacked = total_in - (total_out - 1)
    if npacked > 1:
        for _ in range(npacked):
            r.num()
    return {"coders": coders, "nout": total_out}


def _read_unpack_info(r):
    if r.num() != K_FOLDER:
        raise ValueError("unpack info: folder attendu")
    n = r.num()
    if r.byte() != 0:
        raise ValueError("dossiers externes non gérés")
    folders = [_read_folder(r) for _ in range(n)]
    if r.num() != K_CODERS_UNPACK_SIZE:
        raise ValueError("unpack sizes attendus")
    for f in folders:
        f["unpack_sizes"] = [r.num() for _ in range(f["nout"])]
    while True:
        t = r.num()
        if t == K_END:
            break
        if t == K_CRC:
            defined = r.all_or_bits(n)
            for d in defined:
                if d:
                    r.u32()
        else:
            raise ValueError("unpack info: id %d" % t)
    return folders


def _read_substreams(r, folders):
    counts = [1] * len(folders)
    t = r.num()
    if t == K_NUM_UNPACK_STREAM:
        counts = [r.num() for _ in folders]
        t = r.num()
    sizes = []
    has_sizes = t == K_SIZE
    for f, c in zip(folders, counts):
        if c == 0:
            continue
        total = f["unpack_sizes"][-1]
        s = 0
        for _ in range(c - 1):
            v = r.num() if has_sizes else 0
            sizes.append(v)
            s += v
        sizes.append(total - s)
    if has_sizes:
        t = r.num()
    while t != K_END:
        if t == K_CRC:
            ndig = sum(c for f, c in zip(folders, counts))
            defined = r.all_or_bits(ndig)
            for d in defined:
                if d:
                    r.u32()
        else:
            raise ValueError("substreams: id %d" % t)
        t = r.num()
    return counts, sizes


def _read_streams_info(r):
    info = {"pack_pos": 0, "pack_sizes": [], "folders": [], "counts": None, "sizes": None}
    while True:
        t = r.num()
        if t == K_END:
            break
        if t == K_PACK_INFO:
            info["pack_pos"], info["pack_sizes"] = _read_pack_info(r)
        elif t == K_UNPACK_INFO:
            info["folders"] = _read_unpack_info(r)
        elif t == K_SUBSTREAMS_INFO:
            info["counts"], info["sizes"] = _read_substreams(r, info["folders"])
        else:
            raise ValueError("streams info: id %d" % t)
    if info["counts"] is None:
        info["counts"] = [1] * len(info["folders"])
        info["sizes"] = [f["unpack_sizes"][-1] for f in info["folders"]]
    return info


def _decode_folder(data, folder):
    if len(folder["coders"]) != 1:
        raise ValueError("dossier multi-codeurs non géré")
    c = folder["coders"][0]
    size = folder["unpack_sizes"][-1]
    cid, props = c["id"], c["props"]
    if cid in (b"", b"\x00"):  # copie ; ID vide = cas des icônes sbsar
        return data[:size]
    if cid == b"\x03\x01\x01":  # LZMA
        dict_size = struct.unpack("<I", props[1:5])[0]
        d = props[0]
        lc, rem = d % 9, d // 9
        lp, pb = rem % 5, rem // 5
        dec = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[
            {"id": lzma.FILTER_LZMA1, "dict_size": dict_size, "lc": lc, "lp": lp, "pb": pb}])
        return dec.decompress(data, max_length=size)[:size]
    if cid == b"\x21":  # LZMA2
        p = props[0]
        dict_size = (2 | (p & 1)) << (p // 2 + 11) if p < 40 else 0xFFFFFFFF
        dec = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[
            {"id": lzma.FILTER_LZMA2, "dict_size": dict_size}])
        return dec.decompress(data)[:size]
    raise ValueError("codeur 7z non géré : %s" % cid.hex())


def _unpack_all(buf, base, info):
    """Décode chaque dossier et le découpe en sous-flux."""
    streams = []
    pos = base + info["pack_pos"]
    si = 0
    for fi, folder in enumerate(info["folders"]):
        psize = info["pack_sizes"][fi]
        out = _decode_folder(buf[pos:pos + psize], folder)
        pos += psize
        off = 0
        for _ in range(info["counts"][fi]):
            sz = info["sizes"][si]
            streams.append(out[off:off + sz])
            off += sz
            si += 1
    return streams


def _read_files_info(r, nstreams):
    n = r.num()
    names, empty = [None] * n, [False] * n
    while True:
        t = r.num()
        if t == K_END:
            break
        size = r.num()
        end = r.p + size
        if t == K_NAME:
            if r.byte() != 0:
                raise ValueError("noms externes non gérés")
            raw = r.b[r.p:end]
            names = raw.decode("utf-16-le").split("\x00")[:n]
        elif t == K_EMPTY_STREAM:
            empty = r.bits(n)
        r.p = end
    return names, empty


def read_7z(path_or_bytes):
    """Renvoie {nom: contenu} pour tous les fichiers de l'archive."""
    buf = path_or_bytes if isinstance(path_or_bytes, (bytes, bytearray)) else open(path_or_bytes, "rb").read()
    if buf[:6] != SIG:
        raise ValueError("pas une archive 7z")
    next_off, next_size = struct.unpack("<QQ", buf[12:28])
    base = 32
    header = buf[base + next_off: base + next_off + next_size]
    r = Reader(header)
    t = r.num()
    while t == K_ENCODED_HEADER:
        info = _read_streams_info(r)
        header = b"".join(_unpack_all(buf, base, info))
        r = Reader(header)
        t = r.num()
    if t != K_HEADER:
        raise ValueError("en-tête 7z inattendu : %d" % t)
    info = None
    names, empty = [], []
    while True:
        t = r.num()
        if t == K_END:
            break
        if t == K_ARCHIVE_PROPS:
            while True:
                pt = r.num()
                if pt == K_END:
                    break
                r.bytes(r.num())
        elif t == K_MAIN_STREAMS:
            info = _read_streams_info(r)
        elif t == K_FILES_INFO:
            names, empty = _read_files_info(r, 0)
        else:
            raise ValueError("header: id %d" % t)
    streams = _unpack_all(buf, base, info) if info else []
    out, si = {}, 0
    for name, e in zip(names, empty):
        if e:
            out[name] = b""
        else:
            out[name] = streams[si]
            si += 1
    return out


if __name__ == "__main__":
    import sys
    for name, data in read_7z(sys.argv[1]).items():
        print("%10d  %s" % (len(data), name))
