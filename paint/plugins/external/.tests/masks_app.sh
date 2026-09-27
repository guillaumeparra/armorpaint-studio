#!/bin/sh
# Masques empilés (patch mask-multiply) dans ArmorPaint lui-même (--background) : la vue multiplie les masques d'un
# calque, chacun pondéré par son opacité, lerp(1, masque, opacité) (make_mesh.c) ; le flatten, l'export, Apply Mask,
# Merge Down et Merge Group doivent donner la même chose. Upstream fusionnait les masques en MIX : un masque opaque
# remplaçait les autres. Couleurs unies ; k = masque combiné attendu.
#   1. flatten (calque de base caché : texel du flatten = rouge × k) : A noir puis B blanc (l'exemple du bug) ; B noir ;
#      opacités ; un masque caché sans effet ; masques de groupe multipliés avec ceux du calque ;
#   2. Apply Mask de chaque masque tour à tour : flatten inchangé, alpha du calque = k ; Undo rend le masque ;
#   3. Merge Down d'un calque à deux masques (layers_apply_masks) : flatten inchangé ; Undo ;
#   4. Merge Down d'un masque sur un masque d'opacité < 1 : un seul masque, flatten inchangé ;
#   5. Merge Group : deux masques de groupe sur un enfant ; masque d'un enfant et masque de groupe sur deux enfants, le
#      masque de l'enfant ne touche pas l'autre ; flatten inchangé ;
#   6. export (--export-textures png generic du .arm enregistré) : la couleur de base = le flatten.
#
#   sh .tests/masks_app.sh [dossier de travail]
#   APP=<binaire ArmorPaint> pour un autre build (défaut : build Release de paint/). python3 avec Pillow.
set -eu
T=$(cd "$(dirname "$0")" && pwd)
EXT=$(dirname "$T")
APP=${APP:-$EXT/../../build/build/Release/ArmorPaint.app/Contents/MacOS/ArmorPaint}
W=${1:-$(mktemp -d)}
mkdir -p "$W"
W=$(cd "$W" && pwd)
[ -x "$APP" ] || { echo "ArmorPaint not found: $APP" >&2; exit 1; }
python3 -c "import PIL" 2>/dev/null || { echo "python3 with Pillow needed" >&2; exit 1; }
FAIL=0
ok() { echo "ok   $1"; }
ko() { echo "FAIL $1"; FAIL=1; }
check() { if eval "$2"; then ok "$1"; else ko "$1"; fi; }

