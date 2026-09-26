#include "studio_export.h"
// Pas <iron.h> : il définit des fonctions, déjà compilées par main.c (symboles en double au lien).
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
#include "studio_exr.h"
#include "studio_exr_codec.h"
#include "studio_fs.h"
#include "studio_image.h"
#include "studio_json.h"
#include "studio_naming.h"
#include "studio_ocio.h"
#include "studio_preflight.h"
#include "studio_preset.h"
#include "studio_sha256.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef _WIN32
#define SEP      "\\"
#define LIST_SEP ';'
#else
#define SEP      "/"
#define LIST_SEP ':'
#endif

// Cœur ArmorPaint (paint/sources, déclarés dans functions.h et globals.h, qu'on ne peut inclure ici)
extern context_t       *g_context;
extern project_t       *g_project;
extern gpu_texture_t   *layers_expa;
extern gpu_texture_t   *layers_expb;
extern gpu_texture_t   *layers_expc;
extern string_array_t  *box_export_files;
extern i32              box_export_preset_index;
extern char            *manifest_version;
extern char            *ui_files_filename;
extern bool             args_export_textures;
extern bool             args_background;
extern char            *args_export_textures_preset;
buffer_t               *gpu_get_texture_pixels(gpu_texture_t *image); // défini dans iron.h
bool                    slot_layer_is_visible(slot_layer_t *raw);
bool                    slot_layer_is_layer(slot_layer_t *raw);
slot_layer_t_array_t   *slot_layer_get_filters(slot_layer_t *raw, bool include_group_filters);
i32                     config_get_texture_res_x(void);
i32                     config_get_texture_res_y(void);
char                   *config_get_sha(void);
char                   *project_filepath_get(void);
i32                     util_mesh_udim_tile(char *name);
i32                     util_mesh_udim_slot(i32 tile);
bool                    util_mesh_udim_active(void);
void                    export_texture_run(char *path, bool bake_material);
void                    console_info(char *s);
void                    console_error(char *s);
void                    ui_box_show_message(char *title, char *text, bool copyable);
// tinyexr (vendor/.tinyexr/exr_internal.h) : pool de threads spawn/join.
void exr_parallel_for(int nthreads, int njobs, void (*fn)(void *ctx, int job), void *ctx);
// stb_image_write, compilé dans base/sources/iron_image.c
int stbi_write_jpg_to_func(void (*func)(void *context, void *data, int size), void *context, int x, int y, int comp,
                           const void *data, int quality);

// ---- Messages --------------------------------------------------------------------------------------------------------

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

// Export piloté par la fenêtre d'export (studio_export_request) : elle affiche elle-même rapport et erreur.
static bool quiet;
static char last_error[2048];

// Texte long (rapport) : dans le log en entier, ligne par ligne, puis en boîte de dialogue hors --background.
static void show_text(const char *title, const char *text, bool error) {
	const char *p = text;
	while (*p != 0) {
		const char *nl  = strchr(p, '\n');
		size_t      len = nl != NULL ? (size_t)(nl - p) : strlen(p);
		iron_log("studio: %.*s", (int)len, p);
		p += len + (nl != NULL ? 1 : 0);
	}
	report(error, "%s", title);
	if (error) {
		snprintf(last_error, sizeof(last_error), "%s\n%s", title, text);
	}
	if (!args_background && !quiet) {
		ui_box_show_message((char *)title, (char *)text, true);
	}
}

// ---- Registre des presets --------------------------------------------------------------------------------------------

// Presets studio : les fichiers de STUDIO_EXPORT_PRESETS (dossiers ou .json), puis data/studio_preset_<nom>.json. Ils
// sont seulement lus, relus à chaque export et à chaque ouverture de la fenêtre : rien n'est écrit ni installé (les
// presets upstream d'export_presets/ ne sont pas touchés). Ceux que l'artiste enregistre vivent dans le sidecar du
// projet, que tient la fenêtre d'export (studio_export_set_preset).
typedef struct {
	char *name;
	char *path;
	bool  shipped; // livré avec l'app (data/studio_preset_<nom>.json)
} entry_t;

static entry_t *entries     = NULL;
static int      entry_count = 0;

static entry_t *find_entry(const char *name) {
	for (int i = 0; name != NULL && i < entry_count; ++i) {
		if (strcmp(entries[i].name, name) == 0) {
			return &entries[i];
		}
	}
	return NULL;
}

static bool ends_with_nocase(const char *s, const char *suffix) {
	size_t n = strlen(s), m = strlen(suffix);
	if (m > n) {
		return false;
	}
	for (size_t i = 0; i < m; ++i) {
		char a = s[n - m + i], b = suffix[i];
		a      = a >= 'A' && a <= 'Z' ? a + 32 : a;
		if (a != b) {
			return false;
		}
	}
	return true;
}

static void add_entry(const char *name, const char *path, bool shipped) {
	if (name[0] == 0 || find_entry(name) != NULL) { // le premier trouvé l'emporte
		return;
	}
	entry_t *grow = realloc(entries, sizeof(entry_t) * (size_t)(entry_count + 1));
	if (grow == NULL) {
		return;
	}
	entries = grow;
	// path_data() donne ".../out//./data//" : chemin nettoyé pour les messages et le manifeste.
	char *clean = strdup(path);
	for (char *r = clean, *w = clean; clean != NULL; ++r) {
		if (r[0] == '/' && r[1] == '.' && r[2] == '/') {
			r++;
			continue;
		}
		if (r[0] == '/' && r[1] == '/' && r > clean) {
			continue;
		}
		*w++ = *r;
		if (*r == 0) {
			break;
		}
	}
	entries[entry_count++] = (entry_t){.name = strdup(name), .path = clean, .shipped = shipped};
}

// Nom de fichier sans dossier ni ".json".
static char *stem(const char *path) {
	const char *base = path;
	for (const char *p = path; *p != 0; ++p) {
		if (*p == '/' || *p == '\\') {
			base = p + 1;
		}
	}
	size_t len = strlen(base);
	char  *s   = strdup(base);
	if (s != NULL && len >= 5) {
		s[len - 5] = 0;
	}
	return s;
}

static void scan_dir(const char *dir, const char *prefix) {
	any_array_t *files = file_read_directory((char *)dir);
	for (int i = 0; files != NULL && i < (int)files->length; ++i) {
		const char *f = files->buffer[i];
		if (!ends_with_nocase(f, ".json") || strncmp(f, prefix, strlen(prefix)) != 0) {
			continue;
		}
		char *path = string_tmp("%s%s%s", dir, SEP, f);
		char *name = stem(f + strlen(prefix));
		if (name != NULL) {
			add_entry(name, path, prefix[0] != 0);
		}
		free(name);
	}
}

static void clear_entries(void) {
	for (int i = 0; i < entry_count; ++i) {
		free(entries[i].name);
		free(entries[i].path);
	}
	free(entries);
	entries     = NULL;
	entry_count = 0;
}

static void scan_presets(void) {
	clear_entries();
	const char *env = getenv("STUDIO_EXPORT_PRESETS");
	if (env != NULL) {
		char *list = strdup(env);
		for (char *item = list; item != NULL && *item != 0;) {
			char *next = strchr(item, LIST_SEP);
			if (next != NULL) {
				*next = 0;
			}
			if (studio_fs_is_dir(item)) {
				scan_dir(item, "");
			}
			else if (ends_with_nocase(item, ".json") && studio_fs_exists(item)) {
				char *name = stem(item);
				add_entry(name, item, false);
				free(name);
			}
			else if (item[0] != 0) {
				iron_log("studio: STUDIO_EXPORT_PRESETS: %s is neither a folder nor a .json file", item);
			}
			item = next != NULL ? next + 1 : NULL;
		}
		free(list);
	}
	scan_dir(path_data(), "studio_preset_");
}

// ---- État d'un export ------------------------------------------------------------------------------------------------

typedef enum { PHASE_IDLE, PHASE_PLANNING, PHASE_WRITING } phase_t;

typedef struct {
	char    *tmp;
	char    *path;
	uint64_t size;
	char     sha[65];
	int      plan_index;
} staged_t;

static struct {
	phase_t          phase;
	bool             preflight_only; // studio_export_preflight : s'arrêter après le rapport
	char            *override;       // preset imposé par un script
	bool             args_consumed;  // --export-textures : le preset de la ligne de commande ne sert qu'une fois
	studio_preset_t *preset;
	char            *overrides; // chosen.overrides appliqués à ce preset, NULL sinon
	char            *naming;    // chosen.naming appliqué à ce preset, NULL sinon
	char            *dir;

	studio_plan_file_t *files;
	int                 file_count;
	char              **tiles;
	int                 tile_count;
	int                 tiles_without_slot; // au-delà des 64 cases de l'atlas (patch udim-tile-limit)
	const char         *first_without_slot; // dans tiles
	int                 layer_count;
	int                 hidden_layers;
	bool                layers_counted;
	char               *naming_error;
	char               *file_base;
	char               *project_name; // file_base sans version ("robot_v003" -> "robot")
	char               *version;      // "003", "" sans version
	int                 source_bits;

	staged_t *staged;
	int       staged_count;
	bool      failed;
	char      error[1024];
	char    **notes; // constats de l'écriture (textures uniformes...), repris dans le manifeste
	int       note_count;

	char *last_report; // JSON du dernier préflight
	bool  last_ok;
} ex;

// Preset studio de l'export courant (fenêtre d'export, script) : repris par Ctrl+E. json : un preset qui n'est pas dans
// les dossiers de presets (sidecar du projet), source : d'où il vient (manifeste). overrides : bits et résolution par
// texture (studio_preset_apply_overrides). naming : modèle de nom à la place de celui du preset.
static struct {
	char *name;
	char *json;
	char *source;
	char *overrides;
	char *naming;
} chosen = {0};

