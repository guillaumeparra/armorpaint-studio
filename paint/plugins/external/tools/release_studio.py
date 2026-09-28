#!/usr/bin/env python3
# Paquet binaire d'ArmorPaint Studio pour macOS : ArmorPaint-Studio-<version>-macos-<arch>.zip (ArmorPaint.app,
# setup_python.sh, requirements.txt, studio.env.example, README.md, LICENSE.md) et SHA256SUMS. Modèles dans
# release/studio/. Le bundle est vérifié avant l'archive : signature intacte, OCIO lié, rien d'écrit à l'exécution dans
# data/, version.json du commit courant, arbre git propre. Les bancs *_app.sh lancent l'app et écrivent config.json et
# studio_audit.jsonl dans son data/ (ce qui casse la signature) : pour une release, supprimer le .app et relancer
# `../base/make --compile` depuis paint/, dans un worktree propre (données du build sans reste d'un ancien build).
#
#   python3 tools/release_studio.py [--app <ArmorPaint.app>] [--out dist/studio] [--check]
#   --check : l'archive, extraite dans un dossier temporaire, garde sa signature, et l'app y démarre en --background
#             (external et OCIO chargés).
# Version : STUDIO_VERSION de studio.h. Versions épinglées de requirements.txt : celles de release_subzero.pins().

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from release_subzero import EPOCH, pins  # noqa: E402

EXT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.normpath(os.path.join(EXT, "..", "..", ".."))
TEMPLATE = os.path.join(EXT, "release", "studio")
LICENSE = os.path.join(REPO, "license.md")
BUILD = os.path.join(REPO, "paint", "build")
RUNTIME = ("config.json", "studio_prefs.json", "studio_audit.jsonl")  # écrits par l'app dans data/


def version():
    text = open(os.path.join(EXT, "studio.h"), encoding="utf-8").read()
    return re.search(r'^#define STUDIO_VERSION "([^"]+)"', text, re.M).group(1)


