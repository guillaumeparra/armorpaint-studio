#!/bin/sh
# Installe les hooks git du dépôt armorpaint qui lancent check_patches.sh après un pull (merge ou rebase).
ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel) || exit 1
HOOKS=$(git -C "$ROOT" rev-parse --git-path hooks)
case "$HOOKS" in /*) ;; *) HOOKS="$ROOT/$HOOKS" ;; esac
for h in post-merge post-rewrite; do
	cat > "$HOOKS/$h" <<'HOOK'
#!/bin/sh
# Installé par paint/plugins/external/tools/install_hooks.sh
[ "$(basename "$0")" = post-rewrite ] && [ "$1" != rebase ] && exit 0
script="$(git rev-parse --show-toplevel)/paint/plugins/external/tools/check_patches.sh"
[ -x "$script" ] || exit 0
echo "── patches studio ArmorPaint ──"
"$script" || echo "⚠️  Des patches studio demandent une action : voir paint/plugins/external/PATCHES.md"
exit 0
HOOK
	chmod +x "$HOOKS/$h"
	echo "hook installé : $HOOKS/$h"
done
