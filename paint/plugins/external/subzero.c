#include "subzero.h"
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
#include "studio_exec.h"
#include "studio_env.h"
#include "studio_json.h"
#include "studio_ocio.h"
#include "studio_ocio_import.h"
#include "studio_ocio_prefs.h"
#include "studio_ocio_view.h"
#include "studio_prefs.h"
#include "studio_project.h"
#include "studio_window.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Cœur ArmorPaint (paint/sources/globals.h, functions.h, ui/) ; box_preferences_add_tab et ui_menubar_add_category
// sont les points d'injection STUDIO-PATCH(preferences-tab) et STUDIO-PATCH(menubar-hook) (PATCHES.md).
extern context_t         *g_context;
extern project_t         *g_project;
extern ui_t              *g_ui;
extern i32                base_bits;
extern i32                box_preferences_tab;
extern any_map_t         *import_text_importers;
extern string_array_t    *_path_text_formats;
extern ui_window_array_t *ui_base_hwnds;
extern ui_window_t       *ui_nodes_hwnd;
char                     *project_filepath_get(void);
i32                       config_get_texture_res_x(void);
i32                       config_get_texture_res_y(void);
slot_material_t          *slot_material_create(shader_data_t *m, ui_node_canvas_t *c);
ui_node_t                *nodes_material_get_node_t(char *node_type);
ui_node_t                *ui_nodes_make_node(ui_node_t *n, ui_nodes_t *nodes, ui_node_canvas_t *canvas);
void                      import_texture_run(char *path, bool hdr_as_envmap);
void                      make_material_parse_paint_material(bool bake_previews);
void                      make_material_parse_brush(void);
void                      util_render_make_material_preview(void);
void                      layers_update_fill_layers(void);
void                      history_new_material(void);
bool                      tab_textures_nodes_use_texture(ui_node_t_array_t *nodes, i32 index);
void                      tab_textures_delete_texture(asset_t *asset);
i32                       box_preferences_add_tab(char *label, void (*draw)(void));
void                      box_preferences_show(void);
void                      ui_menubar_add_category(char *label, void (*draw_items)(void));
bool                      ui_menu_button(char *text, char *label, int icon);
bool                      ui_icon_button(char *text, int icon, int align);
void                      ui_files_show(char *filters, bool is_save, bool open_multiple, void (*files_done)(char *));
void                      console_info(char *s);
void                      console_error(char *s);
void                      console_toast(char *s);

#define OP_ICON_NONE        -1 // paint/sources/enums.h
#define OP_ICON_FOLDER_OPEN 20
#define OP_ICON_REPLAY      122
#define TAB_AREA_MATERIALS  1 // ui_base_hwnds : panneau des matériaux, comme import_folder_run
#define OP_TOOL_COLORID     8 // TOOL_TYPE_COLORID

#define PREF_SBSRENDER "subzero.sbsrender"
#define ENV_SBSRENDER  "SUBZERO_SBSRENDER" // passe devant la préférence (lot E5)
#define RECIPE         "subzero.json"
#define MAX_LINKS      16
#define NODE_X         72 // placement des nœuds Image Texture, comme import_folder_run
#define NODE_Y         100
#define NODE_H         180
#define TEXT_WIDTH     100
#define OCIO_POLL      15
#ifdef __APPLE__
#define DESIGNER_WHERE "/Applications"
#else
#define DESIGNER_WHERE "/opt/Adobe"
#endif

#ifndef _WIN32
extern char **environ;
#endif

static struct {
	studio_window_t *win;
	char            *pending;     // cible à ouvrir au démarrage de la fenêtre, JSON {"file"} ou {"dir"} ; NULL sinon
	int              tab;         // onglet des préférences, -1 sans
	char            *path_text;   // champ sbsrender (ui_text_input le remplace sans libérer l'ancien)
	char            *path_synced; // préférence montrée dans le champ
	int              locate_job;  // studio_exec : subzero_render.py locate, 0 sans
	char            *located;     // préférence de ce résultat
	char            *status;      // « sbsrender 16.0.6 (build 11611): <chemin> » ou l'erreur
	bool             status_error;
	char            *ocio_sent;   // état OCIO que la fenêtre connaît (put_ocio), NULL avant son premier état
	int              ocio_tick;   // on_update : l'état OCIO est comparé toutes les OCIO_POLL frames
} sz = {.tab = -1};

static char *dup(const char *s) {
	return strdup(s != NULL ? s : "");
}

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

// Texte d'un membre de l'objet à `base` (malloc) ; "" si absent.
static char *member_text(const char *json, const char *base, const char *name) {
	studio_json_value_t v = studio_json_find(json, string_tmp("%s/%s", base, name));
	char               *s = v.type == STUDIO_JSON_STRING ? studio_json_text(v) : NULL;
	return s != NULL ? s : strdup("");
}

// Chemin de sbsrender : $SUBZERO_SBSRENDER s'il est défini, sinon la préférence (vide : recherche de Designer).
static const char *sbsrender_env(void) {
	const char *env = getenv(ENV_SBSRENDER);
	return env != NULL && env[0] != 0 ? env : NULL;
}

static const char *sbsrender_setting(void) {
	const char *env = sbsrender_env();
	return env != NULL ? env : studio_prefs_string(PREF_SBSRENDER, "");
}

