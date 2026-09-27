#include "studio_ocio_import.h"
#include "iron_array.h"
#include "iron_draw.h"
#include "iron_file.h"
#include "iron_gpu.h"
#include "iron_map.h"
#include "iron_obj.h"
#include "iron_path.h"
#include "iron_string.h"
#include "iron_system.h"
#include "iron_ui.h"
#include "engine.h"
#include "../../sources/types.h"
#include "studio.h"
#include "studio_json.h"
#include "studio_ocio.h"
#include "studio_ocio_lut.h"
#include "studio_project.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Cœur ArmorPaint (paint/sources/globals.h, io/, ui/, util/) ; iron_texture_decoded est le point d'injection
// STUDIO-PATCH(ocio-import) de base/sources/iron.h, studio_texture_import celui de io/import_texture.c (PATCHES.md).
extern project_t *g_project;
extern context_t *g_context;
extern ui_t      *g_ui;
extern any_map_t *import_texture_importers;
extern void (*iron_texture_decoded)(void *pixels, i32 width, i32 height, i32 format, char *source);
gpu_texture_t *import_texture_default_importer(char *path);
void           import_envmap_run(char *path, gpu_texture_t *image);
void           make_material_parse_paint_material(bool bake_previews);
void           make_material_parse_brush(void);
void           util_render_make_material_preview(void);
void           layers_update_fill_layers(void);
void           console_info(char *s);
void           console_error(char *s);
bool           path_check_ext(char *p, string_array_t *exts);
buffer_t      *gpu_get_texture_pixels(gpu_texture_t *image); // défini dans iron.h
void           gpu_delete_texture(gpu_texture_t *texture);   // défini dans iron.h
bool           ui_menu_button(char *text, char *label, int icon);
bool           ui_icon_button(char *text, int icon, int align);
void           ui_box_show_custom(void (*commands)(void), i32 mw, i32 mh, void (*on_hide)(void), bool draggable, char *title);
void           ui_box_hide(void);
// tinyexr (vendor/.tinyexr/exr_internal.h) : pool de threads spawn/join.
void exr_parallel_for(int nthreads, int njobs, void (*fn)(void *ctx, int job), void *ctx);

#define OI_ICON_NONE  -1 // paint/sources/enums.h
#define OI_ICON_CLOSE 40

#define PACKED_NAME "studio_color_spaces.json" // entrée du paquet d'assets du .arm
#define SCOPE_DEPTH 8                          // imports imbriqués (un importeur qui en appelle un autre)
#define RAW_MAX     4096
#define BAND        65536 // pixels par tâche de conversion

typedef struct {
	char *file;        // chemin absolu normalisé (path_normalize), clé
	char *color_space; // nom canonique au moment du décodage
	char *source;      // "user", "name", "rule", "default"
	bool  is_float;    // décodée en half ou float
	bool  linear;      // valeurs gardées linéaires (flottante, espace couleur)
} record_t;

// Un import en cours : les pixels décodés pendant l'appel sont les siens.
typedef struct {
	char       *path;        // chemin normalisé
	char       *color_space; // NULL : pas de conversion
	const char *source;
	bool        pending; // défaut du format, choisi au premier décodage
	int         decodes;
	bool        failed;
	bool        is_float;
	bool        linear;
} scope_t;

typedef struct {
	char *key;  // clé de data_cached_textures (la source du décodage)
	char *norm; // la même, normalisée
} raw_t;

static struct {
	bool      enabled;
	record_t *records;
	int       count, cap;
	scope_t   scopes[SCOPE_DEPTH];
	int       depth;
	// Textures décodées hors import par leur chemin (le navigateur de fichiers charge l'image entière pour sa vignette)
	// et encore dans le cache : non converties, un import qui les trouverait en cache les décode à nouveau.
	raw_t *raw;
	int    raw_count, raw_cap;
	// Projet courant : ouvrir un projet ou en créer un vide les enregistrements, puis relit ceux du .arm.
	project_t *project;
	void      *assets;
	int        next_asset_id;
	// Défauts résolus, par génération de configuration.
	int         generation;
	const char *srgb, *linear_srgb, *scene_linear, *data;
	// Boîte de dialogue
	string_array_t *names; // "Automatic" puis les espaces de la configuration
	int             names_generation;
	int            *choices; // valeurs des combos : adresses stables (identifiant du widget)
	int             choices_cap;
	char           *box_file; // NULL : toutes les textures du projet
} oi = {.generation = -1, .names_generation = -1};

// console_* passe le message à iron_log comme chaîne de format : les '%' doivent être doublés.
static void report(bool error, const char *format, ...) {
	char    message[2048];
	char    escaped[4096];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	size_t j = 0;
	for (size_t i = 0; message[i] != 0 && j + 2 < sizeof(escaped); i++) {
		if (message[i] == '%') {
			escaped[j++] = '%';
		}
		escaped[j++] = message[i];
	}
	escaped[j] = 0;
	if (error) {
		console_error(escaped);
	}
	else {
		console_info(escaped);
	}
}

static char *dup(const char *s) {
	return strdup(s != NULL ? s : "");
}

static bool ends_with_ci(const char *s, const char *suffix) {
	size_t n = strlen(s), m = strlen(suffix);
	if (m > n) {
		return false;
	}
	for (size_t i = 0; i < m; i++) {
		if (tolower((unsigned char)s[n - m + i]) != tolower((unsigned char)suffix[i])) {
			return false;
		}
	}
	return true;
}

static bool is_path(const char *s) {
	return s != NULL && (strchr(s, '/') != NULL || strchr(s, '\\') != NULL);
}

// Format d'ArmorPaint : ressource ou texture déjà dans ses conventions, jamais convertie.
static bool is_k(const char *s) {
	return ends_with_ci(s, ".k") || strcmp(s, "k") == 0;
}

static char *normalized(const char *path) {
	return path_normalize((char *)path); // malloc ; ".." résolus comme au chargement d'un projet (import_arm.c)
}

// ── enregistrements ───────────────────────────────────────────────────────────────────────────────────────────────