// Espace imposé aux textures de couleur des presets studio (fenêtre d'export, STUDIO_EXPORT_COLOR_SPACE, script) ; NULL :
// celui du preset. Gardé pour la session : les préférences le porteront au lot A5.
static char *color_override = NULL;

// Fichiers décochés (fenêtre d'export, script) : "\n<texture>\t<variant>\t<tuile>\n..." ; NULL : tous. Gardé pour Ctrl+E.
static char *skip_keys = NULL;

static bool is_skipped(const char *texture, const char *variant, const char *udim) {
	if (skip_keys == NULL) {
		return false;
	}
	char *key = string_tmp("\n%s\t%s\t%s\n", texture, variant, udim);
	char *all = string_tmp("\n%s\t*\t*\n", texture); // map désactivée : tous ses fichiers
	return strstr(skip_keys, key) != NULL || strstr(skip_keys, all) != NULL;
}

// Au moins un fichier coché pour cette texture du preset (sinon ses vérifications OCIO ne concernent rien).
static bool texture_selected(const studio_preset_texture_t *t);

static void add_note(const char *format, ...) {
	char    message[512];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	char **grow = realloc(ex.notes, sizeof(char *) * (size_t)(ex.note_count + 1));
	if (grow != NULL) {
		ex.notes                  = grow;
		ex.notes[ex.note_count++] = strdup(message);
	}
}

static void fail(const char *format, ...) {
	if (ex.failed) {
		return;
	}
	ex.failed = true;
	va_list args;
	va_start(args, format);
	vsnprintf(ex.error, sizeof(ex.error), format, args);
	va_end(args);
}

static void reset_plan(void) {
	for (int i = 0; i < ex.file_count; ++i) {
		free(ex.files[i].path);
		free(ex.files[i].variant);
		free(ex.files[i].udim);
	}
	free(ex.files);
	ex.files      = NULL;
	ex.file_count = 0;
	for (int i = 0; i < ex.tile_count; ++i) {
		free(ex.tiles[i]);
	}
	free(ex.tiles);
	ex.tiles              = NULL;
	ex.tile_count         = 0;
	ex.tiles_without_slot = 0;
	ex.first_without_slot = NULL;
	ex.layer_count        = 0;
	ex.hidden_layers      = 0;
	ex.layers_counted     = false;
	free(ex.naming_error);
	ex.naming_error = NULL;
	free(ex.file_base);
	ex.file_base = NULL;
	free(ex.project_name);
	ex.project_name = NULL;
	free(ex.version);
	ex.version = NULL;
	for (int i = 0; i < ex.staged_count; ++i) {
		if (ex.staged[i].tmp != NULL) {
			studio_fs_remove(ex.staged[i].tmp);
		}
		free(ex.staged[i].tmp);
		free(ex.staged[i].path);
	}
	free(ex.staged);
	ex.staged       = NULL;
	ex.staged_count = 0;
	for (int i = 0; i < ex.note_count; ++i) {
		free(ex.notes[i]);
	}
	free(ex.notes);
	ex.notes      = NULL;
	ex.note_count = 0;
	ex.failed     = false;
	ex.error[0]   = 0;
}

static void finish(void) {
	reset_plan();
	studio_preset_free(ex.preset);
	ex.preset = NULL;
	free(ex.overrides);
	ex.overrides = NULL;
	free(ex.naming);
	ex.naming = NULL;
	free(ex.dir);
	ex.dir   = NULL;
	ex.phase = PHASE_IDLE;
}

// Preset choisi pour cet export : celui d'une requête (fenêtre, script), puis celui de --export-textures, puis celui de
// l'export courant, puis celui de la boîte d'export (un preset upstream).
static const char *current_preset_name(void) {
	if (ex.override != NULL) {
		return ex.override;
	}
	if (args_export_textures && !ex.args_consumed && args_export_textures_preset != NULL &&
	    args_export_textures_preset[0] != 0) {
		return args_export_textures_preset;
	}
	if (chosen.name != NULL) {
		return chosen.name;
	}
	if (box_export_files != NULL && box_export_preset_index >= 0 && box_export_preset_index < (i32)box_export_files->length) {
		return box_export_files->buffer[box_export_preset_index];
	}
	return NULL;
}

static int source_bits(void) {
	uint32_t size = layers_expa != NULL ? gpu_texture_format_size(layers_expa->format) : 4;
	return size == 16 ? 32 : size == 8 ? 16 : 8;
}

static const char *mode_name(void) {
	switch (g_context->layers_export) {
	case EXPORT_MODE_SELECTED:
		return "selected";
	case EXPORT_MODE_PER_OBJECT:
		return "per object";
	case EXPORT_MODE_PER_UDIM_TILE:
		return "per UDIM tile";
	default:
		return "visible";
	}
}

// ---- Chemins ---------------------------------------------------------------------------------------------------------

static bool is_absolute(const char *p) {
	return p[0] == '/' || p[0] == '\\' || (p[0] != 0 && p[1] == ':');
}

// Nom de base : le nom tapé dans la boîte de fichiers (ou "untitled"), sans extension d'image.
static char *clean_base(const char *base) {
	static const char *exts[] = {".png", ".jpg", ".jpeg", ".exr", ".tif", ".tiff"};
	char              *s      = strdup(base);
	for (int i = 0; s != NULL && i < 6; ++i) {
		if (ends_with_nocase(s, exts[i])) {
			s[strlen(s) - strlen(exts[i])] = 0;
			break;
		}
	}
	return s;
}

// Chemin d'une texture. NULL et *error (malloc) si le nommage échoue.
static char *texture_path(const char *dir, const char *variant, const char *udim, const studio_preset_texture_t *t,
                          char **error) {
	*error          = NULL;
	const char *ext = studio_format_ext(t->format);
	char       *sg  = NULL;
	int         r   = studio_export_naming_path(variant, t->suffix, udim, ext, &sg, error);
	if (r != 0) {
		return sg;
	}
	// La variante est un objet (Per Object, masque d'objet en Selected) ou un jeu de textures (Visible avec des jeux).
	export_mode_t mode   = g_context->layers_export;
	const char   *object = mode == EXPORT_MODE_PER_OBJECT || mode == EXPORT_MODE_SELECTED ? variant : "";
	const char   *atlas  = mode == EXPORT_MODE_VISIBLE && studio_atlas_active() ? variant : "";
	int           w, h;
	studio_preset_output_size(t, config_get_texture_res_x(), config_get_texture_res_y(), &w, &h);
	char        resolution[16], bits[8];
	snprintf(resolution, sizeof(resolution), "%d", w);
	snprintf(bits, sizeof(bits), "%d", t->bits);
	const char *names[]  = {"file",  "name", "version", "variant", "object", "atlas", "channel",
	                        "UDIM", "resolution", "bits", "preset", "ext"};
	const char *values[] = {ex.file_base, ex.project_name, ex.version, variant,        object,        atlas, t->suffix,
	                        udim,         resolution,      bits,       ex.preset->name, ext};
	char       *name     = studio_naming_format(t->naming != NULL ? t->naming : ex.preset->naming, names, values, 12);
	if (name == NULL) {
		*error = strdup("out of memory");
		return NULL;
	}
	if (is_absolute(name)) {
		return name;
	}
	size_t len  = strlen(dir) + strlen(name) + 2;
	char  *path = malloc(len);
	if (path != NULL) {
		bool sep = dir[0] != 0 && dir[strlen(dir) - 1] != '/' && dir[strlen(dir) - 1] != '\\';
		snprintf(path, len, "%s%s%s", dir, sep ? SEP : "", name);
	}
	free(name);
	return path;
}

// ---- Pixels ----------------------------------------------------------------------------------------------------------

static float half_table[65536];
static float u8_table[256];

static void init_tables(void) {
	for (int i = 0; i < 256; ++i) {
		u8_table[i] = (float)((double)i / 255.0); // comme _export_texture_get_value
	}
	for (uint32_t h = 0; h < 65536; ++h) {
		uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1f, man = h & 0x3ff, bits;
		if (exp == 0) {
			if (man == 0) {
				bits = sign;
			}
			else { // dénormalisé
				exp = 127 - 15 + 1;
				while ((man & 0x400) == 0) {
					man <<= 1;
					exp--;
				}
				bits = sign | (exp << 23) | ((man & 0x3ff) << 13);
			}
		}
		else if (exp == 31) {
			bits = sign | 0x7f800000u | (man << 13);
		}
		else {
			bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
		}
		memcpy(&half_table[h], &bits, 4);
	}
}

static uint16_t float_to_half(float f) {
	uint32_t x;
	memcpy(&x, &f, 4);
	uint32_t sign = (x >> 16) & 0x8000;
	uint32_t abs  = x & 0x7fffffff;
	if (abs >= 0x7f800000) { // inf, NaN
		return (uint16_t)(sign | 0x7c00 | (abs > 0x7f800000 ? 0x200 : 0));
	}
	if (abs >= 0x477ff000) { // au-delà du plus grand half après arrondi
		return (uint16_t)(sign | 0x7c00);
	}
	if (abs < 0x38800000) { // dénormalisé ou zéro
		if (abs < 0x33000000) {
			return (uint16_t)sign;
		}
		uint32_t man   = (abs & 0x7fffff) | 0x800000;
		int      shift = 113 - (int)(abs >> 23) + 13;
		uint32_t half  = man >> shift;
		uint32_t rest  = man & ((1u << shift) - 1);
		uint32_t mid   = 1u << (shift - 1);
		if (rest > mid || (rest == mid && (half & 1))) {
			half++;
		}
		return (uint16_t)(sign | half);
	}
	uint32_t half = ((abs - 0x38000000) >> 13);
	uint32_t rest = abs & 0x1fff;
	if (rest > 0x1000 || (rest == 0x1000 && (half & 1))) {
		half++;
	}
	return (uint16_t)(sign | half);
}

typedef struct {
	const uint8_t *data;
	int            bits;
} source_t;

