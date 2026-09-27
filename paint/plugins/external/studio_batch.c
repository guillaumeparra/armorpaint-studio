#include "studio_batch.h"
// Pas <iron.h> : il définit des fonctions, déjà compilées par main.c (symboles en double au lien).
#include "iron_array.h"
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
#include "studio_fs.h"
#include "studio_json.h"
#include "studio_naming.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <process.h>
#include <windows.h>
#define SEP "\\"
#else
#include <pthread.h>
#include <unistd.h>
#define SEP "/"
#endif

// Cœur ArmorPaint (paint/sources, déclarés dans functions.h et globals.h, qu'on ne peut inclure ici)
extern context_t *g_context;
char             *path_data(void);
int               iron_get_arg_count(void); // iron.h
char             *iron_get_arg(int index);

#define LOG_MAX      2000 // messages gardés dans le rapport (les suivants sont comptés)
#define MESSAGE_MAX  4096 // octets par message
#define ERROR_MAX    200
#define EXPORT_MAX   64
#define OUTPUT_MAX   64

typedef struct {
	char **items;
	int    count, cap;
} list_t;

typedef struct {
	char  *dir, *preset, *manifest;
	bool   studio_preset, to_disk, preflight_only;
	int    mode;
	list_t files;
	int    errors_at_begin, errors;
	bool   open, ended_ok, ok;
} export_t;

static struct {
	bool   active;   // --background
	bool   usage_bad; // ligne de commande fausse
	char  *report;   // --report, absolu
	int    mode;     // --export-mode, -1 sinon
	char  *naming;   // --export-naming
	char  *color_space;
	double start;
	char   started[32];
	list_t log, log_levels, errors;
	int    log_dropped;
	bool   next_is_error; // console_error vient d'être appelé : son console_log suit
	export_t exports[EXPORT_MAX];
	int      export_count;
	char    *output_keys[OUTPUT_MAX], *output_values[OUTPUT_MAX];
	int      output_count;
	bool     finished;
} bt = {.mode = -1};

#ifdef _WIN32
static CRITICAL_SECTION mutex;
static bool             mutex_ready;
static void lock(void) {
	if (!mutex_ready) {
		InitializeCriticalSection(&mutex);
		mutex_ready = true;
	}
	EnterCriticalSection(&mutex);
}
static void unlock(void) {
	LeaveCriticalSection(&mutex);
}
#else
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER; // console_log peut venir d'un autre thread
static void lock(void) {
	pthread_mutex_lock(&mutex);
}
static void unlock(void) {
	pthread_mutex_unlock(&mutex);
}
#endif

// --background, lu à la première demande : les messages des modules initialisés avant studio_batch_init comptent aussi.
static bool active(void) {
	static bool known;
	if (!known) {
		for (int i = 1; i < iron_get_arg_count(); i++) {
			if (strcmp(iron_get_arg(i), "--background") == 0) {
				bt.active = true;
			}
		}
		known = true;
	}
	return bt.active;
}

static void push(list_t *l, const char *s) {
	if (l->count == l->cap) {
		int    cap  = l->cap > 0 ? l->cap * 2 : 16;
		char **grow = realloc(l->items, (size_t)cap * sizeof(char *));
		if (grow == NULL) {
			return;
		}
		l->items = grow;
		l->cap   = cap;
	}
	size_t len = strlen(s);
	char  *copy = malloc(len < MESSAGE_MAX ? len + 1 : MESSAGE_MAX);
	if (copy == NULL) {
		return;
	}
	len = len < MESSAGE_MAX ? len : MESSAGE_MAX - 1;
	memcpy(copy, s, len);
	copy[len]              = 0;
	l->items[l->count++] = copy;
}

static bool contains(const list_t *l, const char *s) {
	for (int i = 0; i < l->count; i++) {
		if (strcmp(l->items[i], s) == 0) {
			return true;
		}
	}
	return false;
}

static char *copy_text(const char *s) {
	return s != NULL ? strdup(s) : NULL;
}

// ---- Rapport ---------------------------------------------------------------------------------------------------------

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

// Une valeur par ligne, indentée de indent espaces.
static void put_list(text_t *b, const list_t *l, int indent) {
	put(b, "[");
	for (int i = 0; i < l->count; i++) {
		put(b, "%s\n%*s", i > 0 ? "," : "", indent + 2, "");
		put_quoted(b, l->items[i]);
	}
	put(b, l->count > 0 ? "\n%*s]" : "]", indent, "");
}

