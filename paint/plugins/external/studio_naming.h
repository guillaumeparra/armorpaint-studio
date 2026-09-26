#pragma once

// Lot B3 : noms des textures exportées d'après un modèle de chemin (op texture_pattern du sidecar ShotGrid, rempli par
// un template Toolkit). Sans dépendance à ArmorPaint (libc, POSIX) : testable hors de l'app (.tests/naming_test.c).
// La colle avec l'app (sidecar, export) est dans studio_export_naming.c.

#include <stdbool.h>
#include <stddef.h>

// Remplace dans pattern les marqueurs <channel>, <UDIM> et <ext>. Dans les valeurs, les séparateurs de chemin ('/',
// '\\') et ':' deviennent '_' : un nom d'objet ou de canal ne crée pas de dossier. Retourne le chemin (malloc), NULL si
// la mémoire manque.
char *studio_naming_expand(const char *pattern, const char *channel, const char *udim, const char *ext);

// Crée les dossiers parents de path (mkdir -p, sans shell). false si le dossier parent n'existe pas au retour.
bool studio_naming_make_parent(const char *path);

// Noms des presets studio (lot B4). Chaque marqueur <nom> dont le nom figure dans names (sans égard à la casse :
// <UDIM> ou <udim>) est remplacé par la valeur correspondante (mêmes substitutions de séparateurs que
// studio_naming_expand) ; un marqueur inconnu reste tel quel.
// Une section [ ... ] disparaît, crochets compris, dès qu'un marqueur qu'elle contient vaut "" ; sinon seuls les
// crochets disparaissent. Pas d'imbrication : un '[' sans ']' qui le suit est littéral. Ex. :
// "<file>[_<variant>]_<channel>[.<UDIM>].<ext>" donne "robot_BaseColor.exr" sans variante ni tuile. Retourne le
// chemin (malloc), NULL si la mémoire manque.
char *studio_naming_format(const char *pattern, const char *const *names, const char *const *values, int count);

// Variables des noms d'export studio, dans l'ordre où la fenêtre d'export les propose. name : le marqueur sans <>.
typedef struct {
	const char *name;
	const char *description;
} studio_naming_variable_t;
extern const studio_naming_variable_t studio_naming_variables[];
extern const int                      studio_naming_variable_count;

// Vérifie un modèle de nom de preset studio : non vide, et chaque <marqueur> est une variable de
// studio_naming_variables. Sinon false et err ("unknown variable <shader> (file, name, ...)").
bool studio_naming_check(const char *pattern, char *err, size_t err_size);

// "robot_v003" -> name "robot", version "003" ; séparateur '_', '.' ou '-', v ou V, en fin de nom. Sans version :
// name = file, version "". Deux chaînes malloc ; false si la mémoire manque.
bool studio_naming_split_version(const char *file, char **name, char **version);
