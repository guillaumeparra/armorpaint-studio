#!/usr/bin/env python3
"""Maillages de test pour les UDIM, les atlas et le nommage des exports (à ouvrir dans ArmorPaint, ou bancs).

    python3 .tests/udim_gen.py [dossier]      (défaut : .tests/meshes)

udim_cube.obj : un cube de 2 unités, une face par tuile UDIM, sur deux rangées pour vérifier le décalage en V :
    1001 avant (+Z)   1002 droite (+X)   1003 arrière (-Z)
    1011 gauche (-X)  1012 dessus (+Y)   1013 dessous (-Y)
  Chaque face couvre sa tuile avec une marge de 2 %, en 8 × 8 quads. Deux matériaux : « body » pour les quatre
  côtés, « cap » pour le dessus et le dessous.
  Import « Split By: UDIM Tile » : 6 objets cube.1001 ... cube.1013 ; « Material » : body et cap ; « Object » : cube.

props.obj : trois objets à UV propres (0-1, qui se chevauchent d'un objet à l'autre, comme des assets distincts), un
  matériau chacun : crate (cube, « wood »), barrel (cylindre, « metal »), ball (sphère, « rubber »). Pour les atlas
  (jeux de textures) : crate et barrel dans un jeu, ball dans un autre.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "meshes")
os.makedirs(out, exist_ok=True)


class Obj:
    """Chaque objet écrit ses v / vt / vn, puis ses faces : l'import OBJ d'ArmorPaint commence un nouvel objet (« .001 »)
    quand un v suit des faces, comme dans les fichiers de Blender ou Maya."""

    def __init__(self, header):
        self.lines = ["# " + l for l in header.splitlines()]
        self.data, self.faces = [], []
        self.v = self.vt = self.vn = 0

    def flush(self):
        self.lines += self.data + self.faces
        self.data, self.faces = [], []

    def obj(self, name):
        self.flush()
        self.lines.append("o " + name)

    def mtl(self, name):
        self.faces.append("usemtl " + name)

    def grid(self, point, normal, uv, n):
        """Grille n × n : point(s, t) et uv(s, t) pour s, t dans [0, 1]."""
        base_v, base_t = self.v, self.vt
        for j in range(n + 1):
            for i in range(n + 1):
                s, t = i / n, j / n
                self.data.append("v %.6f %.6f %.6f" % point(s, t))
                self.data.append("vt %.6f %.6f" % uv(s, t))
        self.v += (n + 1) ** 2
        self.vt += (n + 1) ** 2
        if normal is not None:
            self.data.append("vn %.6f %.6f %.6f" % normal)
            self.vn += 1
        for j in range(n):
            for i in range(n):
                a = j * (n + 1) + i
                quad = (a, a + 1, a + n + 2, a + n + 1)
                vn = " ".join("%d/%d/%d" % (base_v + k + 1, base_t + k + 1, self.vn) for k in quad) if normal else \
                    " ".join("%d/%d" % (base_v + k + 1, base_t + k + 1) for k in quad)
                self.faces.append("f " + vn)

    def save(self, path):
        self.flush()
        with open(path, "w") as f:
            f.write("\n".join(self.lines) + "\n")
        print(path)


# Faces du cube : normale, axes (droite, haut) vus de face, tuile (colonne, rangée).
FACES = [
    ((0, 0, 1), (1, 0, 0), (0, 1, 0), (0, 0), "body"),     # 1001 avant
    ((1, 0, 0), (0, 0, -1), (0, 1, 0), (1, 0), "body"),    # 1002 droite
    ((0, 0, -1), (-1, 0, 0), (0, 1, 0), (2, 0), "body"),   # 1003 arrière
    ((-1, 0, 0), (0, 0, 1), (0, 1, 0), (0, 1), "body"),    # 1011 gauche
    ((0, 1, 0), (1, 0, 0), (0, 0, -1), (1, 1), "cap"),     # 1012 dessus
    ((0, -1, 0), (1, 0, 0), (0, 0, 1), (2, 1), "cap"),     # 1013 dessous
]


def cube_face(o, size, normal, right, up, tile, margin, n):
    h = size / 2
    point = lambda s, t: tuple(h * normal[k] + h * (2 * s - 1) * right[k] + h * (2 * t - 1) * up[k] for k in range(3))
    uv = None
    if tile is not None:
        u0, v0 = tile
        uv = lambda s, t: (u0 + margin + (1 - 2 * margin) * s, v0 + margin + (1 - 2 * margin) * t)
    return point, uv


cube = Obj(__doc__.split("\n\n")[2])
cube.obj("cube")
for normal, right, up, tile, mat in FACES:
    cube.mtl(mat)
    point, uv = cube_face(cube, 2.0, normal, right, up, tile, 0.02, 8)
    cube.grid(point, normal, uv, 8)
cube.save(os.path.join(out, "udim_cube.obj"))

props = Obj(__doc__.split("\n\n")[3])
# crate : cube dont les 6 faces se partagent la tuile 0-1 en 3 × 2 cases.
props.obj("crate")
props.mtl("wood")
for normal, right, up, tile, mat in FACES:
    point, _ = cube_face(props, 1.6, normal, right, up, None, 0, 4)
    point = (lambda p, c=(-2.5, 0.8, 0): lambda s, t: tuple(p(s, t)[k] + c[k] for k in range(3)))(point)
    col, row = tile
    uv = (lambda c, r: lambda s, t: ((c + 0.03 + 0.94 * s) / 3, (r + 0.03 + 0.94 * t) / 2))(col, row)
    props.grid(point, normal, uv, 4)
# barrel : cylindre, sans fonds.
props.obj("barrel")
props.mtl("metal")
props.grid(lambda s, t: (0.8 * math.cos(2 * math.pi * s), 1.8 * t, -0.8 * math.sin(2 * math.pi * s)), None,
           lambda s, t: (s, t), 16)
# ball : sphère.
props.obj("ball")
props.mtl("rubber")
props.grid(lambda s, t: (2.5 + 0.8 * math.sin(math.pi * t) * math.cos(2 * math.pi * s), 0.8 - 0.8 * math.cos(math.pi * t),
                         -0.8 * math.sin(math.pi * t) * math.sin(2 * math.pi * s)), None, lambda s, t: (s, t), 16)
props.save(os.path.join(out, "props.obj"))
