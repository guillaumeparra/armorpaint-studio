#include "studio_window.h"
#include "iron_array.h"
#include "iron_draw.h"
#include "iron_file.h"
#include "iron_gpu.h"
#include "iron_map.h"
#include "iron_obj.h"
#include "iron_string.h"
#include "iron_system.h"
#include "iron_ui.h"
#include "engine.h"
#include "../../sources/types.h"
#include "studio_json.h"
#include "studio_proc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#endif

// Cœur ArmorPaint (paint/sources/globals.h, functions.h) et iron
extern config_t *g_config;
void             console_toast(char *s);
void             ui_box_show_message(char *title, char *text, bool copyable);
char            *path_data(void);
void             iron_delay_idle_sleep(void); // base/sources/iron.h

#define READ_SLICE_MS 50    // attente d'une ligne de la fenêtre entre deux envois
#define WRITE_MS      10000 // une réponse (rapport) que la fenêtre ne lit pas : abandon
#define TAIL_SIZE     4096  // fin du stderr, pour expliquer un échec de lancement

// ---- Messages entre le thread de lecture et celui de l'interface ------------------------------------------------------

typedef struct message {
	struct message *next;
	char           *text;
} message_t;

typedef struct {
	message_t *head, *tail;
} queue_t;

static void queue_push(queue_t *q, char *text) {
	message_t *m = malloc(sizeof(message_t));
	if (m == NULL) {
		free(text);
		return;
	}
	m->next = NULL;
	m->text = text;
	if (q->tail != NULL) {
		q->tail->next = m;
	}
	else {
		q->head = m;
	}
	q->tail = m;
}

static char *queue_pop(queue_t *q) {
	message_t *m = q->head;
	if (m == NULL) {
		return NULL;
	}
	q->head = m->next;
	if (q->head == NULL) {
		q->tail = NULL;
	}
	char *text = m->text;
	free(m);
	return text;
}

static void queue_clear(queue_t *q) {
	char *text;
	while ((text = queue_pop(q)) != NULL) {
		free(text);
	}
}

// Le thread de lecture possède proc ; l'interface ne touche qu'aux files, sous le verrou.
struct studio_window {
	studio_window_desc_t desc;
	studio_proc_t       *proc;
	bool                 started;  // un thread de lecture tourne ou n'a pas encore été rejoint
	bool                 exited;   // le processus est terminé (posé par le thread)
	bool                 answered; // la fenêtre a fait une requête : elle s'est lancée
	char                *request;  // requête relevée, traitée à la frame suivante
	queue_t              in, out, log;
	char                 tail[TAIL_SIZE];
	char                *python;
	int                  exit_code;
#ifndef _WIN32
	pthread_t       thread;
	pthread_mutex_t mutex;
#endif
};

static void lock(studio_window_t *w) {
#ifndef _WIN32
	pthread_mutex_lock(&w->mutex);
#endif
}

static void unlock(studio_window_t *w) {
#ifndef _WIN32
	pthread_mutex_unlock(&w->mutex);
#endif
}

void studio_window_send(studio_window_t *w, const char *text) {
	char *line = malloc(strlen(text) + 2);
	if (line == NULL) {
		return;
	}
	sprintf(line, "%s\n", text);
	lock(w);
	queue_push(&w->out, line);
	unlock(w);
}

// ---- Thread de lecture -----------------------------------------------------------------------------------------------

static void keep_tail(studio_window_t *w, const char *text) {
	size_t have = strlen(w->tail), add = strlen(text);
	if (add >= TAIL_SIZE - 1) {
		memcpy(w->tail, text + add - (TAIL_SIZE - 1), TAIL_SIZE - 1);
		w->tail[TAIL_SIZE - 1] = 0;
		return;
	}
	if (have + add >= TAIL_SIZE) {
		size_t drop = have + add - (TAIL_SIZE - 1);
		memmove(w->tail, w->tail + drop, have - drop + 1);
		have -= drop;
	}
	memcpy(w->tail + have, text, add + 1);
}

