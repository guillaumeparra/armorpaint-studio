#!/bin/sh
# Banc de test du lot C1, hors de l'app, sous ASan/UBSan : exécution de processus (studio_proc.c).
# Usage : .tests/proc.sh [dossier de travail]
# La couche jobs et l'API de scripting (studio_exec.c) se testent dans l'app : voir README.md, « Exécution ».

set -e
T=$(cd "$(dirname "$0")" && pwd)
EXT=$(dirname "$T")
W=${1:-$(mktemp -d)}
mkdir -p "$W"
cc -std=c11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer \
	"$T/proc_test.c" "$EXT/studio_proc.c" -o "$W/proc_test"
"$W/proc_test"
