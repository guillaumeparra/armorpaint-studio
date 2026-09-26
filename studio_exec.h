#pragma once

// Lot C1 : pont d'exécution. Des jobs (un processus lancé sur un thread, résultat interrogé par identifiant) pour les
// modules studio et pour les scripts minic, sur studio_proc (studio_proc.h). Les fonctions de scripting sont
// enregistrées par minic_register() depuis external_init() : aucun patch upstream.
//
// Pour un processus qui reste ouvert et dialogue ligne à ligne (le sidecar ShotGrid du lot C2), utiliser studio_proc
// directement.

#include "studio_proc.h"

void studio_exec_init(void);

#define STUDIO_EXEC_UNKNOWN -1 // identifiant inconnu ou libéré
#define STUDIO_EXEC_RUNNING 0
#define STUDIO_EXEC_DONE    1

// Lance argv (terminé par NULL) avec `in` sur stdin (NULL : rien), sans shell, tué au bout de timeout_ms (< 0 : sans
// limite). Retourne l'identifiant du job (> 0), ou 0 si trop de jobs sont ouverts : un job se libère par
// studio_exec_release(). Un lancement impossible (programme introuvable…) donne un job terminé, avec son erreur.
int studio_exec_start(const char *const *argv, const char *in, int timeout_ms);
// Même chose, mais attend la fin avant de retourner.
int studio_exec_run(const char *const *argv, const char *in, int timeout_ms);

int studio_exec_state(int job);
// Résultat, NULL tant que le job tourne ou s'il est inconnu. Valide jusqu'à studio_exec_release().
const studio_proc_result_t *studio_exec_result(int job);
// Tue le processus (SIGTERM puis SIGKILL) ; le job se termine avec result->cancelled.
void studio_exec_cancel(int job);
// Libère le job. S'il tourne encore, il est annulé et libéré à sa fin.
void studio_exec_release(int job);
