#include "studio_atlas.h"
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
#include "studio_udim.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Cœur ArmorPaint (paint/sources/globals.h, functions.h)
extern context_t *g_context;
extern project_t *g_project;
extern int        plugins_split_by; // paint/plugins/plugins.c : découpage lu par l'import FBX
extern bool       ui_box_show;
extern char      *ui_box_title;
extern bool       import_mesh_append;
extern char      *console_message;
extern float      console_message_timer;
extern i32        base_bits;
extern i32        base_res;
extern f32        base_res_x;
extern f32        base_res_y;
void              layers_on_resized(void);
void              base_init_undo_layers(void);
void              layers_set_object_mask(void);
slot_layer_t     *layers_new_layer(bool clear, i32 position, slot_layer_t *parent);
void              slot_layer_clear(slot_layer_t *raw, i32 base_color, gpu_texture_t *base_image, f32 occlusion, f32 roughness, f32 metallic);
void              history_new_layer(void);
slot_layer_t     *layers_new_mask(bool clear, slot_layer_t *parent, i32 position);
void              history_new_white_mask(void);
void              history_new_black_mask(void);
void              context_set_layer(slot_layer_t *l);
bool              slot_layer_is_mask(slot_layer_t *raw);
slot_layer_t     *layers_new_group(void);
void              history_new_group(void);
extern f32        layers_default_rough;
void              import_mesh_run(char *path, bool clear_layers, bool replace_existing, bool keep_camera);
void              import_mesh_finish_import(void *_);
i32               util_mesh_udim_tile(char *name);
bool              util_mesh_udim_active(void);
i32               util_mesh_atlas_stride(void);
i32               config_get_texture_res_x(void);
i32               config_get_texture_res_y(void);
void              console_info(char *s);
void              console_log(char *s);
char             *tab_meshes_rename_object(i32 i, char *typed);
void              tab_meshes_sort_by_name(void);
void              tab_meshes_sort_hierarchy(void);
char             *util_udim_stats(void);
char             *util_udim_view_stats(gpu_texture_t *view);
void              util_udim_set_tile(i32 tile);
i32               util_udim_tile(void);
void              util_udim_sync(void);
slot_layer_t     *util_udim_flatten_atlas(bool height_to_normal);
buffer_t         *gpu_get_texture_pixels(gpu_texture_t *image);
void              history_duplicate_layer(void);
void              layers_duplicate_layer(slot_layer_t *l);
void              tab_layers_delete_layer(slot_layer_t *l);
void              history_clear_layer(void);
void              history_invert_mask(void);
void              slot_layer_invert_mask(slot_layer_t *raw);
void              history_undo(void);
void              history_redo(void);
void              viewport_update_camera_type(i32 camera_type);
void              make_material_parse_paint_material(bool bake_previews);
void              render_path_base_draw_gbuffer(void);
void              util_uv_cache_uv_island_map(void);
i32               util_udim_cursor_tile(void);
i32               util_udim_picked_tile(void);
extern bool       util_uv_uvislandmap_cached;
extern gpu_texture_t *util_uv_uvmap;
extern gpu_texture_t *util_uv_trianglemap;
extern gpu_texture_t *util_uv_dilatemap;
void              util_uv_cache_uv_map(void);
void              util_uv_cache_triangle_map(void);
void              util_uv_cache_dilate_map(void);
extern any_array_t *history_undo_layers;
i32               color_from_floats(f32 r, f32 g, f32 b, f32 a);
void              _gpu_begin(gpu_texture_t *render_target, any_array_t *additional, gpu_texture_t *depth_buffer, gpu_clear_t flags, unsigned color, float depth);
void              bake_texture_node_bake(ui_node_t *node, i32 bake_type);
void              history_merge_layers(void);
void              history_apply_mask(void);
void              history_layer_opacity(void);
void              layers_merge_down(void);
slot_layer_t     *layers_merge_group(slot_layer_t *l);
slot_layer_t     *layers_flatten(bool height_to_normal, slot_layer_t_array_t *layers);
void              slot_layer_apply_mask(slot_layer_t *raw);
void              slot_layer_to_paint_layer(slot_layer_t *raw);
bool              slot_layer_is_group(slot_layer_t *raw);
void              make_material_parse_mesh_material(void);
extern bool       bake_texture_node_baking;
extern bool       render_path_paint_baking;
extern any_map_t *render_path_render_targets;
void              tab_meshes_set_linked_override(mesh_object_t *o, i32 mat_index);
void              ui_header_draw_tool_properties_import(char *path);
void              ui_header_draw_tool_properties_to_mask(slot_layer_t *m);
extern slot_layer_t *tab_layers_l;
extern char        *ui_files_filename;
void                tab_layers_draw_layer_context_menu_export_on_file_picked(char *path);
void                tab_layers_draw_layer_context_menu_merge_group(void *_);

#define DEFAULT_NAME "Atlas 1"
#define MAX_NAME     64

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

static int object_count(void) {
	return g_project->_->paint_objects != NULL ? (int)g_project->_->paint_objects->length : 0;
}

// Jeux définis pour les objets actuels (un import qui ajoute des objets laisse atlas_objects trop court).
static bool defined(void) {
	i32_array_t    *objects = g_project->atlas_objects;
	string_array_t *names   = g_project->atlas_names;
	if (objects == NULL || names == NULL || (int)objects->length != object_count()) {
		return false;
	}
	for (int i = 0; i < (int)objects->length; ++i) {
		if (objects->buffer[i] < 0 || objects->buffer[i] >= (int)names->length) {
			return false;
		}
	}
	return true;
}

// Jeux utilisés, dans l'ordre de leur premier objet : c'est l'ordre des masques de calque "atlas" d'ArmorPaint
// (project_get_used_atlases). Rend leur nombre ; used : indices dans les noms.
static int used_atlases(const int *objects, int count, int *used) {
	int n = 0;
	for (int i = 0; i < count; ++i) {
		bool seen = false;
		for (int k = 0; k < n && !seen; ++k) {
			seen = used[k] == objects[i];
		}
		if (!seen) {
			used[n++] = objects[i];
		}
	}
	return n;
}

bool studio_atlas_active(void) {
	i32_array_t *objects = g_project->atlas_objects;
	for (int i = 1; objects != NULL && i < (int)objects->length; ++i) {
		if (objects->buffer[i - 1] != objects->buffer[i]) {
			return true;
		}
	}
	return false;
}

// Jeu (indice dans les noms) d'un masque de calque qui désigne un jeu, -1 sinon.
static int layer_atlas(int mask, const int *used, int used_count) {
	int k = mask - object_count() - 1;
	return mask > object_count() && used_count > 1 && k < used_count ? used[k] : -1;
}

char *studio_atlas_json(void) {
	int  n      = object_count();
	bool ok     = defined();
	int *used   = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
	int  nused  = ok && used != NULL ? used_atlases(g_project->atlas_objects->buffer, n, used) : 0;
	text_t b    = {0};
	put(&b, "{\"names\":[");
	if (ok) {
		for (int i = 0; i < (int)g_project->atlas_names->length; ++i) {
			put(&b, i > 0 ? "," : "");
			put_quoted(&b, g_project->atlas_names->buffer[i]);
		}
	}
	else {
		put_quoted(&b, DEFAULT_NAME);
	}
	put(&b, "],\"objects\":[");
	for (int i = 0; i < n; ++i) {
		put(&b, i > 0 ? ",{\"name\":" : "{\"name\":");
		put_quoted(&b, g_project->_->paint_objects->buffer[i]->base->name);
		put(&b, ",\"atlas\":%d}", ok ? g_project->atlas_objects->buffer[i] : 0);
	}
	put(&b, "],\"layers\":[");
	slot_layer_t_array_t *layers = g_project->_->layers;
	for (int i = 0, first = 1; used != NULL && layers != NULL && i < (int)layers->length; ++i) {
		int atlas = layer_atlas(layers->buffer[i]->object_mask, used, nused);
		if (atlas >= 0) {
			put(&b, first ? "{\"layer\":" : ",{\"layer\":");
			put_quoted(&b, layers->buffer[i]->name);
			put(&b, ",\"atlas\":%d}", atlas);
			first = 0;
		}
	}
	put(&b, "]}");
	free(used);
	return b.data;
}

