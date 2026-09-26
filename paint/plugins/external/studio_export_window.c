#include "studio_export_window.h"
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
#include "studio.h"
#include "studio_export.h"
#include "studio_json.h"
#include "studio_ocio.h"
#include "studio_preset.h"
#include "studio_proc.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#endif

// Cœur ArmorPaint (paint/sources/globals.h, functions.h) et iron
extern context_t       *g_context;
extern project_t       *g_project;
extern config_t        *g_config;
extern string_array_t  *box_export_files;
extern i32              box_export_preset_index;
extern export_preset_t *box_export_preset;
extern i32              base_bits;
extern char            *ui_files_filename;
void                    box_export_fetch_presets(void);
char                   *project_filepath_get(void);
i32                     config_get_texture_res_x(void);
i32                     config_get_texture_res_y(void);
void                    render_path_paint_dilate(bool base, bool nor_pack);
i32                     util_mesh_udim_tile(char *name);
void                    console_toast(char *s);
void                    ui_box_show_message(char *title, char *text, bool copyable);
char                   *path_data(void);
void                    iron_delay_idle_sleep(void); // base/sources/iron.h

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

// La fenêtre en cours. Le thread de lecture possède proc ; l'interface ne touche qu'aux files, sous le verrou.
static struct {
	studio_proc_t *proc;
	bool           started;  // un thread de lecture tourne ou n'a pas encore été rejoint
	bool           exited;   // le processus est terminé (posé par le thread)
	bool           answered; // la fenêtre a demandé son état : elle s'est lancée
	char          *request;  // requête relevée, traitée à la frame suivante
	queue_t        in, out, log;
	char           tail[TAIL_SIZE];
	char          *python;
	int            exit_code;
#ifndef _WIN32
	pthread_t       thread;
	pthread_mutex_t mutex;
#endif
} win = {
#ifndef _WIN32
    .mutex = PTHREAD_MUTEX_INITIALIZER,
#endif
};

static void lock(void) {
#ifndef _WIN32
	pthread_mutex_lock(&win.mutex);
#endif
}

static void unlock(void) {
#ifndef _WIN32
	pthread_mutex_unlock(&win.mutex);
#endif
}

static void send_line(const char *text) {
	char *line = malloc(strlen(text) + 2);
	if (line == NULL) {
		return;
	}
	sprintf(line, "%s\n", text);
	lock();
	queue_push(&win.out, line);
	unlock();
}

// ---- Thread de lecture -----------------------------------------------------------------------------------------------

static void keep_tail(const char *text) {
	size_t have = strlen(win.tail), add = strlen(text);
	if (add >= TAIL_SIZE - 1) {
		memcpy(win.tail, text + add - (TAIL_SIZE - 1), TAIL_SIZE - 1);
		win.tail[TAIL_SIZE - 1] = 0;
		return;
	}
	if (have + add >= TAIL_SIZE) {
		size_t drop = have + add - (TAIL_SIZE - 1);
		memmove(win.tail, win.tail + drop, have - drop + 1);
		have -= drop;
	}
	memcpy(win.tail + have, text, add + 1);
}

#ifndef _WIN32
static void relay_stderr(studio_proc_t *p) {
	char *err = studio_proc_take_stderr(p, NULL, NULL);
	if (err != NULL && err[0] != 0) {
		lock();
		keep_tail(err);
		queue_push(&win.log, err);
		unlock();
		return;
	}
	free(err);
}

static void *reader(void *data) {
	studio_proc_t *p = data;
	for (;;) {
		lock();
		char *out = queue_pop(&win.out);
		unlock();
		while (out != NULL) {
			studio_proc_write(p, out, strlen(out), WRITE_MS);
			free(out);
			lock();
			out = queue_pop(&win.out);
			unlock();
		}
		char *line = studio_proc_read_line(p, READ_SLICE_MS);
		relay_stderr(p);
		if (line != NULL) {
			lock();
			queue_push(&win.in, line);
			unlock();
			iron_delay_idle_sleep(); // sans entrée, iron saute les frames, et avec elles window_update
			continue;
		}
		if (studio_proc_wait(p, 0)) {
			while ((line = studio_proc_read_line(p, 0)) != NULL) {
				lock();
				queue_push(&win.in, line);
				unlock();
			}
			relay_stderr(p);
			break;
		}
	}
	lock();
	win.exit_code = studio_proc_exit_code(p);
	win.exited    = true;
	unlock();
	iron_delay_idle_sleep();
	return NULL;
}
#endif

