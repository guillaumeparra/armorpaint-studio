#!/usr/bin/env python3
# studio.env côté Python (lots E5 et E11) : même syntaxe et mêmes règles que studio_env.c, qui le lit au démarrage
# d'ArmorPaint. La fenêtre subzero s'en sert pour son panneau Settings : origine de chaque variable, et écriture des
# réglages dans le fichier (une ligne par variable, commentaires et autres lignes gardés). Bibliothèque standard seule.
#
# Fichiers : $STUDIO_ENV (un ou plusieurs, séparés par ':' — ';' sous Windows ; vide : aucun), sinon studio.env dans
# le dossier de l'utilisateur d'ArmorPaint (iron_internal_save_path) :
#   macOS    ~/Library/Application Support/ArmorPaint/studio.env
#   Linux    ~/.ArmorPaint/studio.env s'il existe, sinon $XDG_DATA_HOME/ArmorPaint/ (~/.local/share/ArmorPaint/)
#   Windows  %APPDATA%\ArmorPaint\studio.env
# Une variable déjà définie est gardée : le premier qui la définit gagne (vraie variable d'environnement, puis fichiers).

import os
import re
import sys

APP = "ArmorPaint"
FILE = "studio.env"
LIST_SEP = ";" if sys.platform == "win32" else ":"
NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
ENVIRONMENT = "environment"  # origine d'une vraie variable d'environnement


def _home():
    return os.environ.get("USERPROFILE" if sys.platform == "win32" else "HOME") or os.path.expanduser("~")


def user_dir():
    """Dossier de l'utilisateur d'ArmorPaint, comme iron_internal_save_path."""
    if sys.platform == "darwin":
        return os.path.join(_home(), "Library", "Application Support", APP)
    if sys.platform == "win32":
        return os.path.join(os.environ.get("APPDATA") or _home(), APP)
    dot = os.path.join(_home(), "." + APP)
    if os.path.isdir(dot):
        return dot
    return os.path.join(os.environ.get("XDG_DATA_HOME") or os.path.join(_home(), ".local", "share"), APP)


def files():
    """Fichiers lus, dans l'ordre : ceux de $STUDIO_ENV (liste vide s'il est vide), sinon celui du dossier de
    l'utilisateur, qu'il existe ou non."""
    listed = os.environ.get("STUDIO_ENV")
    if listed is None:
        return [os.path.join(user_dir(), FILE)]
    out = []
    for p in listed.split(LIST_SEP):
        if p:
            out.append(_home() + p[1:] if p == "~" or p.startswith("~/") else p)
    return out


def write_target():
    """Fichier où la fenêtre écrit ses réglages : le premier lu. None si $STUDIO_ENV est vide."""
    f = files()
    return f[0] if f else None


# ---- Lecture ------------------------------------------------------------------------------------------------------------


class Line(object):
    """Affectation lue : nom, valeur (étendue), numéro de ligne (1 = première), texte brut."""

    def __init__(self, name, value, no, raw):
        self.name, self.value, self.no, self.raw = name, value, no, raw


def _expand(text, i, out, env):
    # $VAR ou ${VAR} en text[i] == '$' : valeur ajoutée à out, rend l'indice qui suit ; sans nom valide, '$' tel quel.
    j = i + 1
    brace = j < len(text) and text[j] == "{"
    if brace:
        j += 1
    m = NAME.match(text, j)
    if m is None or (brace and not text.startswith("}", m.end())):
        out.append("$")
        return i + 1
    out.append(env.get(m.group(0), ""))
    return m.end() + (1 if brace else 0)


def _only_comment(rest):
    rest = rest.lstrip(" \t")
    return rest == "" or rest.startswith("#")


def parse_line(line, env=None):
    """(nom, valeur) d'une ligne, None pour une ligne vide ou un commentaire ; lève ValueError sur une ligne invalide
    (studio_env.c l'ignore avec un avertissement). env : variables pour $VAR (défaut os.environ)."""
    env = os.environ if env is None else env
    if line.endswith("\r"):
        line = line[:-1]
    p = line.lstrip(" \t")
    if p == "" or p.startswith("#"):
        return None
    if p.startswith("export") and p[6:7] in (" ", "\t"):
        p = p[6:].lstrip(" \t")
    m = NAME.match(p)
    if m is None:
        raise ValueError("not a NAME=value line")
    name = m.group(0)
    p = p[m.end():].lstrip(" \t")
    if not p.startswith("="):
        raise ValueError("not a NAME=value line")
    p = p[1:].lstrip(" \t")
    out = []
    if p.startswith("'"):
        close = p.find("'", 1)
        if close < 0:
            raise ValueError("unterminated quote")
        if not _only_comment(p[close + 1:]):
            raise ValueError("text after the closing quote")
        return name, p[1:close]
    if p.startswith('"'):
        i = 1
        while i < len(p) and p[i] != '"':
            if p[i] == "\\" and p[i + 1:i + 2] in ('"', "\\", "$"):
                out.append(p[i + 1])
                i += 2
            elif p[i] == "$":
                i = _expand(p, i, out, env)
            else:
                out.append(p[i])
                i += 1
        if i >= len(p):
            raise ValueError("unterminated quote")
        if not _only_comment(p[i + 1:]):
            raise ValueError("text after the closing quote")
        return name, "".join(out)
    end = 0
    while end < len(p) and not (p[end] == "#" and end > 0 and p[end - 1] in " \t"):
        end += 1
    while end > 0 and p[end - 1] in " \t":
        end -= 1
    i = 0
    if p.startswith("~") and (end == 1 or p[1:2] == "/"):
        out.append(_home())
        i = 1
    while i < end:
        if p[i] == "$":
            i = min(_expand(p[:end], i, out, env), end)
        else:
            out.append(p[i])
            i += 1
    return name, "".join(out)


