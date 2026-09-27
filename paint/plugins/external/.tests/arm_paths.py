# Chemins des fichiers d'un projet .arm tels qu'enregistrés (chaînes armpack : 0xdb, longueur u32, octets) : maillage
# source (.obj), textures et envmap (.png, .hdr).
#   python3 arm_paths.py <projet.arm>                                  les liste
#   python3 arm_paths.py <projet.arm> <dossier> [<dossier interdit>...]  vérifie : un maillage, une texture et une
#       envmap, chacun en chemin relatif qui mène du dossier du .arm à <dossier> sans passer par un dossier interdit
#       (nom d'un élément du chemin) ; code 1 et les écarts sinon.
import os
import struct
import sys


def arm_paths(path):
    with open(path, "rb") as f:
        data = f.read()
    paths, i = [], data.find(b"\xdb")
    while i >= 0:
        n = struct.unpack("<I", data[i + 1 : i + 5])[0] if i + 5 <= len(data) else 0
        s = data[i + 5 : i + 5 + n]
        if 0 < n < 4096 and len(s) == n and all(32 <= b < 127 for b in s) and s.endswith((b".obj", b".png", b".hdr")):
            paths.append(s.decode())
        i = data.find(b"\xdb", i + 1)
    return paths


def main(argv):
    arm = os.path.abspath(argv[0])
    paths = arm_paths(arm)
    if len(argv) == 1:
        print("\n".join(paths))
        return 0
    expected, forbidden = os.path.abspath(argv[1]), argv[2:]
    errors = []
    if {os.path.splitext(p)[1] for p in paths} != {".obj", ".png", ".hdr"}:
        errors.append("mesh, texture and envmap expected: %s" % paths)
    for p in paths:
        parts = p.replace("\\", "/").split("/")
        if os.path.isabs(p):
            errors.append("absolute: %s" % p)
        elif os.path.normpath(os.path.join(os.path.dirname(arm), os.path.dirname(p))) != expected:
            errors.append("not in %s: %s" % (expected, p))
        if any(name in parts for name in forbidden):
            errors.append("through %s: %s" % ("/".join(forbidden), p))
    print("\n".join(errors or paths))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
