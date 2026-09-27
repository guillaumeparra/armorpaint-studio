#include "studio_ocio.h"
#include "studio.h"
#include "studio_ocio_lib.h"
#ifndef WITH_STUDIO_OCIO_STATIC
#include <dlfcn.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Cœur ArmorPaint / iron
char *path_data(void);

#ifdef __APPLE__
#define STUDIO_OCIO_LIB_FILE "libstudio_ocio.dylib"
#else
#define STUDIO_OCIO_LIB_FILE "libstudio_ocio.so"
#endif

// Fonctions de libstudio_ocio, résolues par dlsym : type de retour, nom sans le préfixe studio_ocio_lib_, arguments.
#define STUDIO_OCIO_LIB_FUNCS(X)                                                                                       \
	X(int, abi, (void))                                                                                                \
	X(const char *, ocio_version, (void))                                                                              \
	X(const char *, error, (void))                                                                                     \
	X(studio_ocio_lib_config_t *, config_load, (const char *))                                                         \
	X(void, config_free, (studio_ocio_lib_config_t *))                                                                 \
	X(const char *, config_name, (studio_ocio_lib_config_t *))                                                         \
	X(const char *, config_description, (studio_ocio_lib_config_t *))                                                  \
	X(int, count, (studio_ocio_lib_config_t *, int))                                                                   \
	X(const char *, name, (studio_ocio_lib_config_t *, int, int))                                                      \
	X(const char *, resolve, (studio_ocio_lib_config_t *, const char *))                                               \
	X(int, is_data, (studio_ocio_lib_config_t *, const char *))                                                        \
	X(const char *, encoding, (studio_ocio_lib_config_t *, const char *))                                              \
	X(const char *, family, (studio_ocio_lib_config_t *, const char *))                                                \
	X(const char *, identify_builtin, (studio_ocio_lib_config_t *, const char *))                                      \
	X(const char *, colorspace_from_path, (studio_ocio_lib_config_t *, const char *))                                  \
	X(const char *, file_rule, (studio_ocio_lib_config_t *, const char *))                                             \
	X(const char *, default_display, (studio_ocio_lib_config_t *))                                                     \
	X(const char *, default_view, (studio_ocio_lib_config_t *, const char *))                                          \
	X(int, view_count, (studio_ocio_lib_config_t *, const char *))                                                     \
	X(const char *, view_name, (studio_ocio_lib_config_t *, const char *, int))                                        \
	X(const char *, view_colorspace, (studio_ocio_lib_config_t *, const char *, const char *))                         \
	X(studio_ocio_lib_proc_t *, proc_colorspace, (studio_ocio_lib_config_t *, const char *, const char *))             \
	X(studio_ocio_lib_proc_t *, proc_display,                                                                          \
	  (studio_ocio_lib_config_t *, const char *, const char *, const char *, const char *, int))                       \
	X(void, proc_free, (studio_ocio_lib_proc_t *))                                                                     \
	X(int, proc_is_noop, (studio_ocio_lib_proc_t *))                                                                   \
	X(const char *, proc_cache_id, (studio_ocio_lib_proc_t *))                                                         \
	X(int, apply, (studio_ocio_lib_proc_t *, float *, long, long, int, long))

#define STUDIO_OCIO_FIELD(ret, name, args) ret(*name) args;
static struct {
	bool loaded;
	STUDIO_OCIO_LIB_FUNCS(STUDIO_OCIO_FIELD)
} lib;
#undef STUDIO_OCIO_FIELD

// Processeurs par couple (src, dst) tels que demandés ; pour un processeur d'affichage, dst est la clé
// "\x1fdisplay\x1fview\x1flook" (aucun nom d'espace ne commence par \x1f). Un échec est gardé aussi (proc NULL) : il n'est journalisé
// qu'une fois, et un nom inconnu ne recrée pas un processeur à chaque ligne d'une texture.
#define CACHE_MAX 64
typedef struct {
	char                   *src;
	char                   *dst;
	studio_ocio_lib_proc_t *proc;
} cache_entry_t;