static const char *mode_name(int mode) {
	switch (mode) {
	case EXPORT_MODE_SELECTED:
		return "selected";
	case EXPORT_MODE_PER_OBJECT:
		return "per-object";
	case EXPORT_MODE_PER_UDIM_TILE:
		return "per-tile";
	default:
		return "visible";
	}
}

// Ferme l'export ouvert : il a abouti s'il n'a ni échoué ni signalé d'erreur, et s'il a écrit ses fichiers (sauf
// préflight seul ou textures gardées dans le projet). Un fichier annoncé mais absent est une erreur.
static void close_export(void) {
	if (bt.export_count == 0 || !bt.exports[bt.export_count - 1].open) {
		return;
	}
	export_t *e = &bt.exports[bt.export_count - 1];
	e->open     = false;
	for (int i = 0; i < e->files.count; i++) {
		if (!studio_fs_exists(e->files.items[i])) {
			char message[MESSAGE_MAX];
			snprintf(message, sizeof(message), "Texture export: %s was not written", e->files.items[i]);
			if (bt.errors.count < ERROR_MAX) {
				push(&bt.errors, message);
			}
		}
	}
	e->errors = bt.errors.count - e->errors_at_begin;
	if (e->to_disk && !e->preflight_only && e->files.count == 0 && e->errors == 0) {
		push(&bt.errors, "Texture export: no file written");
		e->errors++;
	}
	e->ok = e->ended_ok && e->errors == 0;
}

static int exit_code(void) {
	return bt.usage_bad ? 2 : bt.errors.count > 0 ? 1 : 0;
}

static char *report_json(const char *status) {
	text_t b = {0};
	put(&b, "{\n  \"version\": 1,\n  \"status\": \"%s\",\n  \"exit_code\": %d,\n", status, exit_code());
	put(&b, "  \"studio_version\": \"%s\",\n  \"pid\": %d,\n  \"started\": \"%s\",\n  \"duration_s\": %.3f,\n",
	    STUDIO_VERSION,
#ifdef _WIN32
	    (int)_getpid(),
#else
	    (int)getpid(),
#endif
	    bt.started, iron_time() - bt.start);
	put(&b, "  \"argv\": [");
	for (int i = 0; i < iron_get_arg_count(); i++) {
		put(&b, i > 0 ? ", " : "");
		put_quoted(&b, iron_get_arg(i));
	}
	put(&b, "],\n  \"exports\": [");
	for (int i = 0; i < bt.export_count; i++) {
		const export_t *e = &bt.exports[i];
		put(&b, "%s\n    {\"dir\": ", i > 0 ? "," : "");
		put_quoted(&b, e->dir);
		put(&b, ", \"preset\": ");
		put_quoted(&b, e->preset);
		put(&b, ", \"studio_preset\": %s, \"mode\": \"%s\", \"destination\": \"%s\", \"preflight_only\": %s,",
		    e->studio_preset ? "true" : "false", mode_name(e->mode), e->to_disk ? "disk" : "project",
		    e->preflight_only ? "true" : "false");
		put(&b, "\n     \"ok\": %s, \"errors\": %d, \"manifest\": ", e->open ? "null" : e->ok ? "true" : "false", e->errors);
		if (e->manifest != NULL) {
			put_quoted(&b, e->manifest);
		}
		else {
			put(&b, "null");
		}
		put(&b, ",\n     \"files\": ");
		put_list(&b, &e->files, 5);
		put(&b, "}");
	}
	put(&b, "%s],\n  \"outputs\": {", bt.export_count > 0 ? "\n  " : "");
	for (int i = 0; i < bt.output_count; i++) {
		put(&b, i > 0 ? ", " : "");
		put_quoted(&b, bt.output_keys[i]);
		put(&b, ": ");
		put_quoted(&b, bt.output_values[i]);
	}
	put(&b, "},\n  \"errors\": ");
	put_list(&b, &bt.errors, 2);
	put(&b, ",\n  \"log_dropped\": %d,\n  \"log\": [", bt.log_dropped);
	for (int i = 0; i < bt.log.count; i++) {
		put(&b, "%s\n    {\"level\": \"%s\", \"message\": ", i > 0 ? "," : "", bt.log_levels.items[i]);
		put_quoted(&b, bt.log.items[i]);
		put(&b, "}");
	}
	put(&b, "%s]\n}\n", bt.log.count > 0 ? "\n  " : "");
	return b.data;
}