// Gestion couleur d'ArmorPaint (lot E11) : la fenêtre convertit les cartes dans la configuration qui lira les
// textures, et montre ses vignettes par la transformation d'affichage du viewport.
static void put_ocio(text_t *b) {
	bool import = studio_ocio_import_enabled();
	put(b, "{\"enabled\":%s,\"status\":", studio_ocio_ready() && import ? "true" : "false");
	put_quoted(b, import ? studio_ocio_status() : "texture color spaces are disabled (STUDIO_OCIO_IMPORT=0)");
	const char *fields[][2] = {
	    {"config", studio_ocio_config_source()},  {"name", studio_ocio_config_name()},
	    {"origin", studio_ocio_config_origin()},  {"version", studio_ocio_version()},
	    {"working_space", studio_ocio_working_space()}, {"display", studio_ocio_view_display()},
	    {"view", studio_ocio_view_view()},       {"look", studio_ocio_view_look()},
	};
	for (int i = 0; i < (int)(sizeof(fields) / sizeof(fields[0])); i++) {
		put(b, ",\"%s\":", fields[i][0]);
		put_quoted(b, fields[i][1]);
	}
	put(b, ",\"viewport\":%s}", studio_ocio_view_active() ? "true" : "false");
}

// studio.env où écrire les réglages, et origine des variables de couleur : un fichier, ou "environment" (une vraie
// variable d'environnement, que le fichier ne peut pas changer).
static void put_env(text_t *b) {
	put(b, "{\"file\":");
	put_quoted(b, studio_env_file());
	put(b, ",\"origins\":{");
	bool first = true;
#ifndef _WIN32
	for (char **e = environ; e != NULL && *e != NULL; e++) {
		const char *eq = strchr(*e, '=');
		size_t      n  = eq != NULL ? (size_t)(eq - *e) : 0;
		if (n == 0 || !((n == 4 && strncmp(*e, "OCIO", 4) == 0) || strncmp(*e, "STUDIO_OCIO_", 12) == 0 || strncmp(*e, "SUBZERO_OCIO_", 13) == 0)) {
			continue;
		}
		char *name = strndup(*e, n);
		if (name == NULL) {
			continue;
		}
		put(b, first ? "" : ",");
		put_quoted(b, name);
		put(b, ":");
		put_quoted(b, studio_env_origin(name));
		first = false;
		free(name);
	}
#endif
	put(b, "}}");
}

static char *ocio_json(void) {
	text_t b = {0};
	put_ocio(&b);
	return b.data;
}

static char *state_json(void) {
	text_t      b    = {0};
	const char *file = project_filepath_get();
	put(&b, "{\"sbsrender\":");
	put_quoted(&b, sbsrender_setting());
	put(&b, ",\"project\":{\"file\":");
	put_quoted(&b, file != NULL ? file : "");
	put(&b, "},\"resolution\":[%d,%d],\"bits\":%d,\"open\":%s,\"ocio\":", config_get_texture_res_x(), config_get_texture_res_y(),
	    base_bits == TEXTURE_BITS_BITS32 ? 32 : base_bits == TEXTURE_BITS_BITS16 ? 16 : 8, sz.pending != NULL ? sz.pending : "null");
	put_ocio(&b);
	put(&b, ",\"env\":");
	put_env(&b);
	put(&b, "}");
	free(sz.ocio_sent);
	sz.ocio_sent = ocio_json();
	return b.data;
}

// ---- Matériau ---------------------------------------------------------------------------------------------------------

typedef struct {
	char *file;
	int   output;      // 0 couleur, 1 alpha
	int   socket;      // entrée de la sortie PBR : 0 base, 1 opacité, 2 occlusion, 3 rugosité, 4 métal, 5 normale, 7 hauteur
	char *color_space;     // espace OCIO de la texture (nom, alias ou rôle de la configuration, sinon espace intégré comme
	                       // srgb_tx), "" : choix automatique
	char *color_space_was; // celui que subzero lui a donné au rendu précédent, "" sans
} link_t;

static bool in_dir(const char *file, const char *dir) {
	size_t n = strlen(dir);
	return n > 0 && strncmp(file, dir, n) == 0 && (file[n] == '/' || file[n] == '\\') && strchr(file + n + 1, '/') == NULL;
}

static void free_links(link_t *links, int count) {
	for (int i = 0; i < count; i++) {
		free(links[i].file);
		free(links[i].color_space);
		free(links[i].color_space_was);
	}
}

// Liens demandés ; faux (message dans err) si l'un d'eux est invalide.
static bool read_links(const char *req, const char *dir, link_t *links, int *count, char *err, size_t size) {
	studio_json_value_t arr = studio_json_find(req, "/args/links");
	int                 n   = arr.type == STUDIO_JSON_ARRAY ? studio_json_length(arr) : 0;
	*count                  = 0;
	if (n == 0 || n > MAX_LINKS) {
		snprintf(err, size, "links: 1 to %d expected", MAX_LINKS);
		return false;
	}
	for (int i = 0; i < n; i++) {
		const char *base   = string_tmp("/args/links/%d", i);
		char       *file   = member_text(req, base, "file");
		int         output = (int)studio_json_int(studio_json_find(req, string_tmp("%s/output", base)), -1);
		int         socket = (int)studio_json_int(studio_json_find(req, string_tmp("%s/socket", base)), -1);
		if (!in_dir(file, dir) || !iron_file_exists(file) || (output != 0 && output != 1) || socket < 0 || socket > 7 || socket == 6) {
			snprintf(err, size, "link %d: bad file, output or socket (%s)", i, file);
			free(file);
			free_links(links, *count);
			*count = 0;
			return false;
		}
		links[(*count)++] = (link_t){.file            = file,
		                             .output          = output,
		                             .socket          = socket,
		                             .color_space     = member_text(req, base, "color_space"),
		                             .color_space_was = member_text(req, base, "color_space_was")};
	}
	return true;
}

