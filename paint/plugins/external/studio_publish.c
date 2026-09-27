#include "studio_publish.h"
#include "iron_array.h"
#include "iron_draw.h"
#include "iron_gpu.h"
#include "iron_image.h"
#include "iron_map.h"
#include "iron_obj.h"
#include "iron_string.h"
#include "iron_system.h"
#include "iron_ui.h"
#include "engine.h"
#include "../../sources/types.h"
#include "studio_audit.h"
#include "studio_export_naming.h"
#include "studio_export_window.h"
#include "studio_json.h"
#include "studio_naming.h"
#include "studio_sg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Cœur ArmorPaint (paint/sources/globals.h, project.c, io/export_arm.c) et iron (définis dans base/sources/iron.h)
extern context_t *g_context;
char             *project_filepath_get(void);
void              export_arm_run_project(char *path);
void              iron_delay_idle_sleep(void);
gpu_texture_t    *gpu_create_render_target(i32 width, i32 height, i32 format);
buffer_t         *gpu_get_texture_pixels(gpu_texture_t *image);
void              gpu_delete_texture(gpu_texture_t *texture);

#define THUMBNAIL_FRAMES 3    // frames rendues sans compas ni sphères d'environnement avant la capture
#define THUMBNAIL_SIZE   1280 // plus grand côté de la vignette, sauf args.size (0 : taille du viewport)

static void reply_ok(int host, const char *data_json) {
	studio_sg_host_reply(string_tmp("{\"host\":%d,\"ok\":true,\"data\":%s}", host, data_json));
}

static void reply_error(int host, const char *code, const char *message) {
	char *m = studio_json_quote(message);
	studio_sg_host_reply(string_tmp("{\"host\":%d,\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":%s}}", host, code, m));
	free(m);
}

// Texte (malloc) de la chaîne à `pointer`, "" si absente.
static char *text_at(const char *json, const char *pointer) {
	studio_json_value_t v = studio_json_find(json, pointer);
	char               *s = v.type == STUDIO_JSON_STRING ? studio_json_text(v) : NULL;
	return s != NULL ? s : strdup("");
}

// ---- Export ----------------------------------------------------------------------------------------------------------

// args.naming : le modèle du projet ouvert, donné par le sidecar (studio_export_naming_set).
static void apply_naming(const char *req, const char *path) {
	if (studio_json_find(req, "/args/naming").type != STUDIO_JSON_OBJECT) {
		return;
	}
	char *template     = text_at(req, "/args/naming/template");
	char *pattern      = text_at(req, "/args/naming/pattern");
	char *pattern_udim = text_at(req, "/args/naming/pattern_udim");
	studio_export_naming_set(path, template, pattern, pattern_udim);
	free(template);
	free(pattern);
	free(pattern_udim);
}

// args.skip : [[texture, variant, tuile], ...] -> lignes de studio_export_set_skip (malloc).
static char *skip_lines(const char *req) {
	studio_json_value_t list = studio_json_find(req, "/args/skip");
	int                 n    = list.type == STUDIO_JSON_ARRAY ? studio_json_length(list) : 0;
	size_t              len  = 0;
	char               *out  = calloc(1, 1);
	for (int i = 0; i < n && out != NULL; ++i) {
		char *part[3];
		for (int k = 0; k < 3; ++k) {
			part[k] = text_at(req, string_tmp("/args/skip/%d/%d", i, k));
		}
		const char *line = string_tmp("%s\t%s\t%s\n", part[0], part[1], part[2]);
		size_t      add  = strlen(line);
		char       *grow = realloc(out, len + add + 1);
		if (grow == NULL) {
			free(out);
		}
		else {
			memcpy(grow + len, line, add + 1);
			len += add;
		}
		out = grow;
		for (int k = 0; k < 3; ++k) {
			free(part[k]);
		}
	}
	return out;
}

