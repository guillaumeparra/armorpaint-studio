#pragma once

// Lots A5 et C5 : ce que les modules studio gardent dans le projet. Le format .arm n'a pas de champ libre : chaque
// module a son entrée, un fichier nommé sans dossier (studio_ocio.json…), dans le paquet d'assets du projet
// (packed_assets), qu'ArmorPaint sans studio relit et réécrit telle quelle. Les modules écrivent leur entrée juste
// avant l'encodage du projet (STUDIO-PATCH(project-entries) de io/export_arm.c) et la lisent quand un projet est ouvert.
//
// Changement de projet (ouverture d'un .arm, nouveau projet) : détecté au plus tard à la frame suivante, ou dès qu'un
// module appelle studio_project_check() ; l'import des textures le fait avant chaque décodage, les textures d'un projet
// qu'on ouvre sont donc décodées après que les modules ont lu leurs entrées. Thread principal.

#include "iron_array.h"
#include <stdbool.h>

void studio_project_init(void); // external_init(), avant les modules qui s'abonnent

// fn(opened) à chaque changement de projet, dans l'ordre d'abonnement : opened vrai pour un projet lu d'un .arm, faux
// pour un nouveau projet (celui du démarrage compris).
void studio_project_on_change(void (*fn)(bool opened));
// fn(texture_files) avant l'encodage du projet ; texture_files : chemins des assets tels qu'écrits dans le .arm.
void studio_project_on_save(void (*fn)(string_array_t *texture_files));

void studio_project_check(void);  // détecte un changement de projet et prévient les abonnés
int  studio_project_serial(void); // change à chaque projet (après studio_project_check)
bool studio_project_opened(void); // le projet courant a été lu d'un .arm

char *studio_project_entry(const char *name);                       // contenu (malloc, terminé par NUL) ; NULL si absente
bool  studio_project_set_entry(const char *name, const char *text); // NULL : retire l'entrée ; faux sans mémoire

// STUDIO-PATCH(project-entries) : export_arm_run_project, avant util_encode_project.
void studio_project_save_entries(string_array_t *texture_files);