static int asset_index(const char *file) {
	asset_t_array_t *assets = g_project->_->assets;
	for (int i = 0; i < (int)assets->length; i++) {
		asset_t *a = assets->buffer[i];
		if (a != NULL && a->file != NULL && strcmp(a->file, file) == 0) {
			return i;
		}
	}
	return -1;
}

// Espace d'un lien : nom, alias ou rôle de la configuration, sinon espace de la configuration intégrée ; NULL sans.
static const char *link_space(const char *name) {
	if (name == NULL || name[0] == 0) {
		return NULL;
	}
	const char *cs = studio_ocio_resolve(name);
	return cs != NULL ? cs : studio_ocio_identify_builtin(name);
}

// Asset du fichier, importé s'il n'est pas dans le projet, rechargé sinon (fichier réécrit par le rendu). Une texture
// neuve prend l'espace du lien (lot E6 : la couleur de base de sbsrender est encodée sRGB, même en EXR, que l'import
// OCIO lirait sinon comme de la lumière linéaire ; lot E11 : espace réglé dans subzero). Une texture déjà là garde le
// sien, que l'artiste a pu changer, sauf s'il vaut encore celui que subzero lui avait donné : elle suit alors le réglage.
static int ensure_asset(const link_t *link, int *reloaded) {
	const char *file = link->file;
	int         i    = asset_index(file);
	bool        ocio = link->color_space[0] != 0 && studio_ocio_import_enabled();
	const char *cs   = ocio ? link_space(link->color_space) : NULL;
	if (i >= 0) {
		const char *now = studio_texture_color_space(file);
		const char *was = cs != NULL ? link_space(link->color_space_was) : NULL;
		if (cs != NULL && now != NULL && was != NULL && strcmp(now, was) == 0 && strcmp(now, cs) != 0 && studio_texture_set_color_space(file, cs)) {
			iron_log("subzero: %s now in %s (was %s)", file, cs, was);
			(*reloaded)++;
		}
		else if (studio_texture_reload(file)) {
			(*reloaded)++;
		}
		return i;
	}
	if (ocio) {
		if (cs == NULL || !studio_texture_set_color_space(file, cs)) {
			iron_log("subzero: color space %s not available for %s, automatic choice", link->color_space, file);
		}
	}
	import_texture_run(string_copy((char *)file), false); // le chemin est gardé par l'asset
	return asset_index(file);
}

// Comme tab_textures_is_texture_used, sauf la texture de l'outil ColorID quand il n'est pas choisi : son indice vaut 0
// par défaut, ce qui garderait à tort la première texture du projet.
static bool texture_used(int index) {
	asset_t *a = g_project->_->assets->buffer[index];
	if ((g_context->tool == OP_TOOL_COLORID && index == g_context->colorid) || (g_project->envmap != NULL && strcmp(g_project->envmap, a->file) == 0)) {
		return true;
	}
	for (int i = 0; i < (int)g_project->_->materials->length; i++) {
		if (tab_textures_nodes_use_texture((ui_node_t_array_t *)g_project->_->materials->buffer[i]->canvas->nodes, index)) {
			return true;
		}
	}
	for (int i = 0; i < (int)g_project->_->material_groups->length; i++) {
		if (tab_textures_nodes_use_texture((ui_node_t_array_t *)g_project->_->material_groups->buffer[i]->canvas->nodes, index)) {
			return true;
		}
	}
	for (int i = 0; i < (int)g_project->_->brushes->length; i++) {
		if (tab_textures_nodes_use_texture((ui_node_t_array_t *)g_project->_->brushes->buffer[i]->canvas->nodes, index)) {
			return true;
		}
	}
	return false;
}

// Textures du dossier que les liens n'ont plus (autre format, sortie retirée) : retirées du projet si plus rien ne
// les utilise, sinon gardées et ajoutées à kept (tableau JSON) ; rend le nombre de retirées.
static int remove_stale_assets(const char *dir, link_t *links, int count, text_t *kept) {
	int              removed = 0;
	asset_t_array_t *assets  = g_project->_->assets;
	for (int i = (int)assets->length - 1; i >= 0; i--) {
		asset_t *a      = assets->buffer[i];
		bool     linked = false;
		for (int k = 0; k < count && !linked; k++) {
			linked = a != NULL && a->file != NULL && strcmp(a->file, links[k].file) == 0;
		}
		if (a == NULL || a->file == NULL || linked || !in_dir(a->file, dir)) {
			continue;
		}
		if (texture_used(i)) {
			put(kept, kept->len > 1 ? "," : "");
			put_quoted(kept, a->file);
			continue;
		}
		iron_log("subzero: texture %s removed from the project (no longer rendered)", a->file);
		tab_textures_delete_texture(a);
		removed++;
	}
	return removed;
}

