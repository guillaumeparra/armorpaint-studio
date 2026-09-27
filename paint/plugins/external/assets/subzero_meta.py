#!/usr/bin/env python3
# Description d'un .sbsar pour subzero (chantier E1), bibliothèque standard seule (Python 3.8+). Repris de l'étude
# (recherche-live/l5/sbsar_meta.py), validé sur les 977 .sbsar de l'étagère de Painter et contre `sbsrender info`.
#
# Un .sbsar est une archive 7z : assemblies/content/NNNN/<nom>.sbsasm (programme compilé, opaque), <nom>.xml
# (description : graphes, entrées, sorties, presets), icônes PNG, ressources annexes. Ce module lit le XML et en tire
# tout ce qu'une interface de réglage façon Painter doit connaître.
#
#   python3 subzero_meta.py fichier.sbsar            # résumé lisible
#   python3 subzero_meta.py fichier.sbsar --json     # description complète en JSON
#   python3 subzero_meta.py fichier.sbsar --args wood_color=0.5,0.4,0.3 age=0.2   # arguments sbsrender

import json
import re
import sys
import xml.etree.ElementTree as ET

from subzero_7z import read_7z

# Types d'entrée/sortie (attribut type du XML) ; vérifié contre `sbsrender info` sur colorful_planks.
TYPES = {0: "float1", 1: "float2", 2: "float3", 3: "float4", 4: "int1", 5: "image",
         6: "string", 7: "font", 8: "int2", 9: "int3", 10: "int4"}
NCOMP = {"float1": 1, "float2": 2, "float3": 3, "float4": 4, "int1": 1, "int2": 2, "int3": 3, "int4": 4}


def decode_format(fmt):
    """Attribut format d'une sortie image -> (canaux, précision).
    Mesuré : 0 RGBA8, 16 RGBA16, 12 L8, 28 L16 (PNG écrits et grilles de quantification) ;
    déduit : 0x40 = flottant (64 RGBA32F, 80 RGBA16F, 76 L32F, 92 L16F) — PROBABLE."""
    if fmt is None:
        return None
    f = int(fmt)
    chans = {0x0: "RGBA", 0x4: "RGBx", 0x8: "RGB", 0xC: "L"}.get(f & 0xC, "?")
    prec = {0x00: "8", 0x10: "16", 0x40: "32F", 0x50: "16F"}.get(f & 0x50, "?")
    if f & ~0x5C:
        prec += "?(%d)" % f
    return chans, prec


def parse_usertag(s):
    """'disable=true;colorspace=normalxyzleft' -> {'disable': 'true', 'colorspace': 'normalxyzleft'}
    (clés en minuscules ; une clé sans '=' vaut '')."""
    out = {}
    for part in (s or "").split(";"):
        part = part.strip()
        if not part:
            continue
        k, _, v = part.partition("=")
        out[k.strip().lower()] = v.strip()
    return out


def parse_value(typ, text):
    if text is None:
        return None
    if typ in ("string", "font", "image"):
        return text
    parts = [p for p in re.split(r"[ ,]+", text.strip()) if p]
    conv = int if typ.startswith("int") else float
    try:
        vals = [conv(float(p)) if conv is int else conv(p) for p in parts]
    except ValueError:
        return text
    return vals[0] if len(vals) == 1 else vals


def format_value(typ, v):
    """Valeur -> texte pour sbsrender --set-value id@texte (composantes séparées par des virgules)."""
    if isinstance(v, (list, tuple)):
        return ",".join(format_value(typ, x) for x in v)
    if typ.startswith("int"):
        return str(int(v))
    return repr(float(v))


# --------------------------------------------------------------------------- visibleif
_TOK = re.compile(r'\s*(?:(input)\s*\[\s*"([^"]*)"\s*\]|(input)\s*\.\s*([A-Za-z_$][\w$]*)|'
                  r'(\d+\.?\d*(?:[eE][-+]?\d+)?|\.\d+)|(true|false)|(&&|\|\||==|!=|<=|>=|[!<>()+\-*/%])|'
                  r'\.([xyzw]))')


