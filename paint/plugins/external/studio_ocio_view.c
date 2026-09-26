#include "studio_ocio_view.h"
#include "iron_array.h"
#include "iron_draw.h"
#include "iron_gpu.h"
#include "iron_map.h"
#include "iron_obj.h"
#include "iron_string.h"
#include "iron_system.h"
#include "iron_ui.h"
#include "engine.h"
#include "../../sources/types.h"
#include "studio.h"
#include "studio_ocio.h"
#include "studio_ocio_lut.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Cœur ArmorPaint (paint/sources/globals.h, ui/ui_menu.c) ; ui_menubar_add_category est le point d'injection
// STUDIO-PATCH(menubar-hook) de ui/ui_menubar.c (PATCHES.md).
extern context_t *g_context;
extern config_t  *g_config;
buffer_t         *gpu_get_texture_pixels(gpu_texture_t *image); // défini dans iron.h
extern ui_t      *g_ui;
extern bool       ui_menu_keep_open;
extern bool       ui_menu_show_first; // vrai : le menu ouvert est remesuré (hauteur de son fond) au prochain rendu
void              ui_menubar_add_category(char *label, void (*draw_items)(void));
bool              ui_menu_sub_button(char *text);
void              ui_menu_sub_begin(i32 items);
void              ui_menu_sub_end(void);
void              ui_menu_separator(void);
void              ui_menu_label(char *text, char *shortcut);

static struct {
	bool              enabled;
	char             *display, *view, *look; // choix courant (copies : un rechargement invalide les noms d'OCIO)
	int               generation;            // configuration de la LUT (studio_ocio_generation)
	bool              dirty;                 // LUT à refaire au prochain ensure()
	studio_ocio_lut_t lut;                   // gardée pour studio_ocio_view_apply
	gpu_texture_t    *texture;
	char              status[1024];
	// Liens d'uniformes d'ArmorPaint (uniforms_ext_init), appelés pour tout ce qui n'est pas à nous.
	vec4_t (*prev_vec4)(object_t *, shader_data_t *, char *);
	gpu_texture_t *(*prev_tex)(object_t *, shader_data_t *, char *);
} ov;

static const char *or_empty(const char *s) {
	return s != NULL ? s : "";
}

static void set_name(char **dst, const char *s) {
	char *copy = strdup(or_empty(s));
	free(*dst);
	*dst = copy;
}

static void release_lut(void) {
	if (ov.texture != NULL) {
		gpu_texture_destroy(ov.texture); // différé par iron jusqu'à la fin de la frame en cours
		free(ov.texture);
		ov.texture = NULL;
	}
	studio_ocio_lut_free(&ov.lut);
}

static void redraw(void) {
	if (g_context != NULL) {
		g_context->ddirty = 2;
	}
}

static bool lut_fn(float *rgb, int count, void *user) {
	(void)user;
	return studio_ocio_display_transform(rgb, count, 1, 3, studio_ocio_working_space(), ov.display, ov.view, ov.look);
}

static bool valid(const char *display, const char *view, const char *look) {
	const char *working = studio_ocio_working_space();
	return working != NULL && studio_ocio_display_transform_id(working, display, view, look) != NULL;
}

// Remet le choix sur les défauts de la configuration quand il n'y existe plus (rechargement, variable invalide).
static void fix_choice(void) {
	if (valid(ov.display, ov.view, ov.look)) {
		return;
	}
	const char *d = studio_ocio_display_default();
	if (valid(ov.display, studio_ocio_view_default(ov.display), NULL)) {
		d = ov.display;
	}
	char *display = strdup(or_empty(d)); // d peut être ov.display, remplacé ci-dessous
	char *view    = strdup(or_empty(studio_ocio_view_default(display)));
	iron_log("studio: OCIO viewport: \"%s\" / \"%s\" / \"%s\" unknown to the config, using \"%s\" / \"%s\"", ov.display,
	         ov.view, ov.look, display, view);
	free(ov.display);
	free(ov.view);
	ov.display = display;
	ov.view    = view;
	set_name(&ov.look, "");
}