static bool is_image_node(ui_node_t *n) {
	return n != NULL && n->type != NULL && strcmp(n->type, "TEX_IMAGE") == 0 && n->buttons != NULL && n->buttons->length > 0 &&
	       n->buttons->buffer[0]->default_value != NULL && n->buttons->buffer[0]->default_value->length > 0;
}

// Fichier de la texture d'un nœud Image Texture, NULL sinon.
static const char *node_file(ui_node_t *n) {
	if (!is_image_node(n)) {
		return NULL;
	}
	int              i      = (int)n->buttons->buffer[0]->default_value->buffer[0];
	asset_t_array_t *assets = g_project->_->assets;
	return i >= 0 && i < (int)assets->length && assets->buffer[i] != NULL ? assets->buffer[i]->file : NULL;
}

static ui_node_t *output_node(ui_node_canvas_t *c) {
	for (int i = 0; i < (int)c->nodes->length; i++) {
		if (strcmp(c->nodes->buffer[i]->type, "OUTPUT_MATERIAL_PBR") == 0) {
			return c->nodes->buffer[i];
		}
	}
	return NULL;
}

static ui_node_t *file_node(ui_node_canvas_t *c, const char *file) {
	for (int i = 0; i < (int)c->nodes->length; i++) {
		const char *f = node_file(c->nodes->buffer[i]);
		if (f != NULL && strcmp(f, file) == 0) {
			return c->nodes->buffer[i];
		}
	}
	return NULL;
}

static int link_into(ui_node_canvas_t *c, int to_id, int to_socket) {
	for (int i = 0; i < (int)c->links->length; i++) {
		ui_node_link_t *l = c->links->buffer[i];
		if (l->to_id == to_id && l->to_socket == to_socket) {
			return i;
		}
	}
	return -1;
}

static bool has_link(ui_node_canvas_t *c, int from_id, int from_socket, int to_id, int to_socket) {
	int i = link_into(c, to_id, to_socket);
	return i >= 0 && c->links->buffer[i]->from_id == from_id && c->links->buffer[i]->from_socket == from_socket;
}

// Lien vers une entrée, qui remplace celui qu'elle avait.
static void set_link(ui_node_canvas_t *c, int from_id, int from_socket, int to_id, int to_socket) {
	int old = link_into(c, to_id, to_socket);
	if (old >= 0) {
		array_splice((any_array_t *)c->links, old, 1);
	}
	ui_node_link_t *l = malloc(sizeof(ui_node_link_t));
	*l = (ui_node_link_t){.id = ui_next_link_id(c->links), .from_id = from_id, .from_socket = from_socket, .to_id = to_id, .to_socket = to_socket};
	any_array_push((any_array_t *)c->links, l);
}

// Nœud Image Texture dans le canevas du matériau (pas celui que montre l'éditeur de nœuds, qui peut en être un autre).
static ui_node_t *add_image_node(slot_material_t *m, int asset, float y) {
	ui_node_t *t = nodes_material_get_node_t("TEX_IMAGE");
	if (t == NULL) {
		return NULL;
	}
	ui_node_t *n = ui_nodes_make_node(t, m->nodes, m->canvas);
	any_array_push((any_array_t *)m->canvas->nodes, n);
	n->buttons->buffer[0]->default_value = f32_array_create_x((float)asset);
	n->x                                 = NODE_X;
	n->y                                 = y;
	return n;
}

// Premier matériau dont un nœud Image Texture lit une texture du dossier.
static slot_material_t *find_material(const char *dir) {
	slot_material_t_array_t *materials = g_project->_->materials;
	for (int i = 0; i < (int)materials->length; i++) {
		ui_node_canvas_t *c = materials->buffer[i]->canvas;
		for (int j = 0; c != NULL && j < (int)c->nodes->length; j++) {
			const char *f = node_file(c->nodes->buffer[j]);
			if (f != NULL && in_dir(f, dir)) {
				return materials->buffer[i];
			}
		}
	}
	return NULL;
}

const char *subzero_material_dir(void *slot_material) {
	slot_material_t *m = slot_material;
	if (m == NULL || m->canvas == NULL) {
		return "";
	}
	for (int j = 0; j < (int)m->canvas->nodes->length; j++) {
		const char *f     = node_file(m->canvas->nodes->buffer[j]);
		const char *slash = f != NULL ? strrchr(f, '/') : NULL;
		if (slash != NULL) {
			char *dir = string_tmp("%.*s", (int)(slash - f), f);
			if (iron_file_exists(string_tmp("%s/%s", dir, RECIPE))) {
				return dir;
			}
		}
	}
	return "";
}

// Comme import_folder_run : matériau neuf, nœud Color retiré, un nœud Image Texture par fichier.
static slot_material_t *create_material(const char *name, link_t *links, int count, int *reloaded) {
	int assets[MAX_LINKS];
	for (int i = 0; i < count; i++) {
		assets[i] = ensure_asset(&links[i], reloaded);
		if (assets[i] < 0) {
			return NULL;
		}
	}
	slot_material_t *m = slot_material_create(g_project->_->materials->buffer[0]->data, NULL);
	any_array_push((any_array_t *)g_project->_->materials, m);
	g_context->material = m;
	ui_node_canvas_t *c = m->canvas;
	c->name             = string_copy((char *)name);
	for (int i = 0; i < (int)c->nodes->length; i++) {
		if (strcmp(c->nodes->buffer[i]->name, "Color") == 0) {
			ui_remove_node(c->nodes->buffer[i], c);
			break;
		}
	}
	ui_node_t *out = output_node(c);
	int        pos = 0;
	for (int i = 0; out != NULL && i < count; i++) {
		ui_node_t *n = file_node(c, links[i].file);
		if (n == NULL) {
			n = add_image_node(m, assets[i], NODE_Y + NODE_H * pos++);
		}
		if (n != NULL) {
			set_link(c, n->id, links[i].output, out->id, links[i].socket);
		}
	}
	make_material_parse_paint_material(true);
	util_render_make_material_preview();
	ui_base_hwnds->buffer[TAB_AREA_MATERIALS]->redraws = 2;
	history_new_material();
	return m;
}