static record_t *record_find_norm(const char *norm) {
	for (int i = 0; i < oi.count; i++) {
		if (strcmp(oi.records[i].file, norm) == 0) {
			return &oi.records[i];
		}
	}
	return NULL;
}

static record_t *record_find(const char *file) {
	if (file == NULL || file[0] == 0) {
		return NULL;
	}
	char     *norm = normalized(file);
	record_t *r    = record_find_norm(norm);
	free(norm);
	return r;
}

static void record_free(record_t *r) {
	free(r->file);
	free(r->color_space);
	free(r->source);
}

static void record_set(const char *norm, const char *color_space, const char *source, bool is_float, bool linear) {
	record_t *r = record_find_norm(norm);
	if (r == NULL) {
		if (oi.count == oi.cap) {
			int       cap   = oi.cap > 0 ? oi.cap * 2 : 16;
			record_t *grown = realloc(oi.records, (size_t)cap * sizeof(record_t));
			if (grown == NULL) {
				return;
			}
			oi.records = grown;
			oi.cap     = cap;
		}
		r       = &oi.records[oi.count++];
		r->file = dup(norm);
	}
	else {
		free(r->color_space);
		free(r->source);
	}
	r->color_space = dup(color_space);
	r->source      = dup(source);
	r->is_float    = is_float;
	r->linear      = linear;
}

static void record_remove(const char *norm) {
	record_t *r = record_find_norm(norm);
	if (r == NULL) {
		return;
	}
	record_free(r);
	*r = oi.records[--oi.count];
}

static void records_clear(void) {
	for (int i = 0; i < oi.count; i++) {
		record_free(&oi.records[i]);
	}
	oi.count = 0;
}

// Libellé fixe d'une source enregistrée ("default" pour une valeur inconnue).
static const char *source_label(const char *source) {
	static const char *labels[] = {"user", "name", "rule", "default"};
	for (int i = 0; i < 4; i++) {
		if (source != NULL && strcmp(source, labels[i]) == 0) {
			return labels[i];
		}
	}
	return "default";
}

// ── textures décodées hors import ─────────────────────────────────────────────────────────────────────────────────

static int raw_index(const char *norm) {
	for (int i = 0; i < oi.raw_count; i++) {
		if (strcmp(oi.raw[i].norm, norm) == 0) {
			return i;
		}
	}
	return -1;
}

static void raw_remove_at(int i) {
	free(oi.raw[i].key);
	free(oi.raw[i].norm);
	oi.raw[i] = oi.raw[--oi.raw_count];
}

static void raw_add(const char *source) {
	char *norm = normalized(source);
	if (raw_index(norm) >= 0) {
		free(norm);
		return;
	}
	if (oi.raw_count == RAW_MAX) {
		raw_remove_at(0);
	}
	if (oi.raw_count == oi.raw_cap) {
		int    cap   = oi.raw_cap > 0 ? oi.raw_cap * 2 : 16;
		raw_t *grown = realloc(oi.raw, (size_t)cap * sizeof(raw_t));
		if (grown == NULL) {
			free(norm);
			return;
		}
		oi.raw     = grown;
		oi.raw_cap = cap;
	}
	oi.raw[oi.raw_count++] = (raw_t){.key = dup(source), .norm = norm};
}

static void raw_clear(void) {
	while (oi.raw_count > 0) {
		raw_remove_at(oi.raw_count - 1);
	}
}

// Chaque frame : une texture sortie du cache (le navigateur jette l'image après sa vignette) n'est plus à surveiller ;
// une autre texture mise plus tard sous ce chemin (paquet d'un projet) ne sera pas prise pour elle.
static void prune_raw(void *data) {
	(void)data;
	for (int i = oi.raw_count - 1; i >= 0; i--) {
		if (data_cached_textures == NULL || any_map_get(data_cached_textures, oi.raw[i].key) == NULL) {
			raw_remove_at(i);
		}
	}
}

static void delete_texture_now(void *data) {
	gpu_delete_texture((gpu_texture_t *)data);
}

static void delete_texture_next(void *data) {
	sys_notify_on_next_frame(delete_texture_now, data);
}

// Détruite deux frames plus tard : la vignette du navigateur (ui_files_make_icon) la dessine à la frame suivante.
static void delete_texture_later(gpu_texture_t *texture) {
	sys_notify_on_next_frame(delete_texture_next, texture);
}

// ── projet ────────────────────────────────────────────────────────────────────────────────────────────────────────

static packed_asset_t *packed_entry(int *index) {
	if (g_project == NULL || g_project->packed_assets == NULL) {
		return NULL;
	}
	for (int i = 0; i < (int)g_project->packed_assets->length; i++) {
		packed_asset_t *pa = g_project->packed_assets->buffer[i];
		if (pa != NULL && pa->name != NULL && strcmp(pa->name, PACKED_NAME) == 0) {
			if (index != NULL) {
				*index = i;
			}
			return pa;
		}
	}
	return NULL;
}

static char *json_text_at(const char *json, const char *format, int i) {
	char pointer[64];
	snprintf(pointer, sizeof(pointer), format, i);
	return studio_json_text(studio_json_find(json, pointer));
}