static bool refuse(char *err, size_t err_size, const char *format, ...) {
	if (err != NULL && err_size > 0) {
		va_list args;
		va_start(args, format);
		vsnprintf(err, err_size, format, args);
		va_end(args);
	}
	return false;
}

static bool valid_name(const char *s) {
	size_t len = strlen(s);
	if (len == 0 || len > MAX_NAME) {
		return false;
	}
	for (size_t i = 0; i < len; ++i) {
		if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f) {
			return false;
		}
	}
	return true;
}

bool studio_atlas_set(const char *json, char *err, size_t err_size) {
	if (err != NULL && err_size > 0) {
		err[0] = 0;
	}
	int                 n     = object_count();
	studio_json_value_t names = studio_json_find(json != NULL ? json : "", "/names");
	studio_json_value_t objs  = studio_json_find(json != NULL ? json : "", "/objects");
	if (names.type != STUDIO_JSON_ARRAY || objs.type != STUDIO_JSON_ARRAY) {
		return refuse(err, err_size, "texture sets: {\"names\": [...], \"objects\": [...]} expected");
	}
	int count = studio_json_length(names);
	if (count < 1 || count > 256) {
		return refuse(err, err_size, "texture sets: 1 to 256 names expected");
	}
	if (studio_json_length(objs) != n) {
		return refuse(err, err_size, "texture sets: one set per object expected (%d objects)", n);
	}
	char **list    = calloc((size_t)count, sizeof(char *));
	int   *objects = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
	int   *used    = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
	int   *before  = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
	bool   ok      = list != NULL && objects != NULL && used != NULL && before != NULL;
	if (!ok) {
		refuse(err, err_size, "out of memory");
	}
	for (int i = 0; ok && i < count; ++i) {
		studio_json_value_t v = studio_json_find(json, string_tmp("/names/%d", i));
		list[i]               = v.type == STUDIO_JSON_STRING ? studio_json_text(v) : NULL;
		ok                    = list[i] != NULL && valid_name(list[i]);
		for (int k = 0; ok && k < i; ++k) {
			ok = strcmp(list[k], list[i]) != 0;
		}
		if (!ok) {
			refuse(err, err_size, "texture sets: name %d is empty, too long, or used twice", i + 1);
		}
	}
	for (int i = 0; ok && i < n; ++i) {
		studio_json_value_t v = studio_json_find(json, string_tmp("/objects/%d", i));
		objects[i]            = (int)studio_json_int(v, -1);
		ok                    = v.type == STUDIO_JSON_NUMBER && objects[i] >= 0 && objects[i] < count;
		if (!ok) {
			refuse(err, err_size, "texture sets: object %s has no set", g_project->_->paint_objects->buffer[i]->base->name);
		}
	}

	// Les calques limités à un jeu le gardent : même indice de nom, nouvelle position parmi les jeux utilisés.
	int nbefore = ok && defined() ? used_atlases(g_project->atlas_objects->buffer, n, before) : 0;
	int nused   = ok ? used_atlases(objects, n, used) : 0;
	slot_layer_t_array_t *layers = g_project->_->layers;
	for (int i = 0; ok && layers != NULL && i < (int)layers->length; ++i) {
		int atlas = layer_atlas(layers->buffer[i]->object_mask, before, nbefore);
		int pos   = -1;
		for (int k = 0; atlas >= 0 && k < nused; ++k) {
			pos = used[k] == atlas ? k : pos;
		}
		if (atlas >= 0 && (pos < 0 || nused < 2)) {
			ok = refuse(err, err_size, "layer '%s' is limited to the set '%s', which would have no object left",
			            layers->buffer[i]->name, atlas < count ? list[atlas] : g_project->atlas_names->buffer[atlas]);
		}
	}
	if (ok) {
		bool remapped = false;
		for (int i = 0; layers != NULL && i < (int)layers->length; ++i) {
			slot_layer_t *l     = layers->buffer[i];
			int           atlas = layer_atlas(l->object_mask, before, nbefore);
			for (int k = 0; atlas >= 0 && k < nused; ++k) {
				if (used[k] == atlas && l->object_mask != n + 1 + k) {
					l->object_mask = n + 1 + k;
					remapped       = true;
				}
			}
		}
		i32_array_t    *a = i32_array_create_from_raw(objects, (uint32_t)n);
		string_array_t *s = (string_array_t *)any_array_create_from_raw((void *[]){}, 0);
		for (int i = 0; i < count; ++i) {
			string_array_push(s, string_copy(list[i]));
		}
		g_project->atlas_objects = a;
		g_project->atlas_names   = s;
		// Le filtre de calques d'ArmorPaint peut désigner un jeu qui n'existe plus.
		if (g_context->layer_filter > n + (nused > 1 ? nused : 0)) {
			g_context->layer_filter = 0;
		}
		if (remapped && g_context->layer != NULL) {
			layers_set_object_mask();
		}
	}
	for (int i = 0; list != NULL && i < count; ++i) {
		free(list[i]);
	}
	free(list);
	free(objects);
	free(used);
	free(before);
	return ok;
}

bool studio_atlas_limit_layer(int atlas) {
	slot_layer_t *l = g_context->layer;
	int           n = object_count();
	if (l == NULL || !defined()) {
		return false;
	}
	int mask = 0;
	if (atlas >= 0) {
		int *used = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
		int  nused = used != NULL ? used_atlases(g_project->atlas_objects->buffer, n, used) : 0;
		int  pos   = -1;
		for (int k = 0; k < nused; ++k) {
			pos = used[k] == atlas ? k : pos;
		}
		free(used);
		if (pos < 0 || nused < 2) {
			return false;
		}
		mask = n + 1 + pos;
	}
	l->object_mask = mask;
	layers_set_object_mask();
	return true;
}

// ---- Fin d'un import UDIM : jeux de textures par matériau, conseil de résolution -------------------------------------

// Résolution des textures de chaque tuile et mémoire d'un calque (studio_udim_hint). Dans la barre d'état si elle est
// libre, dans la console seule sinon.
static void udim_import_hint(void) {
	if (!util_mesh_udim_active()) {
		return;
	}
	bool seen[1000] = {0}; // Tuile - 1000, util_mesh_udim_tile() rend 1001 à 1999
	int  tiles      = 0;
	for (int i = 0; i < object_count(); ++i) {
		i32 tile = util_mesh_udim_tile(g_project->_->paint_objects->buffer[i]->base->name);
		if (tile > 1000 && tile < 2000 && !seen[tile - 1000]) {
			seen[tile - 1000] = true;
			tiles++;
		}
	}
	int bits = base_bits == TEXTURE_BITS_BITS8 ? 8 : base_bits == TEXTURE_BITS_BITS16 ? 16 : 32;
	// Au-delà de 64 tuiles, les calques ne tiennent pas les autres : util_mesh.c le dit déjà (udim-tile-limit)
	char *hint = studio_udim_hint(tiles < 64 ? tiles : 64, config_get_texture_res_x(), config_get_texture_res_y(), bits);
	if (hint == NULL) {
		return;
	}
	if (console_message_timer > 0) {
		console_log(hint); // Un message de l'import (refus, objets sans tuile, tuiles de trop) reste dans la barre d'état
	}
	else {
		console_info(hint);
	}
	free(hint);
}

