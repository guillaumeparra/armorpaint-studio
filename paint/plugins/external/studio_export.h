#pragma once

// Lots B2, B4, B5 : export de textures par preset studio (studio_preset.h). Chaque preset studio valide est installé
// dans la liste d'export d'ArmorPaint sous forme d'un preset upstream (stub) ; quand il est choisi (boîte d'export,
// --export-textures, studio_export_run), l'export passe par ici :
//   1. passe à blanc d'export_texture_run : l'upstream parcourt objets, atlas et tuiles, studio_export_layers note les
//      fichiers qu'il écrirait (plan) ;
//   2. préflight (studio_preflight.h) : bloquant -> rien n'est écrit, rapport affiché ;
//   3. passe réelle : pour chaque variante, relecture des calques aplatis, composition des canaux (expressions),
//      encodage (PNG 8/16, JPG, TIFF 8/16/32 none/lzw/deflate, EXR 16/32), écriture en <fichier>.studio-tmp ;
//   4. tout est renommé en place, puis le manifeste <fichier>.export.json (empreintes SHA-256, contexte) est écrit.
// Une erreur à l'étape 3 retire les fichiers temporaires : aucun fichier existant n'est remplacé.
//
// STUDIO_EXPORT_PRESETS : dossiers ou fichiers .json de presets studio (séparés par ':' , ';' sous Windows), prioritaires
// sur les presets livrés (data/studio_preset_<nom>.json). STUDIO_EXPORT_THREADS : threads d'encodage (défaut : tous).

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
