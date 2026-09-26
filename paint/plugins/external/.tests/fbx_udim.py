"""Banc du patch fbx-split (voir fbx.sh) : cas écrits ici en FBX ASCII, lus par fbx_test en split UDIM (2) ou matériau (1).

fbx_udim.py <dossier> <fbx_test> [<fbx_test upstream>]      cas UDIM et matériaux
fbx_udim.py same <dossier> <fbx_test> <fbx_test upstream>   non-régression des splits par objet et par matériau
fbx_udim.py fuzz <dossier> <n> <fbx_test> <fbx_test upstream>

Les cas UDIM reprennent ceux d'obj_udim.py (mêmes triangles, mêmes vérifications : tuiles, uv locales dans [0, 1],
pas de déformation, surfaces conservées), un nœud FBX par objet. Un fichier refusé (uv hors 1001-1990, plus de 64
tuiles) ne doit produire aucun mesh et un message qui donne le nombre de triangles et la première uv. En plus :
  - nœuds sans uv (mesh à part, sans tuile) mêlés à des nœuds avec uv ;
  - second jeu d'uv et couleurs de sommet, interpolés aux coupes ;
  - split par matériau : plus de 64 matériaux, nœuds avec et sans uv sur un même matériau.
Avec l'exe upstream, les cas marqués « bug » doivent échouer (plantage, ou vérification fausse) : c'est la preuve que
le banc voit les bugs corrigés.

same : fichiers sans UDIM ni bug connu de l'upstream (nœuds avec et sans uv, second jeu d'uv, couleurs, uv hors de
[0, 1], jusqu'à 64 matériaux) ; les splits par objet (0) et par matériau (1) sortent octet pour octet comme l'upstream.
fuzz : mutations des fichiers de <dossier>/udim et <dossier>/same (nombres extrêmes : nan, inf, 1e30...), lues en
split 0, 1 et 2 ; l'arbre courant ne doit ni planter ni signaler d'UB.
"""
import glob, os, random, re, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from obj_udim import T, area3, cases as obj_cases, check, tri_uv  # noqa: E402