// Textures rechargées, nœuds des fichiers disparus retirés, nœuds des nouveaux ajoutés ; un lien manquant n'est remis
// que si l'entrée est libre (l'artiste a pu recâbler le matériau).
static void update_material(slot_material_t *m, const char *dir, link_t *links, int count, int *reloaded) {
	int assets[MAX_LINKS];
	for (int i = 0; i < count; i++) {
		assets[i] = ensure_asset(&links[i], reloaded);
	}
	slot_material_t *current = g_context->material;
	g_context->material      = m; // rappels de ui_remove_node (aperçus des nœuds)
	ui_node_canvas_t *c      = m->canvas;
	for (int i = 0; i < (int)c->nodes->length;) {
		ui_node_t  *n      = c->nodes->buffer[i];
		const char *f      = node_file(n);
		bool        wanted = f == NULL || !in_dir(f, dir);
		for (int k = 0; !wanted && k < count; k++) {
			wanted = strcmp(f, links[k].file) == 0;
		}
		if (wanted) {
			i++;
		}
		else {
			ui_remove_node(n, c);
		}
	}
	ui_node_t *out    = output_node(c);
	float      bottom = NODE_Y - NODE_H;
	for (int i = 0; i < (int)c->nodes->length; i++) {
		if (is_image_node(c->nodes->buffer[i]) && c->nodes->buffer[i]->y > bottom) {
			bottom = c->nodes->buffer[i]->y;
		}
	}
	for (int i = 0; out != NULL && i < count; i++) {
		if (assets[i] < 0) {
			continue;
		}
		ui_node_t *n = file_node(c, links[i].file);
		if (n == NULL) {
			bottom += NODE_H;
			n = add_image_node(m, assets[i], bottom);
			if (n != NULL) {
				set_link(c, n->id, links[i].output, out->id, links[i].socket);
			}
		}
		else if (!has_link(c, n->id, links[i].output, out->id, links[i].socket) && link_into(c, out->id, links[i].socket) < 0) {
			set_link(c, n->id, links[i].output, out->id, links[i].socket);
		}
	}
	g_context->material = current;
}

// Frame suivante : calques de remplissage du matériau, son aperçu, puis le matériau courant.
static void refresh_material(void *data) {
	int                      id        = (int)(intptr_t)data;
	slot_material_t_array_t *materials = g_project->_->materials;
	slot_material_t         *m         = NULL;
	for (int i = 0; i < (int)materials->length; i++) {
		if (materials->buffer[i]->id == id) {
			m = materials->buffer[i];
		}
	}
	if (m == NULL) {
		return;
	}
	int fills = 0;
	for (int i = 0; i < (int)g_project->_->layers->length; i++) {
		fills += g_project->_->layers->buffer[i]->fill_material == m ? 1 : 0;
	}
	iron_log("subzero: material %s refreshed, %d fill layer(s)", m->canvas->name, fills);
	slot_material_t *current = g_context->material;
	g_context->material      = m;
	layers_update_fill_layers();
	make_material_parse_paint_material(true);
	util_render_make_material_preview();
	g_context->material = current;
	if (current != m) {
		make_material_parse_paint_material(true);
	}
	make_material_parse_brush();
	ui_base_hwnds->buffer[TAB_AREA_MATERIALS]->redraws = 2;
	ui_nodes_hwnd->redraws                             = 2;
	g_context->ddirty                                  = 2;
}

static void material_request(int id, const char *op, const char *req) {
	char   *dir  = member_text(req, "/args", "dir");
	char   *name = member_text(req, "/args", "name");
	link_t  links[MAX_LINKS];
	int     count = 0;
	char    err[1024];
	int     reloaded = 0;
	bool    created  = false;
	if (dir[0] == 0 || !read_links(req, dir, links, &count, err, sizeof(err))) {
		studio_window_reply_error(sz.win, id, "bad_request", dir[0] == 0 ? "dir expected" : err);
		free(dir);
		free(name);
		return;
	}
	slot_material_t *m = strcmp(op, "update") == 0 ? find_material(dir) : NULL;
	if (m != NULL) {
		update_material(m, dir, links, count, &reloaded);
		iron_log("subzero: material %s updated from %s, %d texture(s) reloaded", m->canvas->name, dir, reloaded);
		sys_notify_on_next_frame(refresh_material, (void *)(intptr_t)m->id);
	}
	else {
		const char *slash = strrchr(dir, '/');
		m                 = create_material(name[0] != 0 ? name : slash != NULL ? slash + 1 : dir, links, count, &reloaded);
		created           = m != NULL;
		if (created) {
			iron_log("subzero: material %s created from %s", m->canvas->name, dir);
		}
	}
	if (m == NULL) {
		studio_window_reply_error(sz.win, id, "import_failed", "the textures could not be imported: see the console");
	}
	else {
		text_t kept = {0};
		put(&kept, "[");
		int    removed = remove_stale_assets(dir, links, count, &kept);
		text_t b       = {0};
		put(&kept, "]");
		put(&b, "{\"material\":");
		put_quoted(&b, m->canvas->name);
		put(&b, ",\"reloaded\":%d,\"created\":%s,\"removed\":%d,\"kept\":%s}", reloaded, created ? "true" : "false", removed,
		    kept.data != NULL ? kept.data : "[]");
		studio_window_reply(sz.win, id, b.data);
		free(b.data);
		free(kept.data);
	}
	free_links(links, count);
	free(dir);
	free(name);
}