def read(path, env=None):
    """Affectations du fichier, dans l'ordre ([] s'il n'existe pas). Les lignes invalides sont sautées. Les $VAR
    voient les variables des lignes précédentes, comme au chargement."""
    try:
        with open(path, encoding="utf-8", newline="") as f:
            text = f.read()
    except FileNotFoundError:
        return []
    env = dict(os.environ if env is None else env)
    out = []
    for no, raw in enumerate(text.split("\n"), 1):
        try:
            parsed = parse_line(raw, env)
        except ValueError:
            continue
        if parsed is not None:
            out.append(Line(parsed[0], parsed[1], no, raw))
            env.setdefault(parsed[0], parsed[1])
    return out


LOADED = {}  # {nom: fichier} des variables prises par load() dans ce processus


def load(paths=None):
    """Charge les fichiers dans os.environ comme studio_env_init (lanceur autonome) ; rend {nom: origine} des
    variables prises, gardées aussi dans LOADED."""
    taken = {}
    for path in files() if paths is None else paths:
        for line in read(path):
            if line.name not in os.environ:
                os.environ[line.name] = line.value
                taken[line.name] = path
    LOADED.update(taken)
    return taken


def origin(name, env=None):
    """Origine d'une variable du processus : le fichier qui l'a donnée par load(), sinon ENVIRONMENT ; None si elle
    n'est pas définie."""
    env = os.environ if env is None else env
    if name not in env:
        return None
    return LOADED.get(name, ENVIRONMENT)


def defined(paths=None):
    """{nom: (valeur, fichier)} que donnent les fichiers, sans l'environnement : la première définition gagne."""
    out = {}
    for path in files() if paths is None else paths:
        for line in read(path):
            out.setdefault(line.name, (line.value, path))
    return out


# ---- Écriture -----------------------------------------------------------------------------------------------------------

_BARE = re.compile(r"[A-Za-z0-9_./:@+,=-]*")


def quote(value):
    """Valeur écrite de sorte que studio_env.c relise exactement value : telle quelle si elle est simple, entre '…'
    sinon, entre "…" (\\ " $ échappés) si elle contient une apostrophe."""
    if _BARE.fullmatch(value) and not value.startswith("~"):
        return value
    if "'" not in value and "\n" not in value:
        return "'%s'" % value
    return '"%s"' % value.replace("\\", "\\\\").replace('"', '\\"').replace("$", "\\$")


def update(path, changes, header="# subzero settings"):
    """Écrit changes ({nom: valeur, ou None pour retirer}) dans path : la première affectation de chaque nom est
    remplacée (« export » gardé), les suivantes retirées ; un nom absent est ajouté en fin de fichier, sous header.
    Le reste du fichier ne bouge pas. Fichier et dossier créés au besoin ; écriture atomique."""
    if "\n" in "".join(v for v in changes.values() if v is not None):
        raise ValueError("a value cannot span several lines")
    try:
        with open(path, encoding="utf-8", newline="") as f:
            lines = f.read().split("\n")
    except FileNotFoundError:
        lines = [""]
    done = set()
    out = []
    for raw in lines:
        try:
            parsed = parse_line(raw, {})
        except ValueError:
            parsed = None
        name = parsed[0] if parsed else None
        if name not in changes:
            out.append(raw)
            continue
        if name in done or changes[name] is None:
            done.add(name)
            continue
        done.add(name)
        export = "export " if re.match(r"[ \t]*export[ \t]", raw) else ""
        out.append("%s%s=%s" % (export, name, quote(changes[name])))
    added = ["%s=%s" % (n, quote(v)) for n, v in changes.items() if n not in done and v is not None]
    if added:
        while out and out[-1] == "":
            out.pop()
        if out:
            out.append("")
        if header and header not in out:
            out.append(header)
        out += added
    if not out or out[-1] != "":
        out.append("")
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="") as f:
        f.write("\n".join(out))
    os.replace(tmp, path)


def store(path, desired, paths=None, header="# subzero settings"):
    """Écrit dans path ce qu'il faut pour que la lecture des fichiers (paths, défaut files()) donne desired ({nom:
    valeur, None ou "" pour le défaut}) : rien si c'est déjà le cas, la valeur sinon, "" pour masquer celle d'un
    fichier lu après path. Rend les changements écrits ({nom: valeur ou None pour une ligne retirée})."""
    chain = files() if paths is None else list(paths)
    after = chain[chain.index(path) + 1:] if path in chain else []
    own, later = {}, defined(after)
    for line in read(path):
        own.setdefault(line.name, line.value)
    changes = {}
    for name, value in desired.items():
        value = value or None
        below = later.get(name, ("", None))[0]
        if value is None:
            want = "" if below else None
        else:
            want = None if name not in own and below == value else value
        if own.get(name) != want:
            changes[name] = want
    if changes:
        update(path, changes, header)
    return changes
