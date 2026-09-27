#pragma once

// Lot C4 : publish vers ShotGrid par tk-multi-publish2, avec les hooks du moteur tk-armorpaint
// (tk-armorpaint/hooks/tk-multi-publish2/). La fenêtre Publish tourne dans le sidecar (op show_app, comme File Open) ;
// ses hooks demandent ici, par des requêtes host, ce qui se passe dans ArmorPaint :
//   publish_preflight  {"naming","choices"}          préflight de l'export des textures, sans rien écrire
//   publish_export     {"naming","choices","skip"}   export des textures
//   publish_thumbnail  {"path"}                      vignette du viewport (PNG), répondue quelques frames plus tard
//   publish_copy       {"path"}                      copie du projet enregistré (.arm), écrite par ArmorPaint
// naming : le modèle des textures ({"template","pattern","pattern_udim"} de l'op texture_pattern, calculé par le sidecar,
// qui ne peut pas répondre à ArmorPaint pendant que la fenêtre est ouverte). choices : les choix d'un export de la
// fenêtre d'export (args de sa requête export, repris du sidecar du projet) ; absent : l'export courant, celui que
// refait Ctrl+E. skip : fichiers à ne pas écrire, [[texture, variant, tuile], ...] (textures décochées dans Publish).
// Réponse des deux premières : {"path","naming_template","export":{"ok","report","error"[,"code"]}}, report comme
// studio_export_last_report() ; chaque fichier y porte "sequence", son chemin avec <UDIM> à la place de la tuile.
// L'export courant n'est pas changé. publish_copy répond {"path","size"} : la copie désigne les fichiers du projet
// (textures importées, maillage source, envmap…) par des chemins relatifs à son propre dossier, là où une copie du
// fichier les perdrait. L'enregistrement du projet passe par les requêtes save et save_as de studio_workfiles.c.

#include <stdbool.h>

// Requête host op (request : le message entier, {"host":N,"op","args"}) : true si c'est une requête du publish,
// traitée ; sa réponse part tout de suite, ou à une frame suivante pour la vignette.
bool studio_publish_host(int host, const char *op, const char *request);

// Point d'injection STUDIO-PATCH(project-copy) de export_arm_run_project() (paint/sources/io/export_arm.c) : vrai
// pendant l'écriture d'une copie, dont les chemins sont alors relatifs à elle et non au projet ouvert.
bool studio_project_copy(void);
