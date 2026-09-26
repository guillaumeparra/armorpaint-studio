#pragma once

// Lot B4 : canaux calculés des presets studio. Une expression combine les canaux d'ArmorPaint (noms de la liste des
// presets upstream, box_export_channels), des nombres, + - * /, le moins unaire, des parenthèses et les fonctions
// abs, min, max, clamp, pow, mix, sqrt, step (GLSL). Ex. : "occ * rough", "1 - metal", "mix(rough, 0.5, 0.25)".
//
// Compilée une fois en code postfixe, évaluée par ligne de pixels (n valeurs à la fois). Sans dépendance à ArmorPaint :
// la colle (studio_export.c) fournit les lignes de chaque variable, déjà décodées en float.
//
// Garde-fous : division par zéro et racine d'un négatif donnent 0, un NaN en sortie devient 0 ; les infinis restent
// (les encodeurs bornent ou convertissent). Nombres lus sans locale ("0.5", jamais "0,5").

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
	STUDIO_VAR_BASE_R,
	STUDIO_VAR_BASE_G,
	STUDIO_VAR_BASE_B,
	STUDIO_VAR_HEIGHT,
	STUDIO_VAR_METAL,
	STUDIO_VAR_NOR_R,
	STUDIO_VAR_NOR_G,
	STUDIO_VAR_NOR_G_DIRECTX,
	STUDIO_VAR_NOR_B,
	STUDIO_VAR_OCC,
	STUDIO_VAR_OPAC,
	STUDIO_VAR_ROUGH,
	STUDIO_VAR_SMOOTH,
	STUDIO_VAR_EMIS,
	STUDIO_VAR_SUBS,
	STUDIO_VAR_DIFF_R,
	STUDIO_VAR_DIFF_G,
	STUDIO_VAR_DIFF_B,
	STUDIO_VAR_SPEC_R,
	STUDIO_VAR_SPEC_G,
	STUDIO_VAR_SPEC_B,
	STUDIO_VAR_COUNT,
} studio_var_t;

// Variables de couleur (albedo et ce qui en dérive) : les seules que les espaces "linear" et "srgb" décodent.
#define STUDIO_VAR_COLOR_MASK                                                                                          \
	((1u << STUDIO_VAR_BASE_R) | (1u << STUDIO_VAR_BASE_G) | (1u << STUDIO_VAR_BASE_B) | (1u << STUDIO_VAR_DIFF_R) |   \
	 (1u << STUDIO_VAR_DIFF_G) | (1u << STUDIO_VAR_DIFF_B) | (1u << STUDIO_VAR_SPEC_R) | (1u << STUDIO_VAR_SPEC_G) |  \
	 (1u << STUDIO_VAR_SPEC_B))

// Nom d'une variable ("base_r"...), NULL hors bornes ; indice d'un nom, -1 s'il est inconnu.
const char *studio_var_name(int var);
int         studio_var_find(const char *name);

typedef struct studio_expr studio_expr_t;

// NULL et err rempli (position comprise) si l'expression est invalide.
studio_expr_t *studio_expr_compile(const char *source, char *err, size_t err_size);
void           studio_expr_free(studio_expr_t *e);

// Masque des variables lues (bit 1 << studio_var_t).
uint32_t studio_expr_vars(const studio_expr_t *e);
// Vrai si l'expression est une constante (après repli des sous-expressions constantes), sa valeur dans *value.
bool studio_expr_constant(const studio_expr_t *e, float *value);
// La variable si l'expression n'est qu'une variable ("rough"), -1 sinon.
int studio_expr_single_var(const studio_expr_t *e);
// Profondeur de pile : studio_expr_eval demande depth * n floats de travail.
int studio_expr_depth(const studio_expr_t *e);

// out[i] = expression sur les valeurs vars[v][i], pour i dans [0, n). vars a STUDIO_VAR_COUNT entrées ; seules celles
// de studio_expr_vars() sont lues. scratch : studio_expr_depth(e) * n floats. out peut être l'une des lignes de vars.
void studio_expr_eval(const studio_expr_t *e, int n, const float *const *vars, float *out, float *scratch);
