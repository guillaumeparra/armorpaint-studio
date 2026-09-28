#!/usr/bin/env python3
# Paquet binaire d'ArmorPaint Studio, sur le système du build :
# - macOS : ArmorPaint-Studio-<version>-macos-<arch>.zip (ArmorPaint.app, signature intacte, OCIO lié en statique) ;
# - Linux : ArmorPaint-Studio-<version>-linux-<arch>.tar.gz (ArmorPaint/ : binaire, data/ avec libstudio_ocio.so).
# Avec setup_python.sh, requirements.txt, studio.env.example, README.md, LICENSE.md, et SHA256SUMS. Modèles dans
# release/studio/ (README.<système>.md). L'app est vérifiée avant l'archive : OCIO présent, rien d'écrit à l'exécution
# dans data/, version.json du commit courant, arbre git propre. Les bancs *_app.sh lancent l'app, qui écrit
# config.json et studio_audit.jsonl dans son data/ (sur macOS, la signature casse) : pour une release, build neuf dans
# un clone propre au commit (données du build sans reste d'un ancien build), empaqueté avant tout banc.
#
#   python3 tools/release_studio.py [--app <ArmorPaint.app | dossier out/>] [--out dist/studio] [--check]
#   --check : l'archive extraite dans un dossier temporaire (signature gardée sur macOS) démarre en --background, depuis
#             un autre dossier courant, external et OCIO chargés. Sous Linux, il faut un display X (DISPLAY).
# Version : STUDIO_VERSION de studio.h. Versions épinglées de requirements.txt : celles de release_subzero.pins().

import argparse
import gzip
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from release_subzero import EPOCH, pins  # noqa: E402

EXT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.normpath(os.path.join(EXT, "..", "..", ".."))
TEMPLATE = os.path.join(EXT, "release", "studio")
LICENSE = os.path.join(REPO, "license.md")
BUILD = os.path.join(REPO, "paint", "build")
RUNTIME = ("config.json", "studio_prefs.json", "studio_audit.jsonl")  # écrits par l'app dans data/
LINUX_TOP = ("ArmorPaint", "data", "icon.png", "readme.txt")  # dossier out/ d'un build Linux
SYSTEM = {"darwin": "macos", "linux": "linux"}.get(sys.platform)
# Valeurs des modèles propres au système (README.<système>.md, studio.env.example).
PLATFORM = {
    "macos": {"USER_DIR": "/Users/me/Library/Application Support/ArmorPaint", "DESIGNER_WHERE": "/Applications",
              "SBSRENDER": "'/Applications/Adobe Substance 3D Designer'", "LAUNCHED": "from the Finder or the Dock",
              "STUDIO_ENV": "~/Library/Application Support/ArmorPaint/studio.env"},
    "linux": {"USER_DIR": "/home/me/.local/share/ArmorPaint", "DESIGNER_WHERE": "/opt/Adobe",
              "SBSRENDER": "/opt/Adobe/Adobe_Substance_3D_Designer", "LAUNCHED": "from a desktop launcher",
              "STUDIO_ENV": "~/.local/share/ArmorPaint/studio.env (~/.ArmorPaint/studio.env when that folder exists)"},
}


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


def check_data(data, sha, built=None):
    """data/ tel que le build l'a laissé (built : copie du build, tout le reste a été écrit à l'exécution), et
    version.json du commit courant."""
    written = [n for n in RUNTIME if os.path.exists(os.path.join(data, n))]
    if built and os.path.isdir(built):
        written += sorted(set(os.listdir(data)) - set(os.listdir(built)) - set(written))
    if written:
        fail("files written after the build in %s: %s; build again from a clean tree" % (data, ", ".join(written)))
    built_sha = re.search(r'"sha": "([^"]+)"', open(os.path.join(data, "version.json"), encoding="utf-8").read())
    if not built_sha or built_sha.group(1) != sha:
        fail("the app was built from %s, HEAD is %s" % (built_sha.group(1) if built_sha else "?", sha))


def verify_macos(app, sha):
    """Contrôles du bundle ; rend son architecture."""
    exe = os.path.join(app, "Contents", "MacOS", "ArmorPaint")
    data = os.path.join(app, "Contents", "Resources", "out", "data")
    if not os.path.isfile(exe) or not os.path.isdir(data):
        fail("%s: not an ArmorPaint build" % app)
    check_data(data, sha, os.path.join(os.path.dirname(app), "..", "..", "out", "data"))  # <paint>/build/out/data
    got = run("codesign", "--verify", "--deep", "--strict", app)
    if got.returncode != 0:
        fail("signature: %s" % got.stderr.strip())
    arch = run("lipo", "-archs", exe).stdout.strip()
    minos = re.search(r"minos (\S+)", run("otool", "-l", exe).stdout)
    ocio = len(re.findall(r"studio_ocio_lib_", run("nm", exe).stdout))
    if ocio == 0:
        fail("OpenColorIO is not linked (tools/build_ocio.sh, then build again)")
    print("app: %s, macOS %s+, OCIO linked (%d symbols), built from %s, signature ok" % (
        arch, minos.group(1) if minos else "?", ocio, sha))
    return arch


