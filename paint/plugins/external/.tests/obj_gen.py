"""Corpus et fuzz des patches obj-global-indices et obj-float-exponent (voir obj.sh).

obj_gen.py corpus <dossier>
    valid/  : fichiers valides à données séparées par objet (ce que l'upstream lit bien) : sphères en v, v/t, v//n,
              v/t/n, CRLF, usemtl, triangles, n-gones concaves, grille UDIM.
    shared/ : <nom>.obj référence des v/vt/vn d'objets précédents ; <nom>.ref.obj est le même maillage à données
              séparées. Le patch doit lire les deux à l'identique.
    bad/    : fichiers malformés (indices hors bornes, nuls, en avant, entiers énormes, face de 700 sommets, nom de
              5000 caractères, fin de fichier au milieu d'une ligne...). Seule exigence : pas de plantage.
    exp/    : <nom>.obj écrit en notation scientifique (exposants négatifs, positifs, E majuscule, normales et
              coordonnées minuscules) ; <nom>.ref.obj est le même maillage en décimal. Valeurs exactes dans les deux
              écritures (multiples de 1/64) : les deux lectures doivent être identiques.

obj_gen.py fuzz <dossier> <n> <exe>...
    n mutations des fichiers valid/, shared/ et exp/, passées à chaque exe en split o et u. Compte les erreurs mémoire
    (ASan), signaux et timeouts par exe, et garde les fichiers fautifs du premier exe dans <dossier>/fuzz_fail/.
"""
import glob, math, os, random, subprocess, sys


def sphere(cx, r=1.0, nu=12, nv=8):
    vs, vts, vns, quads = [], [], [], []
    for j in range(nv + 1):
        for i in range(nu + 1):
            th, ph = 2 * math.pi * i / nu, math.pi * j / nv
            n = (math.sin(ph) * math.cos(th), math.cos(ph), math.sin(ph) * math.sin(th))
            vs.append((cx + r * n[0], r * n[1], r * n[2]))
            vns.append(n)
            vts.append((i / nu, j / nv))
    for j in range(nv):
        for i in range(nu):
            a = j * (nu + 1) + i
            quads.append((a, a + 1, a + nu + 2, a + nu + 1))
    return vs, vts, vns, quads


def ngon(cx, n, star):
    vs = []
    for i in range(n):
        r = 0.4 if star and i % 2 else 1.0
        a = 2 * math.pi * i / n
        vs.append((cx + r * math.cos(a), 0.0, r * math.sin(a)))
    return vs, [(0.5 + v[0] / 4, 0.5 + v[2] / 4) for v in vs], [(0, 1, 0)] * n, [tuple(range(n))[::-1]]