// Canal c d'ArmorPaint (RGBA) : en 8 bits, les tampons sont BGRA sur Metal et Vulkan desktop.
static inline int swizzle(int c, int bits) {
#ifdef IRON_BGRA
	if (bits == 8) {
		return c == 0 ? 2 : c == 2 ? 0 : c;
	}
#endif
	return c;
}

static inline float sample(const source_t *s, size_t pixel, int c) {
	size_t i = pixel * 4 + (size_t)swizzle(c, s->bits);
	if (s->bits == 8) {
		return u8_table[s->data[i]];
	}
	if (s->bits == 16) {
		return half_table[((const uint16_t *)s->data)[i]];
	}
	return ((const float *)s->data)[i];
}

static inline float decode_color(float v) {
	return v > 0.0f ? powf(v, 2.2f) : 0.0f;
}

typedef struct {
	source_t base, nor, pack;
	int      width;
	int      height;
} sources_t;

// Ligne y de la variable v, en float. decode : couleurs décodées (gamma 2.2 -> linéaire), diff et spec calculés ensuite.
static void load_row(const sources_t *s, int v, int y, bool decode, float *row) {
	size_t p0 = (size_t)y * (size_t)s->width;
	int    w  = s->width;
	switch (v) {
	case STUDIO_VAR_BASE_R:
	case STUDIO_VAR_BASE_G:
	case STUDIO_VAR_BASE_B:
		for (int x = 0; x < w; ++x) {
			float a = sample(&s->base, p0 + x, v - STUDIO_VAR_BASE_R);
			row[x]  = decode ? decode_color(a) : a;
		}
		break;
	case STUDIO_VAR_OPAC:
		for (int x = 0; x < w; ++x) {
			row[x] = sample(&s->base, p0 + x, 3);
		}
		break;
	case STUDIO_VAR_NOR_R:
	case STUDIO_VAR_NOR_G:
	case STUDIO_VAR_NOR_B: {
		int c = v == STUDIO_VAR_NOR_R ? 0 : v == STUDIO_VAR_NOR_G ? 1 : 2;
		for (int x = 0; x < w; ++x) {
			row[x] = sample(&s->nor, p0 + x, c);
		}
	} break;
	case STUDIO_VAR_NOR_G_DIRECTX:
		for (int x = 0; x < w; ++x) {
			row[x] = 1.0f - sample(&s->nor, p0 + x, 1);
		}
		break;
	case STUDIO_VAR_EMIS:
	case STUDIO_VAR_SUBS: {
		int mask = v == STUDIO_VAR_EMIS ? 1 : 2;
		for (int x = 0; x < w; ++x) {
			int id = (int)floorf(sample(&s->nor, p0 + x, 3) * 255.0f + 0.5f);
			row[x] = id % 3 == mask ? 1.0f : 0.0f;
		}
	} break;
	case STUDIO_VAR_OCC:
	case STUDIO_VAR_ROUGH:
	case STUDIO_VAR_METAL:
	case STUDIO_VAR_HEIGHT: {
		int c = v == STUDIO_VAR_OCC ? 0 : v == STUDIO_VAR_ROUGH ? 1 : v == STUDIO_VAR_METAL ? 2 : 3;
		for (int x = 0; x < w; ++x) {
			row[x] = sample(&s->pack, p0 + x, c);
		}
	} break;
	case STUDIO_VAR_SMOOTH:
		for (int x = 0; x < w; ++x) {
			row[x] = 1.0f - sample(&s->pack, p0 + x, 1);
		}
		break;
	default: { // diff_*, spec_*
		bool spec = v >= STUDIO_VAR_SPEC_R;
		int  c    = spec ? v - STUDIO_VAR_SPEC_R : v - STUDIO_VAR_DIFF_R;
		for (int x = 0; x < w; ++x) {
			float albedo = sample(&s->base, p0 + x, c);
			float metal  = sample(&s->pack, p0 + x, 2);
			albedo       = decode ? decode_color(albedo) : albedo;
			row[x]       = spec ? 0.04f * (1.0f - metal) + albedo * metal : albedo * (1.0f - metal);
		}
	}
	}
}

// ---- Composition -----------------------------------------------------------------------------------------------------

typedef enum { OUT_U8, OUT_U16, OUT_HALF, OUT_F32 } out_kind_t;

#define ROWS_PER_JOB 32

// Mise à la résolution de la texture, séparable : pour chaque pixel de sortie, les pixels source et leur poids. Réduction :
// moyenne par zone (chaque pixel source compte pour la part qu'il couvre) ; agrandissement : bilinéaire.
typedef struct {
	int   *first;  // premier pixel source de chaque pixel de sortie
	int   *count;  // nombre de pixels source
	float *weight; // poids, max par pixel de sortie
	int    max;
} taps_t;

static void taps_free(taps_t *t) {
	free(t->first);
	free(t->count);
	free(t->weight);
}

static bool taps_make(taps_t *t, int src, int dst) {
	double scale = (double)src / (double)dst;
	t->max       = src > dst ? (int)ceil(scale) + 1 : 2;
	t->first     = malloc(sizeof(int) * (size_t)dst);
	t->count     = malloc(sizeof(int) * (size_t)dst);
	t->weight    = calloc((size_t)dst * (size_t)t->max, sizeof(float));
	if (t->first == NULL || t->count == NULL || t->weight == NULL) {
		taps_free(t);
		return false;
	}
	for (int i = 0; i < dst; ++i) {
		float *w = t->weight + (size_t)i * (size_t)t->max;
		if (src > dst) {
			double a = i * scale, b = (i + 1) * scale;
			int    j0 = (int)floor(a), j1 = (int)ceil(b);
			j1          = j1 > src ? src : j1;
			t->first[i] = j0;
			t->count[i] = j1 - j0;
			for (int j = j0; j < j1; ++j) {
				double lo = j > a ? j : a, hi = j + 1 < b ? j + 1 : b;
				w[j - j0] = (float)((hi - lo) / scale);
			}
		}
		else {
			double center = (i + 0.5) * scale - 0.5;
			int    j0     = (int)floor(center);
			float  f      = (float)(center - j0);
			if (j0 < 0) {
				j0 = 0;
				f  = 0.0f;
			}
			if (j0 >= src - 1) {
				j0 = src - 1;
				f  = 0.0f;
			}
			t->first[i] = j0;
			t->count[i] = f > 0.0f ? 2 : 1;
			w[0]        = 1.0f - f;
			w[1]        = f;
		}
	}
	return true;
}

typedef struct {
	const sources_t               *src;
	const studio_preset_texture_t *t;
	out_kind_t                     kind;
	int                            out_channels; // t->channels, ou 4 en EXR (RGBA)
	int                            width;        // taille du fichier (studio_preset_output_size)
	int                            height;
	taps_t                         tx, ty;       // si elle diffère de celle des calques
	bool                           resample;
	void                          *out;
	uint32_t                       vars;
	int                            depth;
	float                          (*minmax)[4][2]; // par job : min et max de chaque canal
	bool                           decode;          // couleurs décodées (gamma 2.2 -> linéaire, espace de travail)
	const char                    *ocio_src;        // lot A3 : RGB composé transformé de ocio_src vers ocio_dst
	const char                    *ocio_dst;        // NULL : aucune transformation OCIO
	bool                           failed;
	bool                           ocio_failed;
} compose_t;

// Canaux de la texture pour la ligne y des calques (largeur des calques), avant tout encodage.
static void eval_row(compose_t *c, int y, float **rows, float *scratch, float **res, const bool *constant,
                     const float *value) {
	int w = c->src->width;
	for (int v = 0; v < STUDIO_VAR_COUNT; ++v) {
		if (rows[v] != NULL) {
			load_row(c->src, v, y, c->decode, rows[v]);
		}
	}
	for (int k = 0; k < c->out_channels; ++k) {
		if (constant[k]) {
			for (int x = 0; x < w; ++x) {
				res[k][x] = value[k];
			}
		}
		else {
			studio_expr_eval(c->t->exprs[k], w, (const float *const *)rows, res[k], scratch);
		}
	}
}