def floor(files, prefix):
    """Plus haute version de symbole <prefix>_x.y demandée par ces binaires (glibc, libstdc++) : leur plancher."""
    found = re.findall(r"\b%s_([0-9.]+)" % prefix, run("objdump", "-T", *files).stdout)
    return max(found, key=lambda v: tuple(int(x) for x in v.split(".")), default="")


def verify_linux(out, sha):
    """Contrôles du dossier out/ d'un build Linux ; rend son architecture et son plancher glibc."""
    exe = os.path.join(out, "ArmorPaint")
    data = os.path.join(out, "data")
    so = os.path.join(data, "libstudio_ocio.so")
    if not os.path.isfile(exe) or not os.path.isdir(data):
        fail("%s: not an ArmorPaint build (the out/ folder of paint/build)" % out)
    extra = sorted(set(os.listdir(out)) - set(LINUX_TOP))
    if extra:
        fail("not from the build in %s: %s; build again from a clean tree" % (out, ", ".join(extra)))
    check_data(data, sha)
    with open(exe, "rb") as f:
        elf = f.read(20)
    arch = {0x3E: "x86_64", 0xB7: "aarch64"}.get(int.from_bytes(elf[18:20], "little")) if elf[:4] == b"\x7fELF" else None
    if not arch:
        fail("%s: not an x86_64 or aarch64 ELF binary" % exe)
    if not os.path.isfile(so):
        fail("OpenColorIO: %s missing (tools/build_ocio.sh, then build again)" % so)
    ocio = len(re.findall(r" T studio_ocio_lib_", run("nm", "-D", "--defined-only", so).stdout))
    if ocio == 0:
        fail("%s exports no studio_ocio_lib_ symbol" % so)
    missing = [line.strip() for f in (exe, so) for line in run("ldd", f).stdout.splitlines() if "not found" in line]
    if missing:
        fail("libraries not found: %s" % "; ".join(missing))
    glibc = floor([exe, so], "GLIBC")
    print("app: %s, glibc %s+, libstdc++ %s+, OCIO in data/ (%d symbols), built from %s" % (
        arch, glibc, floor([so], "GLIBCXX"), ocio, sha))
    return arch, glibc


def fill(text, values):
    for k, v in values.items():
        text = text.replace("@%s@" % k, v)
    return text


def stage(folder, app, values):
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    if SYSTEM == "macos":
        subprocess.run(["ditto", "--norsrc", app, os.path.join(folder, "ArmorPaint.app")], check=True)
    else:
        shutil.copytree(app, os.path.join(folder, "ArmorPaint"), symlinks=True)
    for name in sorted(os.listdir(TEMPLATE)):
        target = name
        if name.startswith("README."):
            if name != "README.%s.md" % SYSTEM:
                continue
            target = "README.md"
        data = open(os.path.join(TEMPLATE, name), encoding="utf-8").read()
        with open(os.path.join(folder, target), "w", encoding="utf-8") as f:
            f.write(fill(data, values))
        os.chmod(os.path.join(folder, target), 0o755 if name.endswith(".sh") else 0o644)
    with open(os.path.join(folder, "requirements.txt"), "w") as f:
        f.write("".join("%s==%s\n" % kv for kv in pins().items()))
    shutil.copyfile(LICENSE, os.path.join(folder, "LICENSE.md"))
    # Dates fixées : même archive pour le même build.
    for root, dirs, names in os.walk(folder, topdown=False):
        for n in dirs + names:
            os.utime(os.path.join(root, n), (EPOCH, EPOCH), follow_symlinks=False)
    os.utime(folder, (EPOCH, EPOCH))


def tar_gz(folder, archive):
    """Archive reproductible : entrées triées, propriétaire root, dates et droits normalisés, gzip sans date."""
    base = os.path.dirname(folder)
    paths = [folder]
    for root, dirs, names in os.walk(folder):
        dirs.sort()
        paths += [os.path.join(root, n) for n in sorted(dirs + names)]

    def norm(ti):
        ti.uid = ti.gid = 0
        ti.uname = ti.gname = ""
        ti.mtime = EPOCH
        if not ti.issym():
            ti.mode = 0o755 if ti.isdir() or ti.mode & 0o111 else 0o644
        return ti

    with open(archive, "wb") as raw, gzip.GzipFile("", "wb", 9, raw, mtime=0) as gz:
        with tarfile.open(fileobj=gz, mode="w", format=tarfile.PAX_FORMAT) as tar:
            for p in sorted(set(paths)):
                tar.add(p, os.path.relpath(p, base), recursive=False, filter=norm)