static void export_request(int host, const char *req, bool preflight_only) {
	const char *path = project_filepath_get();
	if (path == NULL || path[0] == 0) {
		reply_error(host, "not_saved", "the project has never been saved: save it first (File Save...)");
		return;
	}
	apply_naming(req, path);
	studio_json_value_t c       = studio_json_find(req, "/args/choices");
	char               *choices = c.type == STUDIO_JSON_OBJECT ? strndup(c.start, c.len) : NULL;
	char               *skip    = skip_lines(req);
	iron_log("studio: publish: %s of the textures of %s", preflight_only ? "preflight" : "export", path);
	char       *result   = studio_export_run_choices(choices, preflight_only, skip);
	const char *template = studio_export_naming_template();
	char       *qpath    = studio_json_quote(path);
	char       *qtmpl    = template != NULL ? studio_json_quote(template) : NULL;
	if (result == NULL || qpath == NULL) {
		reply_error(host, "internal", "out of memory");
	}
	else {
		reply_ok(host, string_tmp("{\"path\":%s,\"naming_template\":%s,\"export\":%s}", qpath, qtmpl != NULL ? qtmpl : "null", result));
	}
	free(qtmpl);
	free(qpath);
	free(result);
	free(skip);
	free(choices);
}

// ---- Copie du projet -------------------------------------------------------------------------------------------------
// Le .arm désigne ses fichiers (textures importées, maillage source, polices, sons, envmap) par des chemins relatifs à
// son dossier : copié tel quel dans un autre dossier, il ne les trouverait plus (textures roses à l'ouverture).
// ArmorPaint écrit donc la copie lui-même, chemins relatifs à la copie (STUDIO-PATCH(project-copy) dans
// export_arm_run_project). Écrite à côté puis renommée : pas de fichier à moitié écrit sous le nom publié.

static bool copying;

bool studio_project_copy(void) {
	return copying;
}

static void copy_request(int host, const char *req) {
	const char *current = project_filepath_get();
	char       *path    = text_at(req, "/args/path");
	size_t      n       = strlen(path);
	if (current == NULL || current[0] == 0) {
		reply_error(host, "not_saved", "the project has never been saved: save it first (File Save...)");
	}
	else if (n <= 4 || strcmp(path + n - 4, ".arm") != 0) {
		reply_error(host, "bad_request", string_tmp("path of the copy (.arm) expected: %s", path));
	}
	else if (strcmp(path, current) == 0) {
		reply_error(host, "bad_request", string_tmp("the copy would replace the open project: %s", path));
	}
	else if (!studio_naming_make_parent(path)) {
		reply_error(host, "write_failed", string_tmp("cannot create the folder of %s", path));
	}
	else {
		char *tmp = strdup(string_tmp("%s.tmp", path));
		remove(tmp);
		iron_log("studio: publish: copy of %s to %s", current, path);
		copying = true;
		export_arm_run_project(tmp);
		copying = false;

		FILE *f    = fopen(tmp, "rb");
		long  size = -1;
		if (f != NULL) {
			fseek(f, 0, SEEK_END);
			size = ftell(f);
			fclose(f);
		}
#ifdef _WIN32
		if (size > 0) {
			remove(path); // rename() ne remplace pas un fichier existant sous Windows
		}
#endif
		if (size <= 0 || rename(tmp, path) != 0) {
			remove(tmp);
			reply_error(host, "write_failed", string_tmp("cannot write the copy %s", path));
		}
		else {
			char *q = studio_json_quote(path);
			char *s = studio_json_quote(current);
			reply_ok(host, string_tmp("{\"path\":%s,\"size\":%ld}", q, size));
			studio_audit("project_copy", string_tmp("\"path\":%s,\"from\":%s,\"size\":%ld", q, s, size));
			free(q);
			free(s);
		}
		free(tmp);
	}
	free(path);
}

// ---- Vignette --------------------------------------------------------------------------------------------------------
// Comme Capture Screenshot (ui_menubar.c, viewport.c) : l'image finale du viewport, rendue sans compas ni sphères.

static struct {
	int   host; // requête en attente de sa capture, 0 sinon
	char *path;
	int   frames;
	int   size; // plus grand côté ; 0 : taille du viewport
} shot;