// Un jeu de textures par matériau, nommé comme lui, avec ses objets <matériau>.<tuile> (et le matériau sans uv, sans
// tuile). Refusé par studio_atlas_set (nom de plus de 64 caractères, plus de 256 matériaux) : les jeux restent ceux
// d'avant, la console dit pourquoi.
static void udim_material_sets(void) {
	int    n       = object_count();
	char **names   = calloc((size_t)(n > 0 ? n : 1), sizeof(char *));
	int   *objects = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
	int    count   = 0;
	for (int i = 0; names != NULL && objects != NULL && i < n; ++i) {
		char  *name = g_project->_->paint_objects->buffer[i]->base->name;
		size_t len  = strlen(name) - (util_mesh_udim_tile(name) > 0 ? 5 : 0); // ".1001"
		int    k    = 0;
		while (k < count && (strlen(names[k]) != len || strncmp(names[k], name, len) != 0)) {
			k++;
		}
		if (k == count) {
			names[count] = calloc(len + 1, 1);
			if (names[count] == NULL) {
				break;
			}
			memcpy(names[count++], name, len);
		}
		objects[i] = k;
	}
	text_t b = {0};
	put(&b, "{\"names\":[");
	for (int k = 0; k < count; ++k) {
		put(&b, k > 0 ? "," : "");
		put_quoted(&b, names[k]);
	}
	put(&b, "],\"objects\":[");
	for (int i = 0; objects != NULL && i < n; ++i) {
		put(&b, i > 0 ? ",%d" : "%d", objects[i]);
	}
	put(&b, "]}");
	char err[512] = "out of memory";
	if (b.data == NULL || names == NULL || objects == NULL || !studio_atlas_set(b.data, err, sizeof(err))) {
		console_log(string_tmp("UDIM import: no texture set per material, %s.", err));
	}
	for (int k = 0; k < count; ++k) {
		free(names[k]);
	}
	free(names);
	free(objects);
	free(b.data);
}

static bool split_file(const char *path) {
	size_t len = path != NULL ? strlen(path) : 0;
	return len > 4 && (strcasecmp(path + len - 4, ".obj") == 0 || strcasecmp(path + len - 4, ".fbx") == 0);
}

void studio_udim_import_done(bool append) {
	string_array_t *assets = g_project->mesh_assets;
	// Le découpage n'est lu que par les importeurs OBJ et FBX : il peut rester sur « matériau et tuile » d'un import
	// précédent quand on importe un .blend ou un .gltf
	if (!append && g_context->split_by == SPLIT_TYPE_MATERIAL_UDIM && assets != NULL && assets->length > 0 && split_file(assets->buffer[0])) {
		udim_material_sets();
	}
	udim_import_hint();
}

// ---- Scripting (minic) -----------------------------------------------------------------------------------------------

#ifdef WITH_EVAL

#include "minic.h"

static minic_val_t mn_studio_atlases(minic_val_t *a, int n) {
	(void)a, (void)n;
	char *json = studio_atlas_json();
	char *s    = string_copy(json != NULL ? json : "");
	free(json);
	return minic_val_ptr(s);
}

// Rend "" si les jeux sont changés, sinon la raison du refus.
static minic_val_t mn_studio_set_atlases(minic_val_t *a, int n) {
	char err[512];
	bool ok = studio_atlas_set(minic_arg_p(a, n, 0), err, sizeof(err));
	return minic_val_ptr(string_copy(ok ? "" : err));
}

void studio_run_next_frames(int from) {
	for (int guard = 0; (i32)_sys_on_next_frames->length > from && guard < 64; ++guard) {
		callback_t *cb = _sys_on_next_frames->buffer[from];
		array_splice(_sys_on_next_frames, from, 1);
		cb->f(cb->data);
		free(cb);
	}
}

// Importe un maillage à la place de celui du projet (calques vidés), découpé comme le « Split By » de la boîte
// d'import : 0 par objet, 1 par matériau, 2 par tuile UDIM, 3 par matériau et par tuile UDIM (un jeu de textures par
// matériau). Pour les scripts batch et les bancs.
static int studio_import(minic_val_t *a, int n, bool append) {
	char *path  = minic_arg_p(a, n, 0);
	int   split = (int)minic_arg_i(a, n, 1);
	if (path == NULL || split < SPLIT_TYPE_OBJECT || split > SPLIT_TYPE_MATERIAL_UDIM) {
		return 0;
	}
	g_context->split_by = (split_type_t)split;
	plugins_split_by    = split;
	gpu_texture_t *current = _draw_current; // comme script_project_open : pas d'import au milieu d'un dessin
	bool           in_use  = gpu_in_use;
	if (in_use) {
		draw_end();
	}
	// La fin de l'import (calques remis à zéro, objets triés par nom et fusionnés) attend l'image suivante : faite ici,
	// pour que la suite du script (jeux de textures, masques de calque) voie les objets dans leur ordre final.
	i32 before = (i32)_sys_on_next_frames->length;
	i32 first  = (i32)g_project->_->paint_objects->length;
	import_mesh_run(path, !append, !append, append);
	studio_run_next_frames(before);
	if (append) { // Comme le bouton Append de la boîte d'import : la fin de l'import n'est pas programmée
		if ((i32)g_project->_->paint_objects->length > first) {
			import_mesh_finish_import(NULL);
		}
		import_mesh_append = false;
	}
	if (in_use) {
		draw_begin(current, false, 0);
	}
	g_context->ddirty = 2;
	return 1;
}

static minic_val_t mn_studio_import_mesh(minic_val_t *a, int n) {
	return minic_val_int(studio_import(a, n, false));
}

// Ajoute un maillage à ceux du projet (calques gardés), comme l'import avec « Replace Existing » décoché.
static minic_val_t mn_studio_append_mesh(minic_val_t *a, int n) {
	return minic_val_int(studio_import(a, n, true));
}

// Titre de la boîte de dialogue ouverte, "" sans boîte : un script batch voit qu'ArmorPaint attend quelqu'un (dépliage
// des uv proposé après l'import d'un maillage sans uv, par exemple).
static minic_val_t mn_studio_dialog_title(minic_val_t *a, int n) {
	(void)a, (void)n;
	return minic_val_ptr(string_copy(ui_box_show && ui_box_title != NULL ? ui_box_title : ""));
}

// Message de la barre d'état, "" s'il est effacé : ce que l'artiste a sous les yeux après une action.
static minic_val_t mn_studio_status_message(minic_val_t *a, int n) {
	(void)a, (void)n;
	return minic_val_ptr(string_copy(console_message_timer > 0 && console_message != NULL ? console_message : ""));
}

// Limite le calque courant à l'objet index (ordre de studio_atlases), -1 : partagé. 1 si fait.
static minic_val_t mn_studio_layer_set_object(minic_val_t *a, int n) {
	int           index = (int)minic_arg_i(a, n, 0);
	slot_layer_t *l     = g_context->layer;
	if (l == NULL || index < -1 || index >= object_count()) {
		return minic_val_int(0);
	}
	l->object_mask = index + 1;
	layers_set_object_mask();
	return minic_val_int(1);
}