def write_fbx(path, nodes):
    """nodes : [(nom, triangles, options)] ; un triangle = ((x, y, z), (u, v)) × 3. Options : uv (défaut True), uv1 et col
    (une valeur par coin, dans l'ordre des triangles), mats (un nom de matériau par triangle)."""
    out = ["; FBX 7.4.0 project file", "FBXHeaderExtension:  {", "\tFBXHeaderVersion: 1003", "\tFBXVersion: 7400", "}", "Objects:  {"]
    conns = []
    nid = 1000
    fmt = lambda xs: ",".join("%.9g" % x for x in xs)
    for name, tris, opt in nodes:
        gid, mid = nid, nid + 1
        nid += 2
        verts = [c for t in tris for c in t]
        out.append('\tGeometry: %d, "Geometry::%s", "Mesh" {' % (gid, name))
        out.append("\t\tVertices: *%d {\n\t\t\ta: %s\n\t\t}" % (len(verts) * 3, fmt(x for p, _ in verts for x in p)))
        pvi = [i if i % 3 != 2 else -i - 1 for i in range(len(verts))]
        out.append("\t\tPolygonVertexIndex: *%d {\n\t\t\ta: %s\n\t\t}" % (len(pvi), ",".join(map(str, pvi))))
        out.append("\t\tGeometryVersion: 124")
        layers = []

        def element(kind, index, data_name, values, width, mapping="ByPolygonVertex"):
            out.append("\t\t%s: %d {" % (kind, index))
            out.append('\t\t\tVersion: 101\n\t\t\tName: "%s%d"' % (data_name, index))
            out.append('\t\t\tMappingInformationType: "%s"\n\t\t\tReferenceInformationType: "Direct"' % mapping)
            out.append("\t\t\t%s: *%d {\n\t\t\t\ta: %s\n\t\t\t}" % (data_name, len(values) * width, fmt(x for v in values for x in v)))
            out.append("\t\t}")
            layers.append((kind, index))

        if opt.get("uv", True):
            element("LayerElementUV", 0, "UV", [uv for _, uv in verts], 2)
        if "uv1" in opt:
            element("LayerElementUV", 1, "UV", opt["uv1"], 2)
        if "col" in opt:
            element("LayerElementColor", 0, "Colors", opt["col"], 4)
        mats = []
        if "mats" in opt:
            for m in opt["mats"]:
                if m not in mats:
                    mats.append(m)
            idx = [mats.index(m) for m in opt["mats"]]
            out.append("\t\tLayerElementMaterial: 0 {")
            out.append('\t\t\tVersion: 101\n\t\t\tName: ""\n\t\t\tMappingInformationType: "ByPolygon"\n\t\t\tReferenceInformationType: "IndexToDirect"')
            out.append("\t\t\tMaterials: *%d {\n\t\t\t\ta: %s\n\t\t\t}" % (len(idx), ",".join(map(str, idx))))
            out.append("\t\t}")
            layers.append(("LayerElementMaterial", 0))
        for li in sorted(set(i for _, i in layers)):
            out.append("\t\tLayer: %d {\n\t\t\tVersion: 100" % li)
            for kind, i in layers:
                if i == li:
                    out.append('\t\t\tLayerElement:  {\n\t\t\t\tType: "%s"\n\t\t\t\tTypedIndex: %d\n\t\t\t}' % (kind, i))
            out.append("\t\t}")
        out.append("\t}")
        out.append('\tModel: %d, "Model::%s", "Mesh" {\n\t\tVersion: 232\n\t}' % (mid, name))
        conns += ['\tC: "OO",%d,0' % mid, '\tC: "OO",%d,%d' % (gid, mid)]
        for m in mats:
            out.append('\tMaterial: %d, "Material::%s", "" {\n\t\tVersion: 102\n\t}' % (nid, m))
            conns.append('\tC: "OO",%d,%d' % (nid, mid))
            nid += 1
    out += ["}", "Connections:  {"] + conns + ["}"]
    open(path, "w").write("\n".join(out) + "\n")


def parse(out):
    """Meshes regroupés par nœud (nom sans le suffixe de tuile), comme les parts d'obj_test ; plus les messages."""
    parts, meshes, msgs = {}, [], {"null": False, "invalid": 0, "invalid_uv": None, "refused_tiles": None, "cut": 0}
    cur = None
    for line in out.splitlines():
        w = line.split()
        if line.startswith("MESH "):
            m = re.match(r"^(.*)\.(1\d\d\d)$", w[1])
            node, tile = (m.group(1), int(m.group(2))) if m else (w[1], None)
            kv = dict(x.split("=") for x in w[2:])
            if node not in parts:
                parts[node] = {"name": node, "verts": [], "tiles": {}, "notile": [], "invalid": 0, "cut": 0, "extra": []}
            cur = (parts[node], tile)
            meshes.append({"name": w[1], "count": int(kv["idx"]), "tex": kv["tex"] == "1", "tex1": kv["tex1"] == "1", "col": kv["col"] == "1", "verts": []})
        elif w and w[0] == "v":
            part, tile = cur
            p = (float(w[1]), float(w[2]), float(w[3]))
            f = dict((w[i], i) for i in range(len(w)) if w[i] in ("uv", "uv1", "col"))
            uv = (int(w[f["uv"] + 1]), int(w[f["uv"] + 2])) if "uv" in f else None
            extra = {k: [int(x) for x in w[f[k] + 1:f[k] + (3 if k != "col" else 5)]] for k in ("uv1", "col") if k in f}
            meshes[-1]["verts"].append((p, uv, extra))
            if tile is None:
                part["notile"].append(p)
            else:
                part["tiles"].setdefault(tile, []).append(len(part["verts"]))
                part["verts"].append((p, uv))
                part["extra"].append(extra)
        elif line == "NULL":
            msgs["null"] = True
        elif "UDIM import refused: uvs outside" in line:
            m = re.search(r"\((\d+) triangles, first uv (\S+) (\S+)\)", line)
            msgs["invalid"], msgs["invalid_uv"] = int(m.group(1)), (float(m.group(2)), float(m.group(3)))
        elif "UDIM import refused:" in line:
            msgs["refused_tiles"] = int(re.search(r"refused: (\d+) tiles", line).group(1))
        elif "UDIM import:" in line and "cut along" in line:
            msgs["cut"] = int(re.search(r"(\d+) triangles cut", line).group(1))
    return list(parts.values()), meshes, msgs