static void compose_job(void *ctx, int job) {
	compose_t                     *c  = ctx;
	const studio_preset_texture_t *t  = c->t;
	int                            sw = c->src->width;
	int                            w  = c->width;
	int                            y0 = job * ROWS_PER_JOB;
	int y1 = y0 + ROWS_PER_JOB < c->height ? y0 + ROWS_PER_JOB : c->height;
	float *rows[STUDIO_VAR_COUNT] = {0};
	float *scratch = malloc(sizeof(float) * (size_t)sw * (size_t)(c->depth > 0 ? c->depth : 1));
	float *res[4]  = {0}; // ligne du fichier
	float *src[4]  = {0}; // ligne des calques, quand la taille diffère
	float *rgb     = c->ocio_dst != NULL ? malloc(sizeof(float) * 3 * (size_t)w) : NULL;
	bool   ok      = scratch != NULL && (c->ocio_dst == NULL || rgb != NULL);
	for (int k = 0; ok && k < c->out_channels; ++k) {
		res[k] = malloc(sizeof(float) * (size_t)(c->resample ? w : sw));
		src[k] = c->resample ? malloc(sizeof(float) * (size_t)sw) : res[k];
		ok     = res[k] != NULL && src[k] != NULL;
	}
	for (int v = 0; ok && v < STUDIO_VAR_COUNT; ++v) {
		if (c->vars & (1u << v)) {
			rows[v] = malloc(sizeof(float) * (size_t)sw);
			ok      = rows[v] != NULL;
		}
	}
	bool  constant[4] = {false};
	float value[4]    = {0};
	for (int k = 0; k < c->out_channels; ++k) {
		constant[k] = k >= t->channels || studio_expr_constant(t->exprs[k], &value[k]);
		if (k >= t->channels) { // canaux RGBA de l'EXR au-delà de ceux de la texture
			value[k] = k == 3 ? 1.0f : 0.0f;
		}
	}
	float(*mm)[2] = c->minmax[job];
	for (int k = 0; k < 4; ++k) {
		mm[k][0] = INFINITY;
		mm[k][1] = -INFINITY;
	}
	for (int y = y0; ok && y < y1; ++y) {
		if (!c->resample) {
			eval_row(c, y, rows, scratch, res, constant, value);
		}
		else {
			for (int k = 0; k < c->out_channels; ++k) {
				for (int x = 0; x < w; ++x) {
					res[k][x] = constant[k] ? value[k] : 0.0f;
				}
			}
			const float *wy = c->ty.weight + (size_t)y * (size_t)c->ty.max;
			for (int j = 0; j < c->ty.count[y]; ++j) {
				eval_row(c, c->ty.first[y] + j, rows, scratch, src, constant, value);
				for (int k = 0; k < c->out_channels; ++k) {
					if (constant[k]) {
						continue;
					}
					for (int x = 0; x < w; ++x) {
						const float *wx  = c->tx.weight + (size_t)x * (size_t)c->tx.max;
						const float *s   = src[k] + c->tx.first[x];
						float        sum = 0.0f;
						for (int i = 0; i < c->tx.count[x]; ++i) {
							sum += s[i] * wx[i];
						}
						res[k][x] += wy[j] * sum;
					}
				}
			}
		}
		// srgb : encodage du résultat (après la mise à la taille, qui moyenne des valeurs linéaires), jamais de
		// l'alpha ni d'une constante.
		for (int k = 0; t->color == STUDIO_COLOR_SRGB && k < 3 && k < c->out_channels; ++k) {
			for (int x = 0; !constant[k] && x < w; ++x) {
				res[k][x] = res[k][x] > 0.0f ? powf(res[k][x], 1.0f / 2.2f) : 0.0f;
			}
		}
		// OCIO : le RGB entier, constantes comprises (une matrice mélange les canaux) ; une texture d'un canal passe
		// comme un gris (v, v, v) dont on garde R. Jamais l'alpha.
		if (c->ocio_dst != NULL) {
			int n = t->channels < 3 ? 1 : 3;
			for (int x = 0; x < w; ++x) {
				for (int j = 0; j < 3; ++j) {
					rgb[(size_t)x * 3 + j] = res[n == 1 ? 0 : j][x];
				}
			}
			if (!studio_ocio_transform(rgb, w, 1, 3, c->ocio_src, c->ocio_dst)) {
				c->ocio_failed = true;
				break;
			}
			for (int x = 0; x < w; ++x) {
				for (int j = 0; j < n; ++j) {
					res[j][x] = rgb[(size_t)x * 3 + j];
				}
			}
		}
		for (int k = 0; k < c->out_channels; ++k) {
			const float *r    = res[k];
			size_t       base = (size_t)y * (size_t)w * (size_t)c->out_channels + (size_t)k;
			int    oc   = c->out_channels;
			switch (c->kind) {
			case OUT_U8: {
				uint8_t *o = (uint8_t *)c->out + base;
				for (int x = 0; x < w; ++x) {
					float v             = !(r[x] > 0.0f) ? 0.0f : r[x] > 1.0f ? 1.0f : r[x]; // NaN : 0
					o[(size_t)x * oc]   = (uint8_t)floorf(v * 255.0f + 0.5f);
				}
			} break;
			case OUT_U16: {
				uint16_t *o = (uint16_t *)c->out + base;
				for (int x = 0; x < w; ++x) {
					float v           = !(r[x] > 0.0f) ? 0.0f : r[x] > 1.0f ? 1.0f : r[x];
					o[(size_t)x * oc] = (uint16_t)floorf(v * 65535.0f + 0.5f);
				}
			} break;
			case OUT_HALF: {
				uint16_t *o = (uint16_t *)c->out + base;
				for (int x = 0; x < w; ++x) {
					o[(size_t)x * oc] = float_to_half(r[x]);
				}
			} break;
			default: {
				float *o = (float *)c->out + base;
				for (int x = 0; x < w; ++x) {
					o[(size_t)x * oc] = r[x];
				}
			}
			}
			if (k < t->channels) {
				for (int x = 0; x < w; ++x) {
					mm[k][0] = r[x] < mm[k][0] ? r[x] : mm[k][0];
					mm[k][1] = r[x] > mm[k][1] ? r[x] : mm[k][1];
				}
			}
		}
	}
	for (int v = 0; v < STUDIO_VAR_COUNT; ++v) {
		free(rows[v]);
	}
	free(scratch);
	for (int k = 0; k < 4; ++k) {
		if (src[k] != res[k]) {
			free(src[k]);
		}
		free(res[k]);
	}
	free(rgb);
	if (!ok) {
		c->failed = true;
	}
}

static int thread_count(void) {
	const char *env = getenv("STUDIO_EXPORT_THREADS");
	int         n   = env != NULL && atoi(env) > 0 ? atoi(env) : iron_hardware_threads();
	return n > 0 ? n : 1;
}

static void parallel(int threads, int jobs, studio_image_job_fn fn, void *ctx) {
	exr_parallel_for(threads, jobs, fn, ctx);
}

typedef struct {
	uint8_t *data;
	size_t   len;
	size_t   cap;
	bool     failed;
} jpg_buf_t;

static void jpg_write(void *context, void *data, int size) {
	jpg_buf_t *b = context;
	if (b->failed || size <= 0) {
		return;
	}
	if (b->len + (size_t)size > b->cap) {
		size_t   cap  = (b->len + (size_t)size) * 2;
		uint8_t *grow = realloc(b->data, cap);
		if (grow == NULL) {
			b->failed = true;
			return;
		}
		b->data = grow;
		b->cap  = cap;
	}
	memcpy(b->data + b->len, data, (size_t)size);
	b->len += (size_t)size;
}

static const char *software(void) {
	static char s[256] = "";
	if (s[0] == 0) {
		snprintf(s, sizeof(s), "ArmorPaint %s (%s), studio %s", manifest_version, config_get_sha(), STUDIO_VERSION);
	}
	return s;
}

// Compose et encode la texture t : *data (malloc) et *size. false et ex.error sinon.
static bool encode_texture(const sources_t *src, const studio_preset_texture_t *t, void **data, size_t *size) {
	*data            = NULL;
	*size            = 0;
	bool       exr   = t->format == STUDIO_FORMAT_EXR;
	out_kind_t kind  = t->bits == 8 ? OUT_U8 : exr && t->bits == 16 ? OUT_HALF : t->bits == 16 ? OUT_U16 : OUT_F32;
	int        oc    = exr ? 4 : t->channels;
	int        w, h;
	studio_preset_output_size(t, src->width, src->height, &w, &h);
	size_t    bytes = (size_t)w * (size_t)h * (size_t)oc * (size_t)(t->bits / 8);
	compose_t c     = {.src          = src,
	                   .t            = t,
	                   .kind         = kind,
	                   .out_channels = oc,
	                   .width        = w,
	                   .height       = h,
	                   .resample     = w != src->width || h != src->height,
	                   .decode       = t->color != STUDIO_COLOR_RAW};
	if (c.resample && (!taps_make(&c.tx, src->width, w) || !taps_make(&c.ty, src->height, h))) {
		taps_free(&c.tx);
		fail("%s: out of memory", t->name);
		return false;
	}
	if (t->color == STUDIO_COLOR_OCIO) { // validé par check_ocio avant l'écriture ; revérifié : la config a pu changer
		int data = studio_ocio_is_data(t->color_space);
		c.decode = data == 0;
		if (data == 0) {
			c.ocio_src = studio_ocio_working_space();
			c.ocio_dst = t->color_space;
		}
		if (data < 0 || (data == 0 && c.ocio_src == NULL)) {
			fail("%s: OCIO colorspace '%s' unavailable (%s)", t->name, t->color_space, studio_ocio_status());
			return false;
		}
	}
	for (int k = 0; k < t->channels; ++k) {
		c.vars |= studio_expr_vars(t->exprs[k]);
		c.depth = studio_expr_depth(t->exprs[k]) > c.depth ? studio_expr_depth(t->exprs[k]) : c.depth;
	}
	int jobs = (h + ROWS_PER_JOB - 1) / ROWS_PER_JOB;
	c.out    = malloc(bytes);
	c.minmax = malloc(sizeof(float[4][2]) * (size_t)jobs);
	if (c.out == NULL || c.minmax == NULL) {
		free(c.out);
		free(c.minmax);
		taps_free(&c.tx);
		taps_free(&c.ty);
		fail("%s: out of memory (%zu MB)", t->name, bytes >> 20);
		return false;
	}
	exr_parallel_for(thread_count(), jobs, compose_job, &c);
	taps_free(&c.tx);
	taps_free(&c.ty);
	if (c.failed || c.ocio_failed) {
		free(c.out);
		free(c.minmax);
		if (c.ocio_failed) {
			fail("%s: OCIO transform from '%s' to '%s' failed (see the log)", t->name, c.ocio_src, c.ocio_dst);
		}
		else {
			fail("%s: out of memory", t->name);
		}
		return false;
	}
	// Texture uniforme : souvent un canal jamais peint (hauteur plate, calque vide).
	bool  uniform = true;
	float value[4];
	for (int k = 0; k < t->channels; ++k) {
		float lo = INFINITY, hi = -INFINITY;
		for (int j = 0; j < jobs; ++j) {
			lo = c.minmax[j][k][0] < lo ? c.minmax[j][k][0] : lo;
			hi = c.minmax[j][k][1] > hi ? c.minmax[j][k][1] : hi;
		}
		uniform  = uniform && lo == hi;
		value[k] = lo;
	}
	free(c.minmax);
	bool all_constant = true;
	for (int k = 0; k < t->channels; ++k) {
		float unused;
		all_constant = all_constant && studio_expr_constant(t->exprs[k], &unused);
	}
	if (uniform && !all_constant) {
		char values[96] = "";
		for (int k = 0; k < t->channels; ++k) {
			size_t len = strlen(values);
			snprintf(values + len, sizeof(values) - len, "%s%g", k > 0 ? ", " : "", value[k]);
		}
		add_note("%s is uniform (%s): nothing painted in its channels?", t->name, values);
	}

	char err[256] = "";
	bool ok       = false;
	if (exr) {
		int type = t->channels == 1 ? 2 : t->channels == 3 ? 1 : 3;
		int comp = t->compression != NULL ? studio_exr_compression_from_name(t->compression) : -1;
		ok = studio_exr_encode_texture(c.out, bytes, w, h, t->bits, type, 0, comp, data, size, err, sizeof(err));
	}
	else if (t->format == STUDIO_FORMAT_JPG) {
		jpg_buf_t b = {0};
		ok          = stbi_write_jpg_to_func(jpg_write, &b, w, h, t->channels, c.out, t->quality) != 0 &&
		     !b.failed;
		if (ok) {
			*data = b.data;
			*size = b.len;
		}
		else {
			free(b.data);
			snprintf(err, sizeof(err), "JPG encoder failed");
		}
	}
	else {
		studio_image_t img = {.pixels   = c.out,
		                      .width    = w,
		                      .height   = h,
		                      .channels = t->channels,
		                      .bits     = t->bits,
		                      .level    = studio_preset_deflate_level(t->compression),
		                      .software = software(),
		                      .threads  = thread_count(),
		                      .parallel = parallel};
		if (t->format == STUDIO_FORMAT_PNG) {
			ok = studio_png_encode(&img, data, size, err, sizeof(err));
		}
		else {
			img.tiff_compression = strcmp(t->compression, "none") == 0 ? STUDIO_TIFF_NONE
			                       : strcmp(t->compression, "lzw") == 0 ? STUDIO_TIFF_LZW
			                                                            : STUDIO_TIFF_DEFLATE;
			img.level            = studio_preset_deflate_level("default");
			ok                   = studio_tiff_encode(&img, data, size, err, sizeof(err));
		}
	}
	free(c.out);
	if (!ok) {
		fail("%s: %s", t->name, err);
	}
	return ok;
}

