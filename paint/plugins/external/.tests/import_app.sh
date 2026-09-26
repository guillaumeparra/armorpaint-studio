#!/bin/sh
# Imports en ligne de commande dans ArmorPaint lui-même (--background), patches args-cwd-paths et import-null-guards :
# chemins relatifs résolus contre le dossier courant, fichiers absents / illisibles / invalides refusés sans planter,
# fichiers valides importés comme avant.
#
#   sh .tests/import_app.sh [dossier de travail]
#   APP=<binaire ArmorPaint> pour un autre build (défaut : build Release de paint/).
# Demande : python3 ; sips (macOS) pour les cas tiff et psd, sautés sinon.
set -eu
EXT=$(cd "$(dirname "$0")/.." && pwd)
APP=${APP:-$EXT/../../build/build/Release/ArmorPaint.app/Contents/MacOS/ArmorPaint}
W=${1:-$(mktemp -d)}
mkdir -p "$W/sub"
W=$(cd "$W" && pwd)
[ -x "$APP" ] || { echo "ArmorPaint not found: $APP" >&2; exit 1; }
FAIL=0

# run <attendu> <args...> : lance ArmorPaint depuis le dossier courant, exige rc 0 et <attendu> dans la sortie
run() {
	want=$1
	shift
	# -ApplePersistenceIgnoreState : sans lui, un plantage précédent laisse une fenêtre modale qui bloque le lancement
	out=$(perl -e 'alarm 60; exec @ARGV' "$APP" "$@" --background -ApplePersistenceIgnoreState YES 2>&1) && rc=0 || rc=$?
	if [ $rc -eq 0 ] && printf '%s' "$out" | grep -qF -- "$want"; then
		echo "ok   $* -> $want" | sed "s#$W#<w>#g"
	else
		echo "FAIL $* (rc $rc, attendu : $want)" | sed "s#$W#<w>#g"
		printf '%s\n' "$out" | grep -v '^studio: \|ApplePersistence\|^$' | tail -3 | sed 's/^/     /'
		FAIL=1
	fi
}

cd "$W"
cp "$EXT/.tests/obj_names.c" names.c
python3 - <<'EOF'
import base64, json, struct, zlib
open('grid.obj', 'w').write('o grid\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\nf 1/1/1 2/2/1 3/3/1 4/4/1\n')
# glTF / glb : un triangle
b = struct.pack('9f', 0, 0, 0, 1, 0, 0, 0, 1, 0) + struct.pack('9f', *[0, 0, 1] * 3) + struct.pack('6f', 0, 0, 1, 0, 0, 1) + struct.pack('4H', 0, 1, 2, 0)
g = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": "tri"}],
     "meshes": [{"name": "tri", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3}]}],
     "buffers": [{"byteLength": len(b), "uri": "data:application/octet-stream;base64," + base64.b64encode(b).decode()}],
     "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}, {"buffer": 0, "byteOffset": 36, "byteLength": 36},
                     {"buffer": 0, "byteOffset": 72, "byteLength": 24}, {"buffer": 0, "byteOffset": 96, "byteLength": 6}],
     "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
                   {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
                   {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC2"},
                   {"bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR"}]}
open('tri.gltf', 'w').write(json.dumps(g))
g["buffers"] = [{"byteLength": len(b)}]
js = json.dumps(g).encode()
js += b' ' * (-len(js) % 4)
bb = b + b'\0' * (-len(b) % 4)
open('tri.glb', 'wb').write(struct.pack('<III', 0x46546C67, 2, 28 + len(js) + len(bb)) + struct.pack('<II', len(js), 0x4E4F534A) + js +
                            struct.pack('<II', len(bb), 0x004E4942) + bb)
# FBX ASCII : un quad
open('quad.fbx', 'w').write('; FBX 7.4.0 project file\nFBXHeaderExtension:  {\n\tFBXHeaderVersion: 1003\n\tFBXVersion: 7400\n}\n'
    'Objects:  {\n\tGeometry: 1000, "Geometry::quad", "Mesh" {\n\t\tVertices: *12 {\n\t\t\ta: 0,0,0,1,0,0,1,1,0,0,1,0\n\t\t}\n'
    '\t\tPolygonVertexIndex: *4 {\n\t\t\ta: 0,1,2,-4\n\t\t}\n\t\tGeometryVersion: 124\n\t}\n'
    '\tModel: 2000, "Model::quad", "Mesh" {\n\t\tVersion: 232\n\t}\n}\nConnections:  {\n\tC: "OO",2000,0\n\tC: "OO",1000,2000\n}\n')
# PNG 4x4 rouge, SVG
raw = b''.join(b'\0' + bytes([255, 0, 0, 255]) * 4 for _ in range(4))
ch = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
open('red.png', 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', 4, 4, 8, 6, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw)) + ch(b'IEND', b''))
open('red.svg', 'w').write('<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4"><rect width="4" height="4" fill="red"/></svg>')
for ext in ('gltf', 'glb', 'fbx'):
    open('bad.' + ext, 'w').write('garbage')
EOF
cp grid.obj noread.obj
chmod 000 noread.obj
TEX="red.png red.svg"
if command -v sips >/dev/null; then
	sips -s format tiff red.png --out red.tiff >/dev/null && sips -s format psd red.png --out red.psd >/dev/null
	TEX="$TEX red.tiff red.psd"
fi

echo "== args-cwd-paths : chemins relatifs"
run "root=grid idx=6" grid.obj --script names.c
run "root=grid idx=6" ./grid.obj --script ./names.c
cd sub
run "root=grid idx=6" ../grid.obj --script ../names.c
cd ..

echo "== import-null-guards : fichier absent ou illisible"
for e in obj fbx gltf glb psd tiff svg; do
	run "Could not open file $W/missing/x.$e." "$W/missing/x.$e"
done
run "Could not open file $W/noread.obj." "$W/noread.obj"

echo "== import-null-guards : fichier invalide"
for e in gltf glb fbx; do
	run "Could not import file $W/bad.$e." "$W/bad.$e"
done

echo "== non-régression : fichiers valides"
run "root=grid idx=6" "$W/grid.obj" --script "$W/names.c"
run "root=tri idx=3" "$W/tri.gltf" --script "$W/names.c"
run "root=tri idx=3" "$W/tri.glb" --script "$W/names.c"
run "root=quad idx=6" "$W/quad.fbx" --script "$W/names.c"
for f in $TEX; do
	run "Texture imported: $f" "$W/$f"
done

chmod 644 noread.obj
[ $FAIL -eq 0 ] && echo "import_app: tout passe" || echo "import_app: ÉCHECS"
exit $FAIL