def run(exe, path, split):
    try:
        r = subprocess.run([exe, path, str(split)], capture_output=True, text=True, timeout=20)
    except subprocess.TimeoutExpired:
        return None, "boucle sans fin (timeout)"
    if "AddressSanitizer" in r.stderr or r.returncode != 0:
        kind = r.stderr.split("AddressSanitizer: ")[1].split(" ")[0] if "AddressSanitizer: " in r.stderr else "rc=%d" % r.returncode
        return None, "plantage (%s)" % kind
    if "runtime error" in r.stderr:
        return None, "UB : " + r.stderr.split("runtime error: ")[1].splitlines()[0]
    return r.stdout, None


def check_udim(out, nodes, exp, upstream=False):
    parts, meshes, msgs = parse(out)
    if exp.get("null"):
        return [] if msgs["null"] and not meshes else ["%d meshes, attendu aucun" % len(meshes)]
    if not upstream and ("refused" in exp or exp.get("invalid", 0) > 0):
        errs = []
        if not msgs["null"] or meshes:
            errs.append("fichier non refusé (%d meshes)" % len(meshes))
        if exp.get("invalid", 0) > 0:
            if msgs["invalid"] != exp["invalid"]:
                errs.append("écartés %d, attendu %d" % (msgs["invalid"], exp["invalid"]))
            want = exp.get("invalid_uv")
            if want is not None and (msgs["invalid_uv"] is None or any(abs(msgs["invalid_uv"][i] - want[i]) > 1e-3 for i in range(2))):
                errs.append("première uv écartée %s, attendu %s" % (msgs["invalid_uv"], want))
        elif msgs["refused_tiles"] != exp["refused"]:
            errs.append("refus pour %s tuiles, attendu %d" % (msgs["refused_tiles"], exp["refused"]))
        return errs
    if msgs["null"]:
        return ["fichier refusé"]
    objs = [(n, t) for n, t, o in nodes if o.get("uv", True)]
    tiled = [p for p in parts if p["tiles"]]
    for p in tiled:
        p["cut"] = 0
    if tiled:
        tiled[0]["cut"] = msgs["cut"]
    e = dict(exp, invalid=0) if upstream else exp
    errs = check(tiled, objs, e)
    stray = [p["name"] for p in parts if p["notile"] and all(n != p["name"] or o.get("uv", True) for n, _, o in nodes)]
    if stray:
        errs.append("meshes sans tuile %s" % stray)
    for n, tris, o in nodes:  # Nœuds sans uv : un mesh à leur nom, tous leurs triangles
        if o.get("uv", True):
            continue
        part = next((p for p in parts if p["name"] == n), None)
        if part is None or len(part["notile"]) != 3 * len(tris) or part["tiles"]:
            errs.append("%s : sans uv, attendu un mesh de %d sommets sans tuile" % (n, 3 * len(tris)))
    if "extra" in exp:
        errs += exp["extra"](parts, meshes)
    return errs


