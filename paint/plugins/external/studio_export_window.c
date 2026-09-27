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
#include "studio_atlas.h"
#include "studio_export_naming.h"
#include "studio_export.h"
#include "studio_json.h"
#include "studio_naming.h"
#include "studio_ocio.h"
#include "studio_preset.h"
#include "studio_proc.h"
#include "studio_workfiles.h"
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
extern i32              base_bits;
extern char            *ui_files_filename;
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

// Textures d'un preset pour le tableau des maps : ce que le preset fixe, et les profondeurs que le format accepte.
static void put_textures(text_t *b, const studio_preset_t *p) {
	put(b, "[");
	for (int i = 0; i < p->texture_count; ++i) {
		const studio_preset_texture_t *t = &p->textures[i];
		int                            bits[3];
		int                            n = studio_preset_bits_options(t->format, bits);
		put(b, "%s{\"name\":", i > 0 ? "," : "");
		put_quoted(b, t->name);
		put(b, ",\"suffix\":");
		put_quoted(b, t->suffix);
		put(b, ",\"format\":\"%s\",\"bits\":%d,\"bits_options\":[", studio_format_name(t->format), t->bits);
		for (int k = 0; k < n; ++k) {
			put(b, "%s%d", k > 0 ? "," : "", bits[k]);
		}
		put(b, "],\"resolution\":%d,\"channels\":%d,\"naming\":", t->resolution, t->channels);
		if (t->naming != NULL) { // nom propre à la texture : le nom de l'export ne s'y applique pas
			put_quoted(b, t->naming);
		}
		else {
			put(b, "null");
		}
		put(b, ",\"color_space\":");
		put_quoted(b, studio_preset_color(t));
		put(b, ",\"color\":%s}",
		    studio_preset_texture_is_color(t, studio_ocio_ready() ? studio_ocio_is_data : NULL) ? "true" : "false");
	}
	put(b, "]");
}

// {"name","description","valid","error","naming","textures"} d'un preset lu (p) ou refusé (err).
static void put_preset_info(text_t *b, const char *name, const studio_preset_t *p, const char *err) {
	put(b, "{\"name\":");
	put_quoted(b, name);
	put(b, ",\"description\":");
	put_quoted(b, p != NULL && p->description != NULL ? p->description : "");
	put(b, ",\"valid\":%s,\"error\":", p != NULL ? "true" : "false");
	put_quoted(b, p != NULL ? "" : err);
	put(b, ",\"naming\":");
	put_quoted(b, p != NULL ? p->naming : "");
	put(b, ",\"textures\":");
	if (p != NULL) {
		put_textures(b, p);
	}
	else {
		put(b, "[]");
	}
}

// Texte d'un membre de l'objet à `base` (malloc) ; "" si absent.
static char *member_text(const char *json, const char *base, const char *name) {
	studio_json_value_t v = studio_json_find(json, string_tmp("%s/%s", base, name));
	char               *s = v.type == STUDIO_JSON_STRING ? studio_json_text(v) : NULL;
	return s != NULL ? s : strdup("");
}

// Texte JSON brut d'un membre (objet, tableau...), malloc ; "" si absent.
static char *member_raw(const char *json, const char *base, const char *name) {
	studio_json_value_t v = studio_json_find(json, string_tmp("%s/%s", base, name));
	return v.type == STUDIO_JSON_OBJECT || v.type == STUDIO_JSON_ARRAY ? strndup(v.start, v.len) : strdup("");
}

