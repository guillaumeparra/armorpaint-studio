#!/bin/sh
# Vérifie les patches studio appliqués aux fichiers upstream d'ArmorPaint.
# À lancer après chaque pull de l'upstream (lancé automatiquement par les hooks git, voir install_hooks.sh).
#
# Pour chaque patches/<id>.patch :
#   - présent / perdu (réapplicable) / en conflit ;
#   - liste des commits upstream qui ont touché les fichiers patchés depuis la dernière validation
#     (patches/<id>.base) : l'upstream a peut-être corrigé le problème autrement → à réanalyser.
#
# Options : --ack <id>  marque le patch comme revalidé contre l'upstream courant (met à jour <id>.base).
# Code de retour : 0 si tout est à jour, 1 si au moins un patch demande une action.

EXT_DIR=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(git -C "$EXT_DIR/../../.." rev-parse --show-toplevel) || exit 2
UPSTREAM=${STUDIO_UPSTREAM:-upstream} # branche miroir d'armory3d/main
cd "$ROOT" || exit 2

if [ "$1" = "--ack" ]; then
	[ -f "$EXT_DIR/patches/$2.patch" ] || { echo "patch inconnu : $2"; exit 2; }
	git rev-parse "$UPSTREAM" > "$EXT_DIR/patches/$2.base"
	echo "$2 revalidé contre $UPSTREAM ($(git rev-parse --short "$UPSTREAM"))"
	exit 0
fi

status=0
for p in "$EXT_DIR"/patches/*.patch; do
	[ -f "$p" ] || continue
	id=$(basename "$p" .patch)
	files=$(sed -n 's|^+++ b/||p' "$p")

	if git apply --check --reverse "$p" 2>/dev/null; then
		state="✅ présent"
	elif git apply --check "$p" 2>/dev/null; then
		state="⚠️  PERDU — réapplicable : git apply $p"
		status=1
	else
		state="❌ CONFLIT — le code a changé, réanalyser (voir PATCHES.md#$id)"
		status=1
	fi
	echo "[$id] $state"

	base=$(cat "$EXT_DIR/patches/$id.base" 2>/dev/null)
	if [ -n "$base" ] && git rev-parse -q --verify "$UPSTREAM" >/dev/null; then
		touched=$(git log --oneline "$base..$UPSTREAM" -- $files)
		if [ -n "$touched" ]; then
			echo "  🔎 l'upstream a modifié $files depuis la dernière validation — vérifier si le bug est corrigé en amont,"
			echo "     puis : $0 --ack $id   (ou supprimer le patch s'il est devenu inutile)"
			echo "$touched" | sed 's/^/     /'
			status=1
		fi
	fi
done

# Marqueurs STUDIO-PATCH(<id>) dans l'arbre upstream sans .patch correspondant
for id in $(git grep -h -o 'STUDIO-PATCH([a-z0-9-]*)' -- ':!paint/plugins/external' 2>/dev/null | sed 's/STUDIO-PATCH(\(.*\))/\1/' | sort -u); do
	[ -f "$EXT_DIR/patches/$id.patch" ] || { echo "[$id] ❓ marqueur présent dans le code mais aucun patches/$id.patch"; status=1; }
done

exit $status
