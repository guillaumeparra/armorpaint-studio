#!/usr/bin/env python3
# Paquet autonome de subzero (lot E9), à côté d'ArmorPaint Studio : dossier subzero-<version>/ (lanceur bin/subzero,
# modules Python, setup.sh, package.py de rez, pyproject.toml, README), archive .tar.gz, roue pip py3-none-any et
# SHA256SUMS. Bibliothèque standard seule ; archives reproductibles (dates fixées, ordre trié). Aucun fichier d'Adobe :
# sbsrender reste celui de Designer, installé sur la machine.
#
#   python3 tools/release_subzero.py [--out dist] [--check]
#   --check : la roue installée dans un dossier temporaire répond à --version, et chaque module se compile.
# Version : subzero_app.__version__. Versions épinglées de requirements.txt : celles de tools/setup_subzero.sh
# (PySide6, numpy) et d'OCIO dans .ocio/CMakeLists.txt, comme la fenêtre dans ArmorPaint.

import argparse
import base64
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile

EXT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(EXT, "assets")
TEMPLATE = os.path.join(EXT, "release", "subzero")
LICENSE = os.path.join(EXT, "..", "..", "..", "license.md")
MODULES = ["subzero_app.py", "subzero_window.py", "subzero_viewer.py", "subzero_browser.py", "subzero_material.py",
           "subzero_render.py", "subzero_color.py", "subzero_exr.py", "subzero_meta.py", "subzero_7z.py",
           "studio_qt.py", "studio_env.py"]
EPOCH = 1735689600  # 2025-01-01 : dates des archives, pour des sommes identiques d'un build à l'autre
SUMMARY = "subzero: .sbs/.sbsar materials rendered by sbsrender, exported to a folder (window and command line)"


def version():
    text = open(os.path.join(ASSETS, "subzero_app.py"), encoding="utf-8").read()
    return re.search(r'^__version__ = "([^"]+)"', text, re.M).group(1)


def pins():
    setup = open(os.path.join(EXT, "tools", "setup_subzero.sh"), encoding="utf-8").read()
    cmake = open(os.path.join(EXT, ".ocio", "CMakeLists.txt"), encoding="utf-8").read()
    return {"PySide6": re.search(r"^PYSIDE=(\S+)", setup, re.M).group(1),
            "opencolorio": re.search(r'set\(STUDIO_OCIO_TAG "v([0-9.]+)"', cmake).group(1),
            "numpy": re.search(r"^NUMPY=(\S+)", setup, re.M).group(1)}


def fill(text, v):
    return text.replace("@VERSION@", v)


def files(v):
    """{chemin dans le paquet: (contenu, exécutable)}."""
    out = {}
    for m in MODULES:
        out["python/subzero/" + m] = (open(os.path.join(ASSETS, m), "rb").read(), False)
    for root, dirs, names in os.walk(TEMPLATE):
        dirs[:] = sorted(d for d in dirs if d != "__pycache__")
        for n in sorted(names):
            src = os.path.join(root, n)
            rel = os.path.relpath(src, TEMPLATE).replace(os.sep, "/")
            data = open(src, "rb").read()
            if rel.endswith((".md", ".py", ".toml", ".desktop")):
                data = fill(data.decode("utf-8"), v).encode("utf-8")
            out[rel] = (data, os.access(src, os.X_OK) or rel in ("bin/subzero", "setup.sh"))
    out["LICENSE.md"] = (open(LICENSE, "rb").read(), False)
    out["VERSION"] = (("%s\n" % v).encode(), False)
    out["requirements.txt"] = ("".join("%s==%s\n" % kv for kv in pins().items()).encode(), False)
    return out