// Matériaux du projet qui ont un dossier subzero (scripts, bancs de test), malloc :
// {"materials": N, "subzero": [{"name", "dir", "current", "links": [{"file" (nom seul), "output", "socket"}]}]}
static char *materials_json(void) {
	text_t                   b         = {0};
	slot_material_t_array_t *materials = g_project->_->materials;
	put(&b, "{\"materials\":%d,\"subzero\":[", (int)materials->length);
	bool first = true;
	for (int i = 0; i < (int)materials->length; i++) {
		slot_material_t *m   = materials->buffer[i];
		const char      *dir = subzero_material_dir(m);
		if (dir[0] == 0) {
			continue;
		}
		put(&b, "%s{\"name\":", first ? "" : ",");
		put_quoted(&b, m->canvas->name);
		put(&b, ",\"dir\":");
		put_quoted(&b, dir);
		put(&b, ",\"current\":%s,\"links\":[", m == g_context->material ? "true" : "false");
		ui_node_t *out  = output_node(m->canvas);
		bool       more = false;
		for (int k = 0; out != NULL && k < (int)m->canvas->links->length; k++) {
			ui_node_link_t *l = m->canvas->links->buffer[k];
			const char     *f = l->to_id == out->id ? node_file(ui_get_node(m->canvas->nodes, l->from_id)) : NULL;
			if (f != NULL) {
				put(&b, "%s{\"file\":", more ? "," : "");
				put_quoted(&b, strrchr(f, '/') != NULL ? strrchr(f, '/') + 1 : f);
				put(&b, ",\"output\":%d,\"socket\":%d}", l->from_socket, l->to_socket);
				more = true;
			}
		}
		put(&b, "]}");
		first = false;
	}
	put(&b, "]}");
	return b.data;
}

// ---- Fenêtre ----------------------------------------------------------------------------------------------------------

static void show_prefs(const char *tab) {
	if (tab != NULL && strcmp(tab, "color") == 0 && studio_ocio_prefs_show()) {
		return;
	}
	if (sz.tab >= 0) {
		box_preferences_tab = sz.tab;
		box_preferences_show();
	}
}

static void on_request(studio_window_t *w, int id, const char *op, const char *req) {
	(void)w;
	if (strcmp(op, "state") == 0) {
		char *state = state_json();
		studio_window_reply(sz.win, id, state != NULL ? state : "null");
		free(state);
		free(sz.pending);
		sz.pending = NULL;
	}
	else if (strcmp(op, "create") == 0 || strcmp(op, "update") == 0) {
		material_request(id, op, req);
	}
	else if (strcmp(op, "show_prefs") == 0) {
		char *tab = member_text(req, "/args", "tab");
		show_prefs(tab);
		free(tab);
		studio_window_reply(sz.win, id, "{}");
	}
	else {
		studio_window_reply_error(sz.win, id, "bad_request", string_tmp("unknown operation: %s", op));
	}
}

static void on_closed(studio_window_t *w) {
	(void)w;
	free(sz.pending);
	sz.pending = NULL;
	free(sz.ocio_sent);
	sz.ocio_sent = NULL;
}

// {"file": path} ou {"dir": path} (dossier), malloc ; NULL sans chemin.
static char *target_json(const char *path) {
	if (path == NULL || path[0] == 0) {
		return NULL;
	}
	char *q = studio_json_quote(path);
	char *j = NULL;
	if (q != NULL) {
		j = malloc(strlen(q) + 16);
		if (j != NULL) {
			sprintf(j, "{\"%s\":%s}", iron_is_directory((char *)path) ? "dir" : "file", q);
		}
	}
	free(q);
	return j;
}

void subzero_open(const char *path) {
	char *target = target_json(path);
	if (studio_window_is_open(sz.win)) {
		studio_window_send(sz.win, "{\"event\":\"raise\"}");
		if (target != NULL) {
			studio_window_send(sz.win, string_tmp("{\"event\":\"open\",%s", target + 1));
		}
		free(target);
		return;
	}
	free(sz.pending);
	sz.pending = target;
	if (studio_window_open(sz.win, NULL)) {
		console_toast("Opening subzero");
	}
}

bool subzero_is_open(void) {
	return studio_window_is_open(sz.win);
}

static void send_state_event(const char *event) {
	if (!studio_window_is_open(sz.win)) {
		return;
	}
	char *state = state_json();
	if (state != NULL) {
		studio_window_send(sz.win, string_tmp("{\"event\":\"%s\",\"state\":%s}", event, state));
	}
	free(state);
}

