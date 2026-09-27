#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE // clock_gettime, nanosleep en -std=c11
#endif
#include "studio_sg.h"
#include "studio_json.h"
#include "studio_proc.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#define SG_MAX_JOBS 256
#define SG_DEFAULT_HELLO_MS 60000
#define SG_DEFAULT_CALL_MS 300000
// Tranche d'attente d'une réponse : entre deux tranches, le stderr du sidecar est relayé (progression d'un amorçage).
#define SG_SLICE_MS 250
#define SG_HOST_SLICE_MS 10 // tranche pendant qu'une requête host attend la réponse de l'app
#define SG_WRITE_MS 10000
// Arrêt : délai laissé au sidecar pour sortir seul après EOF (ou après avoir annoncé "restart").
#define SG_EXIT_GRACE_MS 3000
#define SG_TAIL_SIZE 4096     // fin du stderr gardée pour expliquer un arrêt
#define SG_LOG_LINE_MAX 4096  // ligne de stderr sans '\n' plus longue : relayée en morceaux
#define SG_MAX_OUTPUT ((size_t)256 << 20) // taille maximale d'une réponse

static int64_t now_ms(void) {
#ifdef _WIN32
	return (int64_t)GetTickCount64();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

// ---- Verrous ----------------------------------------------------------------------------------------------------------
// call : le sidecar et la configuration (un appel à la fois). state : la table des jobs et la poignée de main, jamais
// tenu longtemps. Ordre : call puis state.

#ifdef _WIN32
// studio_proc n'est pas encore implémenté sous Windows : tout échoue aussitôt, sans thread.
static void call_lock(void) {}
static void call_unlock(void) {}
static void state_lock(void) {}
static void state_unlock(void) {}
#else
static pthread_mutex_t call_mutex  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  state_cond  = PTHREAD_COND_INITIALIZER;

static void call_lock(void) {
	pthread_mutex_lock(&call_mutex);
}

static void call_unlock(void) {
	pthread_mutex_unlock(&call_mutex);
}

static void state_lock(void) {
	pthread_mutex_lock(&state_mutex);
}

static void state_unlock(void) {
	pthread_mutex_unlock(&state_mutex);
}
#endif

// ---- Configuration et état du sidecar (sous call) ---------------------------------------------------------------------

static char **cfg_argv;
static char **cfg_env;
static int    cfg_hello_ms = SG_DEFAULT_HELLO_MS;
static int    cfg_call_ms  = SG_DEFAULT_CALL_MS;
static void (*cfg_log)(const char *line);
static void (*cfg_wake)(void);

static studio_proc_t *sidecar;
static int            next_id = 1;
static char           tail[SG_TAIL_SIZE]; // fin du stderr, pour les messages d'erreur
static size_t         tail_len;
static char           partial[SG_LOG_LINE_MAX]; // ligne de stderr pas encore terminée
static size_t         partial_len;
static volatile int   closed; // studio_sg_shutdown() est passé

static char *hello; // sous state

// Requêtes du sidecar vers l'app pendant un appel ({"host":N,...}, voir studio_sg.h) et réponses de l'app, sous state.
typedef struct sg_line {
	char           *text;
	struct sg_line *next;
} sg_line_t;
static sg_line_t *host_requests;
static sg_line_t *host_replies;
static int        host_waiting; // sous call : requêtes de l'appel en cours dont la réponse n'est pas encore écrite

static void sg_log(const char *format, ...) {
	if (cfg_log == NULL) {
		return;
	}
	char    line[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	cfg_log(line);
}

static void strv_free(char **v) {
	if (v != NULL) {
		for (int i = 0; v[i] != NULL; i++) {
			free(v[i]);
		}
		free(v);
	}
}

static char **strv_copy(const char *const *v) {
	if (v == NULL) {
		return NULL;
	}
	int n = 0;
	while (v[n] != NULL) {
		n++;
	}
	char **copy = calloc((size_t)n + 1, sizeof(char *));
	for (int i = 0; copy != NULL && i < n; i++) {
		copy[i] = strdup(v[i]);
		if (copy[i] == NULL) {
			strv_free(copy);
			return NULL;
		}
	}
	return copy;
}

static void flush_partial(void) {
	if (partial_len > 0) {
		partial[partial_len] = 0;
		if (cfg_log != NULL) {
			cfg_log(partial);
		}
		partial_len = 0;
	}
}

// Relaye le stderr disponible, ligne par ligne, et en garde la fin dans `tail`.
static void pump_stderr(void) {
	if (sidecar == NULL) {
		return;
	}
	size_t n;
	bool   truncated;
	char  *chunk = studio_proc_take_stderr(sidecar, &n, &truncated);
	if (chunk == NULL) {
		return;
	}
	if (n >= SG_TAIL_SIZE) {
		memcpy(tail, chunk + n - SG_TAIL_SIZE + 1, SG_TAIL_SIZE - 1);
		tail_len = SG_TAIL_SIZE - 1;
	}
	else {
		if (tail_len + n >= SG_TAIL_SIZE) {
			size_t drop = tail_len + n - (SG_TAIL_SIZE - 1);
			memmove(tail, tail + drop, tail_len - drop);
			tail_len -= drop;
		}
		memcpy(tail + tail_len, chunk, n);
		tail_len += n;
	}
	tail[tail_len] = 0;
	for (size_t i = 0; i < n; i++) {
		if (chunk[i] == '\n') {
			flush_partial();
		}
		else if (chunk[i] != '\r') {
			if (partial_len == SG_LOG_LINE_MAX - 1) {
				flush_partial();
			}
			partial[partial_len++] = chunk[i];
		}
	}
	if (truncated) {
		sg_log("(sidecar stderr truncated)");
	}
	free(chunk);
}

// Dernières lignes du stderr, sur une ligne, pour un message d'erreur : "…" si rien.
static const char *tail_excerpt(char *buf, size_t size) {
	const char *start = tail;
	if (tail_len > 600) {
		start = tail + tail_len - 600;
		const char *nl = strchr(start, '\n');
		if (nl != NULL && nl[1] != 0) {
			start = nl + 1;
		}
	}
	size_t k = 0;
	for (const char *c = start; *c && k + 1 < size; c++) {
		buf[k++] = *c == '\n' || *c == '\r' ? ' ' : *c;
	}
	while (k > 0 && buf[k - 1] == ' ') {
		k--;
	}
	buf[k] = 0;
	return k > 0 ? buf : "no output on stderr";
}

// Réponse d'erreur synthétisée par le superviseur. id < 0 : null.
static char *synth(int id, const char *code, const char *format, ...) {
	char    message[1600];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	char *quoted = studio_json_quote(message);
	if (quoted == NULL) {
		return NULL;
	}
	char id_text[16] = "null";
	if (id >= 0) {
		snprintf(id_text, sizeof(id_text), "%d", id);
	}
	size_t size = strlen(quoted) + strlen(code) + 64;
	char  *reply = malloc(size);
	if (reply != NULL) {
		snprintf(reply, size, "{\"id\":%s,\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":%s}}", id_text, code, quoted);
	}
	free(quoted);
	return reply;
}

static void set_hello(char *line) {
	state_lock();
	free(hello);
	hello = line;
	state_unlock();
}

// Libère le sidecar, qui doit être arrêté ou tué.
static void release_sidecar(void) {
	pump_stderr();
	flush_partial();
	if (studio_proc_exited(sidecar)) {
		int code = studio_proc_exit_code(sidecar), sig = studio_proc_signal(sidecar);
		if (sig != 0) {
			sg_log("sidecar stopped (signal %d)", sig);
		}
		else {
			sg_log("sidecar stopped (exit code %d)", code);
		}
	}
	studio_proc_free(sidecar);
	sidecar = NULL;
	set_hello(NULL);
}

// Arrêt demandé : EOF sur stdin, le sidecar sort seul ; sinon tué au bout de grace_ms.
static void stop_sidecar(int grace_ms) {
	if (sidecar == NULL) {
		return;
	}
	studio_proc_set_cancel(sidecar, NULL);
	studio_proc_close_stdin(sidecar);
	if (!studio_proc_wait(sidecar, grace_ms)) {
		sg_log("sidecar did not exit within %d ms, killing it", grace_ms);
		studio_proc_kill(sidecar, 500);
	}
	release_sidecar();
}

static void kill_sidecar(const char *reason) {
	if (sidecar == NULL) {
		return;
	}
	sg_log("killing the sidecar: %s", reason);
	studio_proc_set_cancel(sidecar, NULL);
	studio_proc_kill(sidecar, 500);
	release_sidecar();
}

static bool lines_push(sg_line_t **list, char *text) {
	sg_line_t *line = malloc(sizeof(sg_line_t));
	if (line == NULL) {
		return false;
	}
	line->text = text;
	line->next = NULL;
	while (*list != NULL) {
		list = &(*list)->next;
	}
	*list = line;
	return true;
}

static char *lines_pop(sg_line_t **list) {
	sg_line_t *line = *list;
	if (line == NULL) {
		return NULL;
	}
	char *text = line->text;
	*list      = line->next;
	free(line);
	return text;
}

static void lines_clear(sg_line_t **list) {
	char *text;
	while ((text = lines_pop(list)) != NULL) {
		free(text);
	}
}

// Sous call, en début et en fin d'appel : une requête ou une réponse d'un autre appel n'a plus de destinataire.
static void host_reset(void) {
	state_lock();
	lines_clear(&host_requests);
	lines_clear(&host_replies);
	state_unlock();
	host_waiting = 0;
}

// Sous call : écrit au sidecar les réponses que l'app a données depuis la dernière tranche.
static void write_host_replies(void) {
	state_lock();
	sg_line_t *list = host_replies;
	host_replies    = NULL;
	state_unlock();
	char *text;
	while ((text = lines_pop(&list)) != NULL) {
		if (!studio_proc_write(sidecar, text, strlen(text), SG_WRITE_MS)) {
			sg_log("could not write a host reply to the sidecar");
		}
		host_waiting = host_waiting > 0 ? host_waiting - 1 : 0;
		free(text);
	}
}

static bool is_cancelled(volatile int *cancel) {
	return cancel != NULL && *cancel != 0;
}

static bool sidecar_exited(void) {
	return studio_proc_wait(sidecar, 0); // sans attendre (et même si l'annulation est demandée : l'état est lu d'abord)
}

// Sous call : le sidecar tourne (parti de lui-même, il est libéré ; le prochain appel le relancera).
static bool sidecar_running(void) {
	if (sidecar != NULL && sidecar_exited()) {
		sg_log("sidecar exited on its own");
		release_sidecar();
	}
	return sidecar != NULL && !closed;
}

// Lance le sidecar s'il ne tourne pas, jusqu'à la poignée de main. NULL si prêt, sinon la réponse d'erreur pour `id`.
static char *ensure_sidecar(int id, volatile int *cancel) {
	if (closed) {
		return synth(id, "sidecar_failed", "ShotGrid is unavailable: ArmorPaint is exiting");
	}
	if (sidecar != NULL && sidecar_exited()) {
		sg_log("sidecar exited on its own, restarting it");
		release_sidecar();
	}
	if (sidecar != NULL) {
		return NULL;
	}
	if (cfg_argv == NULL || cfg_argv[0] == NULL) {
		return synth(id, "sidecar_failed", "ShotGrid sidecar not configured");
	}
	char               err[512];
	studio_proc_opts_t opts = {.env = (const char *const *)cfg_env, .max_output = SG_MAX_OUTPUT};
	tail_len                = 0;
	tail[0]                 = 0;
	sidecar                 = studio_proc_spawn((const char *const *)cfg_argv, &opts, err, sizeof(err));
	if (sidecar == NULL) {
		return synth(id, "sidecar_failed", "cannot start the ShotGrid sidecar (%s): %s", cfg_argv[0], err);
	}
	sg_log("sidecar starting: %s %s", cfg_argv[0], cfg_argv[1] != NULL ? cfg_argv[1] : "");
	studio_proc_set_cancel(sidecar, cancel);
	int64_t deadline = now_ms() + cfg_hello_ms;
	while (true) {
		int64_t left = deadline - now_ms();
		char   *line = studio_proc_read_line(sidecar, left <= 0 ? 0 : left < SG_SLICE_MS ? (int)left : SG_SLICE_MS);
		pump_stderr();
		if (line == NULL) {
			char buf[700];
			if (is_cancelled(cancel)) {
				kill_sidecar("cancelled during startup");
				return synth(id, "cancelled", "cancelled while starting the ShotGrid sidecar");
			}
			if (sidecar_exited()) {
				int code = studio_proc_exit_code(sidecar);
				release_sidecar();
				return synth(id, "sidecar_failed", "the ShotGrid sidecar exited during startup (exit code %d): %s", code,
				             tail_excerpt(buf, sizeof(buf)));
			}
			if (left <= 0) {
				kill_sidecar("no handshake");
				return synth(id, "sidecar_failed", "the ShotGrid sidecar did not answer within %d s: %s", cfg_hello_ms / 1000,
				             tail_excerpt(buf, sizeof(buf)));
			}
			continue;
		}
		if (studio_json_int(studio_json_find(line, "/id"), -1) != 0) {
			sg_log("ignored before handshake: %.200s", line);
			free(line);
			continue;
		}
		if (!studio_json_truthy(studio_json_find(line, "/ok"))) {
			char *message = studio_json_text(studio_json_find(line, "/error/message"));
			char *reply   = synth(id, "sidecar_failed", "%s", message != NULL ? message : line);
			free(message);
			free(line);
			stop_sidecar(SG_EXIT_GRACE_MS);
			return reply;
		}
		long long protocol = studio_json_int(studio_json_find(line, "/data/protocol"), -1);
		if (protocol != 1) {
			free(line);
			stop_sidecar(SG_EXIT_GRACE_MS);
			return synth(id, "sidecar_failed", "ShotGrid sidecar protocol %lld, ArmorPaint expects 1 (mismatched install?)",
			             protocol);
		}
		char *core  = studio_json_text(studio_json_find(line, "/data/toolkit/core"));
		char *error = studio_json_text(studio_json_find(line, "/data/toolkit/error"));
		if (studio_json_truthy(studio_json_find(line, "/data/toolkit/available"))) {
			sg_log("sidecar ready, Toolkit core %s", core != NULL ? core : "?");
		}
		else {
			sg_log("sidecar ready, Toolkit unavailable: %s", error != NULL ? error : "?");
		}
		free(core);
		free(error);
		set_hello(line);
		return NULL;
	}
}

// ---- Joignabilité du site (lot C5, voir studio_sg.h) ------------------------------------------------------------------

// Écrits sous call et state (network_set), lus sous l'un ou l'autre.
static int   net_state;
static char *net_reason;
static int   net_serial;

static void network_set(int state, char *reason) {
	state_lock();
	bool changed = state != net_state;
	if (changed) {
		net_state = state;
		net_serial++;
	}
	if (state != STUDIO_SG_NET_OFFLINE || reason != NULL) {
		free(net_reason);
		net_reason = reason;
		reason     = NULL;
	}
	state_unlock();
	free(reason);
	if (changed) {
		if (state == STUDIO_SG_NET_OFFLINE) {
			sg_log("ShotGrid site unreachable: %s", net_reason != NULL ? net_reason : "?");
		}
		else {
			sg_log(state == STUDIO_SG_NET_ONLINE ? "ShotGrid site reachable" : "ShotGrid site state unknown");
		}
		if (cfg_wake != NULL) {
			cfg_wake();
		}
	}
}

// Ce que la réponse d'un appel dit du site : son champ "sg", ou une erreur sg_unreachable. Sous call.
static void observe_network(const char *reply) {
	if (reply == NULL) {
		return;
	}
	studio_json_value_t online = studio_json_find(reply, "/sg/online");
	if (online.type == STUDIO_JSON_BOOL) {
		bool up = studio_json_truthy(online);
		network_set(up ? STUDIO_SG_NET_ONLINE : STUDIO_SG_NET_OFFLINE, up ? NULL : studio_json_text(studio_json_find(reply, "/sg/reason")));
		return;
	}
	char *code = studio_json_text(studio_json_find(reply, "/error/code"));
	if (code != NULL && strcmp(code, "sg_unreachable") == 0) {
		network_set(STUDIO_SG_NET_OFFLINE, studio_json_text(studio_json_find(reply, "/error/message")));
	}
	free(code);
}

int studio_sg_network(char **reason) {
	state_lock();
	int state = net_state;
	if (reason != NULL) {
		*reason = state == STUDIO_SG_NET_OFFLINE && net_reason != NULL ? strdup(net_reason) : NULL;
	}
	state_unlock();
	return state;
}

int studio_sg_network_serial(void) {
	state_lock();
	int serial = net_serial;
	state_unlock();
	return serial;
}

// Un appel, sous call. op_json : op déjà en littéral JSON ; args_json : objet JSON sur une ligne.
static char *call_locked(const char *op, const char *op_json, const char *args_json, int timeout_ms, volatile int *cancel) {
	int    id   = next_id++;
	size_t size = strlen(op_json) + strlen(args_json) + 48;
	char  *line = malloc(size);
	if (line == NULL) {
		return NULL;
	}
	int     len      = snprintf(line, size, "{\"id\":%d,\"op\":%s,\"args\":%s}\n", id, op_json, args_json);
	int     timeout  = timeout_ms == 0 ? cfg_call_ms : timeout_ms;
	int64_t deadline = timeout < 0 ? -1 : now_ms() + timeout;
	char   *reply    = NULL;
	char    buf[700];
	host_reset();

	for (int attempt = 0; attempt < 2 && reply == NULL; attempt++) {
		if ((reply = ensure_sidecar(id, cancel)) != NULL) {
			break;
		}
		studio_proc_set_cancel(sidecar, cancel);
		if (!studio_proc_write(sidecar, line, (size_t)len, SG_WRITE_MS)) {
			pump_stderr();
			if (is_cancelled(cancel)) {
				kill_sidecar("cancelled");
				reply = synth(id, "cancelled", "%s cancelled", op);
			}
			else if (sidecar_exited()) {
				// Parti avant d'avoir lu la requête : la rejouer ne peut rien doubler.
				release_sidecar();
				if (attempt == 1) {
					reply = synth(id, "sidecar_died", "the ShotGrid sidecar stopped before reading %s: %s", op,
					              tail_excerpt(buf, sizeof(buf)));
				}
			}
			else {
				kill_sidecar("request not read");
				reply = synth(id, "sidecar_failed", "the ShotGrid sidecar did not read the %s request", op);
			}
			continue;
		}

		bool restart = false;
		while (reply == NULL && !restart) {
			write_host_replies();
			// Une requête host attend l'app : tranches courtes, pour lui écrire la réponse dès qu'elle est donnée, et
			// réveil à chacune (un réveil isolé peut se perdre dans la boucle de l'app).
			if (host_waiting > 0 && cfg_wake != NULL) {
				cfg_wake();
			}
			int     max   = host_waiting > 0 ? SG_HOST_SLICE_MS : SG_SLICE_MS;
			int64_t left  = deadline < 0 ? max : deadline - now_ms();
			int     slice = left <= 0 ? 0 : left < max ? (int)left : max;
			int64_t t0    = now_ms();
			char   *got   = studio_proc_read_line(sidecar, slice);
			pump_stderr();
			if (got != NULL && studio_json_int(studio_json_find(got, "/host"), -1) > 0) {
				state_lock();
				bool queued = lines_push(&host_requests, got);
				state_unlock();
				if (queued) {
					host_waiting++;
					if (cfg_wake != NULL) {
						cfg_wake();
					}
				}
				else {
					free(got);
				}
				continue;
			}
			if (got != NULL) {
				if (studio_json_int(studio_json_find(got, "/id"), -1) != id) {
					sg_log("discarding unexpected reply: %.200s", got);
					free(got);
					continue;
				}
				if (!studio_json_truthy(studio_json_find(got, "/restart"))) {
					reply = got;
					continue;
				}
				// Le sidecar s'arrête juste après cette réponse ; relancé, il rejoue la requête (une fois).
				char *why = studio_json_text(studio_json_find(got, "/error/message"));
				sg_log("sidecar restarting: %s", why != NULL ? why : "");
				free(why);
				stop_sidecar(SG_EXIT_GRACE_MS);
				if (attempt == 1) {
					reply = got;
				}
				else {
					free(got);
					restart = true;
				}
				continue;
			}
			if (is_cancelled(cancel)) {
				kill_sidecar("cancelled");
				reply = synth(id, "cancelled", "%s cancelled", op);
			}
			else if (sidecar_exited() || (slice > 0 && now_ms() - t0 < slice / 2 && !studio_proc_wait(sidecar, 1000))) {
				// Arrêté, ou stdout fermé / réponse démesurée (read_line rend la main aussitôt) : fini dans tous les cas.
				int  code   = studio_proc_exited(sidecar) ? studio_proc_exit_code(sidecar) : -1;
				bool exited = studio_proc_exited(sidecar);
				if (!exited) {
					kill_sidecar("unreadable reply");
				}
				else {
					release_sidecar();
				}
				reply = exited ? synth(id, "sidecar_died", "the ShotGrid sidecar stopped during %s (exit code %d): %s", op,
				                       code, tail_excerpt(buf, sizeof(buf)))
				               : synth(id, "sidecar_failed", "unreadable reply from the ShotGrid sidecar to %s", op);
			}
			else if (deadline >= 0 && left <= 0) {
				kill_sidecar("timeout");
				reply = synth(id, "timeout", "no reply to %s within %d s", op, timeout / 1000);
			}
		}
	}
	if (sidecar != NULL) {
		studio_proc_set_cancel(sidecar, NULL);
	}
	host_reset();
	free(line);
	observe_network(reply);
	return reply;
}

// Vérifie op et args, les met sous la forme de la requête. NULL si valides, sinon la réponse d'erreur.
static char *prepare(const char *op, const char *args_json, char **op_json, char **args_line) {
	*op_json   = NULL;
	*args_line = NULL;
	if (op == NULL || op[0] == 0) {
		return synth(-1, "bad_request", "empty op");
	}
	const char *a = args_json != NULL ? args_json : "";
	while (*a == ' ' || *a == '\t' || *a == '\n' || *a == '\r') {
		a++;
	}
	if (*a == 0) {
		a = "{}";
	}
	studio_json_type_t type;
	const char        *end = studio_json_skip(a, &type);
	while (end != NULL && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
		end++;
	}
	if (type != STUDIO_JSON_OBJECT || end == NULL || *end != 0) {
		return synth(-1, "bad_request", "%s: args must be a JSON object, got: %.200s", op, a);
	}
	*op_json   = studio_json_quote(op);
	*args_line = strdup(a);
	if (*op_json == NULL || *args_line == NULL) {
		free(*op_json);
		free(*args_line);
		return synth(-1, "internal", "out of memory");
	}
	// Une requête tient sur une ligne : hors des chaînes, un saut de ligne n'est qu'un espace (dans une chaîne, il est
	// forcément échappé, sinon args aurait été refusé).
	for (char *c = *args_line; *c; c++) {
		if (*c == '\n' || *c == '\r') {
			*c = ' ';
		}
	}
	return NULL;
}

char *studio_sg_call(const char *op, const char *args_json, int timeout_ms, volatile int *cancel) {
	char *op_json, *args_line;
	char *reply = prepare(op, args_json, &op_json, &args_line);
	if (reply != NULL) {
		return reply;
	}
	call_lock();
	reply = call_locked(op, op_json, args_line, timeout_ms, cancel);
	call_unlock();
	free(op_json);
	free(args_line);
	return reply;
}

void studio_sg_configure(const studio_sg_config_t *config) {
	call_lock();
	stop_sidecar(SG_EXIT_GRACE_MS);
	strv_free(cfg_argv);
	strv_free(cfg_env);
	cfg_argv     = strv_copy(config->argv);
	cfg_env      = strv_copy(config->env);
	cfg_hello_ms = config->hello_timeout_ms > 0 ? config->hello_timeout_ms : SG_DEFAULT_HELLO_MS;
	cfg_call_ms  = config->call_timeout_ms != 0 ? config->call_timeout_ms : SG_DEFAULT_CALL_MS;
	cfg_log      = config->log;
	cfg_wake     = config->wake;
	network_set(STUDIO_SG_NET_UNKNOWN, NULL);
	call_unlock();
}

void studio_sg_stop(void) {
	call_lock();
	stop_sidecar(SG_EXIT_GRACE_MS);
	call_unlock();
}

char *studio_sg_hello(void) {
	state_lock();
	char *copy = hello != NULL ? strdup(hello) : NULL;
	state_unlock();
	return copy;
}

char *studio_sg_host_take(void) {
	state_lock();
	char *text = lines_pop(&host_requests);
	state_unlock();
	return text;
}

void studio_sg_host_reply(const char *reply) {
	if (reply == NULL || studio_json_int(studio_json_find(reply, "/host"), -1) <= 0) {
		sg_log("host reply without \"host\" ignored: %.200s", reply != NULL ? reply : "(null)");
		return;
	}
	size_t len  = strlen(reply);
	char  *line = malloc(len + 2);
	if (line == NULL) {
		return;
	}
	for (size_t i = 0; i < len; i++) { // une ligne : hors des chaînes JSON, un saut de ligne n'est qu'un espace
		line[i] = reply[i] == '\n' || reply[i] == '\r' ? ' ' : reply[i];
	}
	line[len]     = '\n';
	line[len + 1] = 0;
	state_lock();
	if (!lines_push(&host_replies, line)) {
		free(line);
	}
	state_unlock();
}

// ---- Jobs -------------------------------------------------------------------------------------------------------------

typedef struct sg_job {
	int          id;
	char        *op;
	char        *op_json;
	char        *args_line;
	int          timeout_ms;
	volatile int cancel;
	int          state;      // sous state
	int64_t      not_before; // en file : pas avant (now_ms), 0 : dès que possible
	bool         released; // idem : libéré pendant qu'il tournait, le thread le libérera
	bool         if_running; // studio_sg_start_if_running
	char        *result;
	char       **texts; // rendus par studio_sg_get, vivent jusqu'au release
	int          text_count;
} sg_job_t;

static sg_job_t *jobs[SG_MAX_JOBS];
static int       job_next_id = 1;
static bool      worker_started;
static bool      quitting;

static void job_free(sg_job_t *j) {
	free(j->op);
	free(j->op_json);
	free(j->args_line);
	free(j->result);
	for (int i = 0; i < j->text_count; i++) {
		free(j->texts[i]);
	}
	free(j->texts);
	free(j);
}

// Sous state.
static sg_job_t *job_find(int id) {
	for (int i = 0; i < SG_MAX_JOBS; i++) {
		if (jobs[i] != NULL && jobs[i]->id == id) {
			return jobs[i];
		}
	}
	return NULL;
}

// Exécute le job (sur le thread appelant) et le marque fini.
static void job_execute(sg_job_t *j) {
	char *reply = NULL;
	if (j->result != NULL) {
		reply     = j->result; // refusé à la création
		j->result = NULL;
	}
	else if (j->cancel) {
		reply = synth(-1, "cancelled", "%s cancelled before it started", j->op);
	}
	else {
		call_lock();
		if (j->if_running && !sidecar_running()) {
			reply = synth(-1, "sidecar_stopped", "the ShotGrid sidecar is not running: %s not sent", j->op);
		}
		else {
			reply = call_locked(j->op, j->op_json, j->args_line, j->timeout_ms, &j->cancel);
		}
		call_unlock();
	}
	state_lock();
	j->result   = reply;
	j->state    = STUDIO_SG_DONE;
	bool orphan = j->released;
	state_unlock();
	if (orphan) {
		job_free(j);
	}
	else if (cfg_wake != NULL) {
		cfg_wake();
	}
}

// Crée et inscrit un job dans l'état donné ; en file, il ne part pas avant delay_ms. 0 si la table est pleine.
static int job_create(const char *op, const char *args_json, int timeout_ms, int state, int delay_ms, bool if_running, sg_job_t **out) {
	sg_job_t *j = calloc(1, sizeof(sg_job_t));
	if (j == NULL) {
		return 0;
	}
	j->op         = strdup(op != NULL ? op : "");
	j->timeout_ms = timeout_ms;
	j->state      = state;
	j->not_before = delay_ms > 0 ? now_ms() + delay_ms : 0;
	j->if_running = if_running;
	j->result     = prepare(op, args_json, &j->op_json, &j->args_line); // erreur : le job la rendra telle quelle
	if (j->op == NULL) {
		job_free(j);
		return 0;
	}
	state_lock();
	int slot = -1;
	for (int i = 0; i < SG_MAX_JOBS && slot < 0; i++) {
		if (jobs[i] == NULL) {
			slot = i;
		}
	}
	if (slot >= 0) {
		j->id      = job_next_id++;
		jobs[slot] = j;
	}
	state_unlock();
	if (slot < 0) {
		sg_log("job refused, %d jobs open (studio_sg_release missing?)", SG_MAX_JOBS);
		job_free(j);
		return 0;
	}
	*out = j;
	return j->id;
}

#ifndef _WIN32
// Attente du thread des jobs (sous state) : un signal, ou l'heure `due` (now_ms) du prochain job différé. Par tranches
// d'une seconde au plus : pthread_cond_timedwait suit l'horloge murale, qui peut sauter.
static void worker_wait(int64_t due) {
	if (due == 0) {
		pthread_cond_wait(&state_cond, &state_mutex);
		return;
	}
	int64_t wait = due - now_ms();
	if (wait <= 0) {
		return;
	}
	if (wait > 1000) {
		wait = 1000;
	}
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += (time_t)(wait / 1000);
	ts.tv_nsec += (long)(wait % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec += 1;
		ts.tv_nsec -= 1000000000L;
	}
	pthread_cond_timedwait(&state_cond, &state_mutex, &ts);
}

static void *worker(void *arg) {
	(void)arg;
	state_lock();
	while (!quitting) {
		sg_job_t *next = NULL;
		int64_t   now  = now_ms();
		int64_t   due  = 0; // le plus proche des jobs différés qui ne sont pas encore dus
		for (int i = 0; i < SG_MAX_JOBS; i++) {
			sg_job_t *j = jobs[i];
			if (j == NULL || j->state != STUDIO_SG_QUEUED) {
				continue;
			}
			if (j->not_before > now && !j->cancel) {
				due = due == 0 || j->not_before < due ? j->not_before : due;
			}
			else if (next == NULL || j->id < next->id) {
				next = j;
			}
		}
		if (next == NULL) {
			worker_wait(due);
			continue;
		}
		next->state = STUDIO_SG_RUNNING;
		state_unlock();
		job_execute(next);
		state_lock();
	}
	state_unlock();
	return NULL;
}
#endif

int studio_sg_start(const char *op, const char *args_json, int timeout_ms) {
	return studio_sg_start_after(op, args_json, timeout_ms, 0);
}

static int start_queued(const char *op, const char *args_json, int timeout_ms, int delay_ms, bool if_running) {
	sg_job_t *j;
	int       id = job_create(op, args_json, timeout_ms, STUDIO_SG_QUEUED, delay_ms, if_running, &j);
	if (id == 0) {
		return 0;
	}
#ifdef _WIN32
	state_lock();
	j->state = STUDIO_SG_RUNNING;
	state_unlock();
	job_execute(j);
#else
	state_lock();
	bool start = !worker_started;
	if (start) {
		pthread_attr_t attr;
		pthread_t      thread;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		worker_started = pthread_create(&thread, &attr, worker, NULL) == 0;
		pthread_attr_destroy(&attr);
	}
	bool threaded = worker_started;
	if (!threaded) {
		j->state = STUDIO_SG_RUNNING; // pas de thread disponible : on exécute ici (sans délai) plutôt que de perdre le job
	}
	pthread_cond_signal(&state_cond);
	state_unlock();
	if (!threaded) {
		job_execute(j);
	}
#endif
	return id;
}

int studio_sg_start_after(const char *op, const char *args_json, int timeout_ms, int delay_ms) {
	return start_queued(op, args_json, timeout_ms, delay_ms, false);
}

int studio_sg_start_if_running(const char *op, const char *args_json, int timeout_ms, int delay_ms) {
	return start_queued(op, args_json, timeout_ms, delay_ms, true);
}

int studio_sg_run(const char *op, const char *args_json, int timeout_ms) {
	sg_job_t *j;
	int       id = job_create(op, args_json, timeout_ms, STUDIO_SG_RUNNING, 0, false, &j);
	if (id != 0) {
		job_execute(j);
	}
	return id;
}

int studio_sg_state(int job) {
	state_lock();
	sg_job_t *j     = job_find(job);
	int       state = j != NULL ? j->state : STUDIO_SG_UNKNOWN;
	state_unlock();
	return state;
}

void studio_sg_cancel(int job) {
	state_lock();
	sg_job_t *j = job_find(job);
	if (j != NULL) {
		j->cancel = 1;
	}
#ifndef _WIN32
	pthread_cond_signal(&state_cond); // un job différé annulé part tout de suite (réponse "cancelled")
#endif
	state_unlock();
}

void studio_sg_release(int job) {
	state_lock();
	sg_job_t *j   = NULL;
	bool      now = false;
	for (int i = 0; i < SG_MAX_JOBS; i++) {
		if (jobs[i] != NULL && jobs[i]->id == job) {
			j       = jobs[i];
			jobs[i] = NULL;
		}
	}
	if (j != NULL) {
		now = j->state != STUDIO_SG_RUNNING; // en file : retiré de la table, le thread ne le verra plus
		if (!now) {
			j->released = true;
			j->cancel   = 1;
		}
	}
	state_unlock();
	if (now) {
		job_free(j);
	}
}

const char *studio_sg_result(int job) {
	state_lock();
	sg_job_t   *j      = job_find(job);
	const char *result = j != NULL && j->state == STUDIO_SG_DONE ? j->result : NULL;
	state_unlock();
	return result;
}

bool studio_sg_ok(int job) {
	return studio_json_truthy(studio_json_find(studio_sg_result(job), "/ok"));
}

const char *studio_sg_get(int job, const char *pointer) {
	const char *text = "";
	state_lock();
	sg_job_t *j = job_find(job);
	if (j != NULL && j->state == STUDIO_SG_DONE) {
		char  *value = studio_json_text(studio_json_find(j->result, pointer));
		char **grown = value != NULL ? realloc(j->texts, sizeof(char *) * (size_t)(j->text_count + 1)) : NULL;
		if (grown != NULL) {
			j->texts                  = grown;
			j->texts[j->text_count++] = value;
			text                      = value;
		}
		else {
			free(value);
		}
	}
	state_unlock();
	return text;
}

int studio_sg_length(int job, const char *pointer) {
	return studio_json_length(studio_json_find(studio_sg_result(job), pointer));
}

long long studio_sg_int(int job, const char *pointer, long long fallback) {
	return studio_json_int(studio_json_find(studio_sg_result(job), pointer), fallback);
}

void studio_sg_shutdown(void) {
	closed = 1;
	state_lock();
	quitting = true;
	for (int i = 0; i < SG_MAX_JOBS; i++) {
		if (jobs[i] != NULL) {
			jobs[i]->cancel = 1;
		}
	}
#ifndef _WIN32
	pthread_cond_broadcast(&state_cond);
#endif
	state_unlock();
#ifdef _WIN32
	stop_sidecar(1000);
#else
	// L'appel en cours voit l'annulation en 20 ms et tue le sidecar en 500 ms au plus. Si le verrou ne se libère pas,
	// on part sans y toucher : le sidecar recevra EOF à la fermeture de nos descripteurs et sortira seul.
	for (int waited = 0; waited < 3000; waited += 10) {
		if (pthread_mutex_trylock(&call_mutex) == 0) {
			stop_sidecar(1000);
			call_unlock();
			return;
		}
		struct timespec ts = {0, 10 * 1000000};
		nanosleep(&ts, NULL);
	}
#endif
}
