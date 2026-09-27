#pragma once

// Lot B6 : fenêtre d'export de textures du studio, en Qt. File > Export > Textures, l'icône d'export de la barre,
// Export du menu d'un calque et les raccourcis ouvrent assets/studio_export_window.py (PySide6) à la place de la boîte
// d'export d'ArmorPaint (STUDIO-PATCH(export-window) de paint/sources/ui/box_export.c). Elle ne propose que les presets
// studio, et l'export passe par studio_export_request.
//
// La fenêtre est un processus à part, lancé à l'ouverture et arrêté à sa fermeture ; elle s'arrête aussi quand
// ArmorPaint quitte (EOF sur son stdin). Protocole, une ligne JSON par message sur stdin et stdout :
//   requête de la fenêtre   {"id":1,"op":"state"}                      état pour remplir la fenêtre
//                           {"id":2,"op":"preflight","args":{...}}     rapport sans rien écrire
//                           {"id":3,"op":"export","args":{...}}        export, puis rapport
//                           {"id":4,"op":"naming","args":{"refresh":b}} nommage ShotGrid (redemandé avec refresh)
//                           {"id":5,"op":"version_up"}                 projet enregistré en version suivante
//   réponse d'ArmorPaint    {"id":N,"ok":true,"data":...} ou {"id":N,"ok":false,"error":{"code","message"}}
//   évènement d'ArmorPaint  {"event":"raise","mode":M}                 menu rouvert : la fenêtre repasse devant
// args : preset, color_space ("" : celui du preset), dir, mode (EXPORT_MODE_*), padding, skip (fichiers décochés :
// [[texture, variant, udim], ...], voir studio_export_set_skip). L'état donne aussi les objets du projet et ses tuiles
// UDIM, et sous un template ShotGrid (lot B6) "sg_naming" : templates possibles et version du fichier de travail
// (studio_export_naming_json) ; le template choisi est écrit par la fenêtre dans le sidecar du projet
// ("texture_template"), puis naming avec refresh. Les requêtes sont traitées une à une, à la frame suivante, hors du dessin. Un export abouti devient l'export
// courant d'ArmorPaint (dossier, preset, mode, marge, espace, fichiers décochés) : Ctrl+E le refait ; Cancel ne
// change rien.
//
// Interpréteur : STUDIO_PYTHON, sinon STUDIO_SG_PYTHON, sinon python3 dans le PATH ; il lui faut PySide6.
// STUDIO_EXPORT_WINDOW remplace le script (data/studio_export_window.py). Pas encore sous Windows (studio_proc).

#include <stdbool.h>

void studio_export_window_init(void);
void studio_export_window_show(void); // ouvre la fenêtre, ou la ramène devant
bool studio_export_window_is_open(void);

// Export ou préflight comme la fenêtre, pour le publish (lot C4, studio_publish.c), sans changer l'export courant.
// choices : objet JSON, les args d'une requête export ; NULL ou "" : l'export courant, celui que refait Ctrl+E.
// more_skip : fichiers à ne pas écrire en plus (lignes de studio_export_set_skip), NULL : aucun. Rend (malloc)
// {"ok","report","error"}, ou {"ok":false,"report":null,"code","error"} si les choix sont refusés (code no_preset :
// aucun export studio dans la session). Thread principal, hors du dessin.
char *studio_export_run_choices(const char *choices, bool preflight_only, const char *more_skip);
