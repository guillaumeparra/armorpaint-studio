"""Banc du patch udim-import côté OBJ (voir obj.sh) : cas UDIM écrits ici, lus par obj_test en « o udim ».

obj_udim.py <dossier> <obj_test> [<obj_test upstream>]

Chaque cas est un OBJ dont on connaît les triangles d'origine (positions et uv globales). La sortie d'obj_test
(OBJ_TEST_TILES=1) est relue et vérifiée :
  - objets et tuiles attendus, triangles écartés (uv hors 1001-1990) et coupés ;
  - uv locales dans [0, 1] ;
  - pas de déformation : chaque sommet produit, remis en uv globale (uv locale + tuile), retombe sur un triangle
    d'origine à la position 3D que ce triangle donne à cette uv ;
  - surfaces 3D et uv conservées (les morceaux d'un triangle coupé recouvrent le triangle).
Avec l'exe upstream, les cas marqués « bug » doivent échouer (plantage, boucle sans fin ou vérification fausse) :
c'est la preuve que le banc voit les bugs corrigés.
"""
import math, os, subprocess, sys

# Un triangle : ((x, y, z), (u, v)) × 3. Un cas : objets [(nom, [triangles])], attendu, cas de bug upstream
T = lambda *c: tuple(c)


def tri_uv(u0, v0, u1, v1, u2, v2, z=0.0):
    """Triangle dont la position 3D est son uv (x = u, y = v), décalé en z : la déformation se voit directement."""
    return T(((u0, v0, z), (u0, v0)), ((u1, v1, z), (u1, v1)), ((u2, v2, z), (u2, v2)))


def cases():
    c = {}
    c["neg"] = ([("neg", [tri_uv(0.2, 0.2, 0.8, 0.2, 0.2, 0.8), tri_uv(-1.8, 0.2, -1.2, 0.2, -1.8, 0.8)])],
                {"tiles": {"neg": {1001}}, "invalid": 1, "invalid_uv": (-1.8, 0.2), "cut": 0}, True)
    c["neg_small"] = ([("neg_small", [tri_uv(0.2, 0.2, 0.8, 0.2, 0.2, 0.8), tri_uv(-0.3, 0.2, 0.3, 0.2, 0.0, 0.6)])],
                      {"tiles": {"neg_small": {1001}}, "invalid": 1, "invalid_uv": (-0.3, 0.2), "cut": 0}, False)
    c["u10"] = ([("u10", [tri_uv(9.2, 0.2, 9.8, 0.2, 9.2, 0.8), tri_uv(10.2, 0.2, 10.8, 0.2, 10.2, 0.8)])],
                {"tiles": {"u10": {1010}}, "invalid": 1, "invalid_uv": (10.2, 0.2), "cut": 0}, True)
    c["u10_border"] = ([("u10_border", [tri_uv(9.5, 0.2, 10.0, 0.2, 9.5, 1.0)])], {"tiles": {"u10_border": {1010}}, "invalid": 0, "cut": 0}, False)
    c["straddle"] = ([("straddle", [tri_uv(0.5, 0.2, 1.5, 0.2, 0.5, 0.8)])], {"tiles": {"straddle": {1001, 1002}}, "invalid": 0, "cut": 1}, True)
    c["straddle_v"] = ([("straddle_v", [tri_uv(2.2, 0.6, 2.8, 0.6, 2.5, 1.7)])], {"tiles": {"straddle_v": {1003, 1013}}, "invalid": 0, "cut": 1}, True)
    c["straddle4"] = ([("straddle4", [tri_uv(0.6, 0.6, 1.6, 0.7, 0.9, 1.5)])], {"tiles": {"straddle4": {1001, 1002, 1011, 1012}}, "invalid": 0, "cut": 1}, True)
    c["long"] = ([("long", [tri_uv(0.1, 0.4, 3.9, 0.5, 0.2, 0.6)])], {"tiles": {"long": {1001, 1002, 1003, 1004}}, "invalid": 0, "cut": 1}, True)
    # Sommets à moins de la tolérance hors de la tuile : ni coupe, ni refus, uv ramenées au bord
    c["eps"] = ([("eps", [tri_uv(-0.00005, 0.2, 1.00005, 0.2, 0.5, 0.99995)])], {"tiles": {"eps": {1001}}, "invalid": 0, "cut": 0}, False)
    # uv écrasées sur un bord : le triangle garde sa géométrie, dans une seule tuile
    c["collapsed"] = ([("collapsed", [T(((0, 0, 0), (1.0, 0.5)), ((1, 0, 0), (1.0, 0.5)), ((0, 1, 0), (1.0, 0.5)))])],
                      {"tiles": {"collapsed": None}, "invalid": 0, "cut": 0, "skip_map": True}, False)
    c["huge"] = ([("huge", [tri_uv(0.2, 0.2, 0.8, 0.2, 0.2, 0.8), T(((0.9, 0.2, 0), (1e30, 0.2)), ((0.8, 0.3, 0), (0.8, 0.3)), ((0.2, 0.9, 0), (0.2, 0.9)))])],
                 {"tiles": {"huge": {1001}}, "invalid": 1, "cut": 0}, True)
    c["objects"] = ([("a", [tri_uv(0.2, 0.2, 0.8, 0.2, 0.2, 0.8), tri_uv(1.2, 0.2, 1.8, 0.2, 1.2, 0.8)]),
                     ("b", [tri_uv(0.2, 1.2, 0.8, 1.2, 0.2, 1.8, z=1.0)])],
                    {"tiles": {"a": {1001, 1002}, "b": {1011}}, "invalid": 0, "cut": 0}, False)
    tris = [tri_uv(u + 0.2, v + 0.2, u + 0.8, v + 0.2, u + 0.2, v + 0.8) for v in range(7) for u in range(10)]
    c["tiles70"] = ([("tiles70", tris)], {"tiles": {"tiles70": {1001 + u + 10 * v for v in range(7) for u in range(10)}}, "invalid": 0, "cut": 0}, False)
    return c


