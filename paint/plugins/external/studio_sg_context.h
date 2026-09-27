#pragma once

// Lot C5 : contexte ShotGrid du projet ouvert, celui de la dernière fenêtre Toolkit (File Open, File Save, Publish...)
// ou celui qu'a gardé le .arm. Il est enregistré dans le projet, entrée studio_shotgrid.json (studio_project.h) :
//   {"version":1,"context":{"site":"https://studio.shotgrid.autodesk.com","project_id":12,"project":"Demo",
//    "entity_type":"Asset","entity_id":34,"entity":"robot","step":"Texture","task_id":56,"task":"texturing",
//    "label":"Demo / robot / Texture / texturing"}}
// (le contexte du moteur tk-armorpaint, engine.context_info). Un projet rouvert par le menu File d'ArmorPaint, sur une
// autre machine ou après un redémarrage, reprend sa tâche : la prochaine fenêtre ShotGrid s'y ouvre, le publish y va.
// Ouvrir un .arm prend son contexte, ou aucun s'il n'en a pas ; un nouveau projet garde le contexte courant (le DCC
// reste dans sa tâche). Un contexte d'un autre site ShotGrid reste dans le fichier, le sidecar l'ignore.
//
// Le titre de la fenêtre le montre, et le mode dégradé (studio_sg_offline.h) : « robot.v003 - Demo / robot / Texture /
// texturing (ShotGrid offline) - ArmorPaint ». Thread principal.

#include <stdbool.h>

void studio_sg_context_init(void); // external_init(), après studio_project_init(), avant les modules qui s'en servent

// Contexte pris dans le document json à pointer : un objet {"project_id","task_id","label","site",...}. Un objet sans
// project_id retire le contexte ; autre chose qu'un objet est ignoré.
void studio_sg_context_set(const char *json, const char *pointer);
void studio_sg_context_clear(void);

const char *studio_sg_context_json(void);       // l'objet du contexte (JSON sur une ligne), NULL sans contexte
int         studio_sg_context_project_id(void); // 0 sans contexte
int         studio_sg_context_task_id(void);    // 0 sans tâche
const char *studio_sg_context_label(void);      // "Demo / robot / Texture / texturing", "" sans contexte
const char *studio_sg_context_site(void);       // "" si inconnu

// Arguments d'une requête au sidecar dans ce contexte (op list_commands, show_app, texture_pattern…) : first (membres
// JSON, ou ""), puis task_id, sinon project_id, et site. Chaîne temporaire (string_tmp).
char *studio_sg_context_args(const char *first);
