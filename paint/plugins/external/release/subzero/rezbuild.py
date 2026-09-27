"""rez build of subzero: the package is already built, the install copies it (bin, python, share, docs)."""

import os
import shutil
import sys

SRC = os.path.dirname(os.path.abspath(__file__))
ITEMS = ["bin", "python", "share", "README.md", "LICENSE.md", "VERSION", "requirements.txt", "setup.sh"]


def build(install):
    target = os.environ["REZ_BUILD_INSTALL_PATH"] if install else os.environ["REZ_BUILD_PATH"]
    os.makedirs(target, exist_ok=True)
    for item in ITEMS:
        src, dst = os.path.join(SRC, item), os.path.join(target, item)
        if os.path.isdir(dst):
            shutil.rmtree(dst)
        if os.path.isdir(src):
            shutil.copytree(src, dst, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        elif os.path.isfile(src):
            shutil.copy2(src, dst)


if __name__ == "__main__":
    build("install" in sys.argv[1:])