def write_case(path, objs, shared=False):
    """Les v / vt d'un objet avant ses faces : un v après des faces commence un nouvel objet à l'import."""
    lines, nv, nvt = [], 0, 0
    vt_index = {}
    for name, tris in objs:
        lines.append("o " + name)
        faces = []
        for t in tris:
            ids = []
            for (p, uv) in t:
                lines.append("v %.7f %.7f %.7f" % p)
                nv += 1
                if not (shared and uv in vt_index):
                    lines.append("vt %.7g %.7g" % uv)
                    nvt += 1
                    vt_index[uv] = nvt
                ids.append("%d/%d" % (nv, vt_index[uv]))
            faces.append("f " + " ".join(ids))
        lines += faces
    open(path, "w").write("\n".join(lines) + "\n")


def parse(out):
    parts, cur = [], None
    for line in out.splitlines():
        w = line.split()
        if line.startswith("PART "):
            cur = {"name": w[1], "verts": [], "tiles": {}, "invalid": 0, "invalid_uv": None, "cut": 0}
            parts.append(cur)
        elif w and w[0] == "v":
            cur["verts"].append(((float(w[1]), float(w[2]), float(w[3])), (int(w[9]), int(w[10]))))
        elif w and w[0] == "tile":
            cur["tiles"][int(w[1][:-1])] = [int(x) for x in w[2:]]
        elif line.startswith("  udim invalid="):
            kv = dict(x.split("=") for x in line.split() if "=" in x)
            cur["invalid"], cur["cut"] = int(kv["invalid"]), int(kv["cut"])
            uvs = line.split("uv=")[1].split()
            cur["invalid_uv"] = (float(uvs[0]), float(uvs[1]))
    return parts


def area3(a, b, c):
    u = [b[i] - a[i] for i in range(3)]
    v = [c[i] - a[i] for i in range(3)]
    x = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    return 0.5 * math.sqrt(sum(k * k for k in x))


def area2(a, b, c):
    return 0.5 * abs((b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]))


def to_file_pos(p):
    """obj_parse passe en z-up : (x, y, z) du fichier -> (x, -z, y)."""
    return (p[0], p[2], -p[1])