// ---- Écriture --------------------------------------------------------------------------------------------------------

// Écrit data dans <path>.studio-tmp (dossiers créés) : le renommage vers path n'a lieu qu'une fois tout écrit.
static bool stage(const char *path, const void *data, size_t size, int plan_index) {
	if (!studio_naming_make_parent(path)) {
		fail("cannot create the folder of %s", path);
		return false;
	}
	size_t len = strlen(path) + 16;
	char  *tmp = malloc(len);
	if (tmp == NULL) {
		fail("out of memory");
		return false;
	}
	snprintf(tmp, len, "%s.studio-tmp", path);
	if (!studio_fs_write(tmp, data, size)) {
		if (!studio_fs_is_dir(tmp)) { // remove() efface aussi un dossier vide qui n'est pas à nous
			studio_fs_remove(tmp);
		}
		free(tmp);
		fail("cannot write %s (disk full or no permission?)", path);
		return false;
	}
	staged_t *grow = realloc(ex.staged, sizeof(staged_t) * (size_t)(ex.staged_count + 1));
	if (grow == NULL) {
		studio_fs_remove(tmp);
		free(tmp);
		fail("out of memory");
		return false;
	}
	ex.staged           = grow;
	staged_t *s         = &ex.staged[ex.staged_count++];
	s->tmp              = tmp;
	s->path             = strdup(path);
	s->size             = size;
	s->plan_index       = plan_index;
	studio_sha256_hex(data, size, s->sha);
	return true;
}

// Dossier commun à tous les fichiers (malloc).
static char *common_dir(void) {
	if (ex.staged_count == 0) {
		return strdup(ex.dir);
	}
	char  *dir = studio_fs_dirname(ex.staged[0].path);
	size_t len = dir != NULL ? strlen(dir) : 0;
	for (int i = 1; dir != NULL && i < ex.staged_count; ++i) {
		const char *p = ex.staged[i].path;
		size_t      k = 0;
		while (k < len && p[k] == dir[k]) {
			k++;
		}
		// Recule jusqu'à une frontière de dossier.
		if (k < len || (p[k] != '/' && p[k] != '\\')) {
			while (k > 0 && dir[k] != '/' && dir[k] != '\\') {
				k--;
			}
		}
		len      = k;
		dir[len] = 0;
	}
	return dir;
}

static const char *relative_to(const char *path, const char *dir) {
	size_t n = strlen(dir);
	if (n > 0 && strncmp(path, dir, n) == 0 && (path[n] == '/' || path[n] == '\\')) {
		return path + n + 1;
	}
	return path;
}

typedef struct {
	char  *data;
	size_t len;
	size_t cap;
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
		size_t cap  = (b->len + (size_t)n + 1) * 2;
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
	if (q != NULL) {
		put(b, "%s", q);
		free(q);
	}
}

static char *manifest_json(const char *dir, const studio_report_t *r) {
	text_t b    = {0};
	time_t now  = time(NULL);
	char   date[32];
	struct tm tm;
#ifdef _WIN32
	gmtime_s(&tm, &now);
#else
	gmtime_r(&now, &tm);
#endif
	strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%SZ", &tm);
	char host[256] = "";
#ifdef _WIN32
	const char *h = getenv("COMPUTERNAME");
	snprintf(host, sizeof(host), "%s", h != NULL ? h : "");
	const char *user = getenv("USERNAME");
#else
	gethostname(host, sizeof(host) - 1);
	const char *user = getenv("USER");
#endif
	const studio_preset_t *p = ex.preset;
	put(&b, "{\n  \"schema\": \"studio-texture-export/1\",\n  \"date\": \"%s\",\n  \"user\": ", date);
	put_quoted(&b, user);
	put(&b, ",\n  \"host\": ");
	put_quoted(&b, host);
	put(&b, ",\n  \"armorpaint\": {\"version\": ");
	put_quoted(&b, manifest_version);
	put(&b, ", \"sha\": ");
	put_quoted(&b, config_get_sha());
	put(&b, ", \"studio\": \"%s\"},\n  \"project\": ", STUDIO_VERSION);
	put_quoted(&b, project_filepath_get());
	put(&b, ",\n  \"preset\": {\"name\": ");
	put_quoted(&b, p->name);
	put(&b, ", \"path\": ");
	put_quoted(&b, p->path);
	put(&b, ", \"sha256\": \"%s\", \"color_space_override\": ", p->sha256);
	if (color_override != NULL) {
		put_quoted(&b, color_override);
	}
	else {
		put(&b, "null");
	}
	// Réglages de l'export par-dessus le preset (bits, résolution par texture) : l'objet validé, tel quel.
	put(&b, ", \"overrides\": %s, \"naming\": ", ex.overrides != NULL ? ex.overrides : "null");
	if (ex.naming != NULL) {
		put_quoted(&b, ex.naming);
	}
	else {
		put(&b, "null");
	}
	put(&b, "},\n");
	put(&b, "  \"resolution\": [%d, %d],\n  \"source_bits\": %d,\n  \"mode\": \"%s\",\n", config_get_texture_res_x(),
	    config_get_texture_res_y(), ex.source_bits, mode_name());
	const char *tmpl = studio_export_naming_template();
	put(&b, "  \"shotgrid_template\": ");
	if (tmpl != NULL) {
		put_quoted(&b, tmpl);
	}
	else {
		put(&b, "null");
	}
	put(&b, ",\n  \"ocio\": ");
	bool ocio = false;
	for (int i = 0; i < p->texture_count; ++i) {
		ocio = ocio || p->textures[i].color == STUDIO_COLOR_OCIO;
	}
	if (ocio) { // provenance : la même configuration redonne les mêmes fichiers
		put(&b, "{\"version\": ");
		put_quoted(&b, studio_ocio_version());
		put(&b, ", \"config\": ");
		put_quoted(&b, studio_ocio_config_name());
		put(&b, ", \"source\": ");
		put_quoted(&b, studio_ocio_config_source());
		put(&b, ", \"working_space\": ");
		put_quoted(&b, studio_ocio_working_space());
		put(&b, "}");
	}
	else {
		put(&b, "null");
	}
	put(&b, ",\n  \"files\": [\n");
	for (int i = 0; i < ex.staged_count; ++i) {
		const staged_t                *s = &ex.staged[i];
		const studio_plan_file_t      *f = &ex.files[s->plan_index];
		const studio_preset_texture_t *t = f->texture;
		put(&b, "    {\"path\": ");
		put_quoted(&b, relative_to(s->path, dir));
		put(&b, ", \"texture\": ");
		put_quoted(&b, t->name);
		put(&b, ", \"variant\": ");
		put_quoted(&b, f->variant);
		put(&b, ", \"udim\": ");
		put_quoted(&b, f->udim);
		put(&b, ", \"resolution\": [%d, %d], \"format\": \"%s\", \"bits\": %d, \"compression\": ", f->width, f->height,
		    studio_format_name(t->format), t->bits);
		if (t->compression != NULL) {
			put_quoted(&b, t->compression);
		}
		else if (t->format == STUDIO_FORMAT_JPG) {
			put(&b, "\"quality %d\"", t->quality);
		}
		else {
			put(&b, "\"default\"");
		}
		put(&b, ", \"color_space\": ");
		put_quoted(&b, studio_preset_color(t));
		if (t->color == STUDIO_COLOR_OCIO) {
			const char *cs = studio_ocio_resolve(t->color_space);
			put(&b, ", \"ocio\": {\"colorspace\": ");
			put_quoted(&b, cs);
			if (studio_ocio_is_data(cs) == 1) {
				put(&b, ", \"data\": true}");
			}
			else {
				put(&b, ", \"from\": ");
				put_quoted(&b, studio_ocio_working_space());
				put(&b, ", \"transform\": ");
				put_quoted(&b, studio_ocio_transform_id(studio_ocio_working_space(), cs));
				put(&b, "}");
			}
		}
		put(&b, ", \"channels\": [");
		for (int k = 0; k < t->channels; ++k) {
			put(&b, "%s", k > 0 ? ", " : "");
			put_quoted(&b, t->sources[k]);
		}
		put(&b, "], \"size\": %llu, \"sha256\": \"%s\"}%s\n", (unsigned long long)s->size, s->sha,
		    i + 1 < ex.staged_count ? "," : "");
	}
	put(&b, "  ],\n  \"warnings\": [");
	int n = 0;
	for (int i = 0; r != NULL && i < r->count; ++i) {
		put(&b, "%s\n    ", n++ > 0 ? "," : "");
		put_quoted(&b, r->issues[i].message);
	}
	for (int i = 0; i < ex.note_count; ++i) {
		put(&b, "%s\n    ", n++ > 0 ? "," : "");
		put_quoted(&b, ex.notes[i]);
	}
	put(&b, "%s]\n}\n", n > 0 ? "\n  " : "");
	return b.data;
}