def check(archive, top_name, v):
    """L'archive extraite (signature gardée sur macOS) démarre en --background depuis un autre dossier courant."""
    with tempfile.TemporaryDirectory() as tmp:
        if SYSTEM == "macos":
            subprocess.run(["ditto", "-x", "-k", archive, tmp], check=True)
            top = os.path.join(tmp, top_name)
            app = os.path.join(top, "ArmorPaint.app")
            got = run("codesign", "--verify", "--deep", "--strict", app)
            if got.returncode != 0:
                fail("check: extracted app: %s" % got.stderr.strip())
            cmd = [os.path.join(app, "Contents", "MacOS", "ArmorPaint")]
            extra = ["-ApplePersistenceIgnoreState", "YES"]
        else:
            if not os.environ.get("DISPLAY"):
                fail("check: ArmorPaint needs an X display, also in --background (set DISPLAY)")
            subprocess.run(["tar", "-xzf", archive, "-C", tmp], check=True)
            top = os.path.join(tmp, top_name)
            cmd = [os.path.join(top, "ArmorPaint", "ArmorPaint")]
            extra = []
        script = os.path.join(tmp, "smoke.c")
        with open(script, "w") as f:
            f.write('int main() {\n\tconsole_log(string("ocio=%s", studio_ocio_status()));\n\treturn 0;\n}\n')
        env = dict(os.environ, STUDIO_ENV="")
        env.pop("OCIO", None)
        try:
            got = subprocess.run(cmd + ["--background", "--script", script] + extra, env=env, cwd=tempfile.gettempdir(),
                                 capture_output=True, text=True, timeout=90)
            out, rc = got.stdout + got.stderr, got.returncode
        except subprocess.TimeoutExpired as e:
            out, rc = (e.stdout or b"").decode(errors="replace") + (e.stderr or b"").decode(errors="replace"), -1
        loaded = "studio: external %s loaded" % v in out
        ocio = re.search(r"^ocio=(OCIO \S+.*)$", out, re.M)
        if rc != 0 or not loaded or not ocio:
            fail("check: the extracted app did not start (rc %d):\n%s" % (rc, out[-2000:]))
        for name in ("setup_python.sh", "requirements.txt", "studio.env.example", "README.md", "LICENSE.md"):
            if not os.path.isfile(os.path.join(top, name)):
                fail("check: %s missing" % name)
    print("check: extracted app%s starts in --background: external %s, %s" % (
        " signed," if SYSTEM == "macos" else "", v, ocio.group(1)[:60]))


def main():
    ap = argparse.ArgumentParser(description="Build the binary package of ArmorPaint Studio (macOS or Linux)")
    ap.add_argument("--app", default=os.path.join(BUILD, "build", "Release", "ArmorPaint.app") if SYSTEM == "macos"
                    else os.path.join(BUILD, "out"))
    ap.add_argument("--out", default=os.path.join(EXT, "dist", "studio"))
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    if not SYSTEM:
        fail("macOS or Linux only")
    v, sha = version(), head()
    app = os.path.abspath(a.app)
    p = pins()
    values = dict(PLATFORM[SYSTEM], VERSION=v, SHA=sha, PYSIDE=p["PySide6"], OCIO=p["opencolorio"], NUMPY=p["numpy"])
    if SYSTEM == "macos":
        arch = verify_macos(app, sha)
    else:
        arch, values["GLIBC"] = verify_linux(app, sha)
    top = "ArmorPaint-Studio-%s-%s-%s" % (v, SYSTEM, arch)
    os.makedirs(a.out, exist_ok=True)
    folder = os.path.join(a.out, top)
    stage(folder, app, values)
    archive = folder + (".zip" if SYSTEM == "macos" else ".tar.gz")
    if os.path.exists(archive):
        os.remove(archive)
    if SYSTEM == "macos":
        subprocess.run(["ditto", "-c", "-k", "--norsrc", "--noqtn", "--keepParent", folder, archive], check=True)
    else:
        tar_gz(folder, archive)
    # SHA256SUMS : cette archive, plus les lignes des autres archives encore présentes (paquet de l'autre système copié
    # dans le même dossier pour la release).
    sums = os.path.join(a.out, "SHA256SUMS")
    lines = open(sums).read().splitlines() if os.path.exists(sums) else []
    lines = [l for l in lines if l.split("  ", 1)[-1] != os.path.basename(archive) and
             os.path.exists(os.path.join(a.out, l.split("  ", 1)[-1]))]
    lines.append("%s  %s" % (hashlib.sha256(open(archive, "rb").read()).hexdigest(), os.path.basename(archive)))
    with open(sums, "w") as f:
        f.write("".join(l + "\n" for l in sorted(lines, key=lambda l: l.split("  ", 1)[-1])))
    print("%s (%d MB)" % (archive, os.path.getsize(archive) // (1024 * 1024)))
    if a.check:
        check(archive, top, v)


if __name__ == "__main__":
    main()
