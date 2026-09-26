#pragma once

// Lot B3 : noms des textures exportées d'après un modèle de chemin (op texture_pattern du sidecar ShotGrid, rempli par
// un template Toolkit). Sans dépendance à ArmorPaint (libc, POSIX) : testable hors de l'app (.tests/naming_test.c).
// La colle avec l'app (sidecar, export) est dans studio_export_naming.c.

#include <stdbool.h>

// Remplace dans pattern les marqueurs <channel>, <UDIM> et <ext>. Dans les valeurs, les séparateurs de chemin ('/',
// '\\') et ':' deviennent '_' : un nom d'objet ou de canal ne crée pas de dossier. Retourne le chemin (malloc), NULL si
// la mémoire manque.
char *studio_naming_expand(const char *pattern, const char *channel, const char *udim, const char *ext);

// Crée les dossiers parents de path (mkdir -p, sans shell). false si le dossier parent n'existe pas au retour.
bool studio_naming_make_parent(const char *path);