// Renomme tous les fichiers en place, puis écrit le manifeste (aussi par renommage).
static bool commit(const studio_report_t *r, char **manifest_out) {
	*manifest_out = NULL;
	int done      = 0;
	for (; done < ex.staged_count; ++done) {
		if (!studio_fs_rename(ex.staged[done].tmp, ex.staged[done].path)) {
			fail("cannot replace %s", ex.staged[done].path);
			break;
		}
		free(ex.staged[done].tmp);
		ex.staged[done].tmp = NULL;
	}
	if (done < ex.staged_count) {
		if (done > 0) {
			report(true, "Texture export interrupted: %d of %d files replaced before the error, the others unchanged", done,
			       ex.staged_count);
		}
		return false;
	}
	if (!ex.preset->manifest) {
		return true;
	}
	char *dir  = common_dir();
	char *name = string_tmp("%s%s%s.export.json", dir != NULL ? dir : ex.dir, dir != NULL && dir[0] != 0 ? SEP : "",
	                        ex.file_base);
	char *json = manifest_json(dir != NULL ? dir : "", r);
	free(dir);
	if (json == NULL) {
		fail("manifest: out of memory");
		return false;
	}
	char *tmp = string_tmp("%s.studio-tmp", name);
	bool  ok  = studio_fs_write(tmp, json, strlen(json)) && studio_fs_rename(tmp, name);
	free(json);
	if (!ok) {
		studio_fs_remove(tmp);
		fail("cannot write the manifest %s", name);
		return false;
	}
	*manifest_out = strdup(name);
	return true;
}

// ---- Plan et préflight -----------------------------------------------------------------------------------------------

static void count_layers(slot_layer_t_array_t *layers) {
	if (ex.layers_counted || layers == NULL) {
		return;
	}
	ex.layers_counted    = true;
	bool export_selected = g_context->layers_export == EXPORT_MODE_SELECTED;
	for (int i = 0; i < (int)layers->length; ++i) {
		slot_layer_t *l = layers->buffer[i];
		if (!slot_layer_is_layer(l) || slot_layer_get_filters(l, false) != NULL) {
			continue;
		}
		if (!export_selected && !slot_layer_is_visible(l)) {
			ex.hidden_layers++;
		}
		else {
			ex.layer_count++;
		}
	}
}

static void collect_tiles(void) {
	mesh_object_t_array_t *objects = g_project->_->paint_objects;
	for (int i = 0; objects != NULL && i < (int)objects->length; ++i) {
		char *name = objects->buffer[i]->base->name;
		if (util_mesh_udim_tile(name) < 0) {
			continue;
		}
		const char *tile = name + strlen(name) - 4;
		bool        seen = false;
		for (int k = 0; k < ex.tile_count && !seen; ++k) {
			seen = strcmp(ex.tiles[k], tile) == 0;
		}
		char **grow = seen ? NULL : realloc(ex.tiles, sizeof(char *) * (size_t)(ex.tile_count + 1));
		if (grow != NULL) {
			ex.tiles                  = grow;
			ex.tiles[ex.tile_count++] = strdup(tile);
		}
	}
	// Au-delà de 64 tuiles, les dernières n'ont pas de case dans l'atlas des calques : rien à exporter pour elles
	for (int k = 0; k < ex.tile_count; ++k) {
		if (util_mesh_udim_active() && ex.tiles[k] != NULL && util_mesh_udim_slot(atoi(ex.tiles[k])) < 0) {
			ex.tiles_without_slot++;
			if (ex.first_without_slot == NULL || atoi(ex.tiles[k]) < atoi(ex.first_without_slot)) {
				ex.first_without_slot = ex.tiles[k];
			}
		}
	}
}

static void plan_variant(const char *variant, const char *udim) {
	for (int i = 0; i < ex.preset->texture_count; ++i) {
		const studio_preset_texture_t *t = &ex.preset->textures[i];
		int                            w, h;
		studio_preset_output_size(t, config_get_texture_res_x(), config_get_texture_res_y(), &w, &h);
		char *error = NULL;
		char                          *path  = texture_path(ex.dir, variant, udim, t, &error);
		if (path == NULL) {
			if (ex.naming_error == NULL) {
				ex.naming_error = error != NULL ? error : strdup("naming failed");
			}
			else {
				free(error);
			}
			continue;
		}
		studio_plan_file_t *grow = realloc(ex.files, sizeof(studio_plan_file_t) * (size_t)(ex.file_count + 1));
		if (grow == NULL) {
			free(path);
			continue;
		}
		ex.files                    = grow;
		ex.files[ex.file_count++]   = (studio_plan_file_t){
		      .path     = path,
		      .texture  = t,
		      .variant  = strdup(variant),
		      .udim     = strdup(udim),
		      .width    = w,
		      .height   = h,
		      .raw_size = studio_image_raw_size(w, h, t->channels, t->bits),
		      .skip     = is_skipped(t->name, variant, udim),
        };
	}
}

static int find_plan(const char *path, const studio_preset_texture_t *t) {
	for (int i = 0; i < ex.file_count; ++i) {
		if (ex.files[i].texture == t && strcmp(ex.files[i].path, path) == 0) {
			return i;
		}
	}
	return -1;
}

static bool texture_selected(const studio_preset_texture_t *t) {
	for (int i = 0; i < ex.file_count; ++i) {
		if (ex.files[i].texture == t && !ex.files[i].skip) {
			return true;
		}
	}
	return false;
}

static void write_variant(const char *variant, const char *udim) {
	sources_t src = {.width = config_get_texture_res_x(), .height = config_get_texture_res_y()};
	uint32_t  vars = 0;
	for (int i = 0; i < ex.preset->texture_count; ++i) {
		for (int k = 0; k < ex.preset->textures[i].channels; ++k) {
			vars |= studio_expr_vars(ex.preset->textures[i].exprs[k]);
		}
	}
	const uint32_t base_vars = (1u << STUDIO_VAR_BASE_R) | (1u << STUDIO_VAR_BASE_G) | (1u << STUDIO_VAR_BASE_B) |
	                           (1u << STUDIO_VAR_OPAC) | (7u << STUDIO_VAR_DIFF_R) | (7u << STUDIO_VAR_SPEC_R);
	const uint32_t nor_vars  = (1u << STUDIO_VAR_NOR_R) | (1u << STUDIO_VAR_NOR_G) | (1u << STUDIO_VAR_NOR_G_DIRECTX) |
	                          (1u << STUDIO_VAR_NOR_B) | (1u << STUDIO_VAR_EMIS) | (1u << STUDIO_VAR_SUBS);
	const uint32_t pack_vars = (1u << STUDIO_VAR_OCC) | (1u << STUDIO_VAR_ROUGH) | (1u << STUDIO_VAR_METAL) |
	                           (1u << STUDIO_VAR_HEIGHT) | (1u << STUDIO_VAR_SMOOTH) | (7u << STUDIO_VAR_DIFF_R) |
	                           (7u << STUDIO_VAR_SPEC_R);
	gpu_texture_t *textures[3] = {layers_expa, layers_expb, layers_expc};
	uint32_t       masks[3]    = {base_vars, nor_vars, pack_vars};
	source_t      *targets[3]  = {&src.base, &src.nor, &src.pack};
	for (int i = 0; i < 3; ++i) {
		if ((vars & masks[i]) == 0) {
			continue;
		}
		buffer_t *pixels = gpu_get_texture_pixels(textures[i]);
		uint32_t  size   = gpu_texture_format_size(textures[i]->format);
		if (pixels == NULL || pixels->buffer == NULL || textures[i]->width != src.width || textures[i]->height != src.height) {
			fail("cannot read the flattened layers back from the GPU");
			return;
		}
		*targets[i] = (source_t){.data = pixels->buffer, .bits = size == 16 ? 32 : size == 8 ? 16 : 8};
	}
	for (int i = 0; i < ex.preset->texture_count && !ex.failed; ++i) {
		const studio_preset_texture_t *t     = &ex.preset->textures[i];
		char                          *error = NULL;
		char                          *path  = texture_path(ex.dir, variant, udim, t, &error);
		int                            index = path != NULL ? find_plan(path, t) : -1;
		if (index < 0) {
			fail("%s: the file plan changed during the export (%s)", t->name, error != NULL ? error : path);
			free(path);
			free(error);
			return;
		}
		if (ex.files[index].skip) {
			free(path);
			continue;
		}
		void  *data = NULL;
		size_t size = 0;
		if (encode_texture(&src, t, &data, &size)) {
			stage(path, data, size, index);
		}
		free(data);
		free(path);
		iron_log("studio: export: %s", ex.files[index].path);
	}
}

// ---- Points d'injection ----------------------------------------------------------------------------------------------

static studio_report_t last_report = {0};

static uint32_t color_vars(const studio_preset_texture_t *t, uint32_t *all) {
	uint32_t vars = 0;
	for (int k = 0; k < t->channels && k < 3; ++k) {
		vars |= studio_expr_vars(t->exprs[k]);
	}
	*all = vars;
	return vars & STUDIO_VAR_COLOR_MASK;
}