static pthread_mutex_t           mutex = PTHREAD_MUTEX_INITIALIZER;
static studio_ocio_lib_config_t *config;
static cache_entry_t             cache[CACHE_MAX];
static int                       cache_len;
static char                      source[4096];
static char                      status[1024] = "OCIO not initialized";
static const char               *working; // appartient à config
static char                      working_error[512];
#ifndef WITH_STUDIO_OCIO_STATIC
static bool lib_tried;
#endif

#ifdef WITH_STUDIO_OCIO_STATIC

// macOS : bibliothèque liée en statique (.project/project.c), la table pointe directement sur ses fonctions.
static bool load_lib(void) {
#define STUDIO_OCIO_SYM(ret, name, args) lib.name = studio_ocio_lib_##name;
	STUDIO_OCIO_LIB_FUNCS(STUDIO_OCIO_SYM)
#undef STUDIO_OCIO_SYM
	lib.loaded = true;
	return true;
}

#else

static bool load_lib(void) {
	if (lib.loaded) {
		return true;
	}
	if (lib_tried) { // un échec de chargement ne se retente pas : le message est déjà dans status
		return false;
	}
	lib_tried = true;
	char        path[4096];
	const char *env = getenv("STUDIO_OCIO_LIB");
	if (env != NULL && env[0] != '\0') {
		snprintf(path, sizeof(path), "%s", env);
	}
	else {
#ifdef __APPLE__
		// Chargée par dlopen, la bibliothèque serait refusée par la validation des bibliothèques du hardened runtime :
		// sur macOS, l'app la lie en statique (tools/build_ocio.sh puis rebuild d'ArmorPaint).
		snprintf(status, sizeof(status),
		         "OCIO unavailable: ArmorPaint was built without libstudio_ocio (run tools/build_ocio.sh, then rebuild)");
		return false;
#else
		snprintf(path, sizeof(path), "%s%s", path_data(), STUDIO_OCIO_LIB_FILE);
#endif
	}
	void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (h == NULL) {
		const char *e = dlerror();
		snprintf(status, sizeof(status), "OCIO unavailable: cannot load %s (%s); build it with tools/build_ocio.sh",
		         path, e != NULL ? e : "unknown error");
		return false;
	}
#define STUDIO_OCIO_SYM(ret, name, args)                                                                               \
	*(void **)&lib.name = dlsym(h, "studio_ocio_lib_" #name);                                                          \
	if (lib.name == NULL) {                                                                                            \
		snprintf(status, sizeof(status), "OCIO unavailable: %s has no studio_ocio_lib_%s (stale library?)", path,      \
		         #name);                                                                                               \
		dlclose(h);                                                                                                    \
		memset(&lib, 0, sizeof(lib));                                                                                  \
		return false;                                                                                                  \
	}
	STUDIO_OCIO_LIB_FUNCS(STUDIO_OCIO_SYM)
#undef STUDIO_OCIO_SYM
	if (lib.abi() != STUDIO_OCIO_LIB_ABI) {
		snprintf(status, sizeof(status), "OCIO unavailable: %s has ABI %d, expected %d (rebuild with tools/build_ocio.sh)",
		         path, lib.abi(), STUDIO_OCIO_LIB_ABI);
		dlclose(h);
		memset(&lib, 0, sizeof(lib));
		return false;
	}
	lib.loaded = true;
	return true;
}

#endif

// Appelant : mutex tenu.
static void unload_config(void) {
	for (int i = 0; i < cache_len; i++) {
		if (cache[i].proc != NULL) {
			lib.proc_free(cache[i].proc);
		}
		free(cache[i].src);
		free(cache[i].dst);
	}
	memset(cache, 0, sizeof(cache));
	cache_len = 0;
	if (config != NULL) {
		lib.config_free(config);
		config = NULL;
	}
	source[0]        = '\0';
	working          = NULL;
	working_error[0] = '\0';
}

// Appelant : mutex tenu, config chargée.
static void find_working_space(void) {
	const char *env = getenv("STUDIO_OCIO_WORKING_SPACE");
	if (env != NULL && env[0] != '\0') {
		working = lib.resolve(config, env);
		if (working == NULL) {
			snprintf(working_error, sizeof(working_error), "STUDIO_OCIO_WORKING_SPACE: %s", lib.error());
		}
		return;
	}
	working = lib.identify_builtin(config, STUDIO_OCIO_WORKING_BUILTIN);
	if (working == NULL) {
		snprintf(working_error, sizeof(working_error),
		         "no linear Rec.709 colorspace found in the config (%s): set STUDIO_OCIO_WORKING_SPACE", lib.error());
	}
}

static int generation; // studio_ocio_generation

bool studio_ocio_reload(const char *config_path) {
	pthread_mutex_lock(&mutex);
	generation++;
	if (lib.loaded) {
		unload_config();
	}
	bool ok = false;
	if (load_lib()) {
		const char *origin = "explicit";
		const char *path   = config_path;
		if (path == NULL || path[0] == '\0') {
			origin = "$OCIO";
			path   = getenv("OCIO");
			if (path == NULL || path[0] == '\0') {
				origin = "built-in default";
				path   = STUDIO_OCIO_DEFAULT_CONFIG;
			}
		}
		config = lib.config_load(path);
		if (config == NULL) {
			snprintf(status, sizeof(status), "OCIO unavailable: config %s (%s) rejected: %s", path, origin, lib.error());
		}
		else {
			snprintf(source, sizeof(source), "%s", path);
			find_working_space();
			char ws[600];
			if (working != NULL) {
				snprintf(ws, sizeof(ws), "working space \"%s\"", working);
			}
			else {
				snprintf(ws, sizeof(ws), "working space unknown: %s", working_error);
			}
			snprintf(status, sizeof(status),
			         "OCIO %s, config \"%s\" from %s (%s), %d colorspaces, display \"%s\", %s", lib.ocio_version(),
			         lib.config_name(config), path, origin, lib.count(config, STUDIO_OCIO_LIB_COLORSPACES),
			         lib.default_display(config), ws);
			ok = true;
		}
	}
	pthread_mutex_unlock(&mutex);
	return ok;
}

void studio_ocio_shutdown(void) {
	pthread_mutex_lock(&mutex);
	if (lib.loaded) {
		unload_config();
	}
	snprintf(status, sizeof(status), "OCIO shut down");
	pthread_mutex_unlock(&mutex);
}

int studio_ocio_generation(void) {
	return generation;
}

bool studio_ocio_ready(void) {
	return config != NULL;
}

const char *studio_ocio_status(void) {
	return status;
}

const char *studio_ocio_config_source(void) {
	return source;
}

const char *studio_ocio_version(void) {
	return lib.loaded ? lib.ocio_version() : "";
}

const char *studio_ocio_config_name(void) {
	return config != NULL ? lib.config_name(config) : NULL;
}

static int count(int list) {
	return config != NULL ? lib.count(config, list) : 0;
}

static const char *name(int list, int i) {
	return config != NULL ? lib.name(config, list, i) : NULL;
}

int studio_ocio_colorspace_count(void) {
	return count(STUDIO_OCIO_LIB_COLORSPACES);
}

const char *studio_ocio_colorspace_name(int i) {
	return name(STUDIO_OCIO_LIB_COLORSPACES, i);
}

int studio_ocio_display_count(void) {
	return count(STUDIO_OCIO_LIB_DISPLAYS);
}

const char *studio_ocio_display_name(int i) {
	return name(STUDIO_OCIO_LIB_DISPLAYS, i);
}

int studio_ocio_look_count(void) {
	return count(STUDIO_OCIO_LIB_LOOKS);
}

const char *studio_ocio_look_name(int i) {
	return name(STUDIO_OCIO_LIB_LOOKS, i);
}

int studio_ocio_role_count(void) {
	return count(STUDIO_OCIO_LIB_ROLES);
}

const char *studio_ocio_role_name(int i) {
	return name(STUDIO_OCIO_LIB_ROLES, i);
}

const char *studio_ocio_display_default(void) {
	return config != NULL ? lib.default_display(config) : NULL;
}

const char *studio_ocio_view_default(const char *display) {
	return config != NULL ? lib.default_view(config, display) : NULL;
}

int studio_ocio_view_count(const char *display) {
	int n = config != NULL ? lib.view_count(config, display) : 0;
	return n > 0 ? n : 0;
}

const char *studio_ocio_view_name(const char *display, int i) {
	return config != NULL ? lib.view_name(config, display, i) : NULL;
}

const char *studio_ocio_view_colorspace(const char *display, const char *view) {
	return config != NULL ? lib.view_colorspace(config, display, view) : NULL;
}

int studio_ocio_view_is_hdr(const char *display, const char *view) {
	const char *cs = studio_ocio_view_colorspace(display, view);
	if (cs == NULL) {
		return -1;
	}
	const char *enc = lib.encoding(config, cs);
	return enc != NULL && (strcmp(enc, "hdr-video") == 0 || strcmp(enc, "edr-video") == 0) ? 1 : 0;
}

const char *studio_ocio_working_space(void) {
	return config != NULL ? working : NULL;
}

const char *studio_ocio_working_space_error(void) {
	return config != NULL ? working_error : status;
}

const char *studio_ocio_resolve(const char *cs) {
	return config != NULL ? lib.resolve(config, cs) : NULL;
}

int studio_ocio_is_data(const char *cs) {
	return config != NULL ? lib.is_data(config, cs) : -1;
}

const char *studio_ocio_encoding(const char *cs) {
	return config != NULL ? lib.encoding(config, cs) : NULL;
}

const char *studio_ocio_family(const char *cs) {
	return config != NULL ? lib.family(config, cs) : NULL;
}

const char *studio_ocio_colorspace_from_path(const char *p) {
	return config != NULL ? lib.colorspace_from_path(config, p) : NULL;
}

const char *studio_ocio_file_rule(const char *p) {
	return config != NULL ? lib.file_rule(config, p) : NULL;
}

const char *studio_ocio_identify_builtin(const char *builtin) {
	return config != NULL ? lib.identify_builtin(config, builtin) : NULL;
}

// display NULL : processeur d'espace à espace src → dst ; sinon processeur d'affichage, dst étant sa clé.
static studio_ocio_lib_proc_t *get_proc_ex(const char *src, const char *dst, const char *display, const char *view,
                                           const char *look) {
	if (config == NULL || src == NULL || dst == NULL) {
		return NULL;
	}
	pthread_mutex_lock(&mutex);
	studio_ocio_lib_proc_t *proc = NULL;
	for (int i = 0; i < cache_len; i++) {
		if (strcmp(cache[i].src, src) == 0 && strcmp(cache[i].dst, dst) == 0) {
			proc = cache[i].proc;
			pthread_mutex_unlock(&mutex);
			return proc;
		}
	}
	if (display != NULL) {
		proc = lib.proc_display(config, src, display, view, look, 0);
		if (proc == NULL) {
			iron_log("studio: ocio: no display transform from \"%s\" to \"%s\" / \"%s\" (look \"%s\"): %s", src,
			         display, view, look != NULL ? look : "", lib.error());
		}
	}
	else {
		proc = lib.proc_colorspace(config, src, dst);
		if (proc == NULL) {
			iron_log("studio: ocio: no transform from \"%s\" to \"%s\": %s", src, dst, lib.error());
		}
	}
	if (cache_len == CACHE_MAX) { // plein : on évince le plus ancien (en pratique, quelques couples par export)
		if (cache[0].proc != NULL) {
			lib.proc_free(cache[0].proc);
		}
		free(cache[0].src);
		free(cache[0].dst);
		memmove(cache, cache + 1, sizeof(cache_entry_t) * (CACHE_MAX - 1));
		cache_len--;
	}
	char *s = strdup(src);
	char *d = strdup(dst);
	if (s != NULL && d != NULL) {
		cache[cache_len++] = (cache_entry_t){.src = s, .dst = d, .proc = proc};
	}
	else { // sans mémoire pour la clé : processeur rendu hors cache, perdu ; l'appel suivant réessaiera
		free(s);
		free(d);
		if (proc != NULL) {
			lib.proc_free(proc);
			proc = NULL;
		}
	}
	pthread_mutex_unlock(&mutex);
	return proc;
}

static studio_ocio_lib_proc_t *get_proc(const char *src, const char *dst) {
	return get_proc_ex(src, dst, NULL, NULL, NULL);
}

static studio_ocio_lib_proc_t *get_display_proc(const char *src, const char *display, const char *view, const char *look) {
	if (display == NULL || view == NULL) {
		return NULL;
	}
	char key[1024];
	int  len = snprintf(key, sizeof(key), "\x1f%s\x1f%s\x1f%s", display, view, look != NULL ? look : "");
	if (len < 0 || len >= (int)sizeof(key)) {
		return NULL;
	}
	return get_proc_ex(src, key, display, view, look);
}

bool studio_ocio_transform(float *pixels, int width, int height, int channels, const char *src, const char *dst) {
	if (pixels == NULL || width <= 0 || height <= 0 || (channels != 3 && channels != 4)) {
		return false;
	}
	const char *a = studio_ocio_resolve(src);
	const char *b = studio_ocio_resolve(dst);
	if (a != NULL && b != NULL && strcmp(a, b) == 0) {
		return true;
	}
	studio_ocio_lib_proc_t *proc = get_proc(src, dst);
	if (proc == NULL) {
		return false;
	}
	if (lib.proc_is_noop(proc) == 1) {
		return true;
	}
	if (!lib.apply(proc, pixels, width, height, channels, 0)) {
		iron_log("studio: ocio: transform \"%s\" -> \"%s\" failed: %s", src, dst, lib.error());
		return false;
	}
	return true;
}

const char *studio_ocio_transform_id(const char *src, const char *dst) {
	studio_ocio_lib_proc_t *proc = get_proc(src, dst);
	return proc != NULL ? lib.proc_cache_id(proc) : NULL;
}

bool studio_ocio_display_transform(float *pixels, int width, int height, int channels, const char *src,
                                   const char *display, const char *view, const char *look) {
	if (pixels == NULL || width <= 0 || height <= 0 || (channels != 3 && channels != 4)) {
		return false;
	}
	studio_ocio_lib_proc_t *proc = get_display_proc(src, display, view, look);
	if (proc == NULL) {
		return false;
	}
	if (lib.proc_is_noop(proc) == 1) {
		return true;
	}
	if (!lib.apply(proc, pixels, width, height, channels, 0)) {
		iron_log("studio: ocio: display transform \"%s\" -> \"%s\" / \"%s\" failed: %s", src, display, view,
		         lib.error());
		return false;
	}
	return true;
}

const char *studio_ocio_display_transform_id(const char *src, const char *display, const char *view, const char *look) {
	studio_ocio_lib_proc_t *proc = get_display_proc(src, display, view, look);
	return proc != NULL ? lib.proc_cache_id(proc) : NULL;
}

#ifdef WITH_EVAL

#include "minic.h"

static const char *or_empty(const char *s) {
	return s != NULL ? s : "";
}

static minic_val_t mn_studio_ocio_status(minic_val_t *a, int n) {
	return minic_val_ptr((void *)studio_ocio_status());
}

static minic_val_t mn_studio_ocio_reload(minic_val_t *a, int n) {
	return minic_val_int(studio_ocio_reload(minic_arg_p(a, n, 0)) ? 1 : 0);
}

static minic_val_t mn_studio_ocio_colorspace_count(minic_val_t *a, int n) {
	return minic_val_int(studio_ocio_colorspace_count());
}

static minic_val_t mn_studio_ocio_colorspace_name(minic_val_t *a, int n) {
	return minic_val_ptr((void *)or_empty(studio_ocio_colorspace_name(minic_arg_i(a, n, 0))));
}

static minic_val_t mn_studio_ocio_display_default(minic_val_t *a, int n) {
	return minic_val_ptr((void *)or_empty(studio_ocio_display_default()));
}

static minic_val_t mn_studio_ocio_view_default(minic_val_t *a, int n) {
	return minic_val_ptr((void *)or_empty(studio_ocio_view_default(minic_arg_p(a, n, 0))));
}

static minic_val_t mn_studio_ocio_working_space(minic_val_t *a, int n) {
	return minic_val_ptr((void *)or_empty(studio_ocio_working_space()));
}

static minic_val_t mn_studio_ocio_resolve(minic_val_t *a, int n) {
	return minic_val_ptr((void *)or_empty(studio_ocio_resolve(minic_arg_p(a, n, 0))));
}

static minic_val_t mn_studio_ocio_is_data(minic_val_t *a, int n) {
	return minic_val_int(studio_ocio_is_data(minic_arg_p(a, n, 0)));
}

static minic_val_t mn_studio_ocio_colorspace_from_path(minic_val_t *a, int n) {
	return minic_val_ptr((void *)or_empty(studio_ocio_colorspace_from_path(minic_arg_p(a, n, 0))));
}

// Un pixel, pour les scripts et les tests : "r g b" (%.9g, assez pour relire le float exact), "" en cas d'échec.
static minic_val_t mn_studio_ocio_transform_rgb(minic_val_t *a, int n) {
	static char out[128];
	float       px[3] = {minic_arg_f(a, n, 2), minic_arg_f(a, n, 3), minic_arg_f(a, n, 4)};
	out[0]            = '\0';
	if (studio_ocio_transform(px, 1, 1, 3, minic_arg_p(a, n, 0), minic_arg_p(a, n, 1))) {
		snprintf(out, sizeof(out), "%.9g %.9g %.9g", px[0], px[1], px[2]);
	}
	return minic_val_ptr(out);
}

static void studio_ocio_register_script_api(void) {
	// Signatures au format de minic_register (sans les noms de paramètres) ; la référence est dans README.md.
	minic_register("studio_ocio_status", "p()", mn_studio_ocio_status);
	minic_register("studio_ocio_reload", "i(p)", mn_studio_ocio_reload);
	minic_register("studio_ocio_colorspace_count", "i()", mn_studio_ocio_colorspace_count);
	minic_register("studio_ocio_colorspace_name", "p(i)", mn_studio_ocio_colorspace_name);
	minic_register("studio_ocio_display_default", "p()", mn_studio_ocio_display_default);
	minic_register("studio_ocio_view_default", "p(p)", mn_studio_ocio_view_default);
	minic_register("studio_ocio_working_space", "p()", mn_studio_ocio_working_space);
	minic_register("studio_ocio_resolve", "p(p)", mn_studio_ocio_resolve);
	minic_register("studio_ocio_is_data", "i(p)", mn_studio_ocio_is_data);
	minic_register("studio_ocio_colorspace_from_path", "p(p)", mn_studio_ocio_colorspace_from_path);
	minic_register("studio_ocio_transform_rgb", "p(p,p,f,f,f)", mn_studio_ocio_transform_rgb);
}

#endif

void studio_ocio_init(void) {
	studio_ocio_reload(NULL);
	iron_log("studio: %s", studio_ocio_status());
#ifdef WITH_EVAL
	studio_ocio_register_script_api();
#endif
}