static void rebuild(void) {
	release_lut();
	ov.dirty      = false;
	ov.generation = studio_ocio_generation();
	bool usable   = studio_ocio_ready() && studio_ocio_working_space() != NULL;
	if (usable) {
		fix_choice(); // même désactivé : le choix affiché et rendu aux scripts existe dans la configuration
	}
	if (!ov.enabled) {
		snprintf(ov.status, sizeof(ov.status), "OCIO viewport off: ArmorPaint filmic tonemap");
		return;
	}
	if (!studio_ocio_ready()) {
		snprintf(ov.status, sizeof(ov.status), "OCIO viewport unavailable: %s", studio_ocio_status());
		return;
	}
	if (!usable) {
		snprintf(ov.status, sizeof(ov.status), "OCIO viewport unavailable: working space unknown: %s",
		         or_empty(studio_ocio_working_space_error()));
		return;
	}
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	bool ok = studio_ocio_lut_bake(&ov.lut, STUDIO_OCIO_LUT_SIZE, lut_fn, NULL, 0);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	if (!ok) {
		snprintf(ov.status, sizeof(ov.status), "OCIO viewport unavailable: LUT of \"%s\" / \"%s\" failed", ov.display,
		         ov.view);
		iron_log("studio: %s", ov.status);
		return;
	}
	ov.texture = calloc(1, sizeof(gpu_texture_t));
	gpu_texture_init_from_bytes(ov.texture, ov.lut.texels, ov.lut.width, ov.lut.height, GPU_TEXTURE_FORMAT_RGBA64, false);
	double ms  = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
	int    hdr = studio_ocio_view_is_hdr(ov.display, ov.view);
	snprintf(ov.status, sizeof(ov.status), "OCIO viewport: \"%s\" / \"%s\"%s%s%s from \"%s\", LUT %d^3 in %.0f ms%s",
	         ov.display, ov.view, ov.look[0] ? " / look \"" : "", ov.look, ov.look[0] ? "\"" : "",
	         studio_ocio_working_space(), ov.lut.n, ms, hdr == 1 ? " (HDR view on an SDR viewport)" : "");
	iron_log("studio: %s", ov.status);
}

// La LUT suit le choix et la configuration.
static void ensure(void) {
	if (ov.dirty || ov.generation != studio_ocio_generation()) {
		rebuild();
		redraw();
	}
}

static void on_update(void *data) {
	(void)data;
	ensure();
}

// Sans ensure() : appelé pendant le rendu, où la LUT ne se refait pas.
static bool lut_ready(void) {
	return ov.enabled && ov.texture != NULL;
}

// ── uniformes du compositeur ──────────────────────────────────────────────────────────────────────────────────────

static vec4_t vec4_link(object_t *object, shader_data_t *mat, char *link) {
	if (strcmp(link, "_ocio_lut_data") == 0) {
		if (!lut_ready()) {
			return (vec4_t){0.0f, 0.0f, 0.0f, 0.0f}; // compositeur d'upstream
		}
		return (vec4_t){(float)ov.lut.n, (float)ov.lut.cols, (float)ov.lut.rows, 0.0f};
	}
	return ov.prev_vec4 != NULL ? ov.prev_vec4(object, mat, link) : vec4_nan();
}

static gpu_texture_t *tex_link(object_t *object, shader_data_t *mat, char *link) {
	if (strcmp(link, "_ocio_lut_tex") == 0) {
		if (lut_ready()) {
			return ov.texture;
		}
		render_target_t *rt = any_map_get(render_path_render_targets, "empty_white"); // comme _lut_tex sans LUT
		return rt != NULL ? rt->_image : NULL;
	}
	return ov.prev_tex != NULL ? ov.prev_tex(object, mat, link) : NULL;
}

// ── API ────────────────────────────────────────────────────────────────────────────────────────────────────────────

bool studio_ocio_view_enabled(void) {
	return ov.enabled;
}

bool studio_ocio_view_active(void) {
	ensure();
	return lut_ready();
}

bool studio_ocio_view_set_enabled(bool on) {
	if (on != ov.enabled) {
		ov.enabled = on;
		ov.dirty   = true;
	}
	ensure();
	return studio_ocio_view_active();
}

bool studio_ocio_view_set(const char *display, const char *view, const char *look) {
	ensure(); // noms de la configuration courante
	if (display == NULL || display[0] == '\0') {
		display = studio_ocio_display_default();
	}
	if (view == NULL || view[0] == '\0') {
		view = studio_ocio_view_default(display);
	}
	if (!valid(display, view, look)) {
		iron_log("studio: OCIO viewport: unknown display/view/look \"%s\" / \"%s\" / \"%s\"", or_empty(display),
		         or_empty(view), or_empty(look));
		return false;
	}
	set_name(&ov.display, display);
	set_name(&ov.view, view);
	set_name(&ov.look, look);
	ov.dirty = true;
	ensure();
	return true;
}

const char *studio_ocio_view_display(void) {
	ensure();
	return or_empty(ov.display);
}

const char *studio_ocio_view_view(void) {
	ensure();
	return or_empty(ov.view);
}

const char *studio_ocio_view_look(void) {
	ensure();
	return or_empty(ov.look);
}

const char *studio_ocio_view_status(void) {
	ensure();
	return ov.status;
}

bool studio_ocio_view_apply(float rgb[3]) {
	ensure();
	if (!studio_ocio_view_active()) {
		return false;
	}
	float in[3] = {rgb[0], rgb[1], rgb[2]};
	studio_ocio_lut_sample(&ov.lut, in, rgb);
	return true;
}