// Texte JSON brut d'une valeur des arguments (objet, tableau...), malloc ; "" si absente.
static char *arg_raw(const char *req, const char *name) {
	return member_raw(req, "/args", name);
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

// Dossier du dernier export, sinon celui du projet enregistré (malloc).
static char *current_dir(void) {
	const char *last = g_context->texture_export_path;
	if (last != NULL && last[0] != 0 && strcmp(last, "/") != 0) {
		return strdup(last);
	}
	const char *file = project_filepath_get();
	return dir_of(file != NULL ? file : "");
}

// Presets donnés par la fenêtre (args.presets : [{"name", "preset": {...}}], ceux du sidecar du projet) : lus ici,
// comme à l'export, pour leurs textures et leurs erreurs.
static void put_given_presets(text_t *b, const char *req) {
	studio_json_value_t list = studio_json_find(req, "/args/presets");
	int                 n    = list.type == STUDIO_JSON_ARRAY ? studio_json_length(list) : 0;
	put(b, "[");
	for (int i = 0; i < n; ++i) {
		studio_json_value_t name = studio_json_find(req, string_tmp("/args/presets/%d/name", i));
		studio_json_value_t v    = studio_json_find(req, string_tmp("/args/presets/%d/preset", i));
		char               *text = name.type == STUDIO_JSON_STRING ? studio_json_text(name) : strdup("");
		char               *json = v.type == STUDIO_JSON_OBJECT ? strndup(v.start, v.len) : NULL;
		char                err[512];
		snprintf(err, sizeof(err), "preset object expected");
		studio_preset_t *p = json != NULL && text != NULL ? studio_preset_parse(json, text, err, sizeof(err)) : NULL;
		put(b, i > 0 ? "," : "");
		put_preset_info(b, text != NULL ? text : "", p, err);
		put(b, "}");
		studio_preset_free(p);
		free(json);
		free(text);
	}
	put(b, "]");
}

static char *state_json(const char *req) {
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
	// Jeux de textures (atlas) du projet, et nommage : variables, modèle de la session, template ShotGrid qui l'emporte.
	char *atlases = studio_atlas_json();
	put(&b, "\"atlases\":%s,\"naming\":", atlases != NULL ? atlases : "null");
	free(atlases);
	put_quoted(&b, studio_export_naming());
	put(&b, ",\"naming_template\":");
	if (studio_export_naming_template() != NULL) {
		put_quoted(&b, studio_export_naming_template());
	}
	else {
		put(&b, "null");
	}
	char *sg_naming = studio_export_naming_json(); // lot B6 : templates possibles, version du fichier de travail
	put(&b, ",\"sg_naming\":%s", sg_naming);
	free(sg_naming);
	put(&b, ",\"naming_variables\":[");
	for (int i = 0; i < studio_naming_variable_count; ++i) {
		put(&b, i > 0 ? ",{\"name\":" : "{\"name\":");
		put_quoted(&b, studio_naming_variables[i].name);
		put(&b, ",\"description\":");
		put_quoted(&b, studio_naming_variables[i].description);
		put(&b, "}");
	}
	put(&b, "],");

	put(&b, "\"presets\":[");
	int count = studio_export_preset_count(); // relit les dossiers : les noms valent jusqu'à la relecture suivante
	for (int i = 0; i < count; ++i) {
		const char      *name = studio_export_preset_name(i);
		const char      *path = studio_export_preset_path(name);
		char             err[512];
		studio_preset_t *p = studio_preset_load(path, name, err, sizeof(err));
		put(&b, i > 0 ? "," : "");
		put_preset_info(&b, name, p, err);
		put(&b, ",\"shipped\":%s,\"path\":", studio_export_preset_shipped(name) ? "true" : "false");
		put_quoted(&b, path);
		put(&b, "}");
		studio_preset_free(p);
	}
	put(&b, "],\"project_presets\":");
	put_given_presets(&b, req);
	// Export courant d'ArmorPaint dans cette session (repris si le projet n'a pas encore de sidecar).
	put(&b, ",\"preset\":");
	put_quoted(&b, studio_export_preset());
	studio_json_type_t type      = STUDIO_JSON_INVALID;
	const char        *overrides = studio_export_overrides();
	bool               object    = overrides[0] != 0 && studio_json_skip(overrides, &type) != NULL && type == STUDIO_JSON_OBJECT;
	put(&b, ",\"preset_project\":%s,\"overrides\":%s,\"skip\":[", studio_export_preset_json()[0] != 0 ? "true" : "false",
	    object ? overrides : "null");
	char *skip = strdup(studio_export_skip());
	int   k    = 0;
	for (char *line = strtok(skip, "\n"); line != NULL; line = strtok(NULL, "\n")) {
		char *tab1 = strchr(line, '\t');
		char *tab2 = tab1 != NULL ? strchr(tab1 + 1, '\t') : NULL;
		if (tab2 == NULL) {
			continue;
		}
		*tab1 = *tab2 = 0;
		put(&b, "%s[", k++ > 0 ? "," : "");
		put_quoted(&b, line);
		put(&b, ",");
		put_quoted(&b, tab1 + 1);
		put(&b, ",");
		put_quoted(&b, tab2 + 1);
		put(&b, "]");
	}
	free(skip);
	put(&b, "]");
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

	char *dir = current_dir();
	put(&b, ",\"dir\":");
	put_quoted(&b, dir);
	free(dir);
	put(&b, ",\"mode\":%d,\"padding\":%s}", (int)g_context->layers_export, g_context->export_padding ? "true" : "false");
	return b.data;
}

// ---- Requêtes --------------------------------------------------------------------------------------------------------

static char *arg_text(const char *req, const char *name) {
	return member_text(req, "/args", name);
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

// L'export abouti devient l'export courant d'ArmorPaint : Ctrl+E le refait à l'identique (le preset, ses réglages, l'espace
// et les fichiers décochés restent ceux de la requête).
static void adopt(const char *dir, int mode, bool padding) {
	g_context->texture_export_path = string_copy((char *)dir);
	g_context->layers_export       = (export_mode_t)mode;
	g_context->export_padding      = padding;
}

// Choix de l'export courant, pour les rendre si la requête n'aboutit pas.
typedef struct {
	char *preset, *json, *source, *overrides, *naming, *color_space, *skip;
} selection_t;

static selection_t selection_save(void) {
	return (selection_t){strdup(studio_export_preset()),    strdup(studio_export_preset_json()),
	                     strdup(studio_export_preset_source()), strdup(studio_export_overrides()),
	                     strdup(studio_export_naming()),     strdup(studio_export_color_space()),
	                     strdup(studio_export_skip())};
}

static void selection_free(selection_t *s, bool restore) {
	if (restore) {
		studio_export_set_preset(s->preset, s->json, s->source);
		studio_export_set_overrides(s->overrides);
		studio_export_set_naming(s->naming);
		studio_export_set_color_space(s->color_space);
		studio_export_set_skip(s->skip);
	}
	free(s->preset);
	free(s->json);
	free(s->source);
	free(s->overrides);
	free(s->naming);
	free(s->color_space);
	free(s->skip);
}

// skip de l'objet à `base` : [[texture, variant, tuile], ...] -> lignes de studio_export_set_skip.
static char *skip_keys(const char *json, const char *base) {
	studio_json_value_t list = studio_json_find(json, string_tmp("%s/skip", base));
	int                 n    = list.type == STUDIO_JSON_ARRAY ? studio_json_length(list) : 0;
	text_t              b    = {0};
	for (int i = 0; i < n; ++i) {
		char *part[3];
		for (int k = 0; k < 3; ++k) {
			studio_json_value_t v = studio_json_find(json, string_tmp("%s/skip/%d/%d", base, i, k));
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

// Lignes de studio_export_set_skip, suivies de more (NULL : aucune), malloc.
static char *join_skip(const char *skip, const char *more) {
	text_t b = {0};
	put(&b, "%s", skip);
	if (skip[0] != 0 && skip[strlen(skip) - 1] != '\n') {
		put(&b, "\n");
	}
	put(&b, "%s", more != NULL ? more : "");
	return b.data != NULL ? b.data : strdup("");
}

static char *result_json(bool ok) {
	const char *report = studio_export_last_report();
	text_t      b      = {0};
	put(&b, "{\"ok\":%s,\"report\":%s,\"error\":", ok ? "true" : "false", report[0] != 0 ? report : "null");
	put_quoted(&b, studio_export_last_error());
	put(&b, "}");
	return b.data;
}

// Export ou préflight avec les choix de l'objet JSON à `base` (args d'une requête export). Les choix ne servent qu'à
// cet export, sauf si keep et que l'export aboutit : il devient l'export courant (Ctrl+E) ; Cancel ne change rien.
// more_skip : lignes de studio_export_set_skip ajoutées à celles des choix. Rend {"ok","report","error"} (malloc), ou
// NULL si les choix sont refusés (*code, message).
static char *run_choices(const char *json, const char *base, bool preflight_only, bool keep, const char *more_skip,
                         const char **code, char *message, size_t size) {
	char *preset      = member_text(json, base, "preset");
	char *preset_json = member_raw(json, base, "preset_json");
	char *source      = member_text(json, base, "preset_source");
	char *overrides   = member_raw(json, base, "overrides");
	char *naming      = member_text(json, base, "naming");
	char *space       = member_text(json, base, "color_space");
	char *dir         = member_text(json, base, "dir");
	int   mode        = (int)studio_json_int(studio_json_find(json, string_tmp("%s/mode", base)), EXPORT_MODE_VISIBLE);
	bool  padding     = studio_json_truthy(studio_json_find(json, string_tmp("%s/padding", base)));
	char *result      = NULL;
	if (mode < EXPORT_MODE_VISIBLE || mode > EXPORT_MODE_PER_UDIM_TILE) {
		mode = EXPORT_MODE_VISIBLE;
	}
	if (preset_json[0] == 0 && !studio_export_is_studio_preset(preset)) {
		*code = "bad_request";
		snprintf(message, size, "not a studio preset: %s", preset);
	}
	else if (dir[0] == 0) {
		*code = "no_destination";
		snprintf(message, size, "choose a destination folder");
	}
	else {
		selection_t   previous = selection_save();
		export_mode_t was      = g_context->layers_export;
		char         *keys     = skip_keys(json, base);
		char         *skip     = join_skip(keys, more_skip);
		studio_export_set_preset(preset, preset_json, source);
		studio_export_set_overrides(overrides);
		studio_export_set_naming(naming);
		studio_export_set_color_space(space);
		studio_export_set_skip(skip);
		free(keys);
		free(skip);
		if (!preflight_only && padding) {
			dilate_layers();
		}
		bool ok      = studio_export_request(dir, preset, mode, preflight_only);
		bool adopted = keep && ok && !preflight_only;
		if (adopted) {
			adopt(dir, mode, padding);
		}
		else {
			g_context->layers_export = was;
		}
		selection_free(&previous, !adopted);
		result = result_json(ok);
	}
	free(preset);
	free(preset_json);
	free(source);
	free(overrides);
	free(naming);
	free(space);
	free(dir);
	return result;
}

// L'export courant (Ctrl+E), sans le changer. more_skip comme run_choices.
static char *run_current(bool preflight_only, const char *more_skip, const char **code, char *message, size_t size) {
	const char *preset = studio_export_preset();
	if (preset[0] == 0) {
		*code = "no_preset";
		snprintf(message, size, "no studio export in this session: export the textures once (File > Export Textures)");
		return NULL;
	}
	if (studio_export_preset_json()[0] == 0 && !studio_export_is_studio_preset(preset)) {
		*code = "bad_request";
		snprintf(message, size, "not a studio preset: %s", preset);
		return NULL;
	}
	char       *dir      = current_dir();
	selection_t previous = selection_save();
	char       *skip     = join_skip(previous.skip, more_skip);
	studio_export_set_skip(skip);
	free(skip);
	if (!preflight_only && g_context->export_padding) {
		dilate_layers();
	}
	bool ok = studio_export_request(dir, previous.preset, (int)g_context->layers_export, preflight_only);
	selection_free(&previous, true);
	free(dir);
	return result_json(ok);
}

char *studio_export_run_choices(const char *choices, bool preflight_only, const char *more_skip) {
	const char *code = "internal";
	char        message[512];
	snprintf(message, sizeof(message), "out of memory");
	char *result = choices != NULL && choices[0] != 0
	                   ? run_choices(choices, "", preflight_only, false, more_skip, &code, message, sizeof(message))
	                   : run_current(preflight_only, more_skip, &code, message, sizeof(message));
	if (result == NULL) {
		text_t b = {0};
		put(&b, "{\"ok\":false,\"report\":null,\"code\":");
		put_quoted(&b, code);
		put(&b, ",\"error\":");
		put_quoted(&b, message);
		put(&b, "}");
		result = b.data;
	}
	return result;
}

static void run_request(int id, const char *req, bool preflight_only) {
	const char *code = NULL;
	char        message[512];
	char       *result = run_choices(req, "/args", preflight_only, true, NULL, &code, message, sizeof(message));
	if (result == NULL && code != NULL) {
		reply_error(id, code, message);
		return;
	}
	reply(id, result != NULL ? result : "null");
	free(result);
}

// args : {"names": [...], "objects": [...]} (studio_atlas_set) -> {"ok", "error", "atlases"} : les jeux de textures
// changent dans le projet (enregistrés avec lui), comme l'onglet Atlases de la boîte d'export d'ArmorPaint.
static void atlases_request(int id, const char *req) {
	studio_json_value_t args = studio_json_find(req, "/args");
	char               *json = args.type == STUDIO_JSON_OBJECT ? strndup(args.start, args.len) : strdup("");
	char                err[512];
	bool                ok      = studio_atlas_set(json, err, sizeof(err));
	char               *atlases = studio_atlas_json();
	text_t              b       = {0};
	put(&b, "{\"ok\":%s,\"error\":", ok ? "true" : "false");
	put_quoted(&b, ok ? "" : err);
	put(&b, ",\"atlases\":%s}", atlases != NULL ? atlases : "null");
	reply(id, b.data != NULL ? b.data : "null");
	free(b.data);
	free(atlases);
	free(json);
}

// args : {"name", "preset": {...}} -> {"name","description","valid","error","textures"} : un preset que la fenêtre va
// enregistrer dans le sidecar, vérifié comme à l'export.
static void inspect_request(int id, const char *req) {
	char            *name = arg_text(req, "name");
	char            *json = arg_raw(req, "preset");
	char             err[512];
	studio_preset_t *p = json[0] != 0 ? studio_preset_parse(json, name, err, sizeof(err)) : NULL;
	if (json[0] == 0) {
		snprintf(err, sizeof(err), "preset object expected");
	}
	text_t b = {0};
	put_preset_info(&b, name, p, err);
	put(&b, "}");
	reply(id, b.data != NULL ? b.data : "null");
	free(b.data);
	studio_preset_free(p);
	free(name);
	free(json);
}

// args : {"refresh": bool} -> studio_export_naming_json() : le nommage ShotGrid du projet, redemandé au sidecar avec
// refresh (template choisi dans la fenêtre, écrit dans le sidecar du projet).
static void naming_request(int id, const char *req) {
	if (studio_json_truthy(studio_json_find(req, "/args/refresh"))) {
		studio_export_naming_refresh();
	}
	char *json = studio_export_naming_json();
	reply(id, json);
	free(json);
}

// -> {"path", "sg_naming"} : le projet enregistré comme la version suivante de son fichier de travail (lot B6), ses
// textures iront dans cette version ; erreur version_up sinon.
static void version_up_request(int id) {
	char               *naming = studio_export_naming_json();
	studio_json_value_t v      = studio_json_find(naming, "/name");
	char               *name   = v.type == STUDIO_JSON_STRING ? studio_json_text(v) : NULL;
	char               *error  = NULL;
	char               *path   = name != NULL ? studio_workfiles_version_up(name, &error) : NULL;
	free(naming);
	if (path == NULL) {
		reply_error(id, "version_up", error != NULL ? error : "the work file has no name field (ShotGrid template)");
	}
	else {
		naming   = studio_export_naming_json(); // modèle de la nouvelle version, attendu
		text_t b = {0};
		put(&b, "{\"path\":");
		put_quoted(&b, path);
		put(&b, ",\"sg_naming\":%s}", naming);
		reply(id, b.data);
		free(b.data);
		free(naming);
	}
	free(path);
	free(error);
	free(name);
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
		char *state  = state_json(req);
		reply(id, state != NULL ? state : "null");
		free(state);
	}
	else if (strcmp(op, "preflight") == 0 || strcmp(op, "export") == 0) {
		run_request(id, req, strcmp(op, "preflight") == 0);
	}
	else if (strcmp(op, "inspect") == 0) {
		inspect_request(id, req);
	}
	else if (strcmp(op, "atlases") == 0) {
		atlases_request(id, req);
	}
	else if (strcmp(op, "naming") == 0) {
		naming_request(id, req);
	}
	else if (strcmp(op, "version_up") == 0) {
		version_up_request(id);
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