rm -rf "$W"/*.log "$W"/*.out "$W"/*.arm "$W/out"
cd "$W"

# Lance un script dans ArmorPaint sans interface ; ses lignes « t: » dans $W/$1.out.
script() {
	cat > "$W/$1.c"
	perl -e 'alarm 120; exec @ARGV' "$APP" ${2:-} --background --script "$W/$1.c" -ApplePersistenceIgnoreState YES > "$W/$1.log" 2>&1 || true
	grep '^t: ' "$W/$1.log" | sed 's/^t: //' > "$W/$1.out" || true
}
line() { sed -n "s/^$2 //p" "$W/$1.out"; }
# near "<a b c d>" "<a b c d>" : mêmes valeurs à 0,012 près (masques et calques 8 bits, arrondis de chaque passe)
near() { python3 -c 'import sys
a, b = sys.argv[1].split(), sys.argv[2].split()
sys.exit(0 if len(a) == len(b) == 4 and all(abs(float(x) - float(y)) <= 0.012 for x, y in zip(a, b)) else 1)' "$1" "$2"; }
# Flatten attendu : rouge sur fond transparent (base cachée), ou sur le gris du calque de base, masque combiné k
red() { python3 -c 'import sys; k = float(sys.argv[1]); print("%.3f 0.000 0.000 %.3f" % (k, k))' "$1"; }
G=0.498
over() { python3 -c 'import sys; g, k = float(sys.argv[1]), float(sys.argv[2]); print("%.3f %.3f %.3f 1.000" % (g + (1 - g) * k, g * (1 - k), g * (1 - k)))' "$G" "$1"; }

# Calque 1 : la base, gris ; calque 2 : rouge opaque, courant ; ses masques : A (indice 1) puis B (indice 2), B créé
# après A, au-dessus de lui dans la pile
STACK='
void flat(char *what) {
	console_log(string("t: %s %s", what, studio_udim_pixel(0, 9, -1, 10, 10)));
}
void px(char *what, int i) {
	console_log(string("t: %s %s", what, studio_udim_pixel(i, 0, -1, 10, 10)));
}
void names() {
	console_log(string("t: names %s", studio_layer_masks()));
}
void red_layer() {
	studio_layer_select(0);
	studio_layer_new();
	studio_udim_fill(1, 0, -1, 1.0, 0.0, 0.0, 1.0);
}
// Deux masques gris sur le calque 2 : A = a, opacité oa ; B = b, opacité ob
void two_masks(float a, float oa, float b, float ob) {
	studio_layer_select(1);
	studio_layer_new_mask(1);
	studio_udim_fill(1, 0, -1, a, a, a, 1.0);
	studio_layer_set_opacity(oa);
	studio_layer_new_mask(1);
	studio_udim_fill(2, 0, -1, b, b, b, 1.0);
	studio_layer_set_opacity(ob);
}
'

echo "== 1. flatten"
script flat <<EOF
$STACK
int main() {
	studio_project_res(128);
	studio_layer_select(0);
	studio_layer_set_visible(0);
	red_layer();
	flat("none");
	two_masks(0.0, 1.0, 1.0, 1.0);
	names();
	flat("black_white");
	studio_udim_fill(1, 0, -1, 1.0, 1.0, 1.0, 1.0);
	studio_udim_fill(2, 0, -1, 0.0, 0.0, 0.0, 1.0);
	flat("white_black");
	// A = 0.25 à 80 % : 0.4 ; B = 0.5 à 50 % : 0.75 ; k = 0.3 (upstream : lerp(0.4, 0.5, 0.5) = 0.45)
	studio_udim_fill(1, 0, -1, 0.25, 0.25, 0.25, 1.0);
	studio_udim_fill(2, 0, -1, 0.5, 0.5, 0.5, 1.0);
	studio_layer_select(1);
	studio_layer_set_opacity(0.8);
	studio_layer_select(2);
	studio_layer_set_opacity(0.5);
	flat("opacity");
	// Troisième masque, noir, caché
	studio_layer_new_mask(0);
	studio_layer_set_visible(0);
	names();
	flat("hidden");
	return 0;
}
EOF
check "base cachée : le flatten est le calque rouge" '[ "$(line flat none)" = "$(red 1)" ]'
check "A noir puis B blanc par-dessus : A masque toujours (upstream : B remplaçait A, rouge visible)" '[ "$(line flat names | sed -n 1p)" = "Layer 1=0 Mask 3=0 Mask 4=0 Layer 2=0 filter=0" ] && near "$(line flat black_white)" "$(red 0)"'
check "A blanc, B noir : masqué" 'near "$(line flat white_black)" "$(red 0)"'
check "opacités : lerp(1, A, 0.8) × lerp(1, B, 0.5) = 0.3 (upstream 0.45)" 'near "$(line flat opacity)" "$(red 0.3)"'
check "masque caché : sans effet" '[ "$(line flat names | sed -n 2p)" = "Layer 1=0 Mask 3=0 Mask 4=0 Mask 5=0 Layer 2=0 filter=0" ] && near "$(line flat hidden)" "$(red 0.3)"'

script flatgroup <<EOF
$STACK
int main() {
	studio_project_res(128);
	studio_layer_select(0);
	studio_layer_set_visible(0);
	red_layer();
	two_masks(0.25, 0.8, 0.5, 0.5);
	console_log(string("t: group %d %d", studio_layer_new_group(), studio_layer_set_parent(3, 4)));
	studio_layer_new_mask(1);
	studio_udim_fill(4, 0, -1, 0.5, 0.5, 0.5, 1.0);
	names();
	flat("flat");
	return 0;
}
EOF
check "masque de groupe (0.5) : multiplié avec ceux du calque, k = 0.15 (upstream : il remplaçait, 0.5)" '[ "$(line flatgroup group)" = "1 1" ] && [ "$(line flatgroup names)" = "Layer 1=0 Mask 3=0 Mask 4=0 Layer 2=0 Mask 6=0 Group 5=0 filter=0" ] && near "$(line flatgroup flat)" "$(red 0.15)"'

echo "== 2. Apply Mask de chaque masque"
script apply <<EOF
$STACK
int main() {
	studio_project_res(128);
	red_layer();
	two_masks(0.25, 0.8, 0.5, 0.5);
	flat("before");
	studio_layer_select(2);
	console_log(string("t: applyB %d", studio_layer_op("apply_mask")));
	names();
	flat("afterB");
	studio_layer_select(1);
	console_log(string("t: applyA %d", studio_layer_op("apply_mask")));
	names();
	flat("afterA");
	px("layer", 1);
	studio_layer_op("undo");
	names();
	flat("undo");
	return 0;
}
EOF
check "pile de départ : sur la base grise, k = 0.3" 'near "$(line apply before)" "$(over 0.3)"'
check "Apply Mask de B puis de A : flatten inchangé, masques retirés, alpha du calque 0.3" '[ "$(line apply applyB)" = 1 ] && [ "$(line apply applyA)" = 1 ] && near "$(line apply afterB)" "$(line apply before)" && near "$(line apply afterA)" "$(line apply before)" && [ "$(line apply names | sed -n 2p)" = "Layer 1=0 Layer 2=0 filter=0" ] && near "$(line apply layer)" "1.000 0.000 0.000 0.300"'
# L'undo d'Apply Mask recrée le masque à l'opacité par défaut (layers_new_mask, history.c) : A revient à 1, k = 0.25 × 0.75
check "Undo : le masque A revient (opacité 1, limite upstream), k = 0.1875" '[ "$(line apply names | sed -n 3p)" = "Layer 1=0 Mask 3=0 Layer 2=0 filter=0" ] && near "$(line apply undo)" "$(over 0.1875)"'

echo "== 3. Merge Down d'un calque à deux masques"
script merge <<EOF
$STACK
int main() {
	studio_project_res(128);
	red_layer();
	two_masks(0.25, 0.8, 0.5, 0.5);
	flat("before");
	studio_layer_select(3);
	console_log(string("t: merge %d", studio_layer_op("merge_down")));
	names();
	flat("after");
	px("merged", 0);
	studio_layer_op("undo");
	names();
	return 0;
}
EOF
check "Merge Down : un calque, masques appliqués multipliés, flatten inchangé (upstream : k = 0.65)" '[ "$(line merge merge)" = 1 ] && [ "$(line merge names | sed -n 1p)" = "Layer 1=0 filter=0" ] && near "$(line merge after)" "$(line merge before)" && near "$(line merge merged)" "$(over 0.3)"'
check "Merge Down, Undo : deux calques" '[ "$(line merge names | sed -n 2p | sed "s/Mask [0-9]*=0 //g")" = "Layer 1=0 Layer 2=0 filter=0" ]'

echo "== 4. Merge Down d'un masque sur un masque"
script maskmerge <<EOF
$STACK
int main() {
	studio_project_res(128);
	red_layer();
	two_masks(0.25, 0.8, 0.5, 0.5);
	flat("before");
	studio_layer_select(2);
	console_log(string("t: merge %d", studio_layer_op("merge_down")));
	names();
	flat("after");
	px("mask", 1);
	return 0;
}
EOF
check "masque B fusionné sur A (opacité 0.8) : un masque, 0.4 × 0.75, opacité 1 ; flatten inchangé (upstream : masque 0.375, k = 0.5)" '[ "$(line maskmerge merge)" = 1 ] && [ "$(line maskmerge names)" = "Layer 1=0 Mask 3=0 Layer 2=0 filter=0" ] && near "$(line maskmerge mask)" "0.300 0.300 0.300 1.000" && near "$(line maskmerge after)" "$(line maskmerge before)"'

echo "== 5. Merge Group"
# a. Un enfant (L2 rouge) sous deux masques de groupe, 0.5 et 0.4 à 50 % : k = 0.35
script group <<EOF
$STACK
int main() {
	studio_project_res(128);
	red_layer();
	console_log(string("t: group %d %d", studio_layer_new_group(), studio_layer_set_parent(1, 2)));
	studio_layer_new_mask(1);
	studio_udim_fill(2, 0, -1, 0.5, 0.5, 0.5, 1.0);
	studio_layer_new_mask(1);
	studio_udim_fill(3, 0, -1, 0.4, 0.4, 0.4, 1.0);
	studio_layer_set_opacity(0.5);
	names();
	flat("before");
	studio_layer_select(4);
	console_log(string("t: mergeg %d", studio_layer_op("merge_group")));
	names();
	flat("after");
	px("merged", 1);
	return 0;
}
EOF
check "groupe à deux masques de groupe : flatten multiplié, k = 0.35 (upstream 0.45)" '[ "$(line group group)" = "1 1" ] && [ "$(line group names | sed -n 1p)" = "Layer 1=0 Layer 2=0 Mask 4=0 Mask 5=0 Group 3=0 filter=0" ] && near "$(line group before)" "$(over 0.35)"'
check "Merge Group : un calque d alpha 0.35, flatten inchangé (upstream : 0.75)" '[ "$(line group mergeg)" = 1 ] && [ "$(line group names | sed -n 2p)" = "Layer 1=0 Group 3=0 filter=0" ] && near "$(line group after)" "$(line group before)" && near "$(line group merged)" "1.000 0.000 0.000 0.350"'
# b. Deux enfants opaques, L2 rouge et L3 bleu au-dessus ; masque 0.5 à L3 seul ; masque de groupe blanc
script group2 <<EOF
$STACK
int main() {
	studio_project_res(128);
	red_layer();
	studio_layer_new();
	studio_udim_fill(2, 0, -1, 0.0, 0.0, 1.0, 1.0);
	console_log(string("t: group %d %d %d", studio_layer_new_group(), studio_layer_set_parent(1, 3), studio_layer_set_parent(2, 3)));
	studio_layer_new_mask(1);
	studio_layer_select(2);
	studio_layer_new_mask(1);
	studio_udim_fill(2, 0, -1, 0.5, 0.5, 0.5, 1.0);
	names();
	flat("before");
	studio_layer_select(5);
	console_log(string("t: mergeg %d", studio_layer_op("merge_group")));
	names();
	flat("after");
	return 0;
}
EOF
check "masque d un enfant et masque de groupe : flatten = moitié rouge, moitié bleu" '[ "$(line group2 group)" = "1 1 1" ] && [ "$(line group2 names | sed -n 1p)" = "Layer 1=0 Layer 2=0 Mask 6=0 Layer 3=0 Mask 5=0 Group 4=0 filter=0" ] && near "$(line group2 before)" "0.500 0.000 0.500 1.000"'
check "Merge Group : le masque de L3 ne touche pas L2, flatten inchangé (upstream : L2 à moitié masqué aussi)" '[ "$(line group2 mergeg)" = 1 ] && [ "$(line group2 names | sed -n 2p)" = "Layer 1=0 Group 4=0 filter=0" ] && near "$(line group2 after)" "$(line group2 before)"'

echo "== 6. export"
mkdir -p "$W/out"
# project_save enregistre à l'image suivante : un script par projet
script savebw <<EOF
$STACK
int main() {
	studio_project_res(128);
	red_layer();
	two_masks(0.0, 1.0, 1.0, 1.0);
	flat("flat");
	project_filepath_set("$W/bw.arm");
	project_save(false);
	return 0;
}
EOF
script saveop <<EOF
$STACK
int main() {
	studio_project_res(128);
	red_layer();
	two_masks(0.25, 0.8, 0.5, 0.5);
	flat("flat");
	project_filepath_set("$W/op.arm");
	project_save(false);
	return 0;
}
EOF
for p in bw op; do
	perl -e 'alarm 120; exec @ARGV' "$APP" "$W/$p.arm" --background --export-textures png generic "$W/out" -ApplePersistenceIgnoreState YES > "$W/export_$p.log" 2>&1 || true
done
png() { python3 -c 'import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
print("%.3f %.3f %.3f 1.000" % tuple(c / 255 for c in im.getpixel((10, 10))))' "$1" 2>/dev/null || echo "-"; }
check "A noir, B blanc : couleur de base exportée = flatten, le gris de la base (upstream : rouge)" 'near "$(png "$W/out/bw_base.png")" "$(line savebw flat)" && near "$(line savebw flat)" "$(over 0)"'
check "opacités : couleur de base exportée = flatten, k = 0.3" 'near "$(png "$W/out/op_base.png")" "$(line saveop flat)" && near "$(line saveop flat)" "$(over 0.3)"'

[ $FAIL -eq 0 ] && echo "masks_app : tout est bon" || echo "masks_app : ÉCHECS"
exit $FAIL
