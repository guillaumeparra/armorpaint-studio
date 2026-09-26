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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Cœur ArmorPaint (paint/sources/globals.h, functions.h)
extern context_t *g_context;
extern project_t *g_project;
extern int        plugins_split_by; // paint/plugins/plugins.c : découpage lu par l'import FBX
void              layers_set_object_mask(void);
void              import_mesh_run(char *path, bool clear_layers, bool replace_existing, bool keep_camera);
void              tab_meshes_sort_by_name(void);
void              tab_meshes_sort_hierarchy(void);
slot_layer_t     *layers_new_layer(bool clear, i32 position, slot_layer_t *parent);
void              history_new_layer(void);

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

// Importe un maillage à la place de celui du projet (calques vidés), découpé comme le « Split By » de la boîte
// d'import : 0 par objet, 1 par matériau, 2 par tuile UDIM. Pour les scripts batch et les bancs.
static minic_val_t mn_studio_import_mesh(minic_val_t *a, int n) {
	char *path  = minic_arg_p(a, n, 0);
	int   split = (int)minic_arg_i(a, n, 1);
	if (path == NULL || split < SPLIT_TYPE_OBJECT || split > SPLIT_TYPE_UDIM) {
		return minic_val_int(0);
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
	import_mesh_run(path, true, true, false);
	for (int guard = 0; (i32)_sys_on_next_frames->length > before && guard < 64; ++guard) {
		callback_t *cb = _sys_on_next_frames->buffer[before];
		array_splice(_sys_on_next_frames, before, 1);
		cb->f(cb->data);
		free(cb);
	}
	if (in_use) {
		draw_begin(current, false, 0);
	}
	g_context->ddirty = 2;
	return minic_val_int(1);
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

// Nouveau calque de peinture, comme Layers > New > Paint Layer (limité comme le filtre de calques). 1 si fait.
static minic_val_t mn_studio_layer_new(minic_val_t *a, int n) {
	(void)a, (void)n;
	gpu_texture_t *current = _draw_current;
	bool           in_use  = gpu_in_use;
	if (in_use) {
		draw_end();
	}
	slot_layer_t *l = layers_new_layer(true, -1, NULL);
	if (l != NULL) {
		history_new_layer();
	}
	if (in_use) {
		draw_begin(current, false, 0);
	}
	return minic_val_int(l != NULL ? 1 : 0);
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

#endif

void studio_atlas_init(void) {
#ifdef WITH_EVAL
	minic_register("studio_atlases", "p()", mn_studio_atlases);
	minic_register("studio_set_atlases", "p(p)", mn_studio_set_atlases);
	minic_register("studio_import_mesh", "i(p,i)", mn_studio_import_mesh);
	minic_register("studio_layer_set_atlas", "i(i)", mn_studio_layer_set_atlas);
	minic_register("studio_layer_set_object", "i(i)", mn_studio_layer_set_object);
	minic_register("studio_meshes_sort", "i()", mn_studio_meshes_sort);
	minic_register("studio_mesh_set_parent", "i(p,p)", mn_studio_mesh_set_parent);
	minic_register("studio_layer_new", "i()", mn_studio_layer_new);
	minic_register("studio_layer_masks", "p()", mn_studio_layer_masks);
#endif
}