// Lot A3 : espaces OCIO des textures, vérifiés contre la configuration chargée. Ici et non dans studio_preflight.c,
// qui reste testable sans OCIO.
static void check_ocio(studio_report_t *r) {
	for (int i = 0; i < ex.preset->texture_count; ++i) {
		const studio_preset_texture_t *t = &ex.preset->textures[i];
		if (t->color != STUDIO_COLOR_OCIO || !texture_selected(t)) {
			continue;
		}
		if (!studio_ocio_ready()) {
			studio_report_add(r, true, "ocio_unavailable", "%s: color_space '%s' needs OCIO: %s", t->name,
			                  t->color_space, studio_ocio_status());
			continue;
		}
		const char *cs = studio_ocio_resolve(t->color_space);
		if (cs == NULL) {
			studio_report_add(r, true, "ocio_unknown_colorspace",
			                  "%s: '%s' is not a colorspace, alias or role of the OCIO config \"%s\"", t->name,
			                  t->color_space, studio_ocio_config_name());
			continue;
		}
		uint32_t vars   = 0;
		uint32_t colors = color_vars(t, &vars);
		if (studio_ocio_is_data(cs) == 1) {
			if (colors != 0) {
				studio_report_note(r, "ocio_color_as_data", "%s: colour written to the data colorspace '%s': "
				                  "values as stored by ArmorPaint (gamma 2.2 encoded)", t->name, cs);
			}
			continue;
		}
		const char *ws = studio_ocio_working_space();
		if (ws == NULL) {
			studio_report_add(r, true, "ocio_no_working_space", "%s: %s", t->name, studio_ocio_working_space_error());
			continue;
		}
		if (studio_ocio_transform_id(ws, cs) == NULL) {
			studio_report_add(r, true, "ocio_no_transform", "%s: no OCIO transform from '%s' to '%s' (see the log)",
			                  t->name, ws, cs);
			continue;
		}
		if (vars != 0 && colors == 0) {
			studio_report_note(r, "ocio_data_as_color", "%s: data channels converted to the colour space "
			                  "'%s': use a data colorspace (Raw) to keep their values", t->name, cs);
		}
		const char *enc    = studio_ocio_encoding(cs);
		bool        linear = enc != NULL && strcmp(enc, "scene-linear") == 0;
		if (t->format == STUDIO_FORMAT_EXR && enc != NULL && enc[0] != 0 && !linear) {
			studio_report_note(r, "exr_not_linear", "%s: EXR in '%s' (%s encoding), renderers expect "
			                  "scene-linear values", t->name, cs, enc);
		}
		if (t->bits == 8 && linear && colors != 0) {
			studio_report_note(r, "linear_8bit", "%s: scene-linear colour ('%s') in 8 bits: banding in dark "
			                  "values, use an encoded colorspace (sRGB - Texture) or 16 bits", t->name, cs);
		}
	}
}

// "{}" (la fenêtre envoie ses réglages même vides) : rien à appliquer, null au manifeste.
static bool empty_object(const char *json) {
	while (*json == ' ' || *json == '\t' || *json == '\n' || *json == '\r') {
		++json;
	}
	if (*json++ != '{') {
		return false;
	}
	while (*json == ' ' || *json == '\t' || *json == '\n' || *json == '\r') {
		++json;
	}
	return *json == '}';
}

bool studio_export_begin(char *path, bool bake_material) {
	if (ex.phase != PHASE_IDLE) {
		return true; // passe interne (plan) : l'upstream déroule ses boucles, studio_export_layers intercepte
	}
	const char *name = current_preset_name();
	if (args_export_textures) {
		ex.args_consumed = true;
	}
	bool     inline_preset = name != NULL && chosen.json != NULL && chosen.name != NULL && strcmp(chosen.name, name) == 0;
	entry_t *e             = NULL;
	if (!inline_preset) {
		scan_presets(); // relus à chaque export : un preset ajouté ou modifié compte sans relancer ArmorPaint
		e = find_entry(name);
		if (e == NULL) {
			return true; // preset upstream
		}
	}
	if (g_context->layers_destination == EXPORT_DESTINATION_PACK_INTO_PROJECT) {
		report(false, "Studio preset %s: textures packed into the project use ArmorPaint's generic writer (8-bit, preset "
		              "formats ignored)", name);
		return true;
	}

	char             err[512];
	studio_preset_t *preset = inline_preset ? studio_preset_parse(chosen.json, name, err, sizeof(err))
	                                        : studio_preset_load(e->path, e->name, err, sizeof(err));
	if (preset != NULL && inline_preset && chosen.source != NULL) {
		preset->path = strdup(chosen.source);
	}
	if (preset == NULL) {
		show_text("Texture export: invalid studio preset", string_tmp("%s: %s", name, err), true);
		return false;
	}
	// Les réglages de l'export (fenêtre) ne valent que pour le preset pour lequel ils ont été faits.
	bool overridden = chosen.overrides != NULL && !empty_object(chosen.overrides) && chosen.name != NULL &&
	                  strcmp(chosen.name, name) == 0;
	if (overridden && !studio_preset_apply_overrides(preset, chosen.overrides, err, sizeof(err))) {
		studio_preset_free(preset);
		show_text("Texture export: invalid settings", err, true);
		return false;
	}
	// Sans preset choisi (script : studio_export_set_naming seul), le modèle vaut pour tous.
	bool renamed = chosen.naming != NULL && (chosen.name == NULL || strcmp(chosen.name, name) == 0);
	if (renamed && !studio_naming_check(chosen.naming, err, sizeof(err))) {
		studio_preset_free(preset);
		show_text("Texture export: invalid file name", string_tmp("%s: %s", chosen.naming, err), true);
		return false;
	}
	char *naming = renamed ? strdup(chosen.naming) : NULL;
	if (renamed && naming != NULL) {
		free(preset->naming);
		preset->naming = strdup(naming);
	}
	if ((renamed && (naming == NULL || preset->naming == NULL)) ||
	    (color_override != NULL &&
	     studio_preset_override_color(preset, color_override, studio_ocio_ready() ? studio_ocio_is_data : NULL) < 0)) {
		free(naming);
		studio_preset_free(preset);
		show_text("Texture export failed", "out of memory", true);
		return false;
	}
	reset_plan();
	ex.preset      = preset;
	ex.naming      = naming;
	ex.overrides   = overridden ? strdup(chosen.overrides) : NULL;
	ex.dir         = strdup(path);
	ex.source_bits = source_bits();
	const char *f  = ui_files_filename != NULL && ui_files_filename[0] != 0 ? ui_files_filename : "untitled";
	ex.file_base   = clean_base(f);
	if (ex.file_base == NULL || !studio_naming_split_version(ex.file_base, &ex.project_name, &ex.version)) {
		ex.project_name = strdup("");
		ex.version      = strdup("");
	}

	// Passe à blanc : l'upstream parcourt objets, tuiles et atlas ; chaque appel de studio_export_layers note ses
	// fichiers sans rien lire ni écrire.
	ex.phase = PHASE_PLANNING;
	export_texture_run(path, bake_material);
	collect_tiles();

	studio_plan_t plan = {
	    .preset             = ex.preset,
	    .files              = ex.files,
	    .file_count         = ex.file_count,
	    .width              = config_get_texture_res_x(),
	    .height             = config_get_texture_res_y(),
	    .source_bits        = ex.source_bits,
	    .mode               = bake_material ? "bake material" : mode_name(),
	    .udim_mesh          = util_mesh_udim_active() || ex.tile_count > 0,
	    .per_tile           = g_context->layers_export == EXPORT_MODE_PER_UDIM_TILE && !bake_material,
	    .tiles              = (const char *const *)ex.tiles,
	    .tile_count         = ex.tile_count,
	    .tiles_without_slot = ex.tiles_without_slot,
	    .first_without_slot = ex.first_without_slot,
	    .layer_count        = bake_material ? 1 : ex.layer_count,
	    .hidden_layers      = bake_material ? 0 : ex.hidden_layers,
	    .naming_error       = ex.naming_error,
	    .destination        = path,
	    .color_override     = color_override,
	};
	char *manifest = NULL;
	if (ex.preset->manifest && ex.file_count > 0) {
		manifest           = string_tmp("%s%s%s.export.json", path, SEP, ex.file_base);
		plan.manifest_path = manifest;
	}
	studio_report_free(&last_report);
	studio_preflight_run(&plan, &last_report);
	check_ocio(&last_report);
	free(ex.last_report);
	ex.last_report = studio_report_json(&plan, &last_report);
	ex.last_ok     = last_report.blocking == 0;
	char *text     = studio_report_text(&plan, &last_report);
	if (last_report.blocking > 0) {
		show_text(string_tmp("Texture export blocked (%s): %d problem%s", ex.preset->name, last_report.blocking,
		                     last_report.blocking > 1 ? "s" : ""),
		          text != NULL ? text : "", true);
	}
	else if (ex.preflight_only) {
		iron_log("studio: export preflight (%s):\n%s", ex.preset->name, text != NULL ? text : "");
	}
	else {
		for (int i = 0; i < last_report.count; ++i) {
			if (!last_report.issues[i].note) {
				report(false, "Texture export: %s", last_report.issues[i].message);
			}
		}
	}
	free(text);
	if (last_report.blocking > 0 || ex.preflight_only) {
		finish();
		return false;
	}
	ex.phase = PHASE_WRITING;
	return true;
}

bool studio_export_planning(void) {
	return ex.phase == PHASE_PLANNING;
}