def write_tree(folder, content):
    shutil.rmtree(folder, ignore_errors=True)
    for rel, (data, exe) in sorted(content.items()):
        path = os.path.join(folder, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        os.chmod(path, 0o755 if exe else 0o644)


def write_tarball(path, top, content):
    with open(path, "wb") as raw:
        # gzip sans nom ni date dans son en-tête
        import gzip
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as gz:
            with tarfile.open(fileobj=gz, mode="w", format=tarfile.PAX_FORMAT) as tar:
                dirs = sorted({"/".join(rel.split("/")[:i]) for rel in content for i in range(1, rel.count("/") + 1)})
                for d in [""] + dirs:
                    info = tarfile.TarInfo((top + "/" + d).rstrip("/"))
                    info.type, info.mode, info.mtime = tarfile.DIRTYPE, 0o755, EPOCH
                    tar.addfile(info)
                for rel, (data, exe) in sorted(content.items()):
                    info = tarfile.TarInfo(top + "/" + rel)
                    info.size, info.mode, info.mtime = len(data), 0o755 if exe else 0o644, EPOCH
                    tar.addfile(info, io.BytesIO(data))


def _record_hash(data):
    return "sha256=" + base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()


def write_wheel(folder, v, content):
    """Roue pure Python : le paquet subzero (python/subzero/), le lanceur `subzero` en console_scripts, les extras
    window, color et all de pyproject.toml."""
    dist = "subzero-%s.dist-info" % v
    readme = content["README.md"][0].decode("utf-8")
    metadata = "\n".join([
        "Metadata-Version: 2.1", "Name: subzero", "Version: %s" % v, "Summary: %s" % SUMMARY,
        "License: zlib", "Requires-Python: >=3.9",
        "Provides-Extra: window", 'Requires-Dist: PySide6>=6.5; extra == "window"',
        "Provides-Extra: color", 'Requires-Dist: opencolorio>=2.2; extra == "color"',
        'Requires-Dist: numpy>=1.20; extra == "color"',
        "Provides-Extra: all", 'Requires-Dist: PySide6>=6.5; extra == "all"',
        'Requires-Dist: opencolorio>=2.2; extra == "all"', 'Requires-Dist: numpy>=1.20; extra == "all"',
        "Description-Content-Type: text/markdown", "", readme])
    members = [(rel[len("python/"):], data) for rel, (data, _) in sorted(content.items()) if rel.startswith("python/")]
    members += [
        (dist + "/METADATA", metadata.encode("utf-8")),
        (dist + "/WHEEL", b"Wheel-Version: 1.0\nGenerator: release_subzero.py\nRoot-Is-Purelib: true\nTag: py3-none-any\n"),
        (dist + "/entry_points.txt", b"[console_scripts]\nsubzero = subzero:main\n"),
        (dist + "/top_level.txt", b"subzero\n"),
        (dist + "/LICENSE.md", content["LICENSE.md"][0]),
    ]
    record = "".join("%s,%s,%d\n" % (name, _record_hash(data), len(data)) for name, data in members)
    record += dist + "/RECORD,,\n"
    members.append((dist + "/RECORD", record.encode("utf-8")))
    path = os.path.join(folder, "subzero-%s-py3-none-any.whl" % v)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in members:
            info = zipfile.ZipInfo(name, date_time=(2025, 1, 1, 0, 0, 0))
            info.external_attr = 0o644 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data)
    return path


def check(out, v, wheel):
    """La roue, dépaquetée dans un dossier temporaire, répond à --version ; chaque module se compile."""
    with tempfile.TemporaryDirectory() as tmp:
        with zipfile.ZipFile(wheel) as z:
            z.extractall(tmp)
        env = dict(os.environ, PYTHONPATH=tmp, STUDIO_ENV="")
        got = subprocess.run([sys.executable, "-B", "-m", "subzero", "--version"], env=env, capture_output=True,
                             text=True)
        if got.returncode != 0 or got.stdout.strip() != "subzero %s" % v:
            raise SystemExit("check failed: %s%s" % (got.stdout, got.stderr))
        for m in MODULES:
            source = open(os.path.join(tmp, "subzero", m), encoding="utf-8").read()
            compile(source, m, "exec")
    print("check: python -m subzero --version -> subzero %s, %d modules compile" % (v, len(MODULES)))


def main():
    ap = argparse.ArgumentParser(description="Build the standalone subzero package")
    ap.add_argument("--out", default=os.path.join(EXT, "dist"))
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    v = version()
    content = files(v)
    os.makedirs(a.out, exist_ok=True)
    top = "subzero-%s" % v
    write_tree(os.path.join(a.out, top), content)
    tarball = os.path.join(a.out, top + ".tar.gz")
    write_tarball(tarball, top, content)
    wheel = write_wheel(a.out, v, content)
    sums = []
    for path in (tarball, wheel):
        sums.append("%s  %s\n" % (hashlib.sha256(open(path, "rb").read()).hexdigest(), os.path.basename(path)))
    with open(os.path.join(a.out, "SHA256SUMS"), "w") as f:
        f.writelines(sums)
    for path in (tarball, wheel):
        print("%s (%d KB)" % (path, os.path.getsize(path) // 1024))
    if a.check:
        check(a.out, v, wheel)


if __name__ == "__main__":
    main()