#ifndef _WIN32
static void relay_stderr(studio_window_t *w, studio_proc_t *p) {
	char *err = studio_proc_take_stderr(p, NULL, NULL);
	if (err != NULL && err[0] != 0) {
		lock(w);
		keep_tail(w, err);
		queue_push(&w->log, err);
		unlock(w);
		return;
	}
	free(err);
}

static void *reader(void *data) {
	studio_window_t *w = data;
	studio_proc_t   *p = w->proc;
	for (;;) {
		lock(w);
		char *out = queue_pop(&w->out);
		unlock(w);
		while (out != NULL) {
			studio_proc_write(p, out, strlen(out), WRITE_MS);
			free(out);
			lock(w);
			out = queue_pop(&w->out);
			unlock(w);
		}
		char *line = studio_proc_read_line(p, READ_SLICE_MS);
		relay_stderr(w, p);
		if (line != NULL) {
			lock(w);
			queue_push(&w->in, line);
			unlock(w);
			iron_delay_idle_sleep(); // sans entrée, iron saute les frames, et avec elles window_update
			continue;
		}
		if (studio_proc_wait(p, 0)) {
			while ((line = studio_proc_read_line(p, 0)) != NULL) {
				lock(w);
				queue_push(&w->in, line);
				unlock(w);
			}
			relay_stderr(w, p);
			break;
		}
	}
	lock(w);
	w->exit_code = studio_proc_exit_code(p);
	w->exited    = true;
	unlock(w);
	iron_delay_idle_sleep();
	return NULL;
}
#endif

// ---- Réponses --------------------------------------------------------------------------------------------------------

void studio_window_reply(studio_window_t *w, int id, const char *data_json) {
	char *line = malloc(strlen(data_json) + 64);
	if (line != NULL) {
		sprintf(line, "{\"id\":%d,\"ok\":true,\"data\":%s}", id, data_json);
		studio_window_send(w, line);
		free(line);
	}
}

void studio_window_reply_error(studio_window_t *w, int id, const char *code, const char *message) {
	char *quoted = studio_json_quote(message != NULL ? message : "");
	char *line   = malloc(strlen(code) + (quoted != NULL ? strlen(quoted) : 2) + 96);
	if (line != NULL) {
		sprintf(line, "{\"id\":%d,\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":%s}}", id, code, quoted != NULL ? quoted : "\"\"");
		studio_window_send(w, line);
		free(line);
	}
	free(quoted);
}

// ---- Frame -----------------------------------------------------------------------------------------------------------

static void handle_request(void *data) {
	studio_window_t    *w   = data;
	char               *req = w->request;
	int                 id  = (int)studio_json_int(studio_json_find(req, "/id"), 0);
	studio_json_value_t v   = studio_json_find(req, "/op");
	char               *op  = v.type == STUDIO_JSON_STRING ? studio_json_text(v) : NULL;
	w->answered             = true;
	if (op == NULL) {
		studio_window_reply_error(w, id, "bad_request", "a request is {\"id\":<int>,\"op\":<string>,\"args\":<object>}");
	}
	else {
		w->desc.on_request(w, id, op, req);
	}
	free(op);
	free(req);
	w->request = NULL;
}

static void log_lines(studio_window_t *w, char *text) {
	for (char *line = strtok(text, "\n"); line != NULL; line = strtok(NULL, "\n")) {
		iron_log("studio: %s: %s", w->desc.name, line);
	}
}

static void finish_window(studio_window_t *w) {
#ifndef _WIN32
	pthread_join(w->thread, NULL);
#endif
	studio_proc_free(w->proc);
	w->proc    = NULL;
	w->started = false;
	if (!w->answered) {
		// Lancement raté (PySide6 absent...) : l'artiste doit savoir pourquoi rien ne s'ouvre.
		const char *tail = w->tail;
		const char *cut  = strlen(tail) > 1200 ? tail + strlen(tail) - 1200 : tail;
		ui_box_show_message((char *)w->desc.title,
		                    string_tmp("The %s could not start (%s, exit code %d). It needs Python 3 with PySide6: set STUDIO_PYTHON.\n\n%s",
		                               w->desc.name, w->python, w->exit_code, cut),
		                    true);
	}
	else {
		iron_log("studio: %s closed", w->desc.name);
	}
	lock(w);
	queue_clear(&w->in);
	queue_clear(&w->out);
	unlock(w);
	if (w->desc.on_closed != NULL) {
		w->desc.on_closed(w);
	}
}

