#pragma once

// Lot C2 : superviseur du sidecar ShotGrid (assets/studio_sg_bridge.py, protocole décrit dans son en-tête). Sans
// dépendance à ArmorPaint (libc, POSIX, studio_proc, studio_json) : testable hors de l'app (.tests/sg_super_test.c).
// La colle avec l'app (commande, log, API de scripting) est dans studio_shotgrid.c.
//
// Le sidecar est lancé au premier appel et gardé ensuite : l'amorçage Toolkit coûte des secondes, on ne le paie
// qu'une fois par projet. Les appels passent un par un. S'il meurt, le suivant le relance ; s'il ne répond pas à
// temps, ou si l'appel est annulé, il est tué (son état est inconnu) et le suivant le relance aussi. Quand il annonce
// "restart" (changement de projet Toolkit), il est relancé et la requête rejouée une fois.
//
// Toute réponse a la forme du protocole, {"id":N,"ok":true,"data":...} ou {"id":N,"ok":false,"error":{"code","message"}},
// y compris les erreurs du superviseur, de codes :
//   sidecar_failed  lancement impossible, poignée de main ratée, réponse illisible
//   sidecar_died    le sidecar s'est arrêté pendant l'appel (le message cite la fin de son stderr)
//   timeout         pas de réponse dans le délai (le sidecar est tué)
//   cancelled       annulé (idem)
//   bad_request     op vide, args qui n'est pas un objet JSON
// Un appel qui échoue en sidecar_died, timeout ou cancelled n'est jamais rejoué : l'opération (un publish) peut avoir
// eu lieu.

#include <stdbool.h>

typedef struct studio_sg_config {
	const char *const *argv;             // commande du sidecar, terminée par NULL
	const char *const *env;              // "NOM=valeur" ajoutés à l'environnement, terminé par NULL ; peut être NULL
	int                hello_timeout_ms; // lancement jusqu'à la poignée de main ; 0 = 60 s
	int                call_timeout_ms;  // délai par défaut d'un appel ; 0 = 300 s (un premier amorçage Toolkit peut
	                                     // télécharger la configuration du projet)
	void (*log)(const char *line);       // stderr du sidecar (ligne par ligne) et évènements ; NULL = aucun log
} studio_sg_config_t;

// Copie la configuration. Le sidecar en cours est arrêté : le prochain appel lance la nouvelle commande.
void studio_sg_configure(const studio_sg_config_t *config);

// Appel synchrone, sur le thread appelant (bloque tant qu'un autre appel est en cours). args_json : objet JSON, NULL ou
// "" pour {}. timeout_ms : 0 = délai par défaut, < 0 = sans limite. cancel (optionnel) : non nul annule l'appel.
// Retourne la réponse (malloc), NULL seulement si la mémoire manque.
char *studio_sg_call(const char *op, const char *args_json, int timeout_ms, volatile int *cancel);

// Arrête le sidecar (EOF sur stdin, puis tué s'il traîne) ; le prochain appel le relance. Par exemple après une
// connexion dans ShotGrid Desktop ou un changement de configuration.
void studio_sg_stop(void);

// Poignée de main du sidecar en cours ({"id":0,"ok":true,"data":{"protocol":1,"toolkit":{...},...}}), malloc ; NULL
// s'il ne tourne pas.
char *studio_sg_hello(void);

// ---- Jobs -----------------------------------------------------------------------------------------------------------
// Pour ne pas bloquer l'interface : les jobs passent dans l'ordre sur un thread dédié. Un job garde sa réponse, et les
// textes qu'on en extrait, jusqu'à studio_sg_release(). 0 = échec (table pleine : 256 jobs ouverts au plus).

enum { STUDIO_SG_UNKNOWN = 0, STUDIO_SG_QUEUED = 1, STUDIO_SG_RUNNING = 2, STUDIO_SG_DONE = 3 };

int  studio_sg_start(const char *op, const char *args_json, int timeout_ms);
int  studio_sg_run(const char *op, const char *args_json, int timeout_ms); // synchrone : le job est fini au retour
int  studio_sg_state(int job);
void studio_sg_cancel(int job);  // en file : ne partira pas ; en cours : sidecar tué ; réponse "cancelled"
void studio_sg_release(int job); // en cours : annulé, libéré à la fin

// Réponse du job (NULL tant qu'il n'est pas fini) et lectures par JSON Pointer (studio_json.h) dans cette réponse :
// studio_sg_get(job, "/data/0/code"), studio_sg_get(job, "/error/code")… Les textes rendus vivent jusqu'au release.
const char *studio_sg_result(int job);
bool        studio_sg_ok(int job);
const char *studio_sg_get(int job, const char *pointer);        // "" si absent
int         studio_sg_length(int job, const char *pointer);     // éléments d'un tableau ou d'un objet, -1 sinon
long long   studio_sg_int(int job, const char *pointer, long long fallback);

// À la sortie de l'app : annule les jobs, arrête le sidecar. Les appels suivants échouent en sidecar_failed.
void studio_sg_shutdown(void);
