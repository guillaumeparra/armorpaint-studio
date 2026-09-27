#pragma once

// Lot A5 : réglages de la gestion couleur, entre deux sessions et d'un projet à l'autre.
//
// Préférences de l'application (studio_prefs.h), onglet Color des préférences (STUDIO-PATCH(preferences-tab)) : la
// configuration OCIO ("ocio.config", sans elle la configuration intégrée ; $OCIO l'emporte) et l'espace de travail des
// nouveaux projets ("ocio.working_space", sans lui l'automatique ; $STUDIO_OCIO_WORKING_SPACE l'emporte). Le choix du
// viewport y est aussi gardé (studio_ocio_view.h).
//
// Projet : son espace de travail est dans l'entrée studio_ocio.json du paquet d'assets du .arm (studio_project.h), avec
// le nom et la source de la configuration. Un nouveau projet suit la préférence jusqu'à son enregistrement, qui fige
// l'espace trouvé ; un projet ouvert reprend le sien, et sans entrée (projet d'avant le lot A5, ou d'ArmorPaint sans
// studio) l'automatique : les couleurs d'ArmorPaint. Menu Color > Working Space pour le changer.
//
// Quand la configuration ou l'espace de travail changent en cours de session, les textures converties sont rechargées
// depuis leurs fichiers (studio_ocio_import_reconvert) ; ce qui est déjà peint dans les calques ne l'est pas.
// Au démarrage (hors --background), une boîte signale OCIO indisponible ou un espace de travail introuvable.
// Thread principal.

#include <stdbool.h>

void studio_ocio_prefs_init(void); // external_init(), après studio_prefs_init() et avant studio_ocio_init()

void studio_ocio_prefs_working_space_menu_draw(void); // menu Color (studio_ocio_view.c) : sous-menu Working Space
void studio_ocio_prefs_menu_draw(void);               // menu Color : Color Preferences...
bool studio_ocio_prefs_show(void);                    // ouvre les préférences sur l'onglet Color ; faux sans onglet

// Changent la préférence (enregistrée), rechargent ce qui en dépend et reconvertissent les textures tout de suite.
// Configuration : NULL ou "" pour aucune (configuration intégrée) ; rend studio_ocio_ready(). Avec $OCIO, la
// préférence est gardée pour plus tard, rien n'est rechargé.
bool studio_ocio_prefs_set_config(const char *path);
// Espace de travail des nouveaux projets : NULL ou "" pour l'automatique. Faux, sans rien changer, si le nom est
// inconnu de la configuration chargée.
bool studio_ocio_prefs_set_working_space(const char *name);
// Espace de travail du projet courant (STUDIO_OCIO_WS_*, studio_ocio.h), gardé dans le .arm au prochain
// enregistrement. Faux, sans rien changer, si le mode est inconnu ou le nom inconnu de la configuration.
bool studio_ocio_prefs_set_project_working_space(int mode, const char *name);