static void capture(void) {
	render_target_t *rt             = any_map_get(render_path_render_targets, "last");
	gpu_texture_t   *tex            = rt != NULL ? rt->_image : NULL;
	g_context->capturing_screenshot = false;
	g_context->ddirty               = 2;
	int host                        = shot.host;
	shot.host                       = 0;
	if (tex == NULL || tex->width <= 0 || tex->height <= 0) {
		reply_error(host, "no_viewport", "the viewport has no image to capture");
		return;
	}
	int   longest = tex->width > tex->height ? tex->width : tex->height;
	float scale   = shot.size > 0 && longest > shot.size ? (float)shot.size / (float)longest : 1.0f;
	int   w       = (int)(tex->width * scale + 0.5f);
	int   h       = (int)(tex->height * scale + 0.5f);
	w             = w > 0 ? w : 1;
	h             = h > 0 ? h : 1;

	gpu_texture_t *image = gpu_create_render_target(w, h, GPU_TEXTURE_FORMAT_RGBA32);
	draw_begin(image, false, 0);
	draw_scaled_image(tex, 0, 0, (float)w, (float)h);
	draw_end();
	iron_write_png(shot.path, gpu_get_texture_pixels(image), w, h, 0);
	gpu_delete_texture(image);

	FILE *f = fopen(shot.path, "rb");
	if (f == NULL) {
		reply_error(host, "write_failed", string_tmp("cannot write the thumbnail %s", shot.path));
		return;
	}
	fclose(f);
	char *q = studio_json_quote(shot.path);
	reply_ok(host, string_tmp("{\"path\":%s,\"width\":%d,\"height\":%d}", q, w, h));
	free(q);
	iron_log("studio: publish: thumbnail %s (%dx%d)", shot.path, w, h);
}

static void thumbnail_update(void *data) {
	(void)data;
	iron_delay_idle_sleep(); // les frames doivent tourner jusqu'à la capture, même sans entrée de l'artiste
	if (++shot.frames < THUMBNAIL_FRAMES) {
		return;
	}
	sys_remove_update(thumbnail_update);
	capture();
}

static void thumbnail_request(int host, const char *req) {
	char *path = text_at(req, "/args/path");
	if (path[0] == 0) {
		reply_error(host, "bad_request", "path of the thumbnail (.png) expected");
		free(path);
		return;
	}
	studio_json_value_t size = studio_json_find(req, "/args/size");
	bool                has  = size.type != STUDIO_JSON_INVALID && size.type != STUDIO_JSON_NULL;
	if (has && (size.type != STUDIO_JSON_NUMBER || studio_json_int(size, -1) < 0)) {
		reply_error(host, "bad_request", "size of the thumbnail: a number of pixels, 0 for the viewport size");
		free(path);
		return;
	}
	if (shot.host != 0) {
		reply_error(host, "busy", "a thumbnail is already being captured");
		free(path);
		return;
	}
	free(shot.path);
	shot.path                       = path;
	long long n                     = has ? studio_json_int(size, 0) : THUMBNAIL_SIZE;
	shot.size                       = n > 65536 ? 0 : (int)n; // plus grand que tout viewport : sa taille
	shot.host                       = host;
	shot.frames                     = 0;
	g_context->capturing_screenshot = true;
	g_context->ddirty               = 2;
	sys_notify_on_update(thumbnail_update, NULL);
}

// ---- Requêtes --------------------------------------------------------------------------------------------------------

bool studio_publish_host(int host, const char *op, const char *request) {
	if (strcmp(op, "publish_preflight") == 0 || strcmp(op, "publish_export") == 0) {
		export_request(host, request, strcmp(op, "publish_preflight") == 0);
	}
	else if (strcmp(op, "publish_thumbnail") == 0) {
		thumbnail_request(host, request);
	}
	else if (strcmp(op, "publish_copy") == 0) {
		copy_request(host, request);
	}
	else {
		return false;
	}
	return true;
}