// ---- Réponses --------------------------------------------------------------------------------------------------------

typedef struct {
	char  *data;
	size_t len, cap;
} text_t;

static void put(text_t *b, const char *format, ...) {
	va_list args;
	va_start(args, format);
	int n = vsnprintf(NULL, 0, format, args);
	va_end(args);
	if (n < 0) {
		return;
	}
	if (b->len + (size_t)n + 1 > b->cap) {
		size_t cap  = (b->cap + (size_t)n + 1) * 2;
		char  *grow = realloc(b->data, cap);
		if (grow == NULL) {
			return;
		}
		b->data = grow;
		b->cap  = cap;
	}
	va_start(args, format);
	vsnprintf(b->data + b->len, b->cap - b->len, format, args);
	va_end(args);
	b->len += (size_t)n;
}

static void put_quoted(text_t *b, const char *s) {
	char *q = studio_json_quote(s != NULL ? s : "");
	put(b, "%s", q != NULL ? q : "\"\"");
	free(q);
}

static void reply(int id, const char *data_json) {
	char *line = malloc(strlen(data_json) + 64);
	if (line != NULL) {
		sprintf(line, "{\"id\":%d,\"ok\":true,\"data\":%s}", id, data_json);
		send_line(line);
		free(line);
	}
}

static void reply_error(int id, const char *code, const char *message) {
	text_t b = {0};
	put(&b, "{\"id\":%d,\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":", id, code);
	put_quoted(&b, message);
	put(&b, "}}");
	if (b.data != NULL) {
		send_line(b.data);
	}
	free(b.data);
}

// ---- État d'ArmorPaint -----------------------------------------------------------------------------------------------

// Preset studio de l'export courant d'ArmorPaint (choisi au dernier export), NULL si c'est un preset upstream.
static const char *current_preset(void) {
	if (box_export_files == NULL || box_export_preset_index < 0 || box_export_preset_index >= (i32)box_export_files->length) {
		return NULL;
	}
	const char *name = box_export_files->buffer[box_export_preset_index];
	return studio_export_is_studio_preset(name) ? name : NULL;
}

static char *dir_of(const char *path) {
	const char *slash = strrchr(path, '/');
#ifdef _WIN32
	const char *back = strrchr(path, '\\');
	if (back != NULL && (slash == NULL || back > slash)) {
		slash = back;
	}
#endif
	return slash != NULL ? strndup(path, (size_t)(slash - path)) : strdup("");
}

static char *state_json(void) {
	text_t      b    = {0};
	const char *file = project_filepath_get();
	put(&b, "{\"project\":{\"name\":");
	put_quoted(&b, ui_files_filename != NULL && ui_files_filename[0] != 0 ? ui_files_filename : "untitled");
	put(&b, ",\"path\":");
	put_quoted(&b, file);
	put(&b, ",\"width\":%d,\"height\":%d,\"bits\":%d,", config_get_texture_res_x(), config_get_texture_res_y(),
	    base_bits == TEXTURE_BITS_BITS32 ? 32 : base_bits == TEXTURE_BITS_BITS16 ? 16 : 8);
	// Objets du projet, et tuiles UDIM : un maillage à tuiles est découpé en un objet par tuile ("tete.1001").
	string_array_t        *objects = (string_array_t *)any_array_create_from_raw((void *[]){}, 0);
	string_array_t        *tiles   = (string_array_t *)any_array_create_from_raw((void *[]){}, 0);
	mesh_object_t_array_t *paint   = g_project->_->paint_objects;
	for (int i = 0; paint != NULL && i < (int)paint->length; ++i) {
		char *name = paint->buffer[i]->base->name;
		int   len  = (int)strlen(name);
		if (util_mesh_udim_tile(name) >= 0 && len > 5) {
			char *tile = substring(name, len - 4, len);
			if (string_array_index_of(tiles, tile) == -1) {
				any_array_push((any_array_t *)tiles, tile);
			}
			name = substring(name, 0, len - 5);
		}
		if (string_array_index_of(objects, name) == -1) {
			any_array_push((any_array_t *)objects, name);
		}
	}
	put(&b, "\"objects\":[");
	for (int i = 0; i < (int)objects->length; ++i) {
		put(&b, i > 0 ? "," : "");
		put_quoted(&b, objects->buffer[i]);
	}
	put(&b, "],\"tiles\":[");
	for (int i = 0; i < (int)tiles->length; ++i) {
		put(&b, i > 0 ? "," : "");
		put_quoted(&b, tiles->buffer[i]);
	}
	put(&b, "]},");

	put(&b, "\"presets\":[");
	for (int i = 0; i < studio_export_preset_count(); ++i) {
		const char      *name = studio_export_preset_name(i);
		const char      *path = studio_export_preset_path(name);
		char             err[512];
		studio_preset_t *p = studio_preset_load(path, name, err, sizeof(err));
		put(&b, "%s{\"name\":", i > 0 ? "," : "");
		put_quoted(&b, name);
		put(&b, ",\"description\":");
		put_quoted(&b, p != NULL ? (p->description != NULL ? p->description : "") : err);
		put(&b, ",\"valid\":%s,\"shipped\":%s,\"path\":", p != NULL ? "true" : "false",
		    studio_export_preset_shipped(name) ? "true" : "false");
		put_quoted(&b, path);
		put(&b, "}");
		studio_preset_free(p);
	}
	put(&b, "],\"preset\":");
	const char *preset = current_preset();
	put_quoted(&b, preset != NULL ? preset : "");

	put(&b, ",\"ocio\":{\"ready\":%s,\"config\":", studio_ocio_ready() ? "true" : "false");
	put_quoted(&b, studio_ocio_ready() ? studio_ocio_config_name() : "");
	put(&b, ",\"status\":");
	put_quoted(&b, studio_ocio_status());
	put(&b, "},\"color_spaces\":[");
	int n = studio_ocio_ready() ? studio_ocio_colorspace_count() : 0;
	for (int i = 0, first = 1; i < n; ++i) {
		const char *cs = studio_ocio_colorspace_name(i);
		if (cs != NULL && studio_ocio_is_data(cs) != 1) { // les espaces de donnée ne s'appliquent pas aux couleurs
			put(&b, first ? "" : ",");
			put_quoted(&b, cs);
			first = 0;
		}
	}
	put(&b, "],\"color_space\":");
	put_quoted(&b, studio_export_color_space());

	// Dossier du dernier export, sinon celui du projet enregistré.
	char *dir = g_context->texture_export_path != NULL && g_context->texture_export_path[0] != 0 &&
	                    strcmp(g_context->texture_export_path, "/") != 0
	                ? strdup(g_context->texture_export_path)
	                : dir_of(file != NULL ? file : "");
	put(&b, ",\"dir\":");
	put_quoted(&b, dir);
	free(dir);
	put(&b, ",\"mode\":%d,\"padding\":%s}", (int)g_context->layers_export, g_context->export_padding ? "true" : "false");
	return b.data;
}

// ---- Requêtes --------------------------------------------------------------------------------------------------------

static char *arg_text(const char *req, const char *name) {
	studio_json_value_t v = studio_json_find(req, string_tmp("/args/%s", name));
	char               *s = v.type == STUDIO_JSON_STRING ? studio_json_text(v) : NULL;
	return s != NULL ? s : strdup("");
}

// Marge : comme la boîte d'export d'ArmorPaint (box_export_tab_export_textures_run), chaque calque est dilaté.
static void dilate_layers(void) {
	i32           radius      = g_config->dilate_radius;
	slot_layer_t *layer       = g_context->layer;
	g_config->dilate_radius   = 256;
	slot_layer_t_array_t *all = g_project->_->layers;
	for (i32 i = 0; i < (i32)all->length; ++i) {
		g_context->layer = all->buffer[i];
		render_path_paint_dilate(true, true);
	}
	g_context->layer        = layer;
	g_config->dilate_radius = radius;
}

// L'export abouti devient l'export courant d'ArmorPaint : Ctrl+E le refait à l'identique.
static void adopt(const char *dir, const char *preset, int mode, bool padding) {
	g_context->texture_export_path = string_copy((char *)dir);
	g_context->layers_export       = (export_mode_t)mode;
	g_context->export_padding      = padding;
	box_export_fetch_presets();
	int index = string_array_index_of(box_export_files, (char *)preset);
	if (index >= 0) {
		box_export_preset_index = index;
		box_export_preset       = NULL; // relu pour ce preset
	}
}

// args.skip : [[texture, variant, tuile], ...] -> lignes de studio_export_set_skip.
static char *skip_keys(const char *req) {
	studio_json_value_t list = studio_json_find(req, "/args/skip");
	int                 n    = list.type == STUDIO_JSON_ARRAY ? studio_json_length(list) : 0;
	text_t              b    = {0};
	for (int i = 0; i < n; ++i) {
		char *part[3];
		for (int k = 0; k < 3; ++k) {
			studio_json_value_t v = studio_json_find(req, string_tmp("/args/skip/%d/%d", i, k));
			part[k]               = v.type == STUDIO_JSON_STRING ? studio_json_text(v) : NULL;
		}
		if (part[0] != NULL && part[1] != NULL && part[2] != NULL) {
			put(&b, "%s\t%s\t%s\n", part[0], part[1], part[2]);
		}
		for (int k = 0; k < 3; ++k) {
			free(part[k]);
		}
	}
	return b.data != NULL ? b.data : strdup("");
}

static void run_request(int id, const char *req, bool preflight_only) {
	char *preset  = arg_text(req, "preset");
	char *space   = arg_text(req, "color_space");
	char *dir     = arg_text(req, "dir");
	int   mode    = (int)studio_json_int(studio_json_find(req, "/args/mode"), EXPORT_MODE_VISIBLE);
	bool  padding = studio_json_truthy(studio_json_find(req, "/args/padding"));
	if (mode < EXPORT_MODE_VISIBLE || mode > EXPORT_MODE_PER_UDIM_TILE) {
		mode = EXPORT_MODE_VISIBLE;
	}
	if (!studio_export_is_studio_preset(preset)) {
		reply_error(id, "bad_request", string_tmp("not a studio preset: %s", preset));
	}
	else if (dir[0] == 0) {
		reply_error(id, "no_destination", "choose a destination folder");
	}
	else {
		// L'espace et les fichiers choisis ne servent qu'à cette requête, sauf si l'export aboutit (Cancel ne change
		// rien).
		char *previous      = strdup(studio_export_color_space());
		char *previous_skip = strdup(studio_export_skip());
		char *skip          = skip_keys(req);
		studio_export_set_color_space(space);
		studio_export_set_skip(skip);
		free(skip);
		if (!preflight_only && padding) {
			dilate_layers();
		}
		bool ok = studio_export_request(dir, preset, mode, preflight_only);
		if (ok && !preflight_only) {
			adopt(dir, preset, mode, padding);
		}
		else {
			studio_export_set_color_space(previous);
			studio_export_set_skip(previous_skip);
		}
		free(previous);
		free(previous_skip);
		const char *report = studio_export_last_report();
		text_t      b      = {0};
		put(&b, "{\"ok\":%s,\"report\":%s,\"error\":", ok ? "true" : "false", report[0] != 0 ? report : "null");
		put_quoted(&b, studio_export_last_error());
		put(&b, "}");
		reply(id, b.data != NULL ? b.data : "null");
		free(b.data);
	}
	free(preset);
	free(space);
	free(dir);
}

// Même moment que l'export de la boîte d'ArmorPaint (sys_notify_on_next_frame) : hors du dessin de l'interface.
static void handle_request(void *data) {
	(void)data;
	char               *req = win.request;
	int                 id  = (int)studio_json_int(studio_json_find(req, "/id"), 0);
	studio_json_value_t v   = studio_json_find(req, "/op");
	char               *op  = v.type == STUDIO_JSON_STRING ? studio_json_text(v) : NULL;
	if (op == NULL) {
		reply_error(id, "bad_request", "a request is {\"id\":<int>,\"op\":<string>,\"args\":<object>}");
	}
	else if (strcmp(op, "state") == 0) {
		win.answered = true;
		char *state  = state_json();
		reply(id, state != NULL ? state : "null");
		free(state);
	}
	else if (strcmp(op, "preflight") == 0 || strcmp(op, "export") == 0) {
		run_request(id, req, strcmp(op, "preflight") == 0);
	}
	else {
		reply_error(id, "bad_request", string_tmp("unknown operation: %s", op));
	}
	free(op);
	free(req);
	win.request = NULL;
}

// ---- Frame -----------------------------------------------------------------------------------------------------------

static void log_lines(char *text) {
	for (char *line = strtok(text, "\n"); line != NULL; line = strtok(NULL, "\n")) {
		iron_log("studio: export window: %s", line);
	}
}

static void finish_window(void) {
#ifndef _WIN32
	pthread_join(win.thread, NULL);
#endif
	studio_proc_free(win.proc);
	win.proc    = NULL;
	win.started = false;
	if (!win.answered) {
		// Lancement raté (PySide6 absent...) : l'artiste doit savoir pourquoi rien ne s'ouvre.
		const char *tail = win.tail;
		const char *cut  = strlen(tail) > 1200 ? tail + strlen(tail) - 1200 : tail;
		ui_box_show_message("Export window", string_tmp("The export window could not start (%s, exit code %d). It needs Python 3 "
		                                                "with PySide6: set STUDIO_PYTHON.\n\n%s",
		                                                win.python, win.exit_code, cut),
		                    true);
	}
	else {
		iron_log("studio: export window closed");
	}
	lock();
	queue_clear(&win.in);
	queue_clear(&win.out);
	unlock();
}

static void window_update(void *data) {
	(void)data;
	if (!win.started) {
		return;
	}
	lock();
	char *log     = queue_pop(&win.log);
	bool  exited  = win.exited;
	char *request = win.request == NULL ? queue_pop(&win.in) : NULL;
	bool  pending = win.in.head != NULL;
	unlock();
	while (log != NULL) {
		log_lines(log);
		free(log);
		lock();
		log = queue_pop(&win.log);
		unlock();
	}
	if (request != NULL) {
		if (studio_json_find(request, "/id").type == STUDIO_JSON_NUMBER) {
			win.request = request;
			sys_notify_on_next_frame(handle_request, NULL);
		}
		else {
			iron_log("studio: export window: ignored: %.200s", request);
			free(request);
		}
	}
	if (exited && win.request == NULL && !pending && request == NULL) {
		finish_window();
	}
}

// ---- Ouverture -------------------------------------------------------------------------------------------------------

static const char *env_or(const char *name, const char *fallback) {
	const char *v = getenv(name);
	return v != NULL && v[0] != 0 ? v : fallback;
}

bool studio_export_window_is_open(void) {
	return win.started;
}

void studio_export_window_show(void) {
	if (win.started) {
		send_line(string_tmp("{\"event\":\"raise\",\"mode\":%d}", (int)g_context->layers_export));
		return;
	}
#ifdef _WIN32
	ui_box_show_message("Export window", "The studio export window is not available on Windows yet.", false);
#else
	if (studio_export_preset_count() == 0) {
		ui_box_show_message("Export window", "No valid studio export preset: see the console.", false);
		return;
	}
	free(win.python);
	win.python = strdup(env_or("STUDIO_PYTHON", env_or("STUDIO_SG_PYTHON", "python3")));
	char script[4096];
	snprintf(script, sizeof(script), "%s", env_or("STUDIO_EXPORT_WINDOW", string_tmp("%sstudio_export_window.py", path_data())));
	const char *argv[] = {win.python, script, NULL};
	// Pas de __pycache__ dans data/ : le paquet de l'app est signé (macOS), et ce dossier peut être en lecture seule.
	// STUDIO_UI_SCALE : l'échelle de l'interface d'ArmorPaint, que la fenêtre reprend.
	char scale[64];
	snprintf(scale, sizeof(scale), "STUDIO_UI_SCALE=%.2f", g_config->window_scale);
	const char        *env[] = {"PYTHONDONTWRITEBYTECODE=1", scale, NULL};
	studio_proc_opts_t opts  = {.env = env};
	char               err[512];
	win.proc = studio_proc_spawn(argv, &opts, err, sizeof(err));
	if (win.proc == NULL) {
		ui_box_show_message("Export window", string_tmp("The export window could not start: %s. It needs Python 3 with PySide6: "
		                                                "set STUDIO_PYTHON.",
		                                                err),
		                    true);
		return;
	}
	win.exited    = false;
	win.answered  = false;
	win.tail[0]   = 0;
	win.exit_code = 0;
	if (pthread_create(&win.thread, NULL, reader, win.proc) != 0) {
		studio_proc_free(win.proc);
		win.proc = NULL;
		ui_box_show_message("Export window", "The export window could not start: no thread available.", false);
		return;
	}
	win.started = true;
	iron_log("studio: export window: %s %s", win.python, script);
	console_toast("Opening the export window");
#endif
}

// ---- Scripting (minic) -----------------------------------------------------------------------------------------------

#ifdef WITH_EVAL

#include "minic.h"

static minic_val_t mn_studio_export_window_show(minic_val_t *a, int n) {
	(void)a, (void)n;
	studio_export_window_show();
	return minic_val_int(0);
}

static minic_val_t mn_studio_export_window_is_open(minic_val_t *a, int n) {
	(void)a, (void)n;
	return minic_val_int(studio_export_window_is_open() ? 1 : 0);
}

#endif

void studio_export_window_init(void) {
	sys_notify_on_update(window_update, NULL);
#ifdef WITH_EVAL
	minic_register("studio_export_window_show", "v()", mn_studio_export_window_show);
	minic_register("studio_export_window_is_open", "i()", mn_studio_export_window_is_open);
#endif
}