// Enregistrements du .arm ouvert : les chemins relatifs le sont au dossier du projet, comme ses textures.
static void load_project_records(void) {
	packed_asset_t *pa    = packed_entry(NULL);
	buffer_t       *bytes = pa != NULL ? (buffer_t *)pa->bytes : NULL; // struct buffer : buffer_t (types.h)
	if (bytes == NULL) {
		return;
	}
	char *json = malloc((size_t)bytes->length + 1);
	if (json == NULL) {
		return;
	}
	memcpy(json, bytes->buffer, bytes->length);
	json[bytes->length] = 0;
	if (studio_json_int(studio_json_find(json, "/version"), 0) != 1) {
		report(true, "studio: %s: unknown version, texture color spaces of the project ignored", PACKED_NAME);
		free(json);
		return;
	}
	const char *filepath = g_project->_->filepath != NULL ? g_project->_->filepath : "";
	const char *sep      = strrchr(filepath, PATH_SEP[0]);
	char       *base     = strndup(filepath, sep != NULL ? (size_t)(sep - filepath) + 1 : 0);
	int         n        = studio_json_length(studio_json_find(json, "/textures"));
	for (int i = 0; i < n; i++) {
		char *file   = json_text_at(json, "/textures/%d/file", i);
		char *cs     = json_text_at(json, "/textures/%d/color_space", i);
		char *source = json_text_at(json, "/textures/%d/source", i);
		char  pointer[64];
		snprintf(pointer, sizeof(pointer), "/textures/%d/float", i);
		bool is_float = studio_json_truthy(studio_json_find(json, pointer));
		snprintf(pointer, sizeof(pointer), "/textures/%d/linear", i);
		bool linear = studio_json_truthy(studio_json_find(json, pointer));
		if (file != NULL && file[0] != 0 && cs != NULL && cs[0] != 0) {
#ifndef IRON_WINDOWS
			for (char *c = file; *c != 0; c++) {
				if (*c == '\\') {
					*c = '/';
				}
			}
#endif
			char *abs  = data_is_abs(file) ? dup(file) : string("%s%s", base, file);
			char *norm = normalized(abs);
			record_set(norm, cs, source_label(source), is_float, linear);
			free(norm);
			free(abs);
		}
		free(file);
		free(cs);
		free(source);
	}
	free(base); // une autre configuration que celle du projet : signalé par studio_ocio_prefs.c
	free(json);
}

static void sync_project(void) {
	if (g_project == NULL || g_project->_ == NULL) {
		return;
	}
	studio_project_check(); // un projet qu'on ouvre : ses modules (espace de travail, A5) avant ses textures
	bool changed     = g_project != oi.project || (void *)g_project->_->assets != oi.assets || g_project->_->next_asset_id < oi.next_asset_id;
	oi.next_asset_id = g_project->_->next_asset_id;
	if (!changed) {
		return;
	}
	oi.project = g_project;
	oi.assets  = g_project->_->assets;
	records_clear();
	raw_clear();
	load_project_records();
}

static asset_t *find_asset(const char *norm) {
	if (g_project == NULL || g_project->_ == NULL || g_project->_->assets == NULL) {
		return NULL;
	}
	for (int i = 0; i < (int)g_project->_->assets->length; i++) {
		asset_t *a = g_project->_->assets->buffer[i];
		if (a == NULL || a->file == NULL) {
			continue;
		}
		char *n     = normalized(a->file);
		bool  match = strcmp(n, norm) == 0;
		free(n);
		if (match) {
			return a;
		}
	}
	return NULL;
}

// ── résolution de l'espace source ─────────────────────────────────────────────────────────────────────────────────

static void ensure_defaults(void) {
	int generation = studio_ocio_generation();
	if (generation == oi.generation) {
		return;
	}
	oi.generation   = generation;
	oi.srgb         = studio_ocio_identify_builtin("srgb_tx");
	oi.linear_srgb  = studio_ocio_identify_builtin(STUDIO_OCIO_WORKING_BUILTIN);
	oi.scene_linear = studio_ocio_resolve("scene_linear");
	oi.data         = studio_ocio_resolve("data");
	if (oi.data == NULL || studio_ocio_is_data(oi.data) != 1) {
		oi.data = NULL;
		for (int i = 0; i < studio_ocio_colorspace_count() && oi.data == NULL; i++) {
			const char *name = studio_ocio_colorspace_name(i);
			if (name != NULL && studio_ocio_is_data(name) == 1) {
				oi.data = name;
			}
		}
	}
}

// Nom de texture de donnée : les listes d'ArmorPaint (iron_path.c, import de dossier) sur le nom sans extension ni
// numéro de tuile UDIM, plus "mask". Un nom de couleur de base l'emporte ("wood_basecolor_normal" reste couleur).
static bool is_data_name(const char *path) {
	const char *slash  = strrchr(path, '/');
	const char *bslash = strrchr(path, '\\');
	const char *name   = slash != NULL && (bslash == NULL || slash > bslash) ? slash + 1 : bslash != NULL ? bslash + 1 : path;
	char       *base   = dup(name);
	char       *dot    = strrchr(base, '.');
	if (dot != NULL) {
		*dot = 0;
	}
	dot = strrchr(base, '.');
	if (dot != NULL && strlen(dot + 1) == 4 && isdigit((unsigned char)dot[1]) && isdigit((unsigned char)dot[2]) && isdigit((unsigned char)dot[3]) &&
	    isdigit((unsigned char)dot[4])) {
		*dot = 0; // .1001
	}
	for (char *c = base; *c != 0; c++) {
		*c = (char)tolower((unsigned char)*c);
	}
	string_array_t *mask = string_array_create(0);
	string_array_push(mask, "mask");
	bool data = !path_is_base_color_tex(base) &&
	            (path_is_normal_map_tex(base) || path_is_occlusion_tex(base) || path_is_roughness_tex(base) || path_is_metallic_tex(base) ||
	             path_is_displacement_tex(base) || path_is_opacity_tex(base) || path_check_ext(base, mask));
	array_free(mask);
	free(mask);
	free(base);
	return data;
}

static bool ocio_on(void) {
	return studio_ocio_ready() && studio_ocio_working_space() != NULL;
}

// Espace choisi avant le décodage : enregistrement, format d'ArmorPaint, nom de donnée, file_rule ; sinon en attente
// du format (défaut au premier décodage).
static void resolve(scope_t *s) {
	s->color_space = NULL;
	s->source      = "";
	s->pending     = false;
	if (!ocio_on()) {
		return;
	}
	ensure_defaults();
	record_t *r = record_find_norm(s->path);
	if (r != NULL) {
		const char *cs = studio_ocio_resolve(r->color_space);
		if (cs != NULL) {
			s->color_space = dup(cs);
			s->source      = source_label(r->source);
			return;
		}
		report(true, "studio: color space \"%s\" of %s is unknown to the OCIO config, automatic choice", r->color_space, s->path);
	}
	if (is_k(s->path)) {
		return;
	}
	if (oi.data != NULL && is_data_name(s->path)) {
		s->color_space = dup(oi.data);
		s->source      = "name";
		return;
	}
	const char *rule = studio_ocio_file_rule(s->path);
	if (rule != NULL && strcmp(rule, "Default") != 0) {
		const char *cs = studio_ocio_resolve(studio_ocio_colorspace_from_path(s->path));
		if (cs != NULL) {
			s->color_space = dup(cs);
			s->source      = "rule";
			return;
		}
	}
	s->pending = true;
	s->source  = "default";
}

