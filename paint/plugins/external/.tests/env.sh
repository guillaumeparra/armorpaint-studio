#!/bin/sh
# Banc de test du lot E5, hors de l'app, sous ASan/UBSan : lecture de studio.env et de $STUDIO_ENV (studio_env.c).
# Usage : .tests/env.sh [dossier de travail]
# La lecture au démarrage de l'app se vérifie dans son log : voir README.md, « Variables d'environnement ».

set -e
T=$(cd "$(dirname "$0")" && pwd)
EXT=$(dirname "$T")
W=${1:-$(mktemp -d)}
mkdir -p "$W"
cc -std=c11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer \
	-D_DEFAULT_SOURCE "$T/env_test.c" "$EXT/studio_env.c" "$EXT/studio_fs.c" -o "$W/env_test"
"$W/env_test" "$W"