bool studio_export_layers(char *path, char *base, char *f, char *udim, slot_layer_t_array_t *layers) {
	(void)path;
	if (ex.phase == PHASE_IDLE) {
		return false;
	}
	// Variante : ce que l'upstream a ajouté au nom de base (objet, atlas, masque d'objet en export "selected").
	const char *variant = f + strlen(base) + (strlen(f) > strlen(base) ? 1 : 0);
	const char *tile    = udim[0] == '.' ? udim + 1 : udim;
	if (ex.phase == PHASE_PLANNING) {
		count_layers(layers);
		plan_variant(variant, tile);
	}
	else if (!ex.failed) {
		write_variant(variant, tile);
	}
	return true;
}

bool studio_export_end(void) {
	if (ex.phase == PHASE_IDLE) {
		return true;
	}
	if (ex.phase == PHASE_PLANNING) {
		return false; // fin de la passe à blanc : pas de message "Textures exported"
	}
	char *manifest = NULL;
	bool  ok       = !ex.failed && commit(&last_report, &manifest);
	if (ok) {
		uint64_t total = 0;
		for (int i = 0; i < ex.staged_count; ++i) {
			total += ex.staged[i].size;
		}
		char size[32];
		studio_format_size(total, size, sizeof(size));
		report(false, "Textures exported with %s: %d file%s, %s%s%s", ex.preset->name, ex.staged_count,
		       ex.staged_count > 1 ? "s" : "", size, manifest != NULL ? ", manifest " : "", manifest != NULL ? manifest : "");
		for (int i = 0; i < ex.note_count; ++i) {
			report(false, "Texture export: %s", ex.notes[i]);
		}
	}
	else {
		show_text("Texture export failed: nothing was replaced", ex.error, true);
	}
	free(manifest);
	ex.last_ok = ok;
	finish(); // retire aussi les fichiers temporaires restants
	return ok;
}

// ---- API de script ---------------------------------------------------------------------------------------------------

static void run(char *dir, char *preset, int mode, bool preflight_only) {
	free(ex.override);
	ex.override = preset != NULL && preset[0] != 0 ? strdup(preset) : NULL;
	if (mode >= EXPORT_MODE_VISIBLE && mode <= EXPORT_MODE_PER_UDIM_TILE) {
		g_context->layers_export = (export_mode_t)mode;
	}
	g_context->layers_destination = EXPORT_DESTINATION_DISK;
	ex.preflight_only             = preflight_only;
	free(ex.last_report);
	ex.last_report = NULL;
	ex.last_ok     = false;
	// Un script tourne pendant une frame : on suspend le dessin en cours, comme script_gpu_begin (minic_impl.c).
	gpu_texture_t *current = _draw_current;
	bool           in_use  = gpu_in_use;
	if (in_use) {
		draw_end();
	}
	export_texture_run(dir, false);
	if (in_use) {
		draw_begin(current, false, 0);
	}
	ex.preflight_only = false;
	free(ex.override);
	ex.override = NULL;
}

bool studio_export_request(const char *dir, const char *preset, int mode, bool preflight_only) {
	last_error[0] = 0;
	quiet         = true;
	run((char *)dir, (char *)preset, mode, preflight_only);
	quiet = false;
	return ex.last_ok;
}

const char *studio_export_last_error(void) {
	return last_error;
}

int studio_export_preset_count(void) {
	scan_presets();
	return entry_count;
}

const char *studio_export_preset_name(int index) {
	return index >= 0 && index < entry_count ? entries[index].name : NULL;
}

const char *studio_export_last_report(void) {
	return ex.last_report != NULL ? ex.last_report : "";
}

const char *studio_export_preset_path(const char *name) {
	entry_t *e = find_entry(name);
	return e != NULL ? e->path : NULL;
}

bool studio_export_preset_shipped(const char *name) {
	entry_t *e = find_entry(name);
	return e != NULL && e->shipped;
}

static void set_text(char **field, const char *value) {
	char *copy = value != NULL && value[0] != 0 ? strdup(value) : NULL;
	free(*field);
	*field = copy;
}

void studio_export_set_preset(const char *name, const char *json, const char *source) {
	set_text(&chosen.name, name);
	set_text(&chosen.json, json);
	set_text(&chosen.source, source);
}

const char *studio_export_preset(void) {
	return chosen.name != NULL ? chosen.name : "";
}

const char *studio_export_preset_json(void) {
	return chosen.json != NULL ? chosen.json : "";
}

const char *studio_export_preset_source(void) {
	return chosen.source != NULL ? chosen.source : "";
}

const char *studio_export_overrides(void) {
	return chosen.overrides != NULL ? chosen.overrides : "";
}

void studio_export_set_overrides(const char *json) {
	set_text(&chosen.overrides, json);
}

const char *studio_export_naming(void) {
	return chosen.naming != NULL ? chosen.naming : "";
}

void studio_export_set_naming(const char *pattern) {
	set_text(&chosen.naming, pattern);
}

const char *studio_export_color_space(void) {
	return color_override != NULL ? color_override : "";
}

bool studio_export_set_color_space(const char *name) {
	char *copy = name != NULL && name[0] != 0 ? strdup(name) : NULL;
	free(color_override);
	color_override = copy;
	return color_override == NULL || studio_ocio_resolve(color_override) != NULL;
}

const char *studio_export_skip(void) {
	return skip_keys != NULL ? skip_keys + 1 : "";
}

void studio_export_set_skip(const char *keys) {
	free(skip_keys);
	skip_keys = NULL;
	if (keys == NULL || keys[0] == 0) {
		return;
	}
	size_t len = strlen(keys);
	skip_keys  = malloc(len + 3);
	if (skip_keys != NULL) { // bornée par des \n : chaque ligne se cherche entière
		snprintf(skip_keys, len + 3, "\n%s%s", keys, keys[len - 1] == '\n' ? "" : "\n");
	}
}

bool studio_export_is_studio_preset(const char *name) {
	if (name != NULL && chosen.json != NULL && chosen.name != NULL && strcmp(chosen.name, name) == 0) {
		return true;
	}
	scan_presets();
	return find_entry(name) != NULL;
}

#ifdef WITH_EVAL

#include "minic.h"

static minic_val_t mn_studio_export_run(minic_val_t *a, int n) {
	run(minic_arg_p(a, n, 0), minic_arg_p(a, n, 1), minic_arg_i(a, n, 2), false);
	return minic_val_int(ex.last_ok ? 1 : 0);
}

static minic_val_t mn_studio_export_preflight(minic_val_t *a, int n) {
	run(minic_arg_p(a, n, 0), minic_arg_p(a, n, 1), minic_arg_i(a, n, 2), true);
	return minic_val_ptr((void *)studio_export_last_report());
}

static minic_val_t mn_studio_export_last_report(minic_val_t *a, int n) {
	(void)a;
	(void)n;
	return minic_val_ptr((void *)studio_export_last_report());
}

static minic_val_t mn_studio_export_is_studio_preset(minic_val_t *a, int n) {
	return minic_val_int(studio_export_is_studio_preset(minic_arg_p(a, n, 0)) ? 1 : 0);
}

static minic_val_t mn_studio_export_color_space(minic_val_t *a, int n) {
	(void)a;
	(void)n;
	return minic_val_ptr((void *)studio_export_color_space());
}

static minic_val_t mn_studio_export_skip(minic_val_t *a, int n) {
	(void)a;
	(void)n;
	return minic_val_ptr((void *)studio_export_skip());
}

static minic_val_t mn_studio_export_set_skip(minic_val_t *a, int n) {
	studio_export_set_skip(minic_arg_p(a, n, 0));
	return minic_val_int(0);
}

static minic_val_t mn_studio_export_set_preset(minic_val_t *a, int n) {
	studio_export_set_preset(minic_arg_p(a, n, 0), minic_arg_p(a, n, 1), minic_arg_p(a, n, 2));
	return minic_val_int(0);
}

static minic_val_t mn_studio_export_set_overrides(minic_val_t *a, int n) {
	studio_export_set_overrides(minic_arg_p(a, n, 0));
	return minic_val_int(0);
}

static minic_val_t mn_studio_export_set_naming(minic_val_t *a, int n) {
	studio_export_set_naming(minic_arg_p(a, n, 0));
	return minic_val_int(0);
}

static minic_val_t mn_studio_export_set_color_space(minic_val_t *a, int n) {
	return minic_val_int(studio_export_set_color_space(minic_arg_p(a, n, 0)) ? 1 : 0);
}

#endif

void studio_export_init(void) {
	init_tables();
	scan_presets();
	iron_log("studio: %d studio export preset%s", entry_count, entry_count > 1 ? "s" : "");
	const char *cs = getenv("STUDIO_EXPORT_COLOR_SPACE");
	if (cs != NULL && cs[0] != 0 && !studio_export_set_color_space(cs)) {
		// Gardé : l'export sera bloqué avec la raison (ocio_unknown_colorspace), plutôt que fait dans le mauvais espace.
		iron_log("studio: STUDIO_EXPORT_COLOR_SPACE '%s' is unknown to the OCIO config (%s): exports will be blocked", cs,
		         studio_ocio_status());
	}
#ifdef WITH_EVAL
	minic_register("studio_export_run", "i(p,p,i)", mn_studio_export_run);
	minic_register("studio_export_preflight", "p(p,p,i)", mn_studio_export_preflight);
	minic_register("studio_export_last_report", "p()", mn_studio_export_last_report);
	minic_register("studio_export_is_studio_preset", "i(p)", mn_studio_export_is_studio_preset);
	minic_register("studio_export_color_space", "p()", mn_studio_export_color_space);
	minic_register("studio_export_set_color_space", "i(p)", mn_studio_export_set_color_space);
	minic_register("studio_export_skip", "p()", mn_studio_export_skip);
	minic_register("studio_export_set_skip", "v(p)", mn_studio_export_set_skip);
	minic_register("studio_export_set_preset", "v(p,p,p)", mn_studio_export_set_preset);
	minic_register("studio_export_set_overrides", "v(p)", mn_studio_export_set_overrides);
	minic_register("studio_export_set_naming", "v(p)", mn_studio_export_set_naming);
#endif
}