def write(path, objs, mode="vtn", crlf=False, mats=None):
    """objs : [(nom, vs, vts, vns, faces)], données séparées par objet, indices globaux."""
    lines, ov, ot, on = [], 0, 0, 0
    for name, vs, vts, vns, faces in objs:
        lines.append("o %s" % name)
        lines += ["v %.6f %.6f %.6f" % v for v in vs]
        if "t" in mode:
            lines += ["vt %.6f %.6f" % t for t in vts]
        if "n" in mode:
            lines += ["vn %.6f %.6f %.6f" % n for n in vns]
        step = len(faces) // len(mats) + 1 if mats else 0
        for fi, f in enumerate(faces):
            if mats and fi % step == 0:
                lines.append("usemtl %s" % mats[fi // step % len(mats)])
            fmt = {"vtn": "%d/%d/%d", "vn": "%d//%d", "vt": "%d/%d", "v": "%d"}[mode]
            args = {"vtn": lambda x: (x + 1 + ov, x + 1 + ot, x + 1 + on), "vn": lambda x: (x + 1 + ov, x + 1 + on),
                    "vt": lambda x: (x + 1 + ov, x + 1 + ot), "v": lambda x: (x + 1 + ov,)}[mode]
            lines.append("f " + " ".join(fmt % args(x) for x in f))
        ov, ot, on = ov + len(vs), ot + len(vts), on + len(vns)
    eol = "\r\n" if crlf else "\n"
    open(path, "w", newline="").write(eol.join(lines) + eol)


QUADS = """o tile.1001
vn 0 0 1
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
vt 0.1 0.1
vt 0.9 0.1
vt 0.9 0.9
vt 0.1 0.9
F1
o tile.1002
VN2v 2 0 0
v 3 0 0
v 3 1 0
v 2 1 0
vt 1.1 0.1
vt 1.9 0.1
vt 1.9 0.9
vt 1.1 0.9
F2
"""

HEXA = """o a
vn 0 1 0
v 1 0 0
v 0.5 0 0.866
v -0.5 0 0.866
v -1 0 0
v -0.5 0 -0.866
v 0.5 0 -0.866
f 6//1 5//1 4//1 3//1 2//1 1//1
o b
VN2v 4 0 0
v 3.5 0 0.866
v 2.5 0 0.866
v 2 0 0
v 2.5 0 -0.866
v 3.5 0 -0.866
f 12//N 11//N 10//N 9//N 8//N 7//N
"""


def quads(f1, f2, vn2=""):
    return QUADS.replace("F1", f1).replace("F2", f2).replace("VN2", vn2)


def corpus(d):
    for sub in ("valid", "shared", "bad", "exp", "udim"):
        os.makedirs(os.path.join(d, sub), exist_ok=True)
    p = lambda *a: os.path.join(d, *a)

    s1, s2, s3 = sphere(0), sphere(3, 0.5), sphere(-3, 2.0, 7, 5)
    objs = [("a",) + s1, ("b",) + s2, ("c",) + s3]
    for m in ("vtn", "vn", "vt", "v"):
        write(p("valid", "spheres_%s.obj" % m), objs, mode=m)
    write(p("valid", "spheres_crlf.obj"), objs, crlf=True)
    write(p("valid", "spheres_mats.obj"), objs, mats=["red", "green", "blue"])
    tri = lambda s: (s[0], s[1], s[2], [t for q in s[3] for t in ((q[0], q[1], q[2]), (q[0], q[2], q[3]))])
    write(p("valid", "spheres_tri.obj"), [("a",) + tri(s1), ("b",) + tri(s2)])
    write(p("valid", "ngons.obj"), [("a",) + ngon(0, 6, False), ("b",) + ngon(3, 10, True), ("c",) + ngon(6, 7, False)])
    write(p("valid", "ngons_v.obj"), [("a",) + ngon(0, 6, False), ("b",) + ngon(3, 10, True)], mode="v")
    vs, vts, vns, qs = [], [], [], []
    for j in range(3):
        for i in range(4):
            vs.append((i, j, 0))
            vts.append((i * 0.999, j * 0.999))
            vns.append((0, 0, 1))
    for j in range(2):
        for i in range(3):
            a = j * 4 + i
            qs.append((a, a + 1, a + 5, a + 4))
    write(p("valid", "udim_grid.obj"), [("grid", vs, vts, vns, qs)])
    # udim-import : grille UDIM propre (chaque quad dans sa tuile, ses propres vt), lue en udim comme l'upstream
    vs, vts, vns, qs = [], [], [], []
    for j in range(2):
        for i in range(3):
            a = len(vs)
            for (du, dv) in ((0.1, 0.1), (0.9, 0.1), (0.9, 0.9), (0.1, 0.9)):
                vs.append((i + du, j + dv, 0))
                vts.append((i + du, j + dv))
                vns.append((0, 0, 1))
            qs.append((a, a + 1, a + 2, a + 3))
    write(p("udim", "udim_clean.obj"), [("clean", vs, vts, vns, qs)])

    # Le cas du bug : triangles de l'objet 2 sur le vn 1 de l'objet 1
    tris1 = "f 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/4/1"
    open(p("shared", "tris.obj"), "w").write(quads(tris1, "f 5/5/1 6/6/1 7/7/1\nf 5/5/1 7/7/1 8/8/1"))
    open(p("shared", "tris.ref.obj"), "w").write(quads(tris1, "f 5/5/2 6/6/2 7/7/2\nf 5/5/2 7/7/2 8/8/2", "vn 0 0 1\n"))
    quad1, quad2 = "f 1/1/1 2/2/1 3/3/1 4/4/1", "f 5/5/2 6/6/2 7/7/2 8/8/2"
    ref = quads(quad1, quad2, "vn 0 0 1\n")
    open(p("shared", "quads.obj"), "w").write(quads(quad1, "f 5/5/1 6/6/1 7/7/1 8/8/1"))
    open(p("shared", "quads.ref.obj"), "w").write(ref)
    rel = "f -4/-4/-1 -3/-3/-1 -2/-2/-1 -1/-1/-1"  # indices relatifs
    open(p("shared", "relative.obj"), "w").write(quads(rel, rel, "vn 0 0 1\n"))
    open(p("shared", "relative.ref.obj"), "w").write(ref)
    open(p("shared", "ngon.obj"), "w").write(HEXA.replace("VN2", "").replace("//N", "//1"))
    open(p("shared", "ngon.ref.obj"), "w").write(HEXA.replace("VN2", "vn 0 1 0\n").replace("//N", "//2"))
    # L'objet 2 reprend les positions de l'objet 1 : l'échelle doit couvrir les positions utilisées
    open(p("shared", "farref.obj"), "w").write("o a\nv 10 0 0\nv 0 0 1\nv 0 1 0\nf 1 2 3\no b\nv 0.1 0 0\nv 0 0.1 0\nf 1 4 5\n")
    open(p("shared", "farref.ref.obj"), "w").write(
        "o a\nv 10 0 0\nv 0 0 1\nv 0 1 0\nf 1 2 3\no b\nv 0.1 0 0\nv 0 0.1 0\nv 10 0 0\nf 6 4 5\n")

    bad = {
        "oob_pos": "o a\nv 0 0 0\nv 1 0 0\nv 1 1 0\nf 1 2 9\nf 1 2 3\n",
        "oob_uvnor": "o a\nvn 0 0 1\nvt 0 0\nv 0 0 0\nv 1 0 0\nv 1 1 0\nf 1/7/1 2/1/9 3/1/1\nf 1/1/1 2/1/1 3/1/1\n",
        "zero_neg_short": "o a\nv 0 0 0\nv 1 0 0\nv 1 1 0\nf 0 1 2\nf -9 1 2\nf 1 2\nf 1\nf\nf 1 2 3",
        "forward": "o a\nf 1 2 3\nv 0 0 0\nv 1 0 0\nv 1 1 0\n",
        "hugeint": "o a\nv 0 0 0\nv 1 0 0\nv 1 1 0\nf " + "9" * 140 + " 2 3\n",
        "bogus_uv": "o a\nvn 0 0 1\nv 0 0 0\nv 1 0 0\nv 1 1 0\nf 1/1/1 2/2/1 3/3/1\n",
        "mixed_nor": "o a\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3\nvn 0 0 1\nf 1//1 3//1 4//1\n",
        "mixed_uv": "o a\nvt 0.5 0.5\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1/1 2/1 3/1\nf 1 3 4\n",
        "longname": "o " + "x" * 5000 + "\nv 0 0 0\nv 1 0 0\nv 1 1 0\nf 1 2 3\no " + "y" * 300 + "\nv 0 0 0\nv 1 0 0\nv 1 1 0\nf 4 5 6\n",
        "eof_v": "o a\nv 0 0 0\nv",
        "eof_o": "o a\nv 0 0 0\nv 1 0 0\nv 1 1 0\nf 1 2 3\no",
        "eof_usemtl": "usemtl",
        "eof_float": "o a\nv 1 2 3.5",
        "eof_face": "o a\nv 0 0 0\nv 1 0 0\nv 1 1 0\nf 1/",
    }
    n = 700
    bad["bigface"] = "o big\n" + "".join("v %f 0 %f\n" % (math.cos(2 * math.pi * i / n), math.sin(2 * math.pi * i / n)) for i in range(n)) \
        + "f " + " ".join(str(n - i) for i in range(n)) + "\nf 1 2 3\n"
    for k, v in bad.items():
        open(p("bad", k + ".obj"), "w").write(v)

    # obj-float-exponent : grille 5 x 4, deux objets, coordonnées multiples de 1/64 (exactes en %.6f et en %.6e)
    def grid(sci, upper=False):
        num = lambda x: ("%.6e" % x).replace("e", "E") if upper else "%.6e" % x if sci else "%.6f" % x
        lines, base = [], 0
        for o, (ox, sx, sy) in enumerate(((0.0, 0.765625, 0.25), (4.0, 0.015625, 0.03125))):
            lines.append("o g%d" % o)
            for j in range(4):
                for i in range(5):
                    lines.append("v %s %s %s" % (num(ox + (i - 2) * sx), num(j * sy - 0.375), num(i * j / 64)))
                    lines.append("vt %s %s" % (num(i * 0.25), num(j * 0.25)))
                    lines.append("vn %s %s %s" % (num(0), num(0), num(1)))
            for j in range(3):
                for i in range(4):
                    a = base + j * 5 + i + 1
                    lines.append("f " + " ".join("%d/%d/%d" % (x, x, x) for x in (a, a + 1, a + 6, a + 5)))
            base += 20
        return "\n".join(lines) + "\n"
    open(p("exp", "grid.obj"), "w").write(grid(True))
    open(p("exp", "grid_upper.obj"), "w").write(grid(True, True))
    open(p("exp", "grid.ref.obj"), "w").write(grid(False))
    open(p("exp", "grid_upper.ref.obj"), "w").write(grid(False))
    # Normales et coordonnées minuscules d'exporteurs (bruit de calcul) : packées à 0 comme leur version nulle
    tiny = "o t\nv {z} 0 0\nv {one} {z} 0\nv {one} {one} -{z}\nv {z} {one} 0\nvn {n} -{n} {one}\n" \
           "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nf 1/1/1 2/2/1 3/3/1 4/4/1\n"
    open(p("exp", "tiny.obj"), "w").write(tiny.format(z="1.0e-30", n="3.2e-12", one="1E+0"))
    open(p("exp", "tiny.ref.obj"), "w").write(tiny.format(z="0", n="0", one="1"))


def fuzz(d, n, exes):
    seeds = [open(f, "rb").read() for f in sorted(glob.glob(os.path.join(d, "valid", "*.obj")) + glob.glob(os.path.join(d, "shared", "*.obj"))
                                                   + glob.glob(os.path.join(d, "exp", "*.obj")))]
    toks = [b"/", b"//", b" ", b"\n", b"\r", b"\t", b"-", b"0", b"9", b"99999", b"-1", b"-99", b"f ", b"v ", b"vt ", b"vn ", b"o x\n", b"usemtl m\n",
            b"e", b"E+", b"e-", b"e99", b"e-45", b"e999999999999", b"e-2147483649", b"1e10"]
    fails = os.path.join(d, "fuzz_fail")
    os.makedirs(fails, exist_ok=True)
    for e, exe in enumerate(exes):
        random.seed(7)
        stats = {}
        for k in range(n):
            data = bytearray(random.choice(seeds))
            for _ in range(random.randint(1, 8)):
                op, at = random.random(), random.randrange(len(data) + 1)
                if op < 0.4:
                    data[at:at] = random.choice(toks)
                elif op < 0.7:
                    del data[at:at + random.randint(1, 6)]
                elif op < 0.85 and at < len(data):
                    data[at] = random.randrange(256)
                else:
                    data = data[:at]
            path = os.path.join(d, "fuzz.obj")
            open(path, "wb").write(data)
            # udim-import : aussi en udim, pour l'arbre courant seulement (l'upstream boucle sur les uv énormes)
            for split in ["o", "u"] + (["o udim"] if e == 0 else []):
                try:
                    r = subprocess.run([exe, path] + split.split(), capture_output=True, timeout=20)
                    err = r.stderr.decode(errors="replace")
                    kind = None
                    if "AddressSanitizer" in err:
                        kind = err.split("AddressSanitizer: ")[1].split(" ")[0]
                    elif r.returncode != 0:
                        kind = "rc=%d" % r.returncode
                    elif "runtime error" in err:
                        where = err.split("runtime error: ")[0].rsplit("/", 1)[-1].rsplit(":", 2)[0]
                        kind = "UB %s %s" % (where, err.split("runtime error: ")[1].split(":")[0][:30])
                except subprocess.TimeoutExpired:
                    kind = "timeout"
                if kind:
                    stats[kind] = stats.get(kind, 0) + 1
                    if e == 0 and not kind.startswith("UB"):
                        open(os.path.join(fails, "%d_%s.obj" % (k, split.replace(" ", "_"))), "wb").write(data)
        crashes = sum(v for k, v in stats.items() if not k.startswith("UB"))
        print("%s : %d lectures, %d plantages %s" % (os.path.basename(exe), (3 if e == 0 else 2) * n, crashes, stats))


if __name__ == "__main__":
    if sys.argv[1] == "corpus":
        corpus(sys.argv[2])
    else:
        fuzz(sys.argv[2], int(sys.argv[3]), sys.argv[4:])