// Limite le calque courant au jeu atlas (indice dans les noms), -1 : partagé. 1 si fait.
static minic_val_t mn_studio_layer_set_atlas(minic_val_t *a, int n) {
	return minic_val_int(studio_atlas_limit_layer((int)minic_arg_i(a, n, 0)) ? 1 : 0);
}

// Nouveau calque de peinture au-dessus du calque courant, comme le menu New > Paint Layer, vidé tout de suite (le menu
// le vide à l'image suivante, après la suite du script) ; il devient le calque courant. 1 si fait, 0 au-delà du nombre
// de calques permis.
static minic_val_t mn_studio_layer_new(minic_val_t *a, int n) {
	(void)a, (void)n;
	slot_layer_t *l = layers_new_layer(false, -1, NULL);
	if (l == NULL) {
		return minic_val_int(0);
	}
	gpu_texture_t *current = _draw_current;
	bool           in_use  = gpu_in_use;
	if (in_use) {
		draw_end();
	}
	slot_layer_clear(l, 0x00000000, NULL, 1.0, layers_default_rough, 0.0);
	if (in_use) {
		draw_begin(current, false, 0);
	}
	history_new_layer();
	return minic_val_int(1);
}

// Nouveau masque du calque courant (du parent si c'est un masque), blanc (white 1) ou noir (0), comme les menus New >
// White Mask / Black Mask, vidé tout de suite ; il devient le calque courant. 1 si fait.
static minic_val_t mn_studio_layer_new_mask(minic_val_t *a, int n) {
	int           white = (int)minic_arg_i(a, n, 0);
	slot_layer_t *l     = g_context->layer;
	if (l == NULL) {
		return minic_val_int(0);
	}
	if (slot_layer_is_mask(l)) {
		l = l->parent;
	}
	slot_layer_t *m = layers_new_mask(false, l, -1);
	if (m == NULL) {
		return minic_val_int(0);
	}
	gpu_texture_t *current = _draw_current;
	bool           in_use  = gpu_in_use;
	if (in_use) {
		draw_end();
	}
	slot_layer_clear(m, white ? 0xffffffff : 0x00000000, NULL, 1.0, layers_default_rough, 0.0);
	if (in_use) {
		draw_begin(current, false, 0);
	}
	if (white) {
		history_new_white_mask();
	}
	else {
		history_new_black_mask();
	}
	return minic_val_int(1);
}

// Nouveau groupe de calques (vide), devenu le calque courant, comme le bouton New Group. 1 si fait.
static minic_val_t mn_studio_layer_new_group(minic_val_t *a, int n) {
	(void)a, (void)n;
	if (layers_new_group() == NULL) {
		return minic_val_int(0);
	}
	history_new_group();
	return minic_val_int(1);
}

// Renomme le calque courant (le nom que lit layer('<nom>') des presets studio). 1 si fait.
static minic_val_t mn_studio_layer_rename(minic_val_t *a, int n) {
	char *name = minic_arg_p(a, n, 0);
	if (g_context->layer == NULL || name == NULL || name[0] == 0) {
		return minic_val_int(0);
	}
	g_context->layer->name = string_copy(name);
	return minic_val_int(1);
}

// Montre (1) ou cache (0) le calque courant, comme son œil dans l'onglet Layers. 1 si fait.
static minic_val_t mn_studio_layer_set_visible(minic_val_t *a, int n) {
	if (g_context->layer == NULL) {
		return minic_val_int(0);
	}
	g_context->layer->visible = minic_arg_i(a, n, 0) != 0;
	g_context->ddirty         = 2;
	return minic_val_int(1);
}

// Renomme l'objet index (ordre de studio_atlases) comme l'onglet Meshes : le nom donné, suivi du suffixe de tuile UDIM
// de l'objet, qui reste (un suffixe donné est ignoré, un nom vide laisse le nom d'une tuile). Rend le nouveau nom, ""
// pour un index hors des objets.
static minic_val_t mn_studio_object_rename(minic_val_t *a, int n) {
	int   index = (int)minic_arg_i(a, n, 0);
	char *name  = minic_arg_p(a, n, 1);
	if (index < 0 || index >= object_count() || name == NULL) {
		return minic_val_ptr(string_copy(""));
	}
	return minic_val_ptr(string_copy(tab_meshes_rename_object(index, name)));
}

// Menu Edit > Sort de l'onglet Meshes : objets triés par nom (sous leurs parents). Pour les bancs.
static minic_val_t mn_studio_meshes_sort(minic_val_t *a, int n) {
	(void)a, (void)n;
	tab_meshes_sort_by_name();
	g_context->ddirty = 2;
	return minic_val_int(1);
}

static mesh_object_t *find_object(const char *name) {
	for (int i = 0; name != NULL && i < object_count(); ++i) {
		mesh_object_t *o = g_project->_->paint_objects->buffer[i];
		if (strcmp(o->base->name, name) == 0) {
			return o;
		}
	}
	return NULL;
}

// Champ Parent de l'onglet Meshes : objet child sous parent ("" : aucun), la liste est réordonnée. 1 si fait.
static minic_val_t mn_studio_mesh_set_parent(minic_val_t *a, int n) {
	mesh_object_t *child  = find_object(minic_arg_p(a, n, 0));
	char          *name   = minic_arg_p(a, n, 1);
	mesh_object_t *parent = name != NULL && name[0] != 0 ? find_object(name) : NULL;
	if (child == NULL || child == parent || (name != NULL && name[0] != 0 && parent == NULL)) {
		return minic_val_int(0);
	}
	object_set_parent(child->base, parent != NULL ? parent->base : NULL);
	tab_meshes_sort_hierarchy();
	g_project->mesh_parents = i32_array_create(0);
	g_context->ddirty       = 2;
	return minic_val_int(1);
}

// Masques d'objet des calques et filtre de calques : "Layer 1=0 Layer 2=5 filter=0". Pour les bancs.
static minic_val_t mn_studio_layer_masks(minic_val_t *a, int n) {
	(void)a, (void)n;
	text_t                b      = {0};
	slot_layer_t_array_t *layers = g_project->_->layers;
	for (int i = 0; layers != NULL && i < (int)layers->length; ++i) {
		put(&b, "%s=%d ", layers->buffer[i]->name, layers->buffer[i]->object_mask);
	}
	put(&b, "filter=%d", g_context->layer_filter);
	char *s = string_copy(b.data != NULL ? b.data : "");
	free(b.data);
	return minic_val_ptr(s);
}

// Position à l'écran (0..1, comme script_paint) d'un point du monde, « x y » ; « - » derrière la caméra. Pour les
// bancs : viser une tuile précise, le maillage importé gardant ses coordonnées.
static minic_val_t mn_studio_world_screen(minic_val_t *a, int n) {
	vec4_t clip = vec4_apply_mat4((vec4_t){(f32)minic_arg_f(a, n, 0), (f32)minic_arg_f(a, n, 1), (f32)minic_arg_f(a, n, 2), 1.0f},
	                              scene_camera->vp);
	if (clip.w <= 0.0f) {
		return minic_val_ptr(string_copy("-"));
	}
	return minic_val_ptr(string("%f %f", (clip.x / clip.w + 1.0f) * 0.5f, (-clip.y / clip.w + 1.0f) * 0.5f));
}

