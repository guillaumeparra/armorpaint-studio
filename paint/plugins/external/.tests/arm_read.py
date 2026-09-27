"""Lecture d'un .arm (armpack d'iron, base/sources/iron_armpack.c) et des textures lz4 de ses calques, pour les bancs.

arm_read.load(path) : le projet, maps en dict (ordre des clés gardé), tableaux typés en bytes (u8) ou listes.
arm_read.pixels(blob, size) : les octets d'une texture lz4 (bloc LZ4, base/sources/iron_lz4.c) de size octets.
"""
import struct


class _Reader:
    def __init__(self, data):
        self.d, self.i = data, 0

    def u8(self):
        self.i += 1
        return self.d[self.i - 1]

    def u32(self):
        self.i += 4
        return struct.unpack_from("<I", self.d, self.i - 4)[0]

    def string(self):
        n = self.u32()
        self.i += n
        return self.d[self.i - n:self.i].decode("utf-8", "replace")

    def value(self):
        f = self.u8()
        if f == 0xC0:
            return None
        if f in (0xC2, 0xC3):
            return f == 0xC3
        if f == 0xCA:
            self.i += 4
            return struct.unpack_from("<f", self.d, self.i - 4)[0]
        if f == 0xD2:
            self.i += 4
            return struct.unpack_from("<i", self.d, self.i - 4)[0]
        if f == 0xDB:
            return self.string()
        if f == 0xDF:
            m = {}
            for _ in range(struct.unpack("<i", struct.pack("<I", self.u32()))[0]):
                self.u8()  # 0xdb
                k = self.string()
                m[k] = self.value()
            return m
        if f == 0xDD:
            n = self.u32()
            if n == 0:
                return []
            t = self.d[self.i]
            if t == 0xC4:
                self.i += 1 + n
                return bytes(self.d[self.i - n:self.i])
            if t in (0xCA, 0xD2, 0xD1):
                self.i += 1
                size, fmt = {0xCA: (4, "f"), 0xD2: (4, "i"), 0xD1: (2, "h")}[t]
                self.i += size * n
                return list(struct.unpack_from("<%d%s" % (n, fmt), self.d, self.i - size * n))
            if t in (0xC2, 0xC3):
                self.i += n
                return [b == 0xC3 for b in self.d[self.i - n:self.i]]
            return [self.value() for _ in range(n)]
        raise ValueError("armpack: flag 0x%02x at %d" % (f, self.i - 1))


def load(path):
    return _Reader(open(path, "rb").read()).value()


def pixels(blob, size):
    out, i, n = bytearray(), 0, len(blob)
    while i < n:
        tok = blob[i]
        i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = blob[i]
                i += 1
                lit += b
                if b != 255:
                    break
        out += blob[i:i + lit]
        i += lit
        if i >= n:
            break
        off = blob[i] | blob[i + 1] << 8
        i += 2
        m = (tok & 15) + 4
        if m == 19:
            while True:
                b = blob[i]
                i += 1
                m += b
                if b != 255:
                    break
        start = len(out) - off
        if off >= m:
            out += out[start:start + m]
        else:
            pat = bytes(out[start:])
            out += (pat * (m // off + 1))[:m]
    if len(out) != size:
        raise ValueError("lz4: %d bytes, %d expected" % (len(out), size))
    return bytes(out)