def _tokenize(expr):
    pos, out = 0, []
    while pos < len(expr):
        if expr[pos:].strip() == "":
            break
        m = _TOK.match(expr, pos)
        if not m:
            raise ValueError("visibleif illisible à %d : %r" % (pos, expr))
        pos = m.end()
        if m.group(1) or m.group(3):
            out.append(("id", m.group(2) if m.group(1) else m.group(4)))
        elif m.group(5):
            out.append(("num", float(m.group(5))))
        elif m.group(6):
            out.append(("num", 1.0 if m.group(6) == "true" else 0.0))
        elif m.group(7):
            out.append(("op", m.group(7)))
        else:
            out.append(("comp", "xyzw".index(m.group(8))))
    return out


_BIN = {"||": 1, "&&": 2, "==": 3, "!=": 3, "<": 4, ">": 4, "<=": 4, ">=": 4, "+": 5, "-": 5, "*": 6, "/": 6, "%": 6}


def compile_visibleif(expr):
    """Compile une condition de visibilité en fonction(valeurs) -> bool. Grammaire relevée sur les
    977 sbsar de Painter : input["x"] ou input.x, nombres, true/false, ! && || == != < > <= >=, ()."""
    toks = _tokenize(expr)
    pos = [0]

    def peek():
        return toks[pos[0]] if pos[0] < len(toks) else ("end", None)

    def take():
        t = peek()
        pos[0] += 1
        return t

    def primary():
        t = take()
        if t[0] == "num":
            v = t[1]
            return lambda env: v
        if t[0] == "id":
            name = t[1]
            if peek()[0] == "comp":
                c = take()[1]
                return lambda env: _num(env.get(name, 0), c)
            return lambda env: _num(env.get(name, 0))
        if t == ("op", "!"):
            f = primary()
            return lambda env: 0.0 if f(env) else 1.0
        if t == ("op", "-"):
            f = primary()
            return lambda env: -f(env)
        if t == ("op", "("):
            f = binary(0)
            if take() != ("op", ")"):
                raise ValueError("parenthèse manquante : %r" % expr)
            return f
        raise ValueError("jeton inattendu %r dans %r" % (t, expr))

    def binary(minp):
        left = primary()
        while True:
            t = peek()
            if t[0] != "op" or t[1] not in _BIN or _BIN[t[1]] <= minp:
                return left
            op = take()[1]
            right = binary(_BIN[op])
            left = _combine(op, left, right)

    f = binary(0)
    if pos[0] != len(toks):
        raise ValueError("fin inattendue dans %r" % expr)
    return lambda env: bool(f(env))


def _num(v, comp=None):
    if isinstance(v, (list, tuple)):
        v = v[comp or 0]
    if isinstance(v, bool):
        return 1.0 if v else 0.0
    try:
        return float(v)
    except (TypeError, ValueError):
        return 1.0 if v else 0.0


def _combine(op, a, b):
    import operator as o
    fn = {"||": lambda x, y: 1.0 if (x or y) else 0.0, "&&": lambda x, y: 1.0 if (x and y) else 0.0,
          "==": lambda x, y: float(x == y), "!=": lambda x, y: float(x != y), "<": lambda x, y: float(x < y),
          ">": lambda x, y: float(x > y), "<=": lambda x, y: float(x <= y), ">=": lambda x, y: float(x >= y),
          "+": o.add, "-": o.sub, "*": o.mul, "/": lambda x, y: x / y if y else 0.0, "%": lambda x, y: x % y if y else 0.0}[op]
    return lambda env: fn(a(env), b(env))


# --------------------------------------------------------------------------- lecture
def _gui(elem):
    return elem.find("inputgui") if elem.tag == "input" else elem.find("outputgui")