def interp_check(nodes):
    """Second jeu d'uv et couleurs : chaque sommet produit porte l'interpolation, au point de son uv globale, des valeurs du
    triangle d'origine (les coupes créent des sommets)."""
    (name, tris, opt), = nodes

    def f(parts, meshes):
        errs = []
        part = parts[0] if parts else None
        if part is None or not all(m["tex1"] and m["col"] for m in meshes):
            return ["%s : second jeu d'uv ou couleurs absents" % name]
        for tile, idx in part["tiles"].items():
            tu, tv = (tile - 1001) % 10, (tile - 1001) // 10
            for i in idx:
                (_, (tx, ty)), extra = part["verts"][i], part["extra"][i]
                g = (tu + tx / 32767.0, tv + 1.0 - ty / 32767.0)
                for k, t in enumerate(tris):
                    q = [c[1] for c in t]
                    d = (q[1][0] - q[0][0]) * (q[2][1] - q[0][1]) - (q[2][0] - q[0][0]) * (q[1][1] - q[0][1])
                    l1 = ((g[0] - q[0][0]) * (q[2][1] - q[0][1]) - (q[2][0] - q[0][0]) * (g[1] - q[0][1])) / d
                    l2 = ((q[1][0] - q[0][0]) * (g[1] - q[0][1]) - (g[0] - q[0][0]) * (q[1][1] - q[0][1])) / d
                    l = (1.0 - l1 - l2, l1, l2)
                    if min(l) < -2e-3:
                        continue
                    uv1 = [sum(l[c] * opt["uv1"][3 * k + c][j] for c in range(3)) for j in range(2)]
                    col = [sum(l[c] * opt["col"][3 * k + c][j] for c in range(3)) for j in range(4)]
                    got1 = (extra["uv1"][0] / 32767.0, 1.0 - extra["uv1"][1] / 32767.0)
                    gotc = [x / 32767.0 for x in extra["col"]]
                    if max(abs(got1[j] - uv1[j]) for j in range(2)) > 2e-3 or max(abs(gotc[j] - col[j]) for j in range(4)) > 2e-3:
                        errs.append("%s : uv1 %s / couleur %s, attendu %s / %s" % (name, got1, gotc, uv1, col))
                    break
                if errs:
                    return errs
        return errs

    return f


def udim_cases():
    c = {}
    for name, (objs, exp, bug) in obj_cases().items():
        nodes = [(n, tris, {}) for n, tris in objs]
        e = dict(exp)
        if name == "tiles70":
            e = {"refused": 70}
        c[name] = (nodes, e, bug if name != "tiles70" else False)
    # Au-delà de 100 tuiles, l'upstream jetait les triangles des tuiles suivantes
    tris = [tri_uv(u + 0.2, v + 0.2, u + 0.8, v + 0.2, u + 0.2, v + 0.8) for v in range(11) for u in range(10)]
    c["tiles110"] = ([("tiles110", tris, {})], {"refused": 110}, True)
    c["objects"] = (c["objects"][0], c["objects"][1], True)  # L'upstream fusionnait les nœuds sous le nom du premier
    # Nœud sans uv avec un nœud à uv : l'upstream rangeait ses sommets, sans uv, dans 1001 (lecture hors de tf)
    big = [T(((i, 0, 5), (0, 0)), ((i + 1, 0, 5), (0, 0)), ((i, 1, 5), (0, 0))) for i in range(100)]
    c["mixed_uv"] = ([("with_uv", [tri_uv(0.2, 0.2, 0.8, 0.2, 0.2, 0.8)], {}), ("no_uv", big, {"uv": False})],
                     {"tiles": {"with_uv": {1001}}, "invalid": 0, "cut": 0}, True)
    # Nœud sans uv en tête du fichier : il ne doit pas être le premier mesh, sinon ArmorPaint propose de déplier tout le
    # maillage (util_mesh_uv_unwrap) et les uv UDIM de toutes les tuiles avec
    def first_tiled(parts, meshes):
        ok = meshes and re.match(r"^.*\.1\d\d\d$", meshes[0]["name"])
        return [] if ok else ["premier mesh sans tuile (%s)" % (meshes[0]["name"] if meshes else "aucun")]

    c["no_uv_first"] = ([("no_uv", big, {"uv": False}), ("with_uv", [tri_uv(0.2, 0.2, 0.8, 0.2, 1.2, 0.8)], {})],
                        {"tiles": {"with_uv": {1001, 1002}}, "invalid": 0, "cut": 1, "extra": first_tiled}, True)
    # Second jeu d'uv et couleurs, sur un triangle coupé en deux tuiles
    t = tri_uv(0.5, 0.2, 1.5, 0.2, 0.5, 0.8)
    opt = {"uv1": [(0.1, 0.1), (0.9, 0.1), (0.1, 0.9)], "col": [(1, 0, 0, 1), (0, 1, 0, 1), (0, 0, 1, 0.5)]}
    nodes = [("attrs", [t], opt)]
    c["tex1_col"] = (nodes, {"tiles": {"attrs": {1001, 1002}}, "invalid": 0, "cut": 1, "extra": interp_check(nodes)}, True)
    c["empty"] = ([], {"null": True}, True)  # Aucun triangle : l'upstream lisait le tableau des meshes, vide
    return c


