#pragma once

// Lot B4 : presets d'export studio. Un fichier JSON par preset, plus riche que les presets upstream
// (paint/assets/export_presets/*.json : nom, 4 canaux et color_space par texture, lus par json_parse qui ne connaît
// pas d'autre champ). Par texture : format, profondeur, compression, espace colorimétrique, suffixe, modèle de nom et
// canaux calculés (studio_expr). Format complet : README, « Presets d'export studio (B4) ».
//
// Sans dépendance à ArmorPaint (libc, studio_json, studio_expr, studio_sha256) : testable hors de l'app. La lecture est
// stricte : une clé inconnue est une erreur, pour qu'une faute de frappe ("colour_space") ne passe pas inaperçue.

#include "studio_expr.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
	STUDIO_FORMAT_PNG,
	STUDIO_FORMAT_JPG,
	STUDIO_FORMAT_TIFF,
	STUDIO_FORMAT_EXR,
} studio_format_t;

// raw : valeurs d'ArmorPaint telles quelles (= "linear" des presets upstream). linear : les canaux de couleur
// (STUDIO_VAR_COLOR_MASK, stockés encodés gamma 2.2 par ArmorPaint) sont décodés, les données restent telles quelles.
// srgb : comme linear, puis le résultat est encodé en gamma 1/2.2. Les constantes et le 4e canal (alpha) ne sont
// jamais transformés.
typedef enum {
	STUDIO_COLOR_RAW,
	STUDIO_COLOR_LINEAR,
	STUDIO_COLOR_SRGB,
} studio_color_t;

typedef enum {
	STUDIO_OVERWRITE_WARN, // défaut : remplace, signalé dans le rapport
	STUDIO_OVERWRITE_ALLOW,
	STUDIO_OVERWRITE_BLOCK,
} studio_overwrite_t;

typedef struct {
	char           *name;     // identifiant, unique dans le preset
	char           *suffix;   // valeur de <channel> dans le nom de fichier (défaut : name)
	char           *naming;   // modèle de nom propre à la texture, NULL : celui du preset
	int             channels; // 1, 3 ou 4
	char           *sources[4];
	studio_expr_t  *exprs[4];
	studio_format_t format;
	int             bits;
	// Compression normalisée : png "fast" | "default" | "best" ; tiff "none" | "lzw" | "deflate" ; exr : un nom
	// accepté par studio_exr_compression_from_name, ou NULL pour le défaut de l'app (STUDIO_EXR_COMPRESSION, zip).
	// jpg : NULL.
	char          *compression;
	int            quality; // jpg, 1-100
	studio_color_t color;
} studio_preset_texture_t;

typedef struct {
	int width;
	int height;
} studio_resolution_t;

typedef struct {
	char                    *name; // nom du preset (celui de la liste d'export)
	char                    *path; // fichier lu, NULL si parsé depuis une chaîne
	char                     sha256[65];
	char                    *description;
	char                    *naming; // modèle de nom (studio_naming_format)
	studio_resolution_t     *resolutions;
	int                      resolution_count; // 0 : toutes acceptées
	studio_overwrite_t       overwrite;
	bool                     manifest;
	studio_preset_texture_t *textures;
	int                      texture_count;
} studio_preset_t;

// Modèle par défaut : <file> est le nom du projet, <variant> l'objet ou l'atlas (export par objet ou par atlas),
// <channel> le suffixe de la texture, <UDIM> la tuile, <ext> l'extension.
#define STUDIO_PRESET_DEFAULT_NAMING "<file>[_<variant>]_<channel>[.<UDIM>].<ext>"

studio_preset_t *studio_preset_parse(const char *json, const char *name, char *err, size_t err_size);
// Lit et parse path ; sha256 et path renseignés.
studio_preset_t *studio_preset_load(const char *path, const char *name, char *err, size_t err_size);
void             studio_preset_free(studio_preset_t *p);

const char *studio_format_name(studio_format_t format);
const char *studio_format_ext(studio_format_t format);
const char *studio_color_name(studio_color_t color);
// Niveau sdefl (0-8) d'une compression PNG ou TIFF deflate.
int studio_preset_deflate_level(const char *compression);

// Preset upstream équivalent (JSON, malloc) : il fait apparaître le preset dans la liste d'export et sert à l'upstream
// quand l'export n'est pas pris en charge par le studio (textures empaquetées dans le projet). Approximation : une
// expression y devient "0.0", une constante "0.0" ou "1.0", l'espace "linear" des couleurs n'y a pas d'équivalent.
// Seules les clés upstream y figurent, dans leur ordre : json_parse les lit par position, pas par nom.
char *studio_preset_stub(const studio_preset_t *p);
