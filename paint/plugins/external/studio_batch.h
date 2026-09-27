#pragma once

// Lot B7 : mode batch. En --background, ArmorPaint tourne sans fenêtre (STUDIO-PATCH(headless)) ; ce module ajoute à
// la ligne de commande :
//   --report <fichier.json>       rapport JSON du run (état, code de retour, exports et leurs fichiers, erreurs, journal),
//                                 écrit au démarrage ("running") puis à la fin ;
//   --export-mode <mode>          mode de --export-textures : visible (défaut upstream), selected, per-object, per-tile
//                                 (alias udim) ;
//   --export-naming <modèle>      modèle de nom des fichiers (studio_export_set_naming), preset studio seulement ;
//   --export-color-space <espace> espace OCIO des textures de couleur (studio_export_set_color_space), preset studio.
// Code de retour d'un run --background : 0 si tout a abouti ; 1 si une erreur a été signalée (console_error, erreur de
// script minic, export en échec ou sans fichier écrit) ; 2 si la ligne de commande est fausse (rien n'est lancé :
// type, dossier ou preset de --export-textures, option studio inconnue ou sans valeur, script introuvable).
// Hors --background, les options s'appliquent à l'export de --export-textures ; ni rapport ni code de retour.

#include <stdbool.h>

// external_init(), en dernier (les presets et OCIO sont prêts) : lit la ligne de commande ; en --background, une ligne
// fausse écrit le rapport et quitte avec le code 2.
void studio_batch_init(void);
bool studio_batch_active(void); // --background
int  studio_batch_exit_code(void);

// Points d'injection STUDIO-PATCH(batch-report).
void studio_batch_on_error(const char *message); // console_error (paint/sources/console.c), avant son console_log
void studio_batch_on_log(const char *message);   // console_log : chaque message du journal
void studio_batch_on_file(const char *path);     // export_texture_write_texture : un fichier écrit par l'upstream
void studio_batch_finish(void);                  // main.c, après iron_start() : rapport, puis exit(code) si code != 0

// studio_export.c : un export (studio ou upstream) commence ou finit. mode : EXPORT_MODE_*.
void studio_batch_apply_cli(void); // options --export-* : au début de l'export de --export-textures
void studio_batch_export_begin(const char *dir, const char *preset, bool studio_preset, int mode, bool to_disk,
                               bool preflight_only);
void studio_batch_export_end(bool ok, const char *manifest);

// Sortie de script, dans "outputs" du rapport (une clé déjà donnée est remplacée).
void studio_batch_output(const char *key, const char *value);