// Écrit le rapport à côté puis le renomme : un lecteur ne voit jamais un rapport à moitié écrit.
static bool write_report(const char *status) {
	if (bt.report == NULL) {
		return true;
	}
	char *json = report_json(status);
	char  tmp[4096];
	snprintf(tmp, sizeof(tmp), "%s.tmp", bt.report);
	bool ok = json != NULL && studio_fs_write(tmp, json, strlen(json)) && studio_fs_rename(tmp, bt.report);
	free(json);
	if (!ok) {
		studio_fs_remove(tmp);
		fprintf(stderr, "studio: batch: cannot write the report %s\n", bt.report);
	}
	return ok;
}

// ---- Ligne de commande -----------------------------------------------------------------------------------------------

static void usage_error(const char *format, ...) {
	char    message[MESSAGE_MAX];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	iron_log("studio: batch: %s", message);
	lock();
	bt.usage_bad = true;
	push(&bt.errors, message);
	unlock();
}

static bool is_absolute(const char *path) {
#ifdef _WIN32
	return (path[0] != 0 && path[1] == ':') || path[0] == '\\' || path[0] == '/';
#else
	return path[0] == '/';
#endif
}

// Chemin de la ligne de commande résolu contre le dossier courant, comme args_path (STUDIO-PATCH(args-cwd-paths)).
static char *resolve(const char *path) {
	if (is_absolute(path)) {
		return strdup(path);
	}
	char cwd[4096];
#ifdef _WIN32
	wchar_t wcwd[4096];
	DWORD   n  = GetCurrentDirectoryW(4096, wcwd);
	bool    ok = n > 0 && n < 4096 && WideCharToMultiByte(CP_UTF8, 0, wcwd, -1, cwd, sizeof(cwd), NULL, NULL) > 0;
#else
	bool ok = getcwd(cwd, sizeof(cwd)) != NULL;
#endif
	if (!ok) {
		return strdup(path);
	}
	const char *rest = strncmp(path, "./", 2) == 0 ? path + 2 : path;
	size_t      len  = strlen(cwd) + strlen(rest) + 2;
	char       *out  = malloc(len);
	if (out != NULL) {
		snprintf(out, len, "%s%s%s", cwd, SEP, rest);
	}
	return out;
}

static int parse_mode(const char *s) {
	if (strcmp(s, "visible") == 0) {
		return EXPORT_MODE_VISIBLE;
	}
	if (strcmp(s, "selected") == 0) {
		return EXPORT_MODE_SELECTED;
	}
	if (strcmp(s, "per-object") == 0) {
		return EXPORT_MODE_PER_OBJECT;
	}
	if (strcmp(s, "per-tile") == 0 || strcmp(s, "udim") == 0) {
		return EXPORT_MODE_PER_UDIM_TILE;
	}
	return -1;
}

static bool upstream_preset_exists(const char *name) {
	char path[4096];
	snprintf(path, sizeof(path), "%s" SEP "export_presets" SEP "%s.json", path_data(), name);
	return studio_fs_exists(path);
}

