# Chemins des fichiers d'un projet .arm tels qu'enregistrés (chaînes armpack : 0xdb, longueur u32, octets) : maillage
# source (.obj), textures et envmap (.png, .hdr).
#   python3 arm_paths.py <projet.arm>                les liste
#   python3 arm_paths.py <projet.arm> <fichier>...   vérifie qu'ils désignent exactement ces fichiers, chacun par un
#       chemin relatif au dossier du .arm et normalisé (sans dossier/.. : un tel chemin ne se résout plus quand le
#       dossier manque) ; code 1 et les écarts sinon.
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
    expected = {os.path.abspath(f) for f in argv[1:]}
    errors, found = [], set()
    for p in paths:
        q = p.replace("\\", "/")
        if os.path.isabs(q):
            errors.append("absolute: %s" % p)
            continue
        if os.path.normpath(q) != q:
            errors.append("not normalized: %s" % p)
        found.add(os.path.normpath(os.path.join(os.path.dirname(arm), q)))
    if found != expected:
        errors.append("files %s, expected %s" % (sorted(found), sorted(expected)))
    print("\n".join(errors or paths))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