// Défaut du format : 8 bits → sRGB, .hdr → Linear Rec.709 (sRGB), autre flottant → scene_linear.
static const char *default_space(const char *path, bool is_float) {
	ensure_defaults();
	if (!is_float) {
		return oi.srgb;
	}
	if (ends_with_ci(path, ".hdr")) {
		return oi.linear_srgb != NULL ? oi.linear_srgb : oi.scene_linear;
	}
	return oi.scene_linear != NULL ? oi.scene_linear : oi.linear_srgb;
}

// ── conversion ────────────────────────────────────────────────────────────────────────────────────────────────────

static float u8_float[256];
static float encode_threshold[256]; // [k] : plus petite valeur linéaire codée k ou plus, ((k - 0.5) / 255)^2.2

static void init_tables(void) {
	for (int i = 0; i < 256; i++) {
		u8_float[i]         = i / 255.0f;
		encode_threshold[i] = i == 0 ? 0.0f : (float)pow((i - 0.5) / 255.0, 2.2);
	}
}

// Linéaire → 8 bits encodé gamma 2.2, arrondi au plus proche : round(pow(x, 1 / 2.2) * 255), borné ; NaN → 0.
static uint8_t encode_u8(float x) {
	if (!(x >= encode_threshold[1])) {
		return 0;
	}
	int lo = 1, hi = 255;
	while (lo < hi) {
		int mid = (lo + hi + 1) / 2;
		if (encode_threshold[mid] <= x) {
			lo = mid;
		}
		else {
			hi = mid - 1;
		}
	}
	return (uint8_t)lo;
}

static float finite_or_zero(float x, float limit) {
	if (!isfinite(x)) {
		return 0.0f;
	}
	return x > limit ? limit : x < -limit ? -limit : x;
}

typedef struct {
	void         *pixels;
	size_t        count;
	int           format;
	const char   *src, *dst;
	volatile bool failed;
} convert_t;

static void convert_job(void *ctx, int job) {
	convert_t *c     = ctx;
	size_t     first = (size_t)job * BAND;
	size_t     n     = c->count - first < BAND ? c->count - first : BAND;
	float     *tmp   = NULL;
	float     *px;
	if (c->format == GPU_TEXTURE_FORMAT_RGBA128) {
		px = (float *)c->pixels + first * 4;
	}
	else {
		tmp = malloc(n * 4 * sizeof(float));
		if (tmp == NULL) {
			c->failed = true;
			return;
		}
		px = tmp;
		if (c->format == GPU_TEXTURE_FORMAT_RGBA32) {
			const uint8_t *p = (const uint8_t *)c->pixels + first * 4;
			for (size_t i = 0; i < n * 4; i++) {
				px[i] = u8_float[p[i]];
			}
		}
		else {
			const uint16_t *p = (const uint16_t *)c->pixels + first * 4;
			for (size_t i = 0; i < n * 4; i++) {
				px[i] = studio_ocio_half_to_float(p[i]);
			}
		}
	}
	if (!studio_ocio_transform(px, (int)n, 1, 4, c->src, c->dst)) {
		c->failed = true;
		free(tmp);
		return;
	}
	for (size_t i = 0; i < n; i++) { // alpha intact
		for (int k = 0; k < 3; k++) {
			size_t j = i * 4 + k;
			if (c->format == GPU_TEXTURE_FORMAT_RGBA32) {
				((uint8_t *)c->pixels)[first * 4 + j] = encode_u8(px[j]);
			}
			else if (c->format == GPU_TEXTURE_FORMAT_RGBA64) {
				((uint16_t *)c->pixels)[first * 4 + j] = studio_ocio_half_from_float(finite_or_zero(px[j], 65504.0f));
			}
			else {
				px[j] = finite_or_zero(px[j], INFINITY);
			}
		}
	}
	free(tmp);
}

static int thread_count(void) {
	int n = iron_hardware_threads();
	return n > 0 ? n : 1;
}

// Pixels RGBA décodés, de src vers l'espace de travail ; en 8 bits, encodés gamma 2.2 ensuite. Un espace de donnée
// n'est pas converti, un flottant déjà dans l'espace de travail non plus.
static bool convert(void *pixels, int width, int height, int format, const char *src, const char *path) {
	const char *working = studio_ocio_working_space();
	if (working == NULL || studio_ocio_is_data(src) == 1) {
		return working != NULL;
	}
	if (format != GPU_TEXTURE_FORMAT_RGBA32 && strcmp(src, working) == 0) {
		return true;
	}
	if (studio_ocio_transform_id(src, working) == NULL) {
		report(true, "studio: %s: no OCIO transform from \"%s\" to \"%s\", texture not converted", path, src, working);
		return false;
	}
	convert_t c    = {.pixels = pixels, .count = (size_t)width * height, .format = format, .src = src, .dst = working};
	int       jobs = (int)((c.count + BAND - 1) / BAND);
	if (jobs > 0) {
		exr_parallel_for(thread_count(), jobs, convert_job, &c);
	}
	if (c.failed) {
		report(true, "studio: %s: OCIO conversion from \"%s\" failed", path, src);
		return false;
	}
	return true;
}

// Un décodage de l'import en cours (source NULL : octets bruts d'un importeur ; extension : octets encodés ; chemin :
// le fichier). Une ressource .k ou un autre fichier décodé pendant l'appel ne sont pas à lui.
static bool scope_owns(const scope_t *s, const char *source) {
	if (source == NULL) {
		return true;
	}
	if (is_k(source)) {
		return false;
	}
	if (!is_path(source)) {
		return true;
	}
	char *norm = normalized(source);
	bool  own  = strcmp(norm, s->path) == 0;
	free(norm);
	return own;
}