def check(parts, objs, exp):
    errs = []
    names = [p["name"] for p in parts]
    want = [n for n, _ in objs]
    if names != want:
        return ["parts %s, attendu %s" % (names, want)]
    inv = sum(p["invalid"] for p in parts)
    cut = sum(p["cut"] for p in parts)
    if inv != exp["invalid"]:
        errs.append("écartés %d, attendu %d" % (inv, exp["invalid"]))
    if "invalid_uv" in exp:
        got = next((p["invalid_uv"] for p in parts if p["invalid"]), None)
        if got is None or any(abs(got[i] - exp["invalid_uv"][i]) > 1e-4 for i in range(2)):
            errs.append("première uv écartée %s, attendu %s" % (got, exp["invalid_uv"]))
    if cut != exp["cut"]:
        errs.append("coupés %d, attendu %d" % (cut, exp["cut"]))
    for part, (name, tris) in zip(parts, objs):
        want_tiles = exp["tiles"][name]
        if want_tiles is not None and set(part["tiles"]) != want_tiles:
            errs.append("%s : tuiles %s, attendu %s" % (name, sorted(part["tiles"]), sorted(want_tiles)))
        valid = [t for t in tris if all(-1e-4 <= uv[0] <= 10.0001 and -1e-4 <= uv[1] <= 99.0001 for _, uv in t)]
        a3 = a2 = 0.0
        for tile, idx in part["tiles"].items():
            tu, tv = (tile - 1001) % 10, (tile - 1001) // 10
            pts = []
            for i in idx:
                p, (tx, ty) = part["verts"][i]
                if not (0 <= tx <= 32767 and 0 <= ty <= 32767):
                    errs.append("%s : uv locale hors [0, 1] (%d, %d)" % (name, tx, ty))
                g = (tu + tx / 32767.0, tv + 1.0 - ty / 32767.0)
                pts.append((to_file_pos(p), g))
            for k in range(0, len(pts), 3):
                a3 += area3(pts[k][0], pts[k + 1][0], pts[k + 2][0])
                a2 += area2(pts[k][1], pts[k + 1][1], pts[k + 2][1])
            if exp.get("skip_map"):
                continue
            for p, g in pts:  # Pas de déformation : la uv globale donne la position sur un triangle d'origine
                best = None
                for t in valid:
                    (p0, q0), (p1, q1), (p2, q2) = t
                    d = (q1[0] - q0[0]) * (q2[1] - q0[1]) - (q2[0] - q0[0]) * (q1[1] - q0[1])
                    if abs(d) < 1e-12:
                        continue
                    l1 = ((g[0] - q0[0]) * (q2[1] - q0[1]) - (q2[0] - q0[0]) * (g[1] - q0[1])) / d
                    l2 = ((q1[0] - q0[0]) * (g[1] - q0[1]) - (g[0] - q0[0]) * (q1[1] - q0[1])) / d
                    l0 = 1.0 - l1 - l2
                    if min(l0, l1, l2) < -2e-3:
                        continue
                    q = [l0 * p0[i] + l1 * p1[i] + l2 * p2[i] for i in range(3)]
                    e = max(abs(q[i] - p[i]) for i in range(3))
                    best = e if best is None else min(best, e)
                if best is None or best > 2e-3:
                    errs.append("%s : sommet déformé, uv %.4f %.4f -> %s (écart %s)" % (name, g[0], g[1], tuple(round(x, 4) for x in p), best))
                    break
        w3 = sum(area3(*[t[k][0] for k in range(3)]) for t in valid)
        w2 = sum(area2(*[t[k][1] for k in range(3)]) for t in valid)
        if abs(a3 - w3) > 1e-3 * max(w3, 1e-9) + 1e-6 or (not exp.get("skip_map") and abs(a2 - w2) > 1e-3 * max(w2, 1e-9) + 1e-6):
            errs.append("%s : surfaces 3D %.6f / uv %.6f, attendu %.6f / %.6f" % (name, a3, a2, w3, w2))
    return errs


def run(exe, path):
    try:
        r = subprocess.run([exe, path, "o", "udim"], capture_output=True, text=True, timeout=10, env=dict(os.environ, OBJ_TEST_TILES="1"))
    except subprocess.TimeoutExpired:
        return None, "boucle sans fin (timeout)"
    if "AddressSanitizer" in r.stderr or r.returncode != 0:
        kind = r.stderr.split("AddressSanitizer: ")[1].split(" ")[0] if "AddressSanitizer: " in r.stderr else "rc=%d" % r.returncode
        return None, "plantage (%s)" % kind
    if "runtime error" in r.stderr:
        return None, "UB : " + r.stderr.split("runtime error: ")[1].splitlines()[0]
    return r.stdout, None


def main():
    d, exe = sys.argv[1], sys.argv[2]
    up = sys.argv[3] if len(sys.argv) > 3 else None
    os.makedirs(d, exist_ok=True)
    fail = 0
    allc = cases()
    allc["shared_vt"] = ([("shared_vt", [tri_uv(0.5, 0.2, 1.0, 0.5, 0.5, 0.8), tri_uv(1.0, 0.5, 1.5, 0.2, 1.5, 0.8)])],
                         {"tiles": {"shared_vt": {1001, 1002}}, "invalid": 0, "cut": 0}, True)
    for name, (objs, exp, bug) in allc.items():
        path = os.path.join(d, name + ".obj")
        write_case(path, objs, shared=(name == "shared_vt"))
        out, err = run(exe, path)
        errs = [err] if err else check(parse(out), objs, exp)
        line = "  %s %s" % ("✅" if not errs else "❌", name)
        if errs:
            fail = 1
            line += " : " + " ; ".join(errs[:3])
        if up and bug:
            uout, uerr = run(up, path)
            uexp = {k: v for k, v in exp.items() if k != "invalid_uv"}
            uexp.update(invalid=0, cut=0)  # L'upstream ne compte ni les uv écartées ni les coupes
            uerrs = [uerr] if uerr else check(parse(uout), objs, uexp)
            if uerrs:
                line += "   [upstream : %s]" % uerrs[0][:70]
            else:
                line += "   [upstream : ❌ ne montre pas le bug]"
                fail = 1
        print(line)
    sys.exit(fail)


if __name__ == "__main__":
    main()