def run(*cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def fail(msg):
    raise SystemExit("release_studio: " + msg)


def head():
    """Commit court de HEAD ; refus si des fichiers suivis sont modifiés (l'app ne dirait pas ce qu'elle contient)."""
    dirty = run("git", "-C", REPO, "status", "--porcelain", "--untracked-files=no").stdout.strip()
    if dirty:
        fail("uncommitted changes:\n" + dirty)
    return run("git", "-C", REPO, "rev-parse", "--short=7", "HEAD").stdout.strip()


def verify_app(app, sha):
    """Contrôles du bundle ; rend son architecture et son macOS minimal."""
    exe = os.path.join(app, "Contents", "MacOS", "ArmorPaint")
    data = os.path.join(app, "Contents", "Resources", "out", "data")
    if not os.path.isfile(exe) or not os.path.isdir(data):
        fail("%s: not an ArmorPaint build" % app)
    written = [n for n in RUNTIME if os.path.exists(os.path.join(data, n))]
    built = os.path.join(os.path.dirname(app), "..", "..", "out", "data")  # <paint>/build/out/data
    if os.path.isdir(built):  # données copiées par le build : tout le reste a été écrit à l'exécution
        written += sorted(set(os.listdir(data)) - set(os.listdir(built)) - set(written))
    if written:
        fail("files written after the build in %s: %s; delete the .app and build it again" % (data, ", ".join(written)))
    got = run("codesign", "--verify", "--deep", "--strict", app)
    if got.returncode != 0:
        fail("signature: %s" % got.stderr.strip())
    arch = run("lipo", "-archs", exe).stdout.strip()
    minos = re.search(r"minos (\S+)", run("otool", "-l", exe).stdout)
    ocio = len(re.findall(r"studio_ocio_lib_", run("nm", exe).stdout))
    if ocio == 0:
        fail("OpenColorIO is not linked (tools/build_ocio.sh, then build again)")
    built_sha = re.search(r'"sha": "([^"]+)"', open(os.path.join(data, "version.json"), encoding="utf-8").read())
    if not built_sha or built_sha.group(1) != sha:
        fail("the app was built from %s, HEAD is %s" % (built_sha.group(1) if built_sha else "?", sha))
    print("app: %s, macOS %s+, OCIO linked (%d symbols), built from %s, signature ok" % (
        arch, minos.group(1) if minos else "?", ocio, sha))
    return arch


def fill(text, values):
    for k, v in values.items():
        text = text.replace("@%s@" % k, v)
    return text


def stage(folder, app, values):
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    subprocess.run(["ditto", "--norsrc", app, os.path.join(folder, "ArmorPaint.app")], check=True)
    for name in sorted(os.listdir(TEMPLATE)):
        data = open(os.path.join(TEMPLATE, name), encoding="utf-8").read()
        with open(os.path.join(folder, name), "w", encoding="utf-8") as f:
            f.write(fill(data, values) if name.endswith(".md") else data)
        os.chmod(os.path.join(folder, name), 0o755 if name.endswith(".sh") else 0o644)
    with open(os.path.join(folder, "requirements.txt"), "w") as f:
        f.write("".join("%s==%s\n" % kv for kv in pins().items()))
    shutil.copyfile(LICENSE, os.path.join(folder, "LICENSE.md"))
    # Dates fixées : même archive pour le même build.
    for root, dirs, names in os.walk(folder, topdown=False):
        for n in dirs + names:
            os.utime(os.path.join(root, n), (EPOCH, EPOCH), follow_symlinks=False)
    os.utime(folder, (EPOCH, EPOCH))


def check(archive, v):
    """L'archive extraite : signature, et un démarrage en --background (external et OCIO chargés)."""
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(["ditto", "-x", "-k", archive, tmp], check=True)
        top = os.path.join(tmp, os.path.basename(archive)[:-len(".zip")])
        app = os.path.join(top, "ArmorPaint.app")
        got = run("codesign", "--verify", "--deep", "--strict", app)
        if got.returncode != 0:
            fail("check: extracted app: %s" % got.stderr.strip())
        script = os.path.join(tmp, "smoke.c")
        with open(script, "w") as f:
            f.write('int main() {\n\tconsole_log(string("ocio=%s", studio_ocio_status()));\n\treturn 0;\n}\n')
        env = dict(os.environ, STUDIO_ENV="")
        env.pop("OCIO", None)
        got = subprocess.run(["perl", "-e", "alarm 60; exec @ARGV", os.path.join(app, "Contents", "MacOS", "ArmorPaint"),
                              "--background", "--script", script, "-ApplePersistenceIgnoreState", "YES"],
                             env=env, capture_output=True, text=True)
        out = got.stdout + got.stderr
        loaded = "studio: external %s loaded" % v in out
        ocio = re.search(r"^ocio=(OCIO \S+.*)$", out, re.M)
        if got.returncode != 0 or not loaded or not ocio:
            fail("check: the extracted app did not start (rc %d):\n%s" % (got.returncode, out[-2000:]))
        for name in ("setup_python.sh", "requirements.txt", "studio.env.example", "README.md", "LICENSE.md"):
            if not os.path.isfile(os.path.join(top, name)):
                fail("check: %s missing" % name)
    print("check: extracted app signed, starts in --background: external %s, %s" % (v, ocio.group(1)[:60]))


def main():
    ap = argparse.ArgumentParser(description="Build the macOS package of ArmorPaint Studio")
    ap.add_argument("--app", default=os.path.join(BUILD, "build", "Release", "ArmorPaint.app"))
    ap.add_argument("--out", default=os.path.join(EXT, "dist", "studio"))
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    if sys.platform != "darwin":
        fail("macOS only for now")
    v, sha = version(), head()
    arch = verify_app(os.path.abspath(a.app), sha)
    p = pins()
    values = {"VERSION": v, "SHA": sha, "PYSIDE": p["PySide6"], "OCIO": p["opencolorio"], "NUMPY": p["numpy"]}
    top = "ArmorPaint-Studio-%s-macos-%s" % (v, arch)
    os.makedirs(a.out, exist_ok=True)
    folder = os.path.join(a.out, top)
    stage(folder, os.path.abspath(a.app), values)
    archive = folder + ".zip"
    if os.path.exists(archive):
        os.remove(archive)
    subprocess.run(["ditto", "-c", "-k", "--norsrc", "--noqtn", "--keepParent", folder, archive], check=True)
    digest = hashlib.sha256(open(archive, "rb").read()).hexdigest()
    with open(os.path.join(a.out, "SHA256SUMS"), "w") as f:
        f.write("%s  %s\n" % (digest, os.path.basename(archive)))
    print("%s (%d MB)" % (archive, os.path.getsize(archive) // (1024 * 1024)))
    if a.check:
        check(archive, v)


if __name__ == "__main__":
    main()