static void window_update(void *data) {
	studio_window_t *w = data;
	if (!w->started) {
		return;
	}
	lock(w);
	char *log     = queue_pop(&w->log);
	bool  exited  = w->exited;
	char *request = w->request == NULL ? queue_pop(&w->in) : NULL;
	bool  pending = w->in.head != NULL;
	unlock(w);
	while (log != NULL) {
		log_lines(w, log);
		free(log);
		lock(w);
		log = queue_pop(&w->log);
		unlock(w);
	}
	if (request != NULL) {
		if (studio_json_find(request, "/id").type == STUDIO_JSON_NUMBER) {
			w->request = request;
			sys_notify_on_next_frame(handle_request, w);
		}
		else {
			iron_log("studio: %s: ignored: %.200s", w->desc.name, request);
			free(request);
		}
	}
	if (exited && w->request == NULL && !pending && request == NULL) {
		finish_window(w);
	}
}

// ---- Ouverture -------------------------------------------------------------------------------------------------------

static const char *env_or(const char *name, const char *fallback) {
	const char *v = getenv(name);
	return v != NULL && v[0] != 0 ? v : fallback;
}

studio_window_t *studio_window_create(const studio_window_desc_t *desc) {
	studio_window_t *w = calloc(1, sizeof(studio_window_t));
	if (w == NULL) {
		return NULL;
	}
	w->desc = *desc;
#ifndef _WIN32
	pthread_mutex_init(&w->mutex, NULL);
#endif
	sys_notify_on_update(window_update, w);
	return w;
}

bool studio_window_is_open(studio_window_t *w) {
	return w != NULL && w->started;
}

bool studio_window_open(studio_window_t *w, const char *const *env) {
	if (w == NULL || w->started) {
		return false;
	}
#ifdef _WIN32
	ui_box_show_message((char *)w->desc.title, string_tmp("The studio %s is not available on Windows yet.", w->desc.name), false);
	return false;
#else
	free(w->python);
	w->python = strdup(env_or("STUDIO_PYTHON", env_or("STUDIO_SG_PYTHON", "python3")));
	char script[4096];
	snprintf(script, sizeof(script), "%s",
	         w->desc.script_env != NULL ? env_or(w->desc.script_env, string_tmp("%s%s", path_data(), w->desc.script))
	                                    : string_tmp("%s%s", path_data(), w->desc.script));
	const char *argv[] = {w->python, script, NULL};
	// Pas de __pycache__ dans data/ : le paquet de l'app est signé (macOS), et ce dossier peut être en lecture seule.
	// STUDIO_UI_SCALE : l'échelle de l'interface d'ArmorPaint, que la fenêtre reprend.
	char scale[64];
	snprintf(scale, sizeof(scale), "STUDIO_UI_SCALE=%.2f", g_config->window_scale);
	const char *all[34] = {"PYTHONDONTWRITEBYTECODE=1", scale};
	int         n       = 2;
	for (int i = 0; env != NULL && env[i] != NULL && n < 33; i++) {
		all[n++] = env[i];
	}
	all[n]                  = NULL;
	studio_proc_opts_t opts = {.env = all};
	char               err[512];
	w->proc = studio_proc_spawn(argv, &opts, err, sizeof(err));
	if (w->proc == NULL) {
		ui_box_show_message((char *)w->desc.title,
		                    string_tmp("The %s could not start: %s. It needs Python 3 with PySide6: set STUDIO_PYTHON.", w->desc.name, err), true);
		return false;
	}
	w->exited    = false;
	w->answered  = false;
	w->tail[0]   = 0;
	w->exit_code = 0;
	if (pthread_create(&w->thread, NULL, reader, w) != 0) {
		studio_proc_free(w->proc);
		w->proc = NULL;
		ui_box_show_message((char *)w->desc.title, string_tmp("The %s could not start: no thread available.", w->desc.name), false);
		return false;
	}
	w->started = true;
	iron_log("studio: %s: %s %s", w->desc.name, w->python, script);
	return true;
#endif
}