def material_cases():
    c = {}
    # 70 matériaux, un triangle chacun : l'upstream en gardait 64
    tris = [tri_uv(0.2, 0.2, 0.8, 0.2, 0.2, 0.8, z=float(i)) for i in range(70)]
    c["mat70"] = ([("mats", tris, {"mats": ["m%02d" % i for i in range(70)]})], {"meshes": ["m%02d" % i for i in range(70)], "tex": True}, True)
    # Un matériau sur un nœud à uv et un nœud sans uv : uv à 0 pour le second (l'upstream lisait tf au-delà de sa fin)
    big = [T(((i, 0, 5), (0, 0)), ((i + 1, 0, 5), (0, 0)), ((i, 1, 5), (0, 0))) for i in range(100)]
    c["mat_mixed_uv"] = ([("with_uv", [tri_uv(0.2, 0.2, 0.8, 0.2, 0.2, 0.8)], {"mats": ["shared"]}),
                          ("no_uv", big, {"uv": False, "mats": ["shared"] * 100})], {"meshes": ["shared"], "tex": True, "counts": [303]}, True)
    c["mat_empty"] = ([], {"meshes": [], "tex": False, "null": True}, True)
    return c


def check_material(out, nodes, exp):
    _, meshes, msgs = parse(out)
    if exp.get("null") and not msgs["null"]:
        return ["pas de NULL"]
    names = [m["name"] for m in meshes]
    if names != exp["meshes"]:
        return ["meshes %s, attendu %s" % (names[:5] + (["..."] if len(names) > 5 else []), exp["meshes"][:5])]
    errs = []
    for m in meshes:
        if exp["tex"] and not m["tex"]:
            errs.append("%s : pas d'uv" % m["name"])
    if "counts" in exp and [m["count"] for m in meshes] != exp["counts"]:
        errs.append("sommets %s, attendu %s" % ([m["count"] for m in meshes], exp["counts"]))
    return errs


