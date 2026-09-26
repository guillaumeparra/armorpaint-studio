#pragma once

// Lot C1 : exécution de processus externes, sans dépendance à ArmorPaint (libc + POSIX) : testable hors de l'app
// (.tests/proc_test.c). La colle avec l'app (jobs asynchrones, API de scripting) est dans studio_exec.c.
//
// Pourquoi pas iron_exec_async (base/sources/iron_system.h) : un seul processus à la fois, avec un état global
// (iron_exec_async_done) que partagent déjà les nœuds neuraux ; ni stdin, ni stdout en mémoire, ni code de sortie,
// ni timeout ; sous Windows, les '"' des arguments sont remplacés par des '\'' (du JSON passé en argument est cassé).
//
// Pas de shell : argv est passé tel quel au programme (recherché dans le PATH s'il ne contient pas de '/'), aucune
// interprétation, donc pas d'injection par un nom de fichier ou un champ ShotGrid.
//
// Le processus est lancé dans son propre groupe : un timeout ou studio_proc_kill() tue aussi ses descendants
// (un script Python qui lance oiiotool). Quand ArmorPaint quitte, le stdin des processus encore ouverts se ferme :
// un sidecar bien écrit s'arrête sur EOF.
//
// Windows : pas encore implémenté (studio_proc_spawn échoue avec un message explicite).

#include <stdbool.h>
#include <stddef.h>

typedef struct studio_proc studio_proc_t;

typedef struct studio_proc_opts {
	const char        *cwd;          // dossier de travail, NULL = celui d'ArmorPaint
	const char *const *env;          // "NOM=valeur" ajoutés ou remplacés dans l'environnement hérité, terminé par NULL
	bool               merge_stderr; // stderr dans stdout (ordre d'écriture conservé)
	size_t             max_output;   // octets gardés par flux, au-delà ils sont lus et jetés ; 0 = 64 Mio
} studio_proc_opts_t;

// Lance argv[0] avec argv (terminé par NULL). NULL en cas d'échec, message dans err.
studio_proc_t *studio_proc_spawn(const char *const *argv, const studio_proc_opts_t *opts, char *err, size_t err_size);

// Délais en millisecondes ; timeout_ms < 0 : sans limite.

// Écrit tout `data` sur le stdin du processus, en lisant sa sortie en même temps (pas d'interblocage si le processus
// écrit beaucoup avant de lire). false si le délai expire, si le processus a fermé son stdin, ou s'il est fermé ici.
bool studio_proc_write(studio_proc_t *p, const void *data, size_t size, int timeout_ms);
void studio_proc_close_stdin(studio_proc_t *p);

// Prochaine ligne de stdout, sans le '\n' (ni un '\r' final), allouée par malloc. NULL si le délai expire, si stdout
// est fermé sans ligne complète, ou si une ligne dépasse max_output. Pour un protocole ligne à ligne (sidecar JSON).
char *studio_proc_read_line(studio_proc_t *p, int timeout_ms);

// Attend la fin du processus en continuant de lire sa sortie. true s'il est terminé.
bool studio_proc_wait(studio_proc_t *p, int timeout_ms);

// SIGTERM au groupe du processus, puis SIGKILL après grace_ms s'il n'est pas terminé. Attend la fin.
void studio_proc_kill(studio_proc_t *p, int grace_ms);

// Tue le processus s'il tourne encore (grâce de 200 ms), ferme les pipes, libère.
void studio_proc_free(studio_proc_t *p);

// Annulation : tant que *cancel est non nul, studio_proc_write, studio_proc_read_line et studio_proc_wait rendent la
// main (au plus STUDIO_PROC_TICK_MS plus tard) sans avoir abouti. NULL pour détacher. Le processus n'est pas tué.
void studio_proc_set_cancel(studio_proc_t *p, volatile int *cancel);

// État, valide une fois studio_proc_wait() vrai.
bool studio_proc_exited(const studio_proc_t *p);
int  studio_proc_exit_code(const studio_proc_t *p); // code de sortie, -1 si tué par un signal ou inconnu
int  studio_proc_signal(const studio_proc_t *p);    // signal qui l'a tué, 0 sinon

// Sortie accumulée (non consommée par studio_proc_read_line), terminée par NUL. *truncated : max_output dépassé.
const char *studio_proc_stdout(const studio_proc_t *p, size_t *size, bool *truncated);
const char *studio_proc_stderr(const studio_proc_t *p, size_t *size, bool *truncated);

// Lit le stderr disponible sans attendre, puis le consomme : copie malloc (terminée par NUL, NULL si mémoire
// insuffisante) et vide le tampon. *truncated (optionnel) : max_output a été dépassé depuis la dernière prise.
// Pour relayer au fil de l'eau le stderr d'un processus qui dure (sidecar).
char *studio_proc_take_stderr(studio_proc_t *p, size_t *size, bool *truncated);

// ---- Exécution complète ----------------------------------------------------------------------------------------------

typedef struct studio_proc_result {
	bool   started;   // false : lancement impossible, voir error
	bool   timed_out; // tué au bout de timeout_ms
	bool   cancelled; // tué parce que *cancel est passé à non nul
	int    exit_code; // -1 si tué par un signal, timeout compris
	int    signal;
	char  *out; // malloc, terminé par NUL, jamais NULL une fois la fonction retournée
	size_t out_size;
	bool   out_truncated;
	char  *err; // idem ; vide si merge_stderr
	size_t err_size;
	bool   err_truncated;
	char   error[256]; // lancement impossible, écriture du stdin impossible… ; "" sinon
} studio_proc_result_t;

// Lance, écrit `in` sur stdin puis le ferme, attend la fin, capture stdout et stderr. timeout_ms couvre le tout.
// cancel (optionnel) : lu toutes les 20 ms au plus, une valeur non nulle tue le processus.
// Retourne true si le processus a été lancé et s'est terminé seul (quel que soit son code de sortie).
bool studio_proc_run(const char *const *argv, const studio_proc_opts_t *opts, const void *in, size_t in_size, int timeout_ms,
                     volatile int *cancel, studio_proc_result_t *res);
void studio_proc_result_free(studio_proc_result_t *res);