static void on_decoded(void *pixels, i32 width, i32 height, i32 format, char *source) {
	if (pixels == NULL || width <= 0 || height <= 0) {
		return;
	}
	if (oi.depth == 0) {
		if (is_path(source) && !is_k(source)) {
			raw_add(source);
		}
		return;
	}
	scope_t *s = &oi.scopes[oi.depth - 1];
	if (!scope_owns(s, source) || (format != GPU_TEXTURE_FORMAT_RGBA32 && format != GPU_TEXTURE_FORMAT_RGBA64 && format != GPU_TEXTURE_FORMAT_RGBA128)) {
		return;
	}
	if (is_path(source)) {
		int i = raw_index(s->path);
		if (i >= 0) {
			raw_remove_at(i);
		}
	}
	bool is_float = format != GPU_TEXTURE_FORMAT_RGBA32;
	if (s->decodes++ == 0) {
		s->is_float = is_float;
		if (s->pending) {
			s->pending     = false;
			const char *cs = default_space(s->path, is_float);
			s->color_space = cs != NULL ? dup(cs) : NULL;
			if (cs == NULL) {
				report(true, "studio: %s: the OCIO config has no default %s color space, texture not converted", s->path, is_float ? "linear" : "sRGB");
				s->failed = true;
			}
		}
		s->linear = is_float && s->color_space != NULL && studio_ocio_is_data(s->color_space) != 1;
	}
	if (s->color_space == NULL) {
		return;
	}
	if (!convert(pixels, width, height, format, s->color_space, s->path)) {
		s->failed = true;
	}
}

// Une texture du navigateur de fichiers (décodée pour sa vignette, pas convertie) encore en cache : retirée, pour que
// l'import la décode à nouveau.
static void evict_raw(char *path) {
	if (data_cached_textures == NULL || oi.raw_count == 0) {
		return;
	}
	gpu_texture_t *cached = any_map_get(data_cached_textures, path);
	if (cached == NULL) {
		return;
	}
	char *norm = normalized(path);
	int   i    = raw_index(norm);
	free(norm);
	if (i < 0) {
		return;
	}
	raw_remove_at(i);
	map_delete(data_cached_textures, path);
	delete_texture_later(cached);
}

gpu_texture_t *studio_texture_import(char *path, gpu_texture_t *(*fn)(char *path)) {
	if (!oi.enabled || path == NULL) {
		return fn(path);
	}
	sync_project();
	evict_raw(path);
	if (oi.depth == SCOPE_DEPTH) {
		report(true, "studio: %s: too many nested texture imports, not converted", path);
		return fn(path);
	}
	scope_t *s = &oi.scopes[oi.depth++];
	memset(s, 0, sizeof(*s));
	s->path = normalized(path);
	resolve(s);
	gpu_texture_t *image = fn(path);
	scope_t        done  = oi.scopes[--oi.depth];
	if (image != NULL && done.decodes == 0 && oi.depth > 0) {
		// Texture mise en cache par l'import qui l'appelle (calques d'un PSD) : décodée dans cet import, avec son espace.
		scope_t *parent = &oi.scopes[oi.depth - 1];
		if (parent->decodes > 0 && !parent->failed && parent->color_space != NULL) {
			free(done.color_space);
			done.color_space = dup(parent->color_space);
			done.source      = parent->source;
			done.decodes     = parent->decodes;
			done.is_float    = parent->is_float;
			done.linear      = parent->linear;
		}
	}
	if (image != NULL && done.decodes > 0 && !done.failed && done.color_space != NULL) {
		record_set(done.path, done.color_space, done.source, done.is_float, done.linear);
		iron_log("studio: texture %s: %s (%s)%s", done.path, done.color_space, done.source,
		         studio_ocio_is_data(done.color_space) == 1 ? ", data, not converted" : "");
	}
	free(done.path);
	free(done.color_space);
	return image;
}

// ── API ───────────────────────────────────────────────────────────────────────────────────────────────────────────

bool studio_ocio_import_enabled(void) {
	return oi.enabled;
}

const char *studio_texture_color_space(const char *file) {
	if (!oi.enabled || !ocio_on()) {
		return "";
	}
	sync_project();
	record_t *r = record_find(file);
	return r != NULL ? r->color_space : "";
}

const char *studio_texture_color_space_source(const char *file) {
	if (!oi.enabled || !ocio_on()) {
		return "";
	}
	sync_project();
	record_t *r = record_find(file);
	return r != NULL ? r->source : "";
}

bool studio_texture_is_linear(char *file) {
	if (!oi.enabled) {
		return false;
	}
	sync_project();
	record_t *r = record_find(file);
	return r != NULL && r->linear;
}

typedef struct {
	char *envmap; // texture rechargée qui était l'environnement, NULL sinon
} reloaded_t;

// Frame suivante : environnement, matériaux, calques de remplissage et aperçus refaits avec les nouvelles textures.
static void after_reload(void *data) {
	reloaded_t    *r     = data;
	gpu_texture_t *image = r->envmap != NULL && data_cached_textures != NULL ? any_map_get(data_cached_textures, r->envmap) : NULL;
	if (image != NULL) {
		float strength = scene_world->strength;
		bool  show     = g_context->show_envmap;
		import_envmap_run(r->envmap, image);
		scene_world->strength  = strength;
		g_context->show_envmap = show;
	}
	slot_material_t *material = g_context->material;
	for (int i = 0; i < (int)g_project->_->layers->length; i++) {
		slot_material_t *m    = g_project->_->layers->buffer[i]->fill_material;
		bool             seen = m == NULL;
		for (int j = 0; j < i && !seen; j++) {
			seen = g_project->_->layers->buffer[j]->fill_material == m;
		}
		if (!seen) {
			g_context->material = m;
			layers_update_fill_layers();
		}
	}
	g_context->material = material;
	make_material_parse_paint_material(true);
	make_material_parse_brush();
	util_render_make_material_preview();
	g_context->ddirty = 2;
	free(r->envmap);
	free(r);
}

static void schedule_after_reload(const char *envmap) {
	reloaded_t *r = malloc(sizeof(reloaded_t));
	if (r != NULL) {
		r->envmap = envmap != NULL ? dup(envmap) : NULL;
		sys_notify_on_next_frame(after_reload, r);
	}
}