def same_cases():
    """Nœuds [(nom, triangles, options)] et si le split par matériau est comparé : avec des nœuds avec et sans uv,
    l'upstream lisait tf au-delà de sa fin (mat_mixed_uv), l'arbre courant donne des uv à 0."""

    def grid(z, n=6, u0=0.0, v0=0.0):
        return [tri_uv(u0 + 0.15 * (i % 3), v0 + 0.15 * (i // 3), u0 + 0.15 * (i % 3) + 0.1, v0 + 0.15 * (i // 3), u0 + 0.15 * (i % 3),
                       v0 + 0.15 * (i // 3) + 0.1, z=z) for i in range(n)]

    c = {}
    c["one"] = ([("one", grid(0.0), {})], True)
    c["nodes"] = ([("a", grid(0.0), {}), ("b", grid(1.0), {}), ("c", grid(2.0, u0=0.5), {})], True)
    c["no_uv"] = ([("a", grid(0.0), {"uv": False}), ("b", grid(1.0), {"uv": False})], True)
    c["mixed_uv"] = ([("a", grid(0.0), {}), ("b", grid(1.0), {"uv": False})], False)
    attrs = {"uv1": [(0.1 * (k % 7), 0.05 * (k % 11)) for k in range(18)], "col": [((k % 3) / 2.0, (k % 5) / 4.0, 0.5, 1.0) for k in range(18)]}
    c["tex1_col"] = ([("a", grid(0.0), attrs), ("b", grid(1.0), attrs)], True)
    c["uv_out"] = ([("a", grid(0.0, u0=1.5, v0=-0.5), {}), ("b", grid(1.0, u0=-2.0, v0=3.0), {})], True)  # Pas d'UDIM en split 0 et 1
    mats = ["m%d" % (k % 5) for k in range(12)]
    c["mats"] = ([("a", grid(0.0, 12), {"mats": mats}), ("b", grid(1.0, 12), {"mats": mats[::-1]}), ("c", grid(2.0), {})], True)
    c["mats64"] = ([("a", grid(0.0, 64), {"mats": ["m%02d" % k for k in range(64)]})], True)
    return c


def same(d, exe, up):
    os.makedirs(d, exist_ok=True)
    fail = 0
    for name, (nodes, mat) in same_cases().items():
        path = os.path.join(d, name + ".fbx")
        write_fbx(path, nodes)
        for split in (0, 1) if mat else (0,):
            r = subprocess.run([exe, path, str(split)], capture_output=True, timeout=20)
            u = subprocess.run([up, path, str(split)], capture_output=True, timeout=20)
            ok = r.returncode == 0 and u.returncode == 0 and not r.stderr and r.stdout == u.stdout
            fail |= not ok
            meshes = r.stdout.count(b"MESH ")
            print("  %s %s (%s, %d meshes)" % ("✅" if ok else "❌", name, "objet" if split == 0 else "matériau", meshes))
    return fail


def fuzz(d, n, exes):
    seeds = [open(f, "rb").read() for f in sorted(glob.glob(os.path.join(d, "udim", "*.fbx")) + glob.glob(os.path.join(d, "same", "*.fbx")))]
    # ufbx lit nan et inf en notation MSVC (1.#QNAN, 1.#INF) ; « nan » tout court est lu comme le code du caractère
    values = [b"1.#QNAN", b"-1.#IND", b"1.#INF", b"-1.#INF", b"1e30", b"-1e30", b"1e-30", b"0", b"-0", b"-1", b"-0.5", b"0.99999", b"1.00001",
              b"9.99999", b"10", b"99", b"99.5", b"2147483647", b"-2147483648", b"4294967296"]
    toks = [b",", b"*", b"*0", b"*999999", b"{", b"}", b"\n", b" ", b'"', b":", b"a: ", b"-", b"LayerElementUV: 1 {", b"Layer: 1 {",
            b'MappingInformationType: "ByVertice"', b'ReferenceInformationType: "IndexToDirect"', b"UVIndex: *3 {\na: 0,1,99999\n}",
            b'C: "OO",1000,0', b'Model: 5, "Model::x", "Mesh" {\n}']
    num = re.compile(rb"-?\d+(?:\.\d+)?(?:e[-+]?\d+)?")
    fails = os.path.join(d, "fuzz_fail")
    os.makedirs(fails, exist_ok=True)
    for e, exe in enumerate(exes):
        random.seed(11)
        stats = {}
        read = 0  # Lectures qui passent le lecteur ufbx (un mesh ou un message UDIM) : le fuzz atteint io_fbx.c
        for k in range(n):
            data = bytearray(random.choice(seeds))
            for _ in range(random.randint(1, 6)):
                # Une mutation sur deux ne touche que des nombres : le fichier reste lisible par ufbx et io_fbx.c lit les valeurs
                op, at = random.random() * (0.45 if k % 2 == 0 else 1.0), random.randrange(len(data) + 1)
                if op < 0.45:  # Un nombre remplacé par une valeur extrême : surtout dans les tableaux (uv, positions, indices)
                    arrays = [m for a in re.finditer(rb"\ba: [^\n]*", data) for m in num.finditer(data, a.start() + 3, a.end())]
                    found = arrays if arrays and random.random() < 0.8 else list(num.finditer(data, max(0, at - 400), at + 400))
                    if found:
                        m = random.choice(found)
                        data[m.start():m.end()] = random.choice(values)
                elif op < 0.7:
                    data[at:at] = random.choice(toks)
                elif op < 0.85:
                    del data[at:at + random.randint(1, 6)]
                elif op < 0.95 and at < len(data):
                    data[at] = random.randrange(256)
                else:
                    data = data[:at]
            path = os.path.join(d, "fuzz.fbx")
            open(path, "wb").write(data)
            for split in (0, 1, 2):
                try:
                    r = subprocess.run([exe, path, str(split)], capture_output=True, timeout=20)
                    err = r.stderr.decode(errors="replace")
                    read += b"MESH " in r.stdout or b"UDIM import" in r.stdout
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
                    if e == 0:
                        open(os.path.join(fails, "%d_%d.fbx" % (k, split)), "wb").write(data)
        crashes = sum(v for k, v in stats.items() if not k.startswith("UB"))
        print("%s : %d lectures (%d passent ufbx), %d plantages %s" % (os.path.basename(exe), 3 * n, read, crashes, stats))


def main():
    d, exe = sys.argv[1], sys.argv[2]
    up = sys.argv[3] if len(sys.argv) > 3 else None
    os.makedirs(d, exist_ok=True)
    fail = 0
    for split, allc, chk in ((2, udim_cases(), check_udim), (1, material_cases(), check_material)):
        for name, (nodes, exp, bug) in allc.items():
            path = os.path.join(d, name + ".fbx")
            write_fbx(path, nodes)
            out, err = run(exe, path, split)
            errs = [err] if err else chk(out, nodes, exp)
            line = "  %s %s (%s)" % ("✅" if not errs else "❌", name, "udim" if split == 2 else "matériau")
            if errs:
                fail = 1
                line += " : " + " ; ".join(errs[:3])
            if up and bug:
                uout, uerr = run(up, path, split)
                if uerr:
                    uerrs = [uerr]
                elif split == 2:
                    uexp = {k: v for k, v in exp.items() if k not in ("invalid_uv", "refused")}
                    uexp.update(invalid=0, cut=0)
                    if "refused" in exp:  # Plus de tuiles que l'atlas : l'upstream doit au moins toutes les importer
                        got = len(set(t for p in parse(uout)[0] for t in p["tiles"]))
                        uerrs = ["%d tuiles sur %d, triangles perdus sans message" % (got, exp["refused"])] if got < exp["refused"] else []
                    else:
                        uerrs = check_udim(uout, nodes, uexp, upstream=True)
                else:
                    uerrs = chk(uout, nodes, exp)
                if uerrs:
                    line += "   [upstream : %s]" % uerrs[0][:70]
                else:
                    line += "   [upstream : ❌ ne montre pas le bug]"
                    fail = 1
            print(line)
    sys.exit(fail)


if __name__ == "__main__":
    if sys.argv[1] == "same":
        sys.exit(same(sys.argv[2], sys.argv[3], sys.argv[4]))
    elif sys.argv[1] == "fuzz":
        fuzz(sys.argv[2], int(sys.argv[3]), sys.argv[4:])
    else:
        main()
