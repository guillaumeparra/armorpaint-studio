#pragma once

// Lot C5 : mode dégradé. Quand le site ShotGrid ne répond plus (studio_sg_network), ArmorPaint reste utilisable en mode
// fichier : ouvrir, peindre, enregistrer et exporter ne dépendent pas de ShotGrid. Ce qui en dépend échoue vite et le
// dit : les commandes du menu ShotGrid attendent le retour du site, les textures exportées gardent les noms d'ArmorPaint
// (studio_export_naming.h), et un publish en cours met ses enregistrements dans ShotGrid en file d'attente
// (assets/studio_sg_queue.py) : ses fichiers sont écrits, il est enregistré au retour du site.
//
// Le passage hors ligne est dit une fois dans la console, puis dans le titre de la fenêtre et en tête du menu ShotGrid
// (Check Connection) ; le site est de nouveau essayé toutes les $STUDIO_SG_CHECK_INTERVAL secondes (120 par défaut,
// 0 : jamais), ArmorPaint au repos compris. À son retour (le premier appel réussi de la session compte aussi), les
// publishes en file sont rejoués (op replay_publishes), projet par projet ; le menu montre ceux qui restent (Replay
// Pending Publishes).
//
// File d'attente : $STUDIO_SG_QUEUE, défaut le dossier studio_publish_queue à côté des préférences (studio_prefs_dir) ;
// en --background sans l'un ni l'autre, pas de file : un publish interrompu échoue. Thread principal.

#include <stdbool.h>

void studio_sg_offline_init(void); // external_init(), après studio_shotgrid_init()

const char *studio_sg_queue_dir(void); // dossier de la file d'attente, NULL sans file
bool        studio_sg_offline(void);   // la dernière réponse de ShotGrid disait le site injoignable
int         studio_sg_pending(void);   // publishes en file (relevé toutes les 2 s), 0 sans file

// Section du menu ShotGrid (studio_workfiles.c) : état hors ligne et Check Connection, publishes en attente et Replay
// Pending Publishes. Rend le nombre de lignes dessinées.
int  studio_sg_offline_menu(void);
void studio_sg_check_connection(void); // essaie le site maintenant (op check_online), dit le résultat
void studio_sg_replay_pending(void);   // rejoue les publishes en file, projet par projet