def parse_xml(data):
    root = ET.fromstring(data)
    graphs = []
    for g in root.findall("./graphs/graph"):
        outputs, by_uid = [], {}
        for o in g.findall("./outputs/output"):
            typ = TYPES.get(int(o.get("type", 5)), o.get("type"))
            gui = _gui(o)
            chans = o.findall(".//channel")
            ut = parse_usertag(o.get("usertag"))
            cs = [c.get("colorspace") for c in chans if c.get("colorspace")] or ([ut["colorspace"]] if "colorspace" in ut else [])
            out = {
                "identifier": o.get("identifier"), "uid": o.get("uid"), "type": typ,
                "format": decode_format(o.get("format")) if typ == "image" else None,
                "format_raw": o.get("format"),
                "usages": [u for c in chans for u in (c.get("names") or "").split(";") if u],
                "colorspace": cs[0] if cs else "",
                "usertag": ut, "mipmaps": o.get("mipmaps"), "dynamicsize": o.get("dynamicsize") == "yes",
                "label": gui.get("label") if gui is not None else o.get("identifier"),
                "group": gui.get("group") if gui is not None else None,
                "visibleif": gui.get("visibleif") if gui is not None else None,
            }
            outputs.append(out)
            by_uid[out["uid"]] = out["identifier"]
        inputs = []
        for i in g.findall("./inputs/input"):
            typ = TYPES.get(int(i.get("type")), i.get("type"))
            gui = _gui(i)
            inp = {
                "identifier": i.get("identifier"), "uid": i.get("uid"), "type": typ,
                "default": parse_value(typ, i.get("default")),
                "system": i.get("identifier", "").startswith("$"),
                "usertag": parse_usertag(i.get("usertag")),
                "alters": [by_uid.get(u, u) for u in (i.get("alteroutputs") or "").split(",") if u],
                "label": i.get("identifier"), "widget": None, "group": None, "visibleif": None,
                "showas": None, "description": None,
            }
            if gui is not None:
                inp.update(label=gui.get("label") or inp["identifier"], widget=gui.get("widget"),
                           group=gui.get("group"), visibleif=gui.get("visibleif"),
                           showas=gui.get("showas"), description=gui.get("description"))
                for tag in ("guislider", "guiangle"):
                    s = gui.find(tag)
                    if s is not None:
                        inp["min"] = parse_value(typ, s.get("min"))
                        inp["max"] = parse_value(typ, s.get("max"))
                        inp["step"] = parse_value("float1", s.get("step"))
                        inp["clamp"] = s.get("clamp") == "on"
                        labels = [s.get("label%d" % k) for k in range(4) if s.get("label%d" % k)]
                        if labels:
                            inp["component_labels"] = labels
                cb = gui.find("guicombobox")
                if cb is not None:
                    inp["items"] = [(int(it.get("value")), it.get("text")) for it in cb.findall("guicomboboxitem")]
                bt = gui.find("guibutton")
                if bt is not None:
                    inp["button_labels"] = (bt.get("label0"), bt.get("label1"))
                im = gui.find("guiimage")
                if im is not None:
                    inp["image"] = {"colortype": im.get("colortype"), "format": im.get("format")}
                if typ == "image":
                    inp["usages"] = [u for c in gui.findall(".//channel") for u in (c.get("names") or "").split(";") if u]
            inputs.append(inp)
        presets = []
        for p in g.findall("./sbspresets/sbspreset"):
            vals = {}
            for pi in p.findall("presetinput"):
                typ = TYPES.get(int(pi.get("type")), pi.get("type"))
                vals[pi.get("identifier")] = parse_value(typ, pi.get("value"))
            presets.append({"label": p.get("label"), "usertags": p.get("usertags"), "values": vals})
        ps = g.get("physicalSize")
        graphs.append({
            "pkgurl": g.get("pkgurl"), "label": g.get("label"), "graphtype": g.get("graphtype"),
            "category": g.get("category"), "keywords": (g.get("keywords") or "").split(";") if g.get("keywords") else [],
            "author": g.get("author"), "description": g.get("description"), "icon": g.get("icon"),
            "usertag": g.get("usertag"), "primaryinput": g.get("primaryinput"),
            "physical_size": [float(x) for x in ps.split(",")] if ps else None,
            "inputs": inputs, "outputs": outputs, "presets": presets,
        })
    return {"formatversion": root.get("formatversion"), "asmuid": root.get("asmuid"),
            "cookerbuild": root.get("cookerbuild"), "graphs": graphs}