static void on_project_change(bool opened) {
	(void)opened;
	send_state_event("project");
}

// ---- Import --------------------------------------------------------------------------------------------------------

static void import_package(char *path) {
	subzero_open(path);
}

static void register_importers(void) {
	const char *formats[] = {"sbsar", "sbs", "SBSAR", "SBS"};
	for (int i = 0; i < 4; i++) {
		any_map_set(import_text_importers, (char *)formats[i], import_package);
	}
	// path_is_text compare sans la casse : le minuscule suffit pour que l'import aiguille les paquets vers ici.
	for (int i = 0; i < 2; i++) {
		if (string_array_index_of(path_text_formats(), (char *)formats[i]) < 0) {
			any_array_push((any_array_t *)_path_text_formats, (char *)formats[i]);
		}
	}
}

// ---- sbsrender -----------------------------------------------------------------------------------------------------

static const char *python(void) {
	const char *p = getenv("STUDIO_PYTHON");
	if (p == NULL || p[0] == 0) {
		p = getenv("STUDIO_SG_PYTHON");
	}
	return p != NULL && p[0] != 0 ? p : "python3";
}

// subzero_render.py locate, en tâche de fond : version et chemin pour l'onglet et le log.
static void locate_start(void) {
	if (sz.locate_job > 0) {
		studio_exec_release(sz.locate_job);
	}
	const char *pref = sbsrender_setting();
	free(sz.located);
	sz.located         = dup(pref);
	char       *script = string_tmp("%ssubzero_render.py", path_data());
	const char *argv[] = {python(), "-B", script, "--sbsrender", pref, "locate", NULL};
	sz.locate_job      = studio_exec_start(argv, NULL, 30000);
	free(sz.status);
	sz.status       = dup("Looking for sbsrender...");
	sz.status_error = false;
}

static void locate_poll(void) {
	if (sz.locate_job <= 0 || studio_exec_state(sz.locate_job) != STUDIO_EXEC_DONE) {
		return;
	}
	const studio_proc_result_t *r   = studio_exec_result(sz.locate_job);
	const char                 *out = r != NULL && r->out != NULL ? r->out : "";
	studio_json_value_t         ok  = studio_json_find(out, "/ok");
	char                       *message;
	if (studio_json_truthy(ok)) {
		char *version   = member_text(out, "/result", "version");
		char *path      = member_text(out, "/result", "sbsrender");
		bool  detected  = studio_json_truthy(studio_json_find(out, "/result/detected"));
		message         = dup(string_tmp("sbsrender %s%s: %s", version, detected ? " (found in " DESIGNER_WHERE ")" : "", path));
		sz.status_error = false;
		const char *env = sbsrender_env();
		iron_log("subzero: sbsrender %s (%s), %s", version, path,
		         env != NULL ? string_tmp("from $" ENV_SBSRENDER " (%s)", studio_env_origin(ENV_SBSRENDER))
		         : detected  ? "found in " DESIGNER_WHERE
		                     : "from the preference " PREF_SBSRENDER);
		free(version);
		free(path);
	}
	else {
		char *error = member_text(out, "", "error");
		message     = dup(error[0] != 0 ? error
		                                : string_tmp("could not run %s subzero_render.py: %s", python(),
		                                             r != NULL && r->error[0] != 0 ? r->error : r != NULL && r->err != NULL ? r->err : ""));
		sz.status_error = true;
		iron_log("subzero: %s", message);
		free(error);
	}
	free(sz.status);
	sz.status = message;
	studio_exec_release(sz.locate_job);
	sz.locate_job = 0;
}

// La configuration, la transformation du viewport ou l'import OCIO ont changé (menu Color, préférences) : la fenêtre
// reçoit le nouvel état (évènement ocio).
static void ocio_poll(void) {
	if (sz.ocio_sent == NULL || ++sz.ocio_tick < OCIO_POLL || !studio_window_is_open(sz.win)) {
		return;
	}
	sz.ocio_tick = 0;
	char *now    = ocio_json();
	if (now != NULL && strcmp(now, sz.ocio_sent) != 0) {
		send_state_event("ocio"); // remplace sz.ocio_sent
	}
	free(now);
}

static void on_update(void *data) {
	(void)data;
	locate_poll();
	ocio_poll();
}

static void set_sbsrender(void *data) {
	char *path = data;
	studio_prefs_set_string(PREF_SBSRENDER, path);
	locate_start();
	if (studio_window_is_open(sz.win)) {
		char *q = studio_json_quote(sbsrender_setting());
		studio_window_send(sz.win, string_tmp("{\"event\":\"prefs\",\"sbsrender\":%s}", q != NULL ? q : "\"\""));
		free(q);
	}
	free(path);
}

static void schedule_sbsrender(const char *path) {
	sys_notify_on_next_frame(set_sbsrender, dup(path));
}

// ---- Interface -------------------------------------------------------------------------------------------------------

// ui_text ne passe pas à la ligne : coupé aux fins de ligne, puis aux espaces.
static void text_lines(const char *text) {
	const char *p = text != NULL ? text : "";
	while (*p != 0) {
		size_t len = strcspn(p, "\n");
		size_t cut = len;
		if (len > TEXT_WIDTH) {
			cut = TEXT_WIDTH;
			while (cut > 0 && p[cut] != ' ') {
				cut--;
			}
			if (cut == 0) {
				cut = TEXT_WIDTH;
			}
		}
		ui_text(string_tmp("%.*s", (int)cut, p), UI_ALIGN_LEFT, 0);
		p += cut;
		while (*p == ' ' || *p == '\n') {
			p++;
		}
	}
}