// Décode à nouveau la texture depuis son fichier avec l'espace enregistré. L'ancienne reste en place si ça échoue.
// *was_envmap : elle était l'environnement ; after_reload est à programmer par l'appelant.
static bool reload(asset_t *asset, bool *was_envmap) {
	char          *file = asset->file;
	gpu_texture_t *old  = data_cached_textures != NULL ? any_map_get(data_cached_textures, file) : NULL;
	if (old == NULL) {
		old = asset->image;
	}
	const char *dot                    = strrchr(file, '.');
	gpu_texture_t *(*importer)(char *) = dot != NULL ? any_map_get(import_texture_importers, (char *)dot + 1) : NULL;
	if (importer == NULL) {
		importer = import_texture_default_importer;
	}
	// Un script tourne pendant une frame : on suspend le dessin en cours, comme script_gpu_begin (minic_impl.c).
	gpu_texture_t *current = _draw_current;
	bool           in_use  = gpu_in_use;
	if (in_use) {
		draw_end();
	}
	if (data_cached_textures != NULL && any_map_get(data_cached_textures, file) != NULL) {
		map_delete(data_cached_textures, file);
	}
	gpu_texture_t *image = studio_texture_import(file, importer);
	if (in_use) {
		draw_begin(current, false, 0);
	}
	if (image == NULL) {
		if (old != NULL) {
			any_map_set(data_cached_textures, file, old);
		}
		report(true, "studio: could not reload %s", file);
		return false;
	}
	any_map_set(data_cached_textures, file, image);
	asset->image = image;
	// Copie du paquet faite avec l'ancien espace : retirée, le prochain enregistrement avec « Pack Assets » la refait.
	for (int i = 0; g_project->packed_assets != NULL && i < (int)g_project->packed_assets->length; i++) {
		packed_asset_t *pa = g_project->packed_assets->buffer[i];
		if (pa != NULL && pa->name != NULL && strcmp(pa->name, file) == 0) {
			array_splice((any_array_t *)g_project->packed_assets, i, 1);
			break;
		}
	}
	*was_envmap = old != NULL && g_context->saved_envmap == old;
	if (old != NULL && old != image) {
		delete_texture_later(old);
	}
	g_context->ddirty = 2;
	return true;
}

bool studio_texture_set_color_space(const char *file, const char *color_space) {
	if (!oi.enabled) {
		report(true, "studio: texture color spaces are disabled (STUDIO_OCIO_IMPORT=0)");
		return false;
	}
	if (file == NULL || file[0] == 0) {
		return false;
	}
	if (!ocio_on()) {
		report(true, "studio: OCIO unavailable, texture color space not changed: %s", studio_ocio_status());
		return false;
	}
	sync_project();
	const char *cs = NULL;
	if (color_space != NULL && color_space[0] != 0) {
		cs = studio_ocio_resolve(color_space);
		if (cs == NULL) {
			report(true, "studio: unknown OCIO color space \"%s\"", color_space);
			return false;
		}
	}
	char     *norm  = normalized(file);
	asset_t  *asset = find_asset(norm);
	record_t *r     = record_find_norm(norm);
	if (asset == NULL) { // pas encore importée : le choix vaudra pour son import
		if (cs != NULL) {
			record_set(norm, cs, "user", false, false);
		}
		else {
			record_remove(norm);
		}
		free(norm);
		return true;
	}
	if (r != NULL && cs != NULL && strcmp(r->source, "user") == 0 && strcmp(r->color_space, cs) == 0) {
		free(norm);
		return true;
	}
	if (!iron_file_exists(asset->file)) {
		report(true,
		       "studio: %s is not on disk (packed in the project, or layer of an imported file): its pixels are already converted, "
		       "import the file again to change its color space",
		       asset->file);
		free(norm);
		return false;
	}
	record_t saved = {0};
	bool     had   = r != NULL;
	if (had) {
		saved = (record_t){dup(r->file), dup(r->color_space), dup(r->source), r->is_float, r->linear};
	}
	if (cs != NULL) {
		record_set(norm, cs, "user", false, false);
	}
	else {
		record_remove(norm);
	}
	bool was_envmap = false;
	bool ok         = reload(asset, &was_envmap);
	if (ok) {
		schedule_after_reload(was_envmap ? asset->file : NULL);
	}
	else {
		if (had) {
			record_set(norm, saved.color_space, saved.source, saved.is_float, saved.linear);
		}
		else {
			record_remove(norm);
		}
	}
	if (had) {
		record_free(&saved);
	}
	free(norm);
	return ok;
}

int studio_ocio_import_reconvert(void) {
	if (!oi.enabled || g_project == NULL || g_project->_ == NULL || !ocio_on()) {
		return 0;
	}
	sync_project();
	asset_t_array_t *assets = g_project->_->assets;
	int              done = 0, kept = 0;
	const char      *envmap = NULL;
	for (int i = 0; i < (int)assets->length; i++) {
		asset_t  *a = assets->buffer[i];
		record_t *r = a != NULL && a->file != NULL ? record_find(a->file) : NULL;
		if (a == NULL || a->file == NULL || is_k(a->file) || (r != NULL && studio_ocio_is_data(r->color_space) == 1)) {
			continue; // pas convertie : rien ne dépend de l'espace de travail
		}
		if (!iron_file_exists(a->file)) {
			kept++;
			continue;
		}
		bool was_envmap = false;
		if (reload(a, &was_envmap)) {
			done++;
			envmap = was_envmap ? a->file : envmap;
		}
	}
	if (done > 0) {
		schedule_after_reload(envmap);
	}
	if (kept > 0) {
		report(true,
		       "studio: %d texture(s) only in the asset pack of the project keep their pixels: import their files again to convert them to the working space",
		       kept);
	}
	return done;
}