def read_sbsar(path):
    members = read_7z(path)
    descs = []
    for name, data in members.items():
        if name.endswith(".xml"):
            d = parse_xml(data)
            d["member"] = name
            descs.append(d)
    return {"path": path, "members": sorted(members), "assemblies": descs}


# --------------------------------------------------------------------------- aides pour l'hôte
def visible_inputs(graph, values):
    """Identifiants des entrées visibles pour un jeu de valeurs (défauts complétés)."""
    env = {i["identifier"]: i["default"] for i in graph["inputs"]}
    env.update(values)
    vis = []
    for i in graph["inputs"]:
        if i["system"] or i["widget"] is None:
            continue
        cond = i["visibleif"]
        if not cond or compile_visibleif(cond)(env):
            vis.append(i["identifier"])
    return vis


def affected_outputs(graph, changed):
    """Sorties à recalculer quand les entrées `changed` changent (attribut alteroutputs)."""
    by_id = {i["identifier"]: i for i in graph["inputs"]}
    out = set()
    for c in changed:
        out.update(by_id[c]["alters"])
    return [o["identifier"] for o in graph["outputs"] if o["identifier"] in out]


def sbsrender_args(graph, values):
    by_id = {i["identifier"]: i for i in graph["inputs"]}
    args = ["--input-graph", graph["pkgurl"]]
    for k, v in values.items():
        i = by_id[k]
        if i["type"] == "image":
            args += ["--set-entry", "%s@%s" % (k, v)]
        else:
            args += ["--set-value", "%s@%s" % (k, format_value(i["type"], v))]
    return args


def summary(desc):
    lines = []
    for a in desc["assemblies"]:
        for g in a["graphs"]:
            lines.append("GRAPHE %s  « %s »  type=%s  taille physique=%s" % (
                g["pkgurl"], g["label"], g["graphtype"], g["physical_size"]))
            for i in g["inputs"]:
                extra = ""
                if "min" in i:
                    extra = " [%s..%s pas %s%s]" % (i["min"], i["max"], i["step"], " borné" if i["clamp"] else "")
                if "items" in i:
                    extra = " {" + ", ".join("%d=%s" % it for it in i["items"]) + "}"
                if i["type"] == "image":
                    extra = " usages=%s" % i.get("usages")
                lines.append("  in  %-28s %-7s %-12s %-24s grp=%s déf=%s%s%s%s" % (
                    i["identifier"], i["type"], i["widget"], (i["label"] or "")[:24], i["group"], i["default"], extra,
                    "  si " + i["visibleif"] if i["visibleif"] else "", "  tag=" + json.dumps(i["usertag"]) if i["usertag"] else ""))
            for o in g["outputs"]:
                lines.append("  out %-28s %-6s %-12s usages=%s cs=%r tag=%s" % (
                    o["identifier"], o["type"], "%s/%s" % o["format"] if o["format"] else "", o["usages"], o["colorspace"], o["usertag"] or ""))
            for p in g["presets"]:
                lines.append("  preset « %s » (%d valeurs)" % (p["label"], len(p["values"])))
    return "\n".join(lines)


if __name__ == "__main__":
    d = read_sbsar(sys.argv[1])
    if "--json" in sys.argv:
        print(json.dumps(d, indent=1, ensure_ascii=False))
    elif "--args" in sys.argv:
        g = d["assemblies"][0]["graphs"][0]
        vals = {}
        for kv in sys.argv[sys.argv.index("--args") + 1:]:
            k, _, v = kv.partition("=")
            typ = next(i["type"] for i in g["inputs"] if i["identifier"] == k)
            vals[k] = parse_value(typ, v)
        print(" ".join(json.dumps(a) if " " in a else a for a in sbsrender_args(g, vals)))
        print("sorties touchées :", affected_outputs(g, vals.keys()))
    else:
        print(summary(d))
