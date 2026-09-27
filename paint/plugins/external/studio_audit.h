#pragma once

// Lot C5 : journal d'audit local, une ligne JSON par évènement (JSON Lines), dans studio_audit.jsonl du dossier des
// préférences (studio_prefs_dir), ou dans le fichier $STUDIO_AUDIT_LOG. Sans l'un ni l'autre (--background sans
// $STUDIO_PREFS), rien n'est écrit. Le sidecar ShotGrid écrit dans le même fichier (assets/studio_audit.py) : chaque
// ligne part d'une seule écriture en ajout, les lignes des deux processus ne se mêlent pas. Pour le lire :
//   jq -c 'select(.event | startswith("publish"))' studio_audit.jsonl
//
// Champs communs : time (UTC, "2026-09-27T10:04:05.123Z"), source ("armorpaint" ou "sidecar"), pid, host, user,
// event ; puis ceux de l'évènement :
//   armorpaint  project_open       path, sg (contexte ShotGrid du projet, studio_sg_context.h, s'il en a un)
//               project_save       path, size, sg
//               project_copy       path, from (le projet copié), size : la copie publiée du projet (lot C4)
//               sg_offline         reason : le site ShotGrid ne répond plus
//               sg_online          pending : il répond de nouveau ; publishes en file d'attente
//   sidecar     publish            id, puis site, sg_login, project_id, task_id, path, name, version_number
//               publish_queued     key, id (négatif pour un publish qui n'a pas pu être créé), depends_on, error
//               publish_replayed   key, id, queued_at
//               publish_failed     error (replay : pendant un rejeu)
//               publish_discarded  key
// Au démarrage, un journal de plus de 10 Mo passe en .1 (l'ancien .1 est remplacé). Thread principal.
// Le sidecar l'envoie au site ShotGrid en EventLogEntry à chaque essai du site (studio_sg_offline.c, op check_online
// avec "audit") ; où il en est : <journal>.sent (assets/studio_audit.py).

void        studio_audit_init(void); // external_init(), après studio_prefs_init() et studio_project_init()
const char *studio_audit_path(void); // fichier du journal, NULL s'il est désactivé
// Ajoute l'évènement. fields : membres JSON après les champs communs ("\"path\":\"/x.arm\""), NULL ou "" pour aucun.
// Un échec d'écriture est signalé une fois au log, jamais bloquant.
void studio_audit(const char *event, const char *fields);