// Ce que l'upstream (args.c) accepterait en silence : type inconnu (profondeur inchangée), dossier absent (export
// perdu), preset inconnu (remplacé par le premier de la liste), script introuvable. En plus, les options studio.
static void check_command_line(void) {
	int         n        = iron_get_arg_count();
	const char *type     = NULL, *preset = NULL, *dir = NULL, *script = NULL;
	bool        textures = false;
	for (int i = 1; i < n; i++) {
		const char *a     = iron_get_arg(i);
		bool        has_1 = i + 1 < n;
		if (strcmp(a, "--export-textures") == 0) {
			if (i + 3 >= n) { // args.c demande (i + 3) < count, sinon ignore l'option
				usage_error("--export-textures needs <type> <preset> <directory>");
				return;
			}
			textures = true;
			type     = iron_get_arg(i + 1);
			preset   = iron_get_arg(i + 2);
			dir      = iron_get_arg(i + 3);
			i += 3;
		}
		else if (strcmp(a, "--script") == 0 && has_1) {
			script = iron_get_arg(++i);
		}
		else if (strcmp(a, "--export-mesh") == 0 || strcmp(a, "--export-material") == 0) {
			i += has_1 ? 1 : 0;
		}
		else if (strcmp(a, "--report") == 0 || strcmp(a, "--export-mode") == 0 || strcmp(a, "--export-naming") == 0 ||
		         strcmp(a, "--export-color-space") == 0) {
			if (!has_1) {
				usage_error("%s needs a value", a);
				return;
			}
			const char *v = iron_get_arg(++i);
			if (strcmp(a, "--report") == 0) {
				free(bt.report);
				bt.report = resolve(v);
			}
			else if (strcmp(a, "--export-mode") == 0) {
				bt.mode = parse_mode(v);
				if (bt.mode < 0) {
					usage_error("--export-mode %s: expected visible, selected, per-object or per-tile", v);
				}
			}
			else if (strcmp(a, "--export-naming") == 0) {
				free(bt.naming);
				bt.naming = strdup(v);
			}
			else {
				free(bt.color_space);
				bt.color_space = strdup(v);
			}
		}
	}
	if (bt.report != NULL) {
		char *dir_of = studio_fs_dirname(bt.report);
		if (dir_of != NULL && dir_of[0] != 0 && !studio_fs_is_dir(dir_of)) {
			usage_error("--report %s: no such directory", bt.report);
		}
		free(dir_of);
	}
	if (script != NULL) {
		char *path = resolve(script);
		if (path == NULL || !studio_fs_exists(path)) {
			usage_error("--script %s: file not found", script);
		}
		free(path);
	}
	bool studio_options = bt.mode >= 0 || bt.naming != NULL || bt.color_space != NULL;
	if (!textures) {
		if (studio_options) {
			usage_error("--export-mode, --export-naming and --export-color-space apply to --export-textures");
		}
		return;
	}
	if (strcmp(type, "png") != 0 && strcmp(type, "jpg") != 0 && strcmp(type, "exr16") != 0 && strcmp(type, "exr32") != 0) {
		usage_error("--export-textures %s: expected png, jpg, exr16 or exr32", type);
	}
	// Un preset studio crée le dossier qui manque (son préflight vérifie qu'il le peut) ; l'upstream n'écrirait rien.
	bool  studio_preset = studio_export_is_studio_preset(preset);
	char *path          = resolve(dir);
	if (path == NULL || (studio_preset ? studio_fs_exists(path) && !studio_fs_is_dir(path) : !studio_fs_is_dir(path))) {
		usage_error("--export-textures: %s is not a directory", dir);
	}
	free(path);
	if (!studio_preset && !upstream_preset_exists(preset)) {
		usage_error("--export-textures: unknown preset %s", preset);
	}
	if (!studio_preset && (bt.naming != NULL || bt.color_space != NULL)) {
		usage_error("--export-naming and --export-color-space need a studio preset (%s is an ArmorPaint preset)", preset);
	}
	char err[512];
	if (bt.naming != NULL && !studio_naming_check(bt.naming, err, sizeof(err))) {
		usage_error("--export-naming %s: %s", bt.naming, err);
	}
	if (bt.color_space != NULL) {
		char *previous = copy_text(studio_export_color_space());
		if (!studio_export_set_color_space(bt.color_space)) {
			usage_error("--export-color-space %s: not a color space of the OCIO configuration", bt.color_space);
		}
		studio_export_set_color_space(previous); // appliqué au début de l'export (studio_batch_apply_cli)
		free(previous);
	}
}

// ---- Scripting (minic) -----------------------------------------------------------------------------------------------

#ifdef WITH_EVAL

#include "minic.h"

static minic_val_t mn_exit_code(minic_val_t *a, int n) {
	(void)a, (void)n;
	return minic_val_int(studio_batch_exit_code());
}

static minic_val_t mn_output(minic_val_t *a, int n) {
	studio_batch_output(minic_arg_p(a, n, 0), minic_arg_p(a, n, 1));
	return minic_val_int(0);
}

#endif

// ---- API -------------------------------------------------------------------------------------------------------------

void studio_batch_init(void) {
	bt.start = iron_time();
	time_t    now = time(NULL);
	struct tm utc;
#ifdef _WIN32
	gmtime_s(&utc, &now);
#else
	gmtime_r(&now, &utc);
#endif
	strftime(bt.started, sizeof(bt.started), "%Y-%m-%dT%H:%M:%SZ", &utc);
	active();
#ifdef WITH_EVAL
	minic_register("studio_batch_exit_code", "i()", mn_exit_code);
	minic_register("studio_batch_output", "v(p,p)", mn_output);
#endif
	check_command_line();
	if (!bt.active) {
		if (bt.usage_bad) {
			bt.mode = -1; // ligne fausse hors --background : options studio ignorées, l'upstream fait comme avant
			free(bt.naming);
			free(bt.color_space);
			bt.naming = bt.color_space = NULL;
		}
		return;
	}
	if (bt.usage_bad) {
		write_report("failed");
		fprintf(stderr, "studio: batch: invalid command line, nothing run (exit 2)\n");
		fflush(NULL);
		exit(2);
	}
	write_report("running");
	iron_log("studio: batch mode%s%s", bt.report != NULL ? ", report " : "", bt.report != NULL ? bt.report : "");
}

bool studio_batch_active(void) {
	return active();
}

