#pragma once

// Lots B2, B4, B5 : export de textures par preset studio (studio_preset.h). Les presets studio sont seulement lus (rien
// n'est installé ni écrit) ; quand l'un d'eux est choisi (fenêtre d'export, studio_export_window.h ; --export-textures
// --export-preset <nom> ; studio_export_run ; Ctrl+E), l'export passe par ici :
//   1. passe à blanc d'export_texture_run : l'upstream parcourt objets, atlas et tuiles, studio_export_layers note les
//      fichiers qu'il écrirait (plan) ;
//   2. préflight (studio_preflight.h) : bloquant -> rien n'est écrit, rapport affiché ;
//   3. passe réelle : pour chaque variante, relecture des calques aplatis, composition des canaux (expressions),
//      mise à la résolution de la texture (moyenne par zone, bilinéaire pour agrandir), encodage (PNG 8/16, JPG, TIFF 8/16/32 none/lzw/deflate, EXR 16/32), écriture en <fichier>.studio-tmp ;
//   4. tout est renommé en place, puis le manifeste <fichier>.export.json (empreintes SHA-256, contexte) est écrit.
// Une erreur à l'étape 3 retire les fichiers temporaires : aucun fichier existant n'est remplacé.
//
// STUDIO_EXPORT_PRESETS : dossiers ou fichiers .json de presets studio (séparés par ':' , ';' sous Windows), prioritaires
// sur les presets livrés (data/studio_preset_<nom>.json). STUDIO_EXPORT_THREADS : threads d'encodage (défaut : tous).
// STUDIO_EXPORT_COLOR_SPACE : espace OCIO des textures de couleur (studio_export_set_color_space).

#include <stdbool.h>

struct slot_layer_t_array;

void studio_export_init(void);

// Points d'injection de paint/sources/io/export_texture.c (STUDIO-PATCH(studio-export)).
// Au début d'export_texture_run : false -> l'export s'arrête (préflight bloquant, passe à blanc terminée).
bool studio_export_begin(char *path, bool bake_material);
// Dans export_texture_run_layers, calques aplatis : true -> l'export studio a pris la main pour cette variante.
// base : nom de fichier avant la variante, f : avec la variante, udim : ".1001" ou "".
bool studio_export_layers(char *path, char *base, char *f, char *udim, struct slot_layer_t_array *layers);
// Avant "Textures exported" : false -> pas de message upstream (échec, ou fin de la passe à blanc).
bool studio_export_end(void);

// Rapport JSON du dernier préflight ("" si aucun).
const char *studio_export_last_report(void);
bool        studio_export_is_studio_preset(const char *name);
const char *studio_export_preset_path(const char *name); // fichier du preset studio ; NULL si preset upstream
bool        studio_export_preset_shipped(const char *name); // livré avec l'app (data/), pas de STUDIO_EXPORT_PRESETS
// Relit les dossiers de presets et rend leur nombre. Les noms rendus par studio_export_preset_name ne valent que
// jusqu'à la relecture suivante (preset_count, is_studio_preset, un export).
int         studio_export_preset_count(void);
const char *studio_export_preset_name(int index); // NULL hors limites

// Preset de l'export courant, repris par Ctrl+E : name, et json si le preset n'est pas dans les dossiers de presets
// (sidecar du projet ; source : son chemin, pour le manifeste). NULL ou "" : aucun (le preset de la boîte d'export).
void        studio_export_set_preset(const char *name, const char *json, const char *source);
const char *studio_export_preset(void);
const char *studio_export_preset_json(void);
const char *studio_export_preset_source(void);
// Bits et résolution par texture pour ce preset (studio_preset_apply_overrides) : {"normal": {"bits": 16,
// "resolution": 2048}}. "" : ceux du preset.
const char *studio_export_overrides(void);
void        studio_export_set_overrides(const char *json);
// Modèle de nom des fichiers pour ce preset (pour tous tant qu'aucun n'est choisi), à la place du "naming" du preset
// (pas de celui d'une texture qui a le sien) : variables de studio_naming_variables, vérifiées à l'export. "" : celui
// du preset. Ignoré quand un template ShotGrid nomme les textures (studio_export_naming.h).
const char *studio_export_naming(void);
void        studio_export_set_naming(const char *pattern);

// Export ou préflight seul (preflight_only) avec ce preset et ce mode (EXPORT_MODE_*, -1 : celui d'ArmorPaint), sans
// boîte de message d'ArmorPaint : l'appelant affiche studio_export_last_report() et studio_export_last_error(). Rend
// true si l'export a abouti (préflight seul : s'il n'est pas bloquant). Thread principal, hors du dessin.
bool        studio_export_request(const char *dir, const char *preset, int mode, bool preflight_only);
const char *studio_export_last_error(void); // "titre\ntexte" du dernier échec, "" sinon

// Espace colorimétrique imposé aux textures de couleur des presets studio (studio_preset_override_color) : "" ou
// NULL rend l'espace du preset. Rend false si le nom n'est pas un espace, alias ou rôle de la configuration OCIO
// chargée ; il est gardé quand même, et l'export sera bloqué avec la raison. Défaut : STUDIO_EXPORT_COLOR_SPACE.
const char *studio_export_color_space(void); // "" : celui du preset
bool        studio_export_set_color_space(const char *name);

// Fichiers décochés : une ligne "<texture>\t<variant>\t<tuile>" par fichier (texture du preset, objet ou atlas, tuile
// UDIM ; "" si sans), comme "texture", "variant" et "udim" du rapport. Ils restent dans le rapport ("skip": true) mais
// ne sont ni vérifiés ni écrits. "<texture>\t*\t*" : tous les fichiers de la texture (map désactivée). "" : tous les
// fichiers. Gardé pour les exports suivants (Ctrl+E).
const char *studio_export_skip(void);
void        studio_export_set_skip(const char *keys);