// Option de peinture que context_t ne donne pas aux scripts : "camera_align" (pinceau aligné sur la caméra), "ortho"
// (caméra orthographique), "xray", "sym_x", "sym_y", "sym_z" (symétrie, peinte par la frame : render_path_paint_draw,
// pas par script_paint_world) ; "fill_type" prend la valeur (0 objet, 1 face, 2 angle, 3 îlot UV). Le matériau de
// peinture est recompilé. 1 si fait.
static minic_val_t mn_studio_paint_option(minic_val_t *a, int n) {
	char *name = minic_arg_p(a, n, 0);
	bool  on   = minic_arg_i(a, n, 1) != 0;
	if (name == NULL) {
		return minic_val_int(0);
	}
	if (strcmp(name, "camera_align") == 0) {
		g_context->brush_camera_align = on;
	}
	else if (strcmp(name, "ortho") == 0) {
		g_context->camera_type = on ? CAMERA_TYPE_ORTHOGRAPHIC : CAMERA_TYPE_PERSPECTIVE;
		viewport_update_camera_type(g_context->camera_type);
		camera_object_build_mat(scene_camera); // vp, rebuilt by the next frame otherwise
	}
	else if (strcmp(name, "xray") == 0) {
		g_context->xray = on;
	}
	else if (strcmp(name, "sym_x") == 0) {
		g_context->sym_x = on;
	}
	else if (strcmp(name, "sym_y") == 0) {
		g_context->sym_y = on;
	}
	else if (strcmp(name, "sym_z") == 0) {
		g_context->sym_z = on;
	}
	else if (strcmp(name, "fill_type") == 0) {
		g_context->fill_type = (i32)minic_arg_i(a, n, 1);
	}
	else {
		return minic_val_int(0);
	}
	make_material_parse_paint_material(true);
	g_context->ddirty = 2;
	return minic_val_int(1);
}