int studio_batch_exit_code(void) {
	lock();
	int code = exit_code();
	unlock();
	return code;
}

void studio_batch_on_error(const char *message) {
	if (!active() || message == NULL) {
		return;
	}
	lock();
	if (bt.errors.count < ERROR_MAX) {
		push(&bt.errors, message);
	}
	else if (bt.errors.count == ERROR_MAX) {
		push(&bt.errors, "(more errors, see the log)");
	}
	bt.next_is_error = true;
	unlock();
}

// Erreur de minic : "<fichier>:<ligne>: error: <message>" (minic_error, base/sources/libs/minic.c), passée à
// console_log et pas à console_error. Le banc batch_app.sh vérifie ce format.
static bool is_script_error(const char *s) {
	const char *e = strstr(s, ": error: ");
	if (e == NULL) {
		return false;
	}
	const char *p = e;
	while (p > s && p[-1] >= '0' && p[-1] <= '9') {
		p--;
	}
	return p < e && p > s && p[-1] == ':';
}

void studio_batch_on_log(const char *message) {
	if (!active() || message == NULL) {
		return;
	}
	lock();
	bool error = bt.next_is_error;
	if (!error && is_script_error(message)) {
		error = true;
		if (bt.errors.count < ERROR_MAX) {
			push(&bt.errors, message);
		}
	}
	bt.next_is_error = false;
	if (bt.log.count < LOG_MAX) {
		push(&bt.log, message);
		push(&bt.log_levels, error ? "error" : "info");
	}
	else {
		bt.log_dropped++;
	}
	unlock();
}

void studio_batch_on_file(const char *path) {
	if (!active() || path == NULL) {
		return;
	}
	lock();
	if (bt.export_count > 0 && bt.exports[bt.export_count - 1].open && !contains(&bt.exports[bt.export_count - 1].files, path)) {
		push(&bt.exports[bt.export_count - 1].files, path);
	}
	unlock();
}

void studio_batch_apply_cli(void) {
	if (bt.mode >= 0) {
		g_context->layers_export = (export_mode_t)bt.mode;
	}
	if (bt.naming != NULL) {
		studio_export_set_naming(bt.naming);
	}
	if (bt.color_space != NULL) {
		studio_export_set_color_space(bt.color_space);
	}
}

void studio_batch_export_begin(const char *dir, const char *preset, bool studio_preset, int mode, bool to_disk,
                               bool preflight_only) {
	if (!bt.active) {
		return;
	}
	lock();
	close_export();
	if (bt.export_count < EXPORT_MAX) {
		export_t *e        = &bt.exports[bt.export_count++];
		*e                 = (export_t){0};
		e->dir             = copy_text(dir);
		e->preset          = copy_text(preset != NULL ? preset : "");
		e->studio_preset   = studio_preset;
		e->mode            = mode;
		e->to_disk         = to_disk;
		e->preflight_only  = preflight_only;
		e->errors_at_begin = bt.errors.count;
		e->open            = true;
		e->ended_ok        = true;
	}
	unlock();
}

void studio_batch_export_end(bool ok, const char *manifest) {
	if (!bt.active) {
		return;
	}
	lock();
	if (bt.export_count > 0 && bt.exports[bt.export_count - 1].open) {
		export_t *e = &bt.exports[bt.export_count - 1];
		e->ended_ok = ok;
		free(e->manifest);
		e->manifest = copy_text(manifest);
		close_export();
	}
	unlock();
}

void studio_batch_output(const char *key, const char *value) {
	if (key == NULL) {
		return;
	}
	lock();
	int i = 0;
	while (i < bt.output_count && strcmp(bt.output_keys[i], key) != 0) {
		i++;
	}
	if (i < bt.output_count) {
		free(bt.output_values[i]);
		bt.output_values[i] = strdup(value != NULL ? value : "");
	}
	else if (bt.output_count < OUTPUT_MAX) {
		bt.output_keys[bt.output_count]     = strdup(key);
		bt.output_values[bt.output_count++] = strdup(value != NULL ? value : "");
	}
	unlock();
}

void studio_batch_finish(void) {
	if (!bt.active || bt.finished) {
		return;
	}
	bt.finished = true;
	lock();
	close_export();
	int code = exit_code();
	unlock();
	write_report(code == 0 ? "ok" : "failed");
	iron_log("studio: batch done, exit %d (%d error%s)%s%s", code, bt.errors.count, bt.errors.count > 1 ? "s" : "",
	         bt.report != NULL ? ", report " : "", bt.report != NULL ? bt.report : "");
	if (code != 0) {
		fflush(NULL);
		exit(code);
	}
}