// Comme box_preferences_section (static dans ui/box_preferences.c).
static void section(char *title) {
	ui_separator(8, false);
	ui_text(title, UI_ALIGN_LEFT, 0x00000000);
	ui_separator(2, true);
	ui_separator(4, false);
}

static void sbsrender_picked(char *path) {
	schedule_sbsrender(path);
}

static void tab_draw(void) {
	const char *pref = studio_prefs_string(PREF_SBSRENDER, "");
	if (sz.path_synced == NULL || strcmp(sz.path_synced, pref) != 0) {
		free(sz.path_synced);
		free(sz.path_text); // malloc, ou calloc de ui_text_input (string_copy)
		sz.path_synced = dup(pref);
		sz.path_text   = dup(pref);
	}
	const char *env    = sbsrender_env();
	bool        locked = env != NULL;
	section("sbsrender");
	ui_row(f32_array_create_from_raw_tmp((f32[]){0.6f, 0.2f, 0.2f}, 3));
	g_ui->enabled = !locked;
	ui_text_input(&sz.path_text, "sbsrender", UI_ALIGN_LEFT, !locked, false);
	if (!locked && ui_item_changed()) {
		schedule_sbsrender(sz.path_text);
	}
	if (ui_icon_button("Browse", OP_ICON_FOLDER_OPEN, UI_ALIGN_CENTER) && !locked) {
		ui_files_show("", false, false, sbsrender_picked);
	}
	if (ui_icon_button("Detect", OP_ICON_REPLAY, UI_ALIGN_CENTER) && !locked) {
		schedule_sbsrender("");
	}
	g_ui->enabled = true;
	if (sz.located == NULL || strcmp(sz.located, sbsrender_setting()) != 0) {
		locate_start();
	}
	if (locked) {
		text_lines(string_tmp("$" ENV_SBSRENDER " is set (%s, from %s): the preference above is used in sessions without it.", env,
		                      studio_env_origin(ENV_SBSRENDER)));
	}
	text_lines(sz.status);
	text_lines("The sbsrender tool of Adobe Substance 3D Designer renders the textures. Empty: found in " DESIGNER_WHERE ". The "
	           "Designer app or the folder of sbsrender are accepted too.");

	section("Window");
	text_lines(string_tmp("The subzero window runs with %s and needs PySide6: set STUDIO_PYTHON to change it.", python()));
	text_lines("Materials are rendered next to the project, in <project>_subzero/<material>/, with their recipe (subzero.json).");
}

static void open_picked(char *path) {
	subzero_open(path);
}

static void menu_draw(void) {
	if (ui_menu_button("Open...", "", OP_ICON_NONE)) {
		ui_files_show("sbsar,sbs", false, false, open_picked);
	}
	const char *dir = g_context->material != NULL ? subzero_material_dir(g_context->material) : "";
	g_ui->enabled   = dir[0] != 0;
	if (ui_menu_button("Edit Selected Material", "", OP_ICON_NONE) && dir[0] != 0) {
		subzero_open(dir);
	}
	g_ui->enabled = true;
	if (subzero_is_open() && ui_menu_button("Show Window", "", OP_ICON_NONE)) {
		subzero_open(NULL);
	}
	ui_separator(4, true);
	if (ui_menu_button("Preferences...", "", OP_ICON_NONE)) {
		show_prefs(NULL);
	}
}

// ---- Scripting (minic) -----------------------------------------------------------------------------------------------

#ifdef WITH_EVAL

#include "minic.h"

static minic_val_t mn_open(minic_val_t *a, int n) {
	subzero_open(n > 0 ? (const char *)a[0].p : NULL);
	return minic_val_int(0);
}

static minic_val_t mn_is_open(minic_val_t *a, int n) {
	(void)a, (void)n;
	return minic_val_int(subzero_is_open() ? 1 : 0);
}

static minic_val_t mn_materials_json(minic_val_t *a, int n) {
	(void)a, (void)n;
	static char *last = NULL;
	free(last);
	last = materials_json();
	return minic_val_ptr(last);
}

static minic_val_t mn_material_dir(minic_val_t *a, int n) {
	(void)a, (void)n;
	return minic_val_ptr((void *)subzero_material_dir(g_context->material));
}

#endif

void subzero_init(void) {
	studio_window_desc_t desc = {
	    .name       = "subzero window",
	    .title      = "subzero",
	    .script     = "subzero_window.py",
	    .script_env = "SUBZERO_WINDOW",
	    .on_request = on_request,
	    .on_closed  = on_closed,
	};
	sz.win = studio_window_create(&desc);
	sz.tab = box_preferences_add_tab("subzero", tab_draw);
	ui_menubar_add_category("subzero", menu_draw);
	register_importers();
	studio_project_on_change(on_project_change);
	sys_notify_on_update(on_update, NULL);
	locate_start();
#ifdef WITH_EVAL
	minic_register("subzero_open", "v(p)", mn_open);
	minic_register("subzero_is_open", "i()", mn_is_open);
	minic_register("subzero_material_dir", "p()", mn_material_dir);
	minic_register("subzero_materials_json", "p()", mn_materials_json);
#endif
}