// Appelé avant l'encodage du projet (studio_project_on_save) : texture_files est aligné sur g_project->_->assets,
// chemins tels qu'écrits dans le .arm.
static void color_spaces_save(string_array_t *texture_files) {
	if (!oi.enabled || g_project == NULL || g_project->_ == NULL || texture_files == NULL) {
		return;
	}
	sync_project();
	asset_t_array_t *assets = g_project->_->assets;
	size_t           cap = 256, len = 0;
	char            *json    = malloc(cap);
	int              written = 0;
	if (json == NULL) {
		return;
	}
	char *config  = studio_json_quote(studio_ocio_ready() ? studio_ocio_config_name() : "");
	char *working = studio_json_quote(studio_ocio_working_space() != NULL ? studio_ocio_working_space() : "");
	for (int i = -1; i < (int)assets->length && i < (int)texture_files->length; i++) {
		char *entry;
		if (i < 0) {
			entry = string("{\n\t\"version\": 1,\n\t\"config\": %s,\n\t\"working_space\": %s,\n\t\"textures\": [", config, working);
		}
		else {
			record_t *r = record_find(assets->buffer[i]->file);
			if (r == NULL) {
				continue;
			}
			char *file   = studio_json_quote(texture_files->buffer[i]);
			char *cs     = studio_json_quote(r->color_space);
			char *source = studio_json_quote(r->source);
			entry = string("%s\n\t\t{\"file\": %s, \"color_space\": %s, \"source\": %s, \"float\": %s, \"linear\": %s}", written > 0 ? "," : "", file, cs,
			               source, r->is_float ? "true" : "false", r->linear ? "true" : "false");
			free(file);
			free(cs);
			free(source);
			written++;
		}
		size_t n = strlen(entry);
		if (len + n + 16 > cap) {
			while (len + n + 16 > cap) {
				cap *= 2;
			}
			char *grown = realloc(json, cap);
			if (grown == NULL) {
				free(entry);
				free(json);
				free(config);
				free(working);
				return;
			}
			json = grown;
		}
		memcpy(json + len, entry, n);
		len += n;
		free(entry);
	}
	memcpy(json + len, "\n\t]\n}\n", 7);
	len += 6;
	free(config);
	free(working);

	int             index = -1;
	packed_asset_t *pa    = packed_entry(&index);
	if (written == 0) {
		if (pa != NULL) {
			array_splice((any_array_t *)g_project->packed_assets, index, 1);
		}
		free(json);
		return;
	}
	buffer_t *bytes = buffer_create((uint32_t)len);
	memcpy(bytes->buffer, json, len);
	free(json);
	if (pa != NULL) {
		pa->bytes = (struct buffer *)bytes; // l'ancienne peut venir du décodage du projet (bloc unique) : pas libérée
		return;
	}
	if (g_project->packed_assets == NULL) {
		g_project->packed_assets = (packed_asset_t_array_t *)any_array_create_from_raw((void *[]){}, 0); // comme project_new
	}
	pa        = malloc(sizeof(packed_asset_t));
	pa->name  = dup(PACKED_NAME);
	pa->bytes = (struct buffer *)bytes;
	any_array_push((any_array_t *)g_project->packed_assets, pa);
}

// ── interface ─────────────────────────────────────────────────────────────────────────────────────────────────────

static const char *source_text(const char *source) {
	return strcmp(source, "user") == 0 ? "chosen" : strcmp(source, "name") == 0 ? "data name" : strcmp(source, "rule") == 0 ? "file rule" : "default";
}

char *studio_texture_tooltip(char *file, char *tooltip) {
	const char *cs = studio_texture_color_space(file);
	if (cs[0] == 0) {
		return tooltip;
	}
	return string_tmp("%s - %s (%s)", tooltip, cs, source_text(studio_texture_color_space_source(file)));
}

static void ensure_names(void) {
	int generation = studio_ocio_generation();
	if (oi.names != NULL && oi.names_generation == generation) {
		return;
	}
	if (oi.names != NULL) {
		for (int i = 0; i < (int)oi.names->length; i++) {
			free(oi.names->buffer[i]);
		}
		array_free(oi.names);
		free(oi.names);
	}
	oi.names            = string_array_create(0);
	oi.names_generation = generation;
	string_array_push(oi.names, dup("Automatic"));
	for (int i = 0; i < studio_ocio_colorspace_count(); i++) {
		string_array_push(oi.names, dup(studio_ocio_colorspace_name(i)));
	}
}

typedef struct {
	char *file;
	char *color_space; // NULL : automatique
} change_t;

static void apply_change(void *data) {
	change_t *c = data;
	studio_texture_set_color_space(c->file, c->color_space); // erreurs dans la console
	free(c->file);
	free(c->color_space);
	free(c);
}

static void box_draw(void) {
	sync_project();
	if (!oi.enabled) {
		ui_text("Texture color spaces are disabled (STUDIO_OCIO_IMPORT=0).", UI_ALIGN_LEFT, 0);
	}
	else if (!ocio_on()) {
		ui_text(string_tmp("OCIO unavailable: %s", studio_ocio_status()), UI_ALIGN_LEFT, 0);
	}
	else {
		ensure_names();
		ui_text(string_tmp("Working space: %s", studio_ocio_working_space()), UI_ALIGN_LEFT, 0);
		asset_t_array_t *assets = g_project->_->assets;
		if ((int)assets->length > oi.choices_cap) {
			int *grown = realloc(oi.choices, assets->length * sizeof(int));
			if (grown == NULL) {
				return;
			}
			oi.choices     = grown;
			oi.choices_cap = assets->length;
		}
		int shown = 0;
		for (int i = 0; i < (int)assets->length; i++) {
			asset_t *a = assets->buffer[i];
			if (oi.box_file != NULL && strcmp(a->file, oi.box_file) != 0) {
				continue;
			}
			shown++;
			record_t   *r      = record_find(a->file);
			bool        exists = iron_file_exists(a->file);
			const char *state  = r != NULL ? string_tmp("%s (%s)", r->color_space, source_text(r->source)) : "not converted";
			oi.choices[i]      = 0;
			if (r != NULL && strcmp(r->source, "user") == 0) {
				for (int j = 1; j < (int)oi.names->length; j++) {
					if (strcmp(oi.names->buffer[j], r->color_space) == 0) {
						oi.choices[i] = j;
					}
				}
			}
			ui_row(f32_array_create_from_raw_tmp((f32[]){0.3f, 0.35f, 0.35f}, 3));
			ui_text(a->name, UI_ALIGN_LEFT, 0);
			ui_text(exists ? (char *)state : string_tmp("%s, packed", state), UI_ALIGN_LEFT, 0);
			g_ui->enabled = exists;
			ui_combo(&oi.choices[i], oi.names, "Color Space", false, UI_ALIGN_LEFT, true);
			if (exists && ui_item_changed()) {
				change_t *c = malloc(sizeof(change_t));
				if (c != NULL) {
					c->file        = dup(a->file);
					c->color_space = oi.choices[i] > 0 ? dup(oi.names->buffer[oi.choices[i]]) : NULL;
					sys_notify_on_next_frame(apply_change, c);
				}
			}
			g_ui->enabled = true;
		}
		if (shown == 0) {
			ui_text("No texture in the project.", UI_ALIGN_LEFT, 0);
		}
	}
	ui_end_element();
	if (ui_icon_button("Close", OI_ICON_CLOSE, UI_ALIGN_CENTER)) {
		ui_box_hide();
	}
}