// Cible de rendu 1 × 1 RGBA64 de la sonde, créée au premier appel.
static render_target_t *probe_target(char *name) {
	render_target_t *rt = any_map_get(render_path_render_targets, name);
	if (rt == NULL) {
		rt         = render_target_create();
		rt->name   = name;
		rt->width  = 1;
		rt->height = 1;
		rt->format = "RGBA64";
		render_path_create_render_target(rt);
	}
	return rt;
}

bool studio_ocio_view_probe(int color, float out[3]) {
	ensure();
	render_target_t *dst = probe_target("studio_ocio_probe_out");
	probe_target("studio_ocio_probe_in");
	float grain = g_config->rp_grain, vignette = g_config->rp_vignette;
	g_config->rp_grain    = 0.0f;
	g_config->rp_vignette = 0.0f;
	render_path_set_target("studio_ocio_probe_in", NULL, NULL, GPU_CLEAR_COLOR, color, 0.0f);
	render_path_set_target("studio_ocio_probe_out", NULL, NULL, GPU_CLEAR_NONE, 0, 0.0f);
	render_path_bind_target("studio_ocio_probe_in", "tex");
	render_path_draw_shader("Scene/compositor_pass/compositor_pass");
	g_config->rp_grain    = grain;
	g_config->rp_vignette = vignette;
	buffer_t *pixels      = gpu_get_texture_pixels(dst->_image);
	if (pixels == NULL || pixels->length < 8) {
		return false;
	}
	for (int c = 0; c < 3; c++) {
		uint16_t h;
		memcpy(&h, pixels->buffer + c * 2, sizeof(h));
		out[c] = studio_ocio_half_to_float(h);
	}
	return true;
}

// ── menu Color ────────────────────────────────────────────────────────────────────────────────────────────────────

// Radio d'une liste de noms ; rend l'index choisi, -1 sans changement.
static int radio_list(int count, const char *(*name_at)(int, void *), void *user, const char *current, bool hdr_marks) {
	int selected = -1, picked = -1;
	for (int i = 0; i < count; i++) {
		if (strcmp(name_at(i, user), current) == 0) {
			selected = i;
		}
	}
	for (int i = 0; i < count; i++) {
		const char *name = name_at(i, user);
		char        text[256];
		snprintf(text, sizeof(text), "%s%s", name,
		         hdr_marks && studio_ocio_view_is_hdr((const char *)user, name) == 1 ? " (HDR)" : "");
		ui_radio(&selected, i, text, "");
		if (ui_item_changed()) {
			picked = i;
		}
	}
	return picked;
}

static const char *display_at(int i, void *user) {
	(void)user;
	return or_empty(studio_ocio_display_name(i));
}

static const char *view_at(int i, void *user) {
	return or_empty(studio_ocio_view_name((const char *)user, i));
}

static const char *look_at(int i, void *user) {
	(void)user;
	return i == 0 ? "None" : or_empty(studio_ocio_look_name(i - 1));
}

static void color_menu_draw(void) {
	ensure();
	bool enabled = ov.enabled;
	ui_check(&enabled, " OCIO Viewport", "");
	if (ui_item_changed()) {
		studio_ocio_view_set_enabled(enabled);
		ui_menu_keep_open  = true;
		ui_menu_show_first = true;
	}
	if (!studio_ocio_ready() || studio_ocio_working_space() == NULL) {
		ui_menu_label((char *)ov.status, NULL);
		return;
	}
	ui_menu_separator();

	if (ui_menu_sub_button("Display")) {
		int n = studio_ocio_display_count();
		ui_menu_sub_begin(n);
		int i = radio_list(n, display_at, NULL, or_empty(ov.display), false);
		if (i >= 0) {
			studio_ocio_view_set(display_at(i, NULL), NULL, ov.look);
			ui_menu_keep_open = true;
		}
		ui_menu_sub_end();
	}
	if (ui_menu_sub_button("View")) {
		int n = studio_ocio_view_count(ov.display);
		ui_menu_sub_begin(n);
		int i = radio_list(n, view_at, ov.display, or_empty(ov.view), true);
		if (i >= 0) {
			studio_ocio_view_set(ov.display, view_at(i, ov.display), ov.look);
			ui_menu_keep_open = true;
		}
		ui_menu_sub_end();
	}
	int looks = studio_ocio_look_count();
	if (looks > 0 && ui_menu_sub_button("Look")) {
		ui_menu_sub_begin(looks + 1);
		int i = radio_list(looks + 1, look_at, NULL, ov.look[0] ? ov.look : "None", false);
		if (i >= 0) {
			studio_ocio_view_set(ov.display, ov.view, i == 0 ? NULL : look_at(i, NULL));
			ui_menu_keep_open = true;
		}
		ui_menu_sub_end();
	}
	ui_menu_separator();
	ui_menu_label(studio_ocio_view_active() ? string_tmp("%s / %s", ov.display, ov.view) : "Filmic (ArmorPaint)", NULL);
}