// Pipettes UDIM au point du monde (x, y, z), gbuffer redessiné : "cursor" donne la tuile sous le pinceau (-1 sans),
// "islands" calcule la carte des îlots UV comme l'image suivante d'un remplissage UV Island, "picked" rend la tuile de
// la dernière pipette, la couleur et l'uv lues.
static minic_val_t mn_studio_udim_pick(minic_val_t *a, int n) {
	char *op = minic_arg_p(a, n, 0);
	if (op == NULL) {
		return minic_val_ptr(string_copy("-"));
	}
	if (strcmp(op, "picked") == 0) {
		i32 c = g_context->picked_color->base;
		return minic_val_ptr(string("tile=%d rgb=%d %d %d uv=%.3f %.3f", util_udim_picked_tile(), (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff,
		                            g_context->uvx_picked, g_context->uvy_picked));
	}
	vec4_t clip = vec4_apply_mat4((vec4_t){(f32)minic_arg_f(a, n, 1), (f32)minic_arg_f(a, n, 2), (f32)minic_arg_f(a, n, 3), 1.0f}, scene_camera->vp);
	if (clip.w <= 0.0f) {
		return minic_val_ptr(string_copy("-"));
	}
	gpu_texture_t *current = _draw_current;
	bool           in_use  = gpu_in_use;
	if (in_use) {
		draw_end();
	}
	g_context->paint_vec.x = (clip.x / clip.w + 1.0f) * 0.5f;
	g_context->paint_vec.y = (-clip.y / clip.w + 1.0f) * 0.5f;
	render_path_base_draw_gbuffer();
	char *out = string_copy("-");
	if (strcmp(op, "cursor") == 0) {
		out = string("%d", util_udim_cursor_tile());
	}
	else if (strcmp(op, "islands") == 0) {
		util_uv_cache_uv_island_map();
		out = string("tile=%d", util_udim_picked_tile());
	}
	if (in_use) {
		draw_begin(current, false, 0);
	}
	return minic_val_ptr(out);
}

// Source du clone : le point du monde (xs, ys, zs) vu depuis la destination (xd, yd, zd), comme le Alt-clic de l'outil
// Clone. 1 si les deux points sont devant la caméra.
static minic_val_t mn_studio_clone_source(minic_val_t *a, int n) {
	f32 sx[2];
	f32 sy[2];
	for (int k = 0; k < 2; ++k) {
		vec4_t clip = vec4_apply_mat4(
		    (vec4_t){(f32)minic_arg_f(a, n, k * 3), (f32)minic_arg_f(a, n, k * 3 + 1), (f32)minic_arg_f(a, n, k * 3 + 2), 1.0f}, scene_camera->vp);
		if (clip.w <= 0.0f) {
			return minic_val_int(0);
		}
		sx[k] = (clip.x / clip.w + 1.0f) * 0.5f;
		sy[k] = (-clip.y / clip.w + 1.0f) * 0.5f;
	}
	g_context->clone_delta_x = sx[0] - sx[1];
	g_context->clone_delta_y = sy[0] - sy[1];
	return minic_val_int(1);
}

// Bouton Bake du nœud Bake Texture (type : enum bake_type_t ; high_poly : indice de l'objet high poly), comme dans
// l'éditeur de nœuds ; le bake court sur les images suivantes, studio_bake_state() dit quand il est fini. L'id du nœud
// (sa texture : studio_udim_pixel(id, 6, ...)), -1 si rien n'est lancé.
static minic_val_t mn_studio_bake(minic_val_t *a, int n) {
	ui_node_t *node = minic_arg_p(a, n, 0);
	if (node == NULL || strcmp(node->type, "TEX_BAKE") != 0) {
		return minic_val_int(-1);
	}
	g_context->bake_high_poly = (i32)minic_arg_i(a, n, 2);
	bool in_use               = gpu_in_use;
	if (!in_use) { // Le nœud change la profondeur des calques dans une passe (bouton de l'interface)
		draw_begin(NULL, false, 0);
	}
	bake_texture_node_bake(node, (i32)minic_arg_i(a, n, 1));
	if (!in_use) {
		draw_end();
	}
	return minic_val_int(node->id);
}

// État du bake du nœud : "done" une fois la texture rendue au nœud, "baking" avant.
static minic_val_t mn_studio_bake_state(minic_val_t *a, int n) {
	(void)a;
	(void)n;
	return minic_val_ptr(string_copy(bake_texture_node_baking || render_path_paint_baking || g_context->pdirty > 0 ? "baking" : "done"));
}

// Résolution du projet : Custom, px × px, calques redimensionnés tout de suite (le menu Resolution de l'onglet Layers
// attend l'image suivante). Pour les bancs : des projets UDIM légers. 1 si fait.
static minic_val_t mn_studio_project_res(minic_val_t *a, int n) {
	int px = (int)minic_arg_i(a, n, 0);
	if (px < 64 || px > 16384) {
		return minic_val_int(0);
	}
	gpu_texture_t *current = _draw_current;
	bool           in_use  = gpu_in_use;
	if (in_use) {
		draw_end();
	}
	base_res   = TEXTURE_RES_CUSTOM;
	base_res_x = (f32)px;
	base_res_y = (f32)px;
	// layers_resize parcourt history_undo_layers, créé à la première frame (base_update) : un script lancé avant
	base_init_undo_layers();
	i32 before = (i32)_sys_on_next_frames->length;
	layers_on_resized();
	studio_run_next_frames(before);
	if (in_use) {
		draw_begin(current, false, 0);
	}
	g_context->ddirty = 2;
	return minic_val_int(1);
}

// Tuiles UDIM (lot D2) : table, tuile du contexte, magasins. Pour les bancs.
static minic_val_t mn_studio_udim_stats(minic_val_t *a, int n) {
	(void)a, (void)n;
	return minic_val_ptr(util_udim_stats());
}

static char *udim_views(slot_layer_t *l) {
	return string("base=[%s] nor=[%s] pack=[%s]", util_udim_view_stats(l->texpaint), util_udim_view_stats(l->texpaint_nor),
	              util_udim_view_stats(l->texpaint_pack));
}

// Textures du calque i (indice dans g_project->_->layers), ou de l'emplacement d'undo -1 - i : une ligne par texture,
// « single WxH » ou « cur=<tuile du contexte> partial=<0|1> tiles=<tuiles> WxH ».
static minic_val_t mn_studio_udim_layer(minic_val_t *a, int n) {
	int i = (int)minic_arg_i(a, n, 0);
	if (i < 0) {
		i = -1 - i;
		if (history_undo_layers == NULL || i >= (int)history_undo_layers->length) {
			return minic_val_ptr(string_copy("-"));
		}
		return minic_val_ptr(udim_views(history_undo_layers->buffer[i]));
	}
	slot_layer_t_array_t *layers = g_project->_->layers;
	if (i >= (int)layers->length) {
		return minic_val_ptr(string_copy("-"));
	}
	return minic_val_ptr(udim_views(layers->buffer[i]));
}

static float half_to_float(uint16_t h) {
	uint32_t sign = (uint32_t)(h & 0x8000) << 16, exp = (h >> 10) & 0x1f, mant = h & 0x3ff, bits;
	if (exp == 0) {
		float f = mant / 1024.0f / 16384.0f;
		return sign ? -f : f;
	}
	bits = exp == 31 ? sign | 0x7f800000 | (mant << 13) : sign | ((exp + 112) << 23) | (mant << 13);
	float f;
	memcpy(&f, &bits, 4);
	return f;
}

static char *pixel_text(gpu_texture_t *t, float *c) {
#ifdef __APPLE__
	if (t->format == GPU_TEXTURE_FORMAT_RGBA32) { // Cibles de rendu 8 bits en BGRA sous Metal
		float r = c[0];
		c[0]    = c[2];
		c[2]    = r;
	}
#endif
	return string("%.3f %.3f %.3f %.3f", c[0], c[1], c[2], c[3]);
}

// Pixel (x, y) de la texture channel (0 base, 1 nor, 2 pack) du calque i sur la tuile tile (-1 : tuile du contexte),
// « r g b a » de 0 à 1 ; « - » hors bornes. Channel 3, 4, 5 : cartes UV, des triangles et de dilatation du calque
// courant, construites au besoin (i ignoré). Channel 6 : texture du nœud Bake Texture d'id i. Channel 7 : i-ème aperçu
// de nœud du matériau (Curvature Texture...). Channel 8 : base de l'emplacement d'undo i. Channel 9, 10, 11 : base,
// nor et pack de la pile aplatie (layers_flatten, i ignoré). Channel 12, 13, 14 : de même pour le raytrace
// (util_udim_flatten_atlas : chaque calque à tuiles dans les cases de l'atlas).
static minic_val_t mn_studio_udim_pixel(minic_val_t *a, int n) {
	int                   i       = (int)minic_arg_i(a, n, 0);
	int                   channel = (int)minic_arg_i(a, n, 1);
	int                   tile    = (int)minic_arg_i(a, n, 2);
	int                   x       = (int)minic_arg_i(a, n, 3);
	int                   y       = (int)minic_arg_i(a, n, 4);
	slot_layer_t_array_t *layers  = g_project->_->layers;
	if ((channel <= 2 && (i < 0 || i >= (int)layers->length)) || channel < 0 || channel > 14) {
		return minic_val_ptr(string_copy("-"));
	}
	gpu_texture_t *current = _draw_current;
	bool           in_use  = gpu_in_use;
	if (in_use) {
		draw_end();
	}
	i32 saved = util_udim_tile();
	if (tile >= 0) {
		util_udim_set_tile(tile);
	}
	gpu_texture_t *t = NULL;
	if (channel <= 2) {
		slot_layer_t *l = layers->buffer[i];
		t               = channel == 0 ? l->texpaint : channel == 1 ? l->texpaint_nor : l->texpaint_pack;
	}
	else if (channel == 3) {
		util_uv_cache_uv_map();
		t = util_uv_uvmap;
	}
	else if (channel == 4) {
		util_uv_cache_triangle_map();
		t = util_uv_trianglemap;
	}
	else if (channel == 6) {
		render_target_t *rt = any_map_get(render_path_render_targets, string("bake_texture_node_%d", i));
		t                   = rt != NULL ? rt->_image : NULL;
	}
	else if (channel == 8) {
		t = i >= 0 && i < (int)history_undo_layers->length ? ((slot_layer_t *)history_undo_layers->buffer[i])->texpaint : NULL;
	}
	else if (channel >= 9) {
		slot_layer_t *f = channel >= 12 ? util_udim_flatten_atlas(false) : layers_flatten(false, NULL);
		t               = channel % 3 == 0 ? f->texpaint : channel % 3 == 1 ? f->texpaint_nor : f->texpaint_pack;
		free(f);
	}
	else if (channel == 7) {
		string_array_t *keys = g_context->node_previews != NULL ? (string_array_t *)map_keys(g_context->node_previews) : NULL;
		t                    = keys != NULL && i >= 0 && i < (int)keys->length ? any_map_get(g_context->node_previews, keys->buffer[i]) : NULL;
		if (keys != NULL) {
			array_free(keys);
			free(keys);
		}
	}
	else {
		util_uv_cache_dilate_map();
		t = util_uv_dilatemap;
	}
	char *out = string_copy("-");
	if (t != NULL && x >= 0 && y >= 0 && x < (int)t->width && y < (int)t->height) {
		buffer_t *b = gpu_get_texture_pixels(t);
		float     c[4];
		int       at = y * (int)t->width + x;
		for (int k = 0; k < 4; ++k) {
			c[k] = t->format == GPU_TEXTURE_FORMAT_RGBA128  ? ((float *)b->buffer)[at * 4 + k]
			       : t->format == GPU_TEXTURE_FORMAT_RGBA64 ? half_to_float(((uint16_t *)b->buffer)[at * 4 + k])
			       : t->format == GPU_TEXTURE_FORMAT_R8     ? (k == 0 ? b->buffer[at] / 255.0f : k == 3 ? 1.0f : 0.0f)
			                                                : b->buffer[at * 4 + k] / 255.0f;
		}
		out = pixel_text(t, c);
	}
	util_udim_set_tile(saved);
	if (in_use) {
		draw_begin(current, false, 0);
	}
	return minic_val_ptr(out);
}

// Vide la tuile tile (-1 : tuile du contexte) de la texture channel (0 base, 1 nor, 2 pack) du calque i à la couleur
// (r, g, b, a). Pour les bancs : une couleur par tuile sans peindre. 1 si fait.
static minic_val_t mn_studio_udim_fill(minic_val_t *a, int n) {
	int                   i       = (int)minic_arg_i(a, n, 0);
	int                   channel = (int)minic_arg_i(a, n, 1);
	int                   tile    = (int)minic_arg_i(a, n, 2);
	slot_layer_t_array_t *layers  = g_project->_->layers;
	if (i < 0 || i >= (int)layers->length || channel < 0 || channel > 2) {
		return minic_val_int(0);
	}
	slot_layer_t  *l = layers->buffer[i];
	gpu_texture_t *t = channel == 0 ? l->texpaint : channel == 1 ? l->texpaint_nor : l->texpaint_pack;
	if (t == NULL) {
		return minic_val_int(0);
	}
	gpu_texture_t *current = _draw_current;
	bool           in_use  = gpu_in_use;
	if (in_use) {
		draw_end();
	}
	i32 saved = util_udim_tile();
	if (tile >= 0) {
		util_udim_set_tile(tile);
	}
	int done = util_udim_tile() == tile || tile < 0;
	if (done) {
		i32 color = color_from_floats((f32)minic_arg_f(a, n, 3), (f32)minic_arg_f(a, n, 4), (f32)minic_arg_f(a, n, 5), (f32)minic_arg_f(a, n, 6));
		_gpu_begin(t, NULL, NULL, GPU_CLEAR_COLOR, color, 0.0);
		gpu_end();
	}
	util_udim_set_tile(saved);
	if (in_use) {
		draw_begin(current, false, 0);
	}
	g_context->ddirty       = 2;
	g_context->layers_preview_dirty = true;
	return minic_val_int(done);
}

// Pixel d'une render target du chemin de rendu (gbuffer1 : couleur de base du viewport) là où se voit le point du
// monde (x, y, z), « r g b a » de 0 à 1 ; « - » si elle n'existe pas ou si le point est hors de l'écran.
static minic_val_t mn_studio_rt_pixel(minic_val_t *a, int n) {
	char            *name = minic_arg_p(a, n, 0);
	render_target_t *rt   = name != NULL ? any_map_get(render_path_render_targets, name) : NULL;
	if (rt == NULL || rt->_image == NULL) {
		return minic_val_ptr(string_copy("-"));
	}
	gpu_texture_t *current = _draw_current;
	bool           in_use  = gpu_in_use;
	if (in_use) {
		draw_end();
	}
	gpu_texture_t *t    = rt->_image;
	vec4_t         clip = vec4_apply_mat4((vec4_t){(f32)minic_arg_f(a, n, 1), (f32)minic_arg_f(a, n, 2), (f32)minic_arg_f(a, n, 3), 1.0f},
	                                      scene_camera->vp);
	int            x    = (int)((clip.x / clip.w + 1.0f) * 0.5f * t->width);
	int            y    = (int)((-clip.y / clip.w + 1.0f) * 0.5f * t->height);
	char          *out  = string_copy("-");
	if (clip.w > 0.0f && x >= 0 && y >= 0 && x < (int)t->width && y < (int)t->height) {
		buffer_t *b = gpu_get_texture_pixels(t);
		float     c[4];
		int       at = y * (int)t->width + x;
		for (int k = 0; k < 4; ++k) {
			c[k] = t->format == GPU_TEXTURE_FORMAT_RGBA128  ? ((float *)b->buffer)[at * 4 + k]
			       : t->format == GPU_TEXTURE_FORMAT_RGBA64 ? half_to_float(((uint16_t *)b->buffer)[at * 4 + k])
			                                                : b->buffer[at * 4 + k] / 255.0f;
		}
		out = pixel_text(t, c);
	}
	if (in_use) {
		draw_begin(current, false, 0);
	}
	return minic_val_ptr(out);
}

// Met le calque i dans le groupe g (-1 : hors groupe), comme un glisser-déposer sur le groupe de l'onglet Layers ; les
// enfants d'un groupe sont juste en dessous de lui dans la pile. 1 si fait.
static minic_val_t mn_studio_layer_set_parent(minic_val_t *a, int n) {
	int                   i      = (int)minic_arg_i(a, n, 0);
	int                   g      = (int)minic_arg_i(a, n, 1);
	slot_layer_t_array_t *layers = g_project->_->layers;
	if (i < 0 || i >= (int)layers->length || g >= (int)layers->length || (g >= 0 && !slot_layer_is_group(layers->buffer[g]))) {
		return minic_val_int(0);
	}
	layers->buffer[i]->parent = g >= 0 ? layers->buffer[g] : NULL;
	g_context->layers_preview_dirty = true;
	return minic_val_int(1);
}

// Onglet Meshes, un matériau glissé sur le maillage i : ce maillage (et ceux qui partagent ses données) montre le
// matériau m, que le flatten dessine par-dessus les calques ; -1 : plus de surcharge. 1 si fait.
static minic_val_t mn_studio_mesh_override(minic_val_t *a, int n) {
	int                    i       = (int)minic_arg_i(a, n, 0);
	int                    m       = (int)minic_arg_i(a, n, 1);
	mesh_object_t_array_t *objects = g_project->_->paint_objects;
	if (i < 0 || i >= (int)objects->length || m < -1 || m >= (int)g_project->_->materials->length) {
		return minic_val_int(0);
	}
	tab_meshes_set_linked_override(objects->buffer[i], m);
	g_context->ddirty         = 2;
	g_project->mesh_materials = i32_array_create(0);
	return minic_val_int(1);
}

// Outil ColorID, bouton To Mask : la carte d'id path (importée au besoin), la couleur piquée rgb (0xRRGGBB). Un masque
// sur le calque courant, blanc là où la carte a cette couleur ; son indice, -1 si rien.
static minic_val_t mn_studio_colorid_mask(minic_val_t *a, int n) {
	char *path = minic_arg_p(a, n, 0);
	int   rgb  = (int)minic_arg_i(a, n, 1);
	if (path == NULL || !iron_file_exists(path)) {
		return minic_val_int(-1);
	}
	ui_header_draw_tool_properties_import(path);
	render_target_t *rt = any_map_get(render_path_render_targets, "texpaint_colorid");
	draw_begin(rt->_image, true, 0xff000000 | (unsigned)rgb);
	draw_end();
	g_context->colorid_picked = true;
	if (slot_layer_is_mask(g_context->layer)) {
		context_set_layer(g_context->layer->parent);
	}
	slot_layer_t *m = layers_new_mask(false, g_context->layer, -1);
	if (m == NULL) {
		return minic_val_int(-1);
	}
	history_new_white_mask();
	ui_header_draw_tool_properties_to_mask(m);
	return minic_val_int(array_index_of((any_array_t *)g_project->_->layers, m));
}

// Menu Export du masque i, fichier name dans dir, comme le sélecteur de fichiers. 1 si fait.
static minic_val_t mn_studio_mask_export(minic_val_t *a, int n) {
	int                   i      = (int)minic_arg_i(a, n, 0);
	char                 *dir    = minic_arg_p(a, n, 1);
	char                 *name   = minic_arg_p(a, n, 2);
	slot_layer_t_array_t *layers = g_project->_->layers;
	if (i < 0 || i >= (int)layers->length || !slot_layer_is_mask(layers->buffer[i]) || dir == NULL || name == NULL) {
		return minic_val_int(0);
	}
	tab_layers_l      = layers->buffer[i];
	ui_files_filename = name;
	tab_layers_draw_layer_context_menu_export_on_file_picked(dir);
	return minic_val_int(1);
}

// Calque courant = calque i. 1 si fait.
static minic_val_t mn_studio_layer_select(minic_val_t *a, int n) {
	int                   i      = (int)minic_arg_i(a, n, 0);
	slot_layer_t_array_t *layers = g_project->_->layers;
	if (i < 0 || i >= (int)layers->length) {
		return minic_val_int(0);
	}
	context_set_layer(layers->buffer[i]);
	return minic_val_int(1);
}

// Pile de calques, de bas en haut : « nom<parent » séparés par « | » (bancs).
static minic_val_t mn_studio_layer_tree(minic_val_t *a, int n) {
	(void)a, (void)n;
	text_t                b      = {0};
	slot_layer_t_array_t *layers = g_project->_->layers;
	for (int i = 0; layers != NULL && i < (int)layers->length; ++i) {
		slot_layer_t *l = layers->buffer[i];
		put(&b, "%s%s%s%s", i > 0 ? " | " : "", l->name, l->parent != NULL ? "<" : "", l->parent != NULL ? l->parent->name : "");
	}
	char *s = string_copy(b.data != NULL ? b.data : "");
	free(b.data);
	return minic_val_ptr(s);
}

// Opacité du calque courant (0 à 1), comme le curseur Opacity de son menu, avec son pas d'historique. 1 si fait.
static minic_val_t mn_studio_layer_set_opacity(minic_val_t *a, int n) {
	f32 opacity = (f32)minic_arg_f(a, n, 0);
	if (g_context->layer == NULL || opacity < 0.0f || opacity > 1.0f) {
		return minic_val_int(0);
	}
	history_layer_opacity();
	g_context->layer->mask_opacity = opacity;
	make_material_parse_mesh_material();
	return minic_val_int(1);
}

// Opération sur le calque courant, comme les menus, avec son pas d'historique, finie tout de suite : "duplicate",
// "delete", "clear", "invert" (masque), "merge_down" (Merge Down), "merge_group" (Merge Group d'un groupe, par son menu),
// "apply_mask" (Apply Mask d'un masque), "undo", "redo", "sync" (util_udim_sync, fait par le viewport à chaque image). 1 si
// fait.
static minic_val_t mn_studio_layer_op(minic_val_t *a, int n) {
	char         *op = minic_arg_p(a, n, 0);
	slot_layer_t *l  = g_context->layer;
	if (op == NULL || l == NULL) {
		return minic_val_int(0);
	}
	gpu_texture_t *current = _draw_current;
	bool           in_use  = gpu_in_use;
	if (in_use) {
		draw_end();
	}
	i32 before = (i32)_sys_on_next_frames->length;
	int done   = 1;
	if (strcmp(op, "duplicate") == 0) {
		history_duplicate_layer();
		layers_duplicate_layer(l);
	}
	else if (strcmp(op, "delete") == 0) {
		tab_layers_delete_layer(l);
	}
	else if (strcmp(op, "clear") == 0) {
		history_clear_layer();
		slot_layer_clear(l, slot_layer_is_mask(l) ? 0xffffffff : 0x00000000, NULL, 1.0, layers_default_rough, 0.0);
	}
	else if (strcmp(op, "invert") == 0 && slot_layer_is_mask(l)) {
		history_invert_mask();
		slot_layer_invert_mask(l);
	}
	else if (strcmp(op, "merge_down") == 0 && array_index_of((any_array_t *)g_project->_->layers, l) > 0) { // tab_layers.c, menu du calque
		history_merge_layers();
		layers_merge_down();
		if (g_context->layer->fill_material != NULL) {
			slot_layer_to_paint_layer(g_context->layer);
		}
	}
	else if (strcmp(op, "merge_group") == 0 && slot_layer_is_group(l)) { // Par le menu : son pas d'historique (patch merge-group-undo)
		tab_layers_l = l;
		tab_layers_draw_layer_context_menu_merge_group(NULL);
	}
	else if (strcmp(op, "apply_mask") == 0 && slot_layer_is_mask(l)) {
		history_apply_mask();
		slot_layer_apply_mask(l);
		context_set_layer(l->parent);
		make_material_parse_mesh_material();
		g_context->layers_preview_dirty = true;
	}
	else if (strcmp(op, "undo") == 0) {
		history_undo();
	}
	else if (strcmp(op, "redo") == 0) {
		history_redo();
	}
	else if (strcmp(op, "sync") == 0) {
		util_udim_sync();
	}
	else {
		done = 0;
	}
	studio_run_next_frames(before);
	if (in_use) {
		draw_begin(current, false, 0);
	}
	g_context->ddirty = 2;
	return minic_val_int(done);
}

#endif

void studio_atlas_init(void) {
#ifdef WITH_EVAL
	minic_register("studio_atlases", "p()", mn_studio_atlases);
	minic_register("studio_set_atlases", "p(p)", mn_studio_set_atlases);
	minic_register("studio_import_mesh", "i(p,i)", mn_studio_import_mesh);
	minic_register("studio_append_mesh", "i(p,i)", mn_studio_append_mesh);
	minic_register("studio_dialog_title", "p()", mn_studio_dialog_title);
	minic_register("studio_status_message", "p()", mn_studio_status_message);
	minic_register("studio_layer_set_atlas", "i(i)", mn_studio_layer_set_atlas);
	minic_register("studio_layer_set_object", "i(i)", mn_studio_layer_set_object);
	minic_register("studio_layer_new", "i()", mn_studio_layer_new);
	minic_register("studio_layer_new_group", "i()", mn_studio_layer_new_group);
	minic_register("studio_layer_new_mask", "i(i)", mn_studio_layer_new_mask);
	minic_register("studio_layer_rename", "i(p)", mn_studio_layer_rename);
	minic_register("studio_layer_set_visible", "i(i)", mn_studio_layer_set_visible);
	minic_register("studio_object_rename", "p(i,p)", mn_studio_object_rename);
	minic_register("studio_meshes_sort", "i()", mn_studio_meshes_sort);
	minic_register("studio_mesh_set_parent", "i(p,p)", mn_studio_mesh_set_parent);
	minic_register("studio_layer_masks", "p()", mn_studio_layer_masks);
	minic_register("studio_project_res", "i(i)", mn_studio_project_res);
	minic_register("studio_world_screen", "p(f,f,f)", mn_studio_world_screen);
	minic_register("studio_paint_option", "i(p,i)", mn_studio_paint_option);
	minic_register("studio_udim_pick", "p(p,f,f,f)", mn_studio_udim_pick);
	minic_register("studio_clone_source", "i(f,f,f,f,f,f)", mn_studio_clone_source);
	minic_register("studio_bake", "i(p,i,i)", mn_studio_bake);
	minic_register("studio_bake_state", "p()", mn_studio_bake_state);
	minic_register("studio_udim_stats", "p()", mn_studio_udim_stats);
	minic_register("studio_udim_layer", "p(i)", mn_studio_udim_layer);
	minic_register("studio_udim_pixel", "p(i,i,i,i,i)", mn_studio_udim_pixel);
	minic_register("studio_layer_select", "i(i)", mn_studio_layer_select);
	minic_register("studio_layer_set_parent", "i(i,i)", mn_studio_layer_set_parent);
	minic_register("studio_udim_fill", "i(i,i,i,f,f,f,f)", mn_studio_udim_fill);
	minic_register("studio_rt_pixel", "p(p,f,f,f)", mn_studio_rt_pixel);
	minic_register("studio_layer_op", "i(p)", mn_studio_layer_op);
	minic_register("studio_layer_tree", "p()", mn_studio_layer_tree);
	minic_register("studio_layer_set_opacity", "i(f)", mn_studio_layer_set_opacity);
	minic_register("studio_mesh_override", "i(i,i)", mn_studio_mesh_override);
	minic_register("studio_colorid_mask", "i(p,i)", mn_studio_colorid_mask);
	minic_register("studio_mask_export", "i(i,p,p)", mn_studio_mask_export);
#endif
}