static void show_box(const char *file) {
	free(oi.box_file);
	oi.box_file = file != NULL ? dup(file) : NULL;
	ui_box_show_custom(box_draw, 720, file != NULL ? 180 : 480, NULL, true, file != NULL ? "Texture Color Space" : "Texture Color Spaces");
}

// Menu contextuel de l'onglet Textures (STUDIO-PATCH(ocio-import) de ui/tab_textures.c).
void studio_texture_color_space_box(asset_t *asset) {
	if (asset != NULL) {
		show_box(asset->file);
	}
}

void studio_ocio_import_menu_draw(void) {
	if (oi.enabled && ui_menu_button("Texture Color Spaces...", "", OI_ICON_NONE)) {
		show_box(NULL);
	}
}

// ── scripts ───────────────────────────────────────────────────────────────────────────────────────────────────────

// Pixels de la texture en cache pour ce chemin, pour les bancs : "STX1", largeur, hauteur, format (int32 LE), puis
// les pixels tels que relus du GPU.
static bool dump_texture(const char *file, const char *out) {
	gpu_texture_t *t = data_cached_textures != NULL ? any_map_get(data_cached_textures, (char *)file) : NULL;
	if (t == NULL) {
		return false;
	}
	gpu_texture_t *current = _draw_current;
	bool           in_use  = gpu_in_use;
	if (in_use) {
		draw_end();
	}
	buffer_t *px = gpu_get_texture_pixels(t);
	if (in_use) {
		draw_begin(current, false, 0);
	}
	FILE *f = px != NULL ? fopen(out, "wb") : NULL;
	if (f == NULL) {
		return false;
	}
	int32_t header[3] = {(int32_t)t->width, (int32_t)t->height, (int32_t)t->format};
	bool    ok        = fwrite("STX1", 1, 4, f) == 4 && fwrite(header, sizeof(header), 1, f) == 1 && fwrite(px->buffer, 1, px->length, f) == (size_t)px->length;
	return fclose(f) == 0 && ok;
}

#ifdef WITH_EVAL
#include "minic.h"

static minic_val_t mn_enabled(minic_val_t *a, int n) {
	return minic_val_int(studio_ocio_import_enabled() ? 1 : 0);
}

static minic_val_t mn_color_space(minic_val_t *a, int n) {
	return minic_val_ptr((void *)studio_texture_color_space(minic_arg_p(a, n, 0)));
}

static minic_val_t mn_color_space_source(minic_val_t *a, int n) {
	return minic_val_ptr((void *)studio_texture_color_space_source(minic_arg_p(a, n, 0)));
}

static minic_val_t mn_is_linear(minic_val_t *a, int n) {
	return minic_val_int(studio_texture_is_linear(minic_arg_p(a, n, 0)) ? 1 : 0);
}

static minic_val_t mn_set_color_space(minic_val_t *a, int n) {
	return minic_val_int(studio_texture_set_color_space(minic_arg_p(a, n, 0), minic_arg_p(a, n, 1)) ? 1 : 0);
}

static minic_val_t mn_dump(minic_val_t *a, int n) {
	return minic_val_int(dump_texture(minic_arg_p(a, n, 0), minic_arg_p(a, n, 1)) ? 1 : 0);
}

// Option Pack Assets du menu File, que minic_api.c n'expose pas : les textures voyagent dans le .arm, avec leur espace.
// Renvoie la valeur d'avant ; -1 la lit sans la changer.
static minic_val_t mn_pack_assets(minic_val_t *a, int n) {
	bool was = g_context->pack_assets_on_save;
	int  on  = minic_arg_i(a, n, 0);
	if (on >= 0) {
		g_context->pack_assets_on_save = on != 0;
	}
	return minic_val_int(was ? 1 : 0);
}

static minic_val_t mn_reconvert(minic_val_t *a, int n) {
	return minic_val_int(studio_ocio_import_reconvert());
}

static void register_script_api(void) {
	// Signatures au format de minic_register ; la référence est dans README.md.
	minic_register("studio_ocio_import_enabled", "i()", mn_enabled);
	minic_register("studio_ocio_import_reconvert", "i()", mn_reconvert);
	minic_register("studio_texture_color_space", "p(p)", mn_color_space);
	minic_register("studio_texture_color_space_source", "p(p)", mn_color_space_source);
	minic_register("studio_texture_is_linear", "i(p)", mn_is_linear);
	minic_register("studio_texture_set_color_space", "i(p,p)", mn_set_color_space);
	minic_register("studio_texture_dump", "i(p,p)", mn_dump);
	minic_register("studio_project_pack_assets", "i(i)", mn_pack_assets);
}
#endif

void studio_ocio_import_init(void) {
	const char *env = getenv("STUDIO_OCIO_IMPORT");
	oi.enabled      = env == NULL || strcmp(env, "0") != 0;
	init_tables();
	if (oi.enabled) {
		iron_texture_decoded = on_decoded;
		sys_notify_on_update(prune_raw, NULL);
		studio_project_on_save(color_spaces_save);
	}
	iron_log("studio: texture color spaces %s", oi.enabled ? "on" : "off (STUDIO_OCIO_IMPORT=0)");
#ifdef WITH_EVAL
	register_script_api();
#endif
}