// ── scripts ────────────────────────────────────────────────────────────────────────────────────────────────────────

#ifdef WITH_EVAL
#include "minic.h"

static minic_val_t mn_enabled(minic_val_t *a, int n) {
	return minic_val_int(studio_ocio_view_enabled() ? 1 : 0);
}

static minic_val_t mn_active(minic_val_t *a, int n) {
	return minic_val_int(studio_ocio_view_active() ? 1 : 0);
}

static minic_val_t mn_set_enabled(minic_val_t *a, int n) {
	return minic_val_int(studio_ocio_view_set_enabled(minic_arg_i(a, n, 0) != 0) ? 1 : 0);
}

static minic_val_t mn_set(minic_val_t *a, int n) {
	return minic_val_int(studio_ocio_view_set(minic_arg_p(a, n, 0), minic_arg_p(a, n, 1), minic_arg_p(a, n, 2)) ? 1 : 0);
}

static minic_val_t mn_display(minic_val_t *a, int n) {
	return minic_val_ptr((void *)studio_ocio_view_display());
}

static minic_val_t mn_view(minic_val_t *a, int n) {
	return minic_val_ptr((void *)studio_ocio_view_view());
}

static minic_val_t mn_look(minic_val_t *a, int n) {
	return minic_val_ptr((void *)studio_ocio_view_look());
}

static minic_val_t mn_status(minic_val_t *a, int n) {
	return minic_val_ptr((void *)studio_ocio_view_status());
}

// "r g b" (%.9g) ou "" si inactif.
static minic_val_t mn_apply_rgb(minic_val_t *a, int n) {
	static char out[128];
	float       px[3] = {minic_arg_f(a, n, 0), minic_arg_f(a, n, 1), minic_arg_f(a, n, 2)};
	out[0]            = '\0';
	if (studio_ocio_view_apply(px)) {
		snprintf(out, sizeof(out), "%.9g %.9g %.9g", px[0], px[1], px[2]);
	}
	return minic_val_ptr(out);
}

// "r g b" (%.9g) de la sortie du compositeur pour la couleur 0xAARRGGBB, "" en cas d'échec.
static minic_val_t mn_probe(minic_val_t *a, int n) {
	static char out[128];
	float       px[3];
	out[0] = '\0';
	if (studio_ocio_view_probe(minic_arg_i(a, n, 0), px)) {
		snprintf(out, sizeof(out), "%.9g %.9g %.9g", px[0], px[1], px[2]);
	}
	return minic_val_ptr(out);
}

static void register_script_api(void) {
	// Signatures au format de minic_register ; la référence est dans README.md.
	minic_register("studio_ocio_view_enabled", "i()", mn_enabled);
	minic_register("studio_ocio_view_active", "i()", mn_active);
	minic_register("studio_ocio_view_set_enabled", "i(i)", mn_set_enabled);
	minic_register("studio_ocio_view_set", "i(p,p,p)", mn_set);
	minic_register("studio_ocio_view_display", "p()", mn_display);
	minic_register("studio_ocio_view_view", "p()", mn_view);
	minic_register("studio_ocio_view_look", "p()", mn_look);
	minic_register("studio_ocio_view_status", "p()", mn_status);
	minic_register("studio_ocio_view_apply_rgb", "p(f,f,f)", mn_apply_rgb);
	minic_register("studio_ocio_view_probe", "p(i)", mn_probe);
}
#endif

void studio_ocio_view_init(void) {
	const char *env = getenv("STUDIO_OCIO_VIEWPORT");
	ov.enabled      = env == NULL || strcmp(env, "0") != 0;
	set_name(&ov.display, getenv("STUDIO_OCIO_DISPLAY"));
	set_name(&ov.view, getenv("STUDIO_OCIO_VIEW"));
	set_name(&ov.look, getenv("STUDIO_OCIO_LOOK"));
	if (ov.display[0] == '\0' && studio_ocio_ready()) {
		set_name(&ov.display, studio_ocio_display_default());
	}
	if (ov.view[0] == '\0' && studio_ocio_ready()) {
		set_name(&ov.view, studio_ocio_view_default(ov.display));
	}
	ov.dirty = true; // LUT faite à la première frame, GPU prêt

	ov.prev_vec4        = uniforms_vec4_links;
	ov.prev_tex         = uniforms_tex_links;
	uniforms_vec4_links = vec4_link;
	uniforms_tex_links  = tex_link;
	sys_notify_on_update(on_update, NULL);
	ui_menubar_add_category("Color", color_menu_draw);
#ifdef WITH_EVAL
	register_script_api();
#endif
}
