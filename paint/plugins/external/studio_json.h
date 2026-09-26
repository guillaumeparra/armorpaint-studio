#pragma once

// Lot C2 : lecture de JSON à la demande, sans arbre ni allocation, pour les réponses du sidecar ShotGrid (et les
// scripts). Sans dépendance (libc) : testable hors de l'app (.tests/json_test.c).
//
// Une valeur est désignée par un JSON Pointer (RFC 6901) : "" est la racine, "/data/0/code" le champ "code" du premier
// élément de "data". Les clés ShotGrid contiennent des points ("entity.Asset.code"), d'où le "/" ; "~1" désigne un
// "/" dans une clé, "~0" un "~". Dans un objet, la première clé qui correspond l'emporte.
//
// Le texte est parcouru, pas validé en entier : la valeur désignée et tout ce qu'il faut traverser pour l'atteindre
// doivent être bien formés, le reste n'est pas lu. Aucune lecture au-delà du NUL final, profondeur limitée à 512.

#include <stdbool.h>
#include <stddef.h>

typedef enum {
	STUDIO_JSON_INVALID = 0,
	STUDIO_JSON_NULL,
	STUDIO_JSON_BOOL,
	STUDIO_JSON_NUMBER,
	STUDIO_JSON_STRING,
	STUDIO_JSON_ARRAY,
	STUDIO_JSON_OBJECT,
} studio_json_type_t;

typedef struct studio_json_value {
	const char        *start; // premier caractère de la valeur dans le texte source, NULL si introuvable
	size_t             len;   // longueur de la valeur dans le texte source
	studio_json_type_t type;
} studio_json_value_t;

// Valeur à `pointer` dans le document `json` (terminé par NUL). type INVALID si le chemin n'existe pas ou si le texte
// traversé est mal formé ; pointer NULL équivaut à "".
studio_json_value_t studio_json_find(const char *json, const char *pointer);

// Fin de la valeur qui commence à `p` (espaces en tête sautés), NULL si elle est mal formée. *type (optionnel).
const char *studio_json_skip(const char *p, studio_json_type_t *type);

// Texte d'une valeur, malloc : une chaîne est décodée en UTF-8 (échappements, \uXXXX, paires de substitution ; une
// moitié de paire isolée devient U+FFFD) ; nombre, true, false et null sont rendus tels quels ; un objet ou un tableau
// est rendu en JSON. NULL si la valeur est INVALID ou si la mémoire manque.
char *studio_json_text(studio_json_value_t v);

// Nombre d'éléments d'un tableau ou de membres d'un objet, -1 pour les autres valeurs ou si c'est mal formé.
int studio_json_length(studio_json_value_t v);

// Entier d'un nombre (partie fractionnaire tronquée), `fallback` si ce n'est pas un nombre ou qu'il déborde.
long long studio_json_int(studio_json_value_t v, long long fallback);

// true pour true, un nombre non nul, une chaîne non vide, un tableau ou un objet ; false sinon.
bool studio_json_truthy(studio_json_value_t v);

// Littéral JSON (entre guillemets) de la chaîne C `s`, malloc : '"', '\\' et les caractères de contrôle échappés,
// l'UTF-8 gardé tel quel. NULL traité comme "" ; NULL si la mémoire manque.
char *studio_json_quote(const char *s);
