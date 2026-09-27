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
	void (*wake)(void);                  // depuis le thread de l'appel : l'app a quelque chose à relever (requête
	                                     // host, job fini). Pour une boucle d'interface qui dort sans entrée
	                                     // utilisateur ; NULL = rien
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
// Job différé : en file (STUDIO_SG_QUEUED) jusqu'à delay_ms, les jobs suivants passent devant lui d'ici là. Sa réponse
// réveille l'app comme les autres (config.wake), même au repos : de quoi faire quelque chose à heure fixe sans frames.
int  studio_sg_start_after(const char *op, const char *args_json, int timeout_ms, int delay_ms);
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

// ---- Requêtes du sidecar vers l'app (host) ------------------------------------------------------------------------------
// Pendant un appel, le sidecar peut demander une action à l'app : {"host":N,"op":"open","args":{...}} (les opérations
// sur la scène du moteur Toolkit tk-armorpaint, op show_app). Le thread de l'interface les relève à chaque frame et
// répond {"host":N,"ok":true,"data":...} ou {"host":N,"ok":false,"error":{"code":...,"message":...}} ; le thread de
// l'appel écrit la réponse au sidecar, qui attend. Une requête qui reste sans réponse bloque l'appel en cours : un
// appel qui en émet se lance en job (studio_sg_start), jamais par studio_sg_run depuis le thread de l'interface.
// Les requêtes et réponses non consommées à la fin de l'appel sont jetées.

char *studio_sg_host_take(void);               // prochaine requête (à libérer), NULL si aucune
void  studio_sg_host_reply(const char *reply); // réponse (objet JSON portant "host")

// ---- Joignabilité du site ShotGrid (lot C5) ---------------------------------------------------------------------------
// Une réponse du sidecar porte "sg" quand la requête a parlé au site : {"online":true}, ou {"online":false,"reason":...}
// s'il était injoignable ; une erreur sg_unreachable dit la même chose. Le superviseur en tient l'état, le passe au log
// à chaque changement et réveille l'app (config.wake). Les erreurs du superviseur (sidecar mort, timeout, annulation)
// ne disent rien du site : l'état reste celui de la dernière réponse qui en parlait. Inconnu au démarrage et après
// studio_sg_configure.

enum {
	STUDIO_SG_NET_UNKNOWN = 0,
	STUDIO_SG_NET_ONLINE  = 1,
	STUDIO_SG_NET_OFFLINE = 2
};

int studio_sg_network(char **reason); // état ; *reason (optionnel) : message du dernier échec (malloc), NULL en ligne
int studio_sg_network_serial(void);   // change à chaque changement d'état

// À la sortie de l'app : annule les jobs, arrête le sidecar. Les appels suivants échouent en sidecar_failed.
void studio_sg_shutdown(void);
