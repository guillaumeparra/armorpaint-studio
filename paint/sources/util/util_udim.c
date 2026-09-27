#include "../global.h"

// STUDIO-PATCH(udim-core): see util_udim.h

static mesh_object_t *util_udim_paint_object(object_t *object) {
	mesh_object_t_array_t *paint_objects = g_project->_->paint_objects;
	for (i32 i = 0; i < paint_objects->length; ++i) {
		if (paint_objects->buffer[i]->base == object) {
			return paint_objects->buffer[i];
		}
	}
	return NULL;
}

static i32 util_udim_object_tile(object_t *object) {
	if (!util_mesh_udim_active()) {
		return -1;
	}
	mesh_object_t *o = util_udim_paint_object(object);
	return o == NULL ? -1 : util_mesh_udim_tile(o->base->name); // Merged mesh or 2d plane: uvs already in atlas space
}

vec2_t util_udim_tile_uv(object_t *object) {
	i32 tile = util_udim_object_tile(object);
	if (tile < 0 && g_context->paint2d && util_udim_layer_tiled(g_context->layer)) {
		tile = util_udim_tile(); // The 2d plane paints the tile of the context (lot D2b)
	}
	if (tile < 0) {
		return (vec2_t){0.0, 0.0};
	}
	// Uvs are stored with v flipped (tex.y = 1 - v): the tile goes down in tex space
	return (vec2_t){(tile - 1001) % 10, -((tile - 1001) / 10)};
}

vec2_t util_udim_offset(object_t *object) {
	i32 tile = util_udim_object_tile(object);
	i32 slot = tile < 0 ? -1 : util_mesh_udim_slot(tile);
	if (slot < 0) {
		return (vec2_t){0.0, 0.0};
	}
	i32    stride = util_mesh_atlas_stride();
	vec2_t uv     = util_udim_tile_uv(object);
	return (vec2_t){uv.x - (slot % stride), uv.y - (slot / stride)};
}

// Shared layers and layers limited to a texture set, unless a layer filter keeps a single object. The bake and the
// pickers keep the merged mesh: their shaders do not place the objects in their cell (make_paint.c, make_picking.c).
static bool util_udim_paint_per_object() {
	slot_layer_t *l = g_context->layer;
	if (l == NULL || !util_mesh_udim_layer(l) || context_layer_filter_used()) {
		return false;
	}
	i32 tool = g_context->tool;
	return tool != TOOL_TYPE_BAKE && tool != TOOL_TYPE_COLORID && tool != TOOL_TYPE_PICKER && tool != TOOL_TYPE_MATERIAL && tool != TOOL_TYPE_CURSOR;
}

static bool udim_draw_tile_meshes();

void util_udim_draw_paint_meshes() {
	if (udim_draw_tile_meshes()) { // Tiled layer (lot D2)
		return;
	}
	mesh_object_t         *merged = g_context->merged_object;
	mesh_object_t_array_t *scope  = util_mesh_merged_scope();
	if (merged == NULL || scope == NULL || scope->length == 0 || !util_udim_paint_per_object()) {
		render_path_draw_meshes("paint");
		return;
	}
	// The objects of the merged mesh are drawn instead of it, hidden ones excepted, then everything is put back
	char        *merged_skip = merged->skip_context;
	any_array_t *skips       = any_array_create(scope->length);
	merged->skip_context     = "paint";
	for (i32 i = 0; i < scope->length; ++i) {
		skips->buffer[i]               = scope->buffer[i]->skip_context;
		scope->buffer[i]->skip_context = "";
	}
	render_path_draw_meshes("paint");
	for (i32 i = 0; i < scope->length; ++i) {
		scope->buffer[i]->skip_context = skips->buffer[i];
	}
	merged->skip_context = merged_skip;
	array_delete(skips);
}

// Tile context (lot D2), see util_udim.h

void history_copy_to_undo(i32 from_id, i32 to_id, bool is_mask); // history.c, not in functions.h

typedef struct udim_store {
	gpu_texture_t *view;
	i32            cur;     // Tile whose content is in the view, -1 for the placeholder
	bool           partial; // Undo slot: the tiles it saved, not those of the table
	bool           whole;   // Undo slot that saved every tile of its layer: the tiles the layer gained since join it
	i32            count;
	i32            ids[UDIM_MAX_TILES];
	gpu_texture_t  tiles[UDIM_MAX_TILES]; // Content of each tile, stale for the one in the view
} udim_store_t;

static udim_store_t **udim_stores      = NULL;
static i32            udim_store_count = 0;
static i32            udim_store_cap   = 0;
static i32            udim_cur         = -1; // Tile of the context
static i32            udim_table[UDIM_MAX_TILES];
static i32            udim_table_count = 0; // Tiles the stores follow, 0 when udim is off
static gpu_texture_t *udim_placeholder = NULL;
static i32            udim_loop_saved[8];
static bool           udim_loop_tiled[8];
static i32            udim_loop_depth = 0;
static bool           udim_copying    = false; // Inside the upstream history_copy_to_undo()
static bool           udim_painting   = false; // Inside the paint loop
static i32            udim_dirty[UDIM_MAX_TILES]; // Tiles painted since the dilation of their nor and pack
static i32            udim_dirty_count = 0;
static slot_layer_t  *udim_arm_layer   = NULL; // Layer of the paint step, its tiles saved when first drawn to
static gpu_texture_t *udim_arm_views[3];
static gpu_texture_t *udim_arm_slot_views[3];
static i32            udim_picked      = -1; // Tile of the last pick
static i32            udim_island_tile = -1; // Tile of util_uv_uvislandmap

static udim_store_t *udim_store_of(gpu_texture_t *view) {
	if (view == NULL) {
		return NULL;
	}
	for (i32 i = 0; i < udim_store_count; ++i) {
		if (udim_stores[i]->view == view) {
			return udim_stores[i];
		}
	}
	return NULL;
}

static i32 udim_index(udim_store_t *s, i32 tile) {
	for (i32 i = 0; i < s->count; ++i) {
		if (s->ids[i] == tile) {
			return i;
		}
	}
	return -1;
}

static i32 udim_table_index(i32 tile) {
	for (i32 i = 0; i < udim_table_count; ++i) {
		if (udim_table[i] == tile) {
			return i;
		}
	}
	return -1;
}

// gpu_end() leaves no render target behind: a view is only busy during the pass drawing into it
static bool udim_busy(gpu_texture_t *view) {
	if (!gpu_in_use) {
		return false;
	}
	for (uint32_t i = 0; i < current_render_targets_count; ++i) {
		if (current_render_targets[i] == view) {
			return true;
		}
	}
	return false;
}

static gpu_pipeline_t *udim_copy_pipe(gpu_texture_format_t format) {
	return format == GPU_TEXTURE_FORMAT_RGBA64 ? pipes_copy64 : format == GPU_TEXTURE_FORMAT_RGBA128 ? pipes_copy128 : pipes_copy;
}

// A new texture content, cleared or filled from src, stretched
static gpu_texture_t udim_new_content(i32 width, i32 height, gpu_texture_format_t format, gpu_texture_t *src) {
	gpu_texture_t *t = gpu_create_render_target(width, height, format);
	if (src == NULL) {
		_gpu_begin(t, NULL, NULL, GPU_CLEAR_COLOR, 0x00000000, 0.0);
		gpu_end();
	}
	else {
		draw_begin(t, false, 0);
		draw_set_pipeline(udim_copy_pipe(format));
		draw_scaled_image(src, 0, 0, width, height);
		draw_set_pipeline(NULL);
		draw_end();
	}
	gpu_texture_t content = *t;
	free(t);
	return content;
}

// The background of a texture: its texel (0, 0) in a 1x1 render target, to stretch over a new tile
static gpu_texture_t *udim_background(gpu_texture_t *view) {
	gpu_texture_t *bg = gpu_create_render_target(1, 1, view->format);
	draw_begin(bg, false, 0);
	draw_set_pipeline(udim_copy_pipe(view->format));
	draw_scaled_sub_image(view, 0, 0, 1, 1, 0, 0, 1, 1);
	draw_set_pipeline(NULL);
	draw_end();
	return bg;
}

static void udim_destroy(gpu_texture_t content) {
	gpu_texture_t *t = malloc(sizeof(gpu_texture_t));
	*t               = content;
	gpu_delete_texture(t); // Not a view: the deleting hook lets it go
}

static bool udim_placeholder_ready() {
	if (udim_placeholder == NULL && !gpu_in_use) {
		udim_placeholder = gpu_create_render_target(1, 1, GPU_TEXTURE_FORMAT_RGBA32);
		_gpu_begin(udim_placeholder, NULL, NULL, GPU_CLEAR_COLOR, 0x00000000, 0.0);
		gpu_end();
	}
	return udim_placeholder != NULL;
}

// The content of a tile into the view, the one it replaces back to its slot. -1 or a tile not held: the placeholder
static void udim_show(udim_store_t *s, i32 tile) {
	i32 k      = tile < 0 ? -1 : udim_index(s, tile);
	i32 target = k < 0 ? -1 : tile;
	if (s->cur == target) {
		return;
	}
	if (udim_busy(s->view) || (k < 0 && !udim_placeholder_ready())) {
		static bool reported = false;
		if (!reported) {
			reported = true;
			iron_log("UDIM: tile %d not set, its texture is being drawn to or no placeholder yet", tile);
		}
		return;
	}
	if (s->cur >= 0) {
		s->tiles[udim_index(s, s->cur)] = *s->view;
	}
	else if (s->view->buffer != NULL) { // Pixels read from the placeholder
		free(s->view->buffer->buffer);
		free(s->view->buffer);
	}
	if (k >= 0) {
		*s->view = s->tiles[k];
	}
	else {
		*s->view        = *udim_placeholder;
		s->view->buffer = NULL;
	}
	s->cur = target;
}

static void udim_texture_deleting(gpu_texture_t *texture);

static udim_store_t *udim_register(gpu_texture_t *view, bool partial) {
	gpu_texture_deleting = udim_texture_deleting;
	if (udim_store_count == udim_store_cap) {
		udim_store_cap = udim_store_cap == 0 ? 64 : udim_store_cap * 2;
		udim_stores    = realloc(udim_stores, udim_store_cap * sizeof(udim_store_t *));
	}
	udim_store_t *s                 = calloc(1, sizeof(udim_store_t));
	s->view                         = view;
	s->partial                      = partial;
	s->cur                          = -1;
	udim_stores[udim_store_count++] = s;
	return s;
}

static void udim_unregister(udim_store_t *s) {
	for (i32 i = 0; i < udim_store_count; ++i) {
		if (udim_stores[i] == s) {
			udim_stores[i] = udim_stores[--udim_store_count];
			break;
		}
	}
	free(s);
}

// The view gets tiles: its texture goes to the tile of the context (or to the first of ids), the other tiles are new,
// cleared or given its background (texel (0, 0))
static udim_store_t *udim_tile_view(gpu_texture_t *view, bool partial, i32 n, i32 *ids, bool background) {
	i32 home = ids[0];
	for (i32 i = 0; i < n; ++i) {
		if (ids[i] == udim_cur) {
			home = udim_cur;
		}
	}
	gpu_texture_t *bg = background && n > 1 ? udim_background(view) : NULL;
	udim_store_t  *s  = udim_register(view, partial);
	s->ids[0]         = home;
	s->count        = 1;
	s->cur          = home;
	for (i32 i = 0; i < n && s->count < UDIM_MAX_TILES; ++i) {
		if (ids[i] == home) {
			continue;
		}
		s->ids[s->count]   = ids[i];
		s->tiles[s->count] = udim_new_content(view->width, view->height, view->format, bg);
		s->count++;
	}
	if (bg != NULL) {
		gpu_delete_texture(bg);
	}
	udim_show(s, udim_cur);
	return s;
}

// Back to a single texture, that of the tile kept (the tile shown or the first when keep is not held)
static void udim_untile(udim_store_t *s, i32 keep) {
	if (udim_index(s, keep) < 0) {
		keep = s->cur >= 0 ? s->cur : s->ids[0];
	}
	udim_show(s, keep);
	if (s->cur != keep) {
		return;
	}
	for (i32 i = 0; i < s->count; ++i) {
		if (s->ids[i] != keep) {
			udim_destroy(s->tiles[i]);
		}
	}
	udim_unregister(s);
}

static void udim_disarm() {
	udim_arm_layer = NULL;
	for (i32 i = 0; i < 3; ++i) {
		udim_arm_views[i]      = NULL;
		udim_arm_slot_views[i] = NULL;
	}
}

// gpu_delete_texture() of a view: its tiles go with it
static void udim_texture_deleting(gpu_texture_t *texture) {
	for (i32 i = 0; i < 3 && udim_arm_layer != NULL; ++i) {
		if (texture == udim_arm_views[i] || texture == udim_arm_slot_views[i]) {
			udim_disarm();
		}
	}
	udim_store_t *s = udim_store_of(texture);
	if (s == NULL) {
		return;
	}
	if (s->cur < 0) { // The placeholder is shared: a tile of its own goes in the view
		if (texture->buffer != NULL) {
			free(texture->buffer->buffer);
			free(texture->buffer);
		}
		*texture = s->tiles[0];
		s->cur   = s->ids[0];
	}
	for (i32 i = 0; i < s->count; ++i) {
		if (s->ids[i] != s->cur) {
			udim_destroy(s->tiles[i]);
		}
	}
	udim_unregister(s);
}

static bool udim_ids_have(i32 n, i32 *ids, i32 tile) {
	for (i32 i = 0; i < n; ++i) {
		if (ids[i] == tile) {
			return true;
		}
	}
	return false;
}

// A layer store follows the table: the tiles gone are freed, the new ones get the background of the layer (texel (0, 0)
// of a tile it keeps, or of its first)
static void udim_follow_table(udim_store_t *s, i32 n, i32 *tiles) {
	gpu_texture_t *bg = NULL;
	for (i32 i = 0; i < n && bg == NULL; ++i) {
		if (udim_index(s, tiles[i]) >= 0) {
			continue;
		}
		i32 src = s->ids[0];
		for (i32 j = 0; j < s->count; ++j) {
			if (udim_ids_have(n, tiles, s->ids[j])) {
				src = s->ids[j];
				break;
			}
		}
		udim_show(s, src);
		bg = udim_background(s->view);
	}
	i32                  width  = s->view->width;
	i32                  height = s->view->height;
	gpu_texture_format_t format = s->view->format;
	udim_show(s, -1);
	if (s->cur >= 0) { // No placeholder
		if (bg != NULL) {
			gpu_delete_texture(bg);
		}
		return;
	}
	for (i32 i = s->count - 1; i >= 0; --i) {
		if (!udim_ids_have(n, tiles, s->ids[i])) {
			udim_destroy(s->tiles[i]);
			s->count--;
			s->ids[i]   = s->ids[s->count];
			s->tiles[i] = s->tiles[s->count];
		}
	}
	for (i32 i = 0; i < n && s->count < UDIM_MAX_TILES; ++i) {
		if (udim_index(s, tiles[i]) < 0) {
			s->ids[s->count]   = tiles[i];
			s->tiles[s->count] = udim_new_content(width, height, format, bg);
			s->count++;
		}
	}
	if (bg != NULL) {
		gpu_delete_texture(bg);
	}
}

static void udim_update_table() {
	i32 *tiles = NULL;
	i32  n     = util_mesh_udim_tile_table(&tiles);
	n          = n > UDIM_MAX_TILES ? UDIM_MAX_TILES : n;
	i32 m      = n > 1 ? n : 0;
	if (m == udim_table_count && memcmp(tiles, udim_table, m * sizeof(i32)) == 0) {
		return;
	}
	if (m == 0) { // Udim off: every view back to a single texture, that of the remaining tile
		i32 keep = n == 1 ? tiles[0] : udim_cur;
		for (i32 i = udim_store_count - 1; i >= 0; --i) {
			udim_untile(udim_stores[i], keep);
		}
		udim_table_count = 0;
		udim_cur         = -1;
		return;
	}
	if (!udim_placeholder_ready()) {
		return;
	}
	i32 cur = udim_ids_have(m, tiles, udim_cur) ? udim_cur : tiles[0];
	for (i32 i = 0; i < udim_store_count; ++i) {
		udim_store_t *s = udim_stores[i];
		if (!s->partial) {
			udim_follow_table(s, m, tiles);
		}
		udim_show(s, cur);
	}
	memcpy(udim_table, tiles, m * sizeof(i32));
	udim_table_count = m;
	udim_cur         = cur;
}

void util_udim_set_tile(i32 tile) {
	if (tile == udim_cur) {
		return;
	}
	for (i32 i = 0; i < udim_store_count; ++i) {
		udim_show(udim_stores[i], tile);
	}
	udim_cur = tile;
}

i32 util_udim_tile() {
	return udim_cur;
}

i32 util_udim_tile_count() {
	return udim_table_count;
}

i32 util_udim_tile_at(i32 i) {
	return i >= 0 && i < udim_table_count ? udim_table[i] : -1;
}

bool util_udim_layer_tiled(slot_layer_t *l) {
	return l != NULL && udim_store_of(l->texpaint) != NULL;
}

// By the table of util_mesh.c, which the stores follow at the next sync: the shaders parsed before it are right
bool util_udim_layer_should_tile(slot_layer_t *l) {
	i32 *tiles = NULL;
	i32  n     = util_mesh_udim_tile_table(&tiles);
	n          = n > UDIM_MAX_TILES ? UDIM_MAX_TILES : n;
	// Groups, undo slots ("_undo0") and the live layer ("_live") are not
	if (n < 2 || l == NULL || l->texpaint == NULL || l->ext == NULL || l->ext[0] == '_') {
		return false;
	}
	if (slot_layer_is_mask(l)) {
		return l->parent != NULL && (slot_layer_is_group(l->parent) || util_udim_layer_should_tile(l->parent));
	}
	if (l->uv_map == 1) {
		return false;
	}
	i32                    mask    = slot_layer_get_object_mask(l);
	mesh_object_t_array_t *objects = g_project->_->paint_objects;
	if (mask > 0 && mask <= objects->length) { // Limited to an object: tiled when it has a tile
		return udim_ids_have(n, tiles, util_mesh_udim_tile(objects->buffer[mask - 1]->base->name));
	}
	return true;
}

static void udim_untile_layer(slot_layer_t *l) {
	gpu_texture_t *views[3] = {l->texpaint, l->texpaint_nor, l->texpaint_pack};
	for (i32 i = 0; i < 3; ++i) {
		udim_store_t *s = udim_store_of(views[i]);
		if (s != NULL) {
			udim_untile(s, udim_cur);
		}
	}
}

bool util_udim_tile_layer(slot_layer_t *l, bool background) {
	if (!gpu_in_use) {
		udim_update_table();
	}
	if (util_udim_layer_tiled(l)) {
		return true;
	}
	if (gpu_in_use || !util_udim_layer_should_tile(l)) {
		return false;
	}
	gpu_texture_t *views[3] = {l->texpaint, l->texpaint_nor, l->texpaint_pack};
	for (i32 i = 0; i < 3; ++i) {
		if (views[i] != NULL && udim_store_of(views[i]) == NULL) {
			udim_tile_view(views[i], false, udim_table_count, udim_table, background);
		}
	}
	return true;
}

void util_udim_sync() {
	if (gpu_in_use || g_project == NULL || g_project->_->layers == NULL) {
		return;
	}
	udim_update_table();
	slot_layer_t_array_t *layers = g_project->_->layers;
	for (i32 i = 0; i < layers->length; ++i) {
		slot_layer_t *l = layers->buffer[i];
		if (slot_layer_is_group(l)) {
			continue;
		}
		if (util_udim_layer_should_tile(l)) {
			util_udim_tile_layer(l, true);
		}
		else if (util_udim_layer_tiled(l)) {
			udim_untile_layer(l);
		}
	}
}

void util_udim_replace(gpu_texture_t *view, gpu_texture_t *fresh) {
	udim_store_t *s = udim_store_of(view);
	if (s != NULL && s->cur < 0) { // The placeholder: no tile to replace
		gpu_delete_texture(fresh);
		return;
	}
	gpu_texture_t old = *view;
	*view             = *fresh;
	free(fresh);
	udim_destroy(old);
}

bool util_udim_clear(gpu_texture_t *view, u32 color) {
	udim_store_t *s = udim_store_of(view);
	if (s == NULL || gpu_in_use) {
		return false;
	}
	for (i32 i = 0; i < s->count; ++i) {
		udim_show(s, s->ids[i]);
		_gpu_begin(view, NULL, NULL, GPU_CLEAR_COLOR, color, 0.0);
		gpu_end();
	}
	udim_show(s, udim_cur);
	return true;
}

bool util_udim_tile_like(gpu_texture_t *view, gpu_texture_t *from) {
	udim_store_t *f = udim_store_of(from);
	if (f == NULL || gpu_in_use || udim_store_of(view) != NULL) {
		return false;
	}
	i32           home = f->cur >= 0 ? f->cur : f->ids[0];
	udim_store_t *s    = udim_register(view, f->partial);
	s->ids[0]          = home;
	s->count           = 1;
	s->cur             = home;
	for (i32 i = 0; i < f->count; ++i) {
		udim_show(f, f->ids[i]);
		if (f->ids[i] == home) {
			draw_begin(view, false, 0);
			draw_set_pipeline(udim_copy_pipe(view->format));
			draw_scaled_image(from, 0, 0, view->width, view->height);
			draw_set_pipeline(NULL);
			draw_end();
			continue;
		}
		s->ids[s->count]   = f->ids[i];
		s->tiles[s->count] = udim_new_content(view->width, view->height, view->format, from);
		s->count++;
	}
	udim_show(f, udim_cur);
	udim_show(s, udim_cur);
	return true;
}

static void udim_hold(gpu_texture_t *view);

bool util_udim_tile_preview(gpu_texture_t *image) {
	udim_store_t *s = udim_store_of(image);
	if (gpu_in_use) {
		return false;
	}
	if (!util_udim_layer_tiled(g_context->layer)) {
		if (s != NULL) {
			udim_untile(s, udim_cur);
		}
		return false;
	}
	udim_hold(image);
	return true;
}

bool util_udim_resize_layer(slot_layer_t *l, i32 width, i32 height) {
	if (!util_udim_layer_tiled(l) || gpu_in_use) {
		return false;
	}
	gpu_texture_format_t format   = slot_layer_is_mask(l)             ? GPU_TEXTURE_FORMAT_RGBA32
	                                : base_bits == TEXTURE_BITS_BITS8  ? GPU_TEXTURE_FORMAT_RGBA32
	                                : base_bits == TEXTURE_BITS_BITS16 ? GPU_TEXTURE_FORMAT_RGBA64
	                                                                   : GPU_TEXTURE_FORMAT_RGBA128;
	gpu_texture_t       *views[3] = {l->texpaint, l->texpaint_nor, l->texpaint_pack};
	for (i32 i = 0; i < 3; ++i) {
		if (views[i] == NULL) {
			continue;
		}
		udim_store_t *s = udim_store_of(views[i]);
		for (i32 j = 0; j < (s != NULL ? s->count : 1); ++j) { // A view without tiles (undo slot saving a mask) as one tile
			if (s != NULL) {
				udim_show(s, s->ids[j]);
			}
			gpu_texture_t *fresh = gpu_create_render_target(width, height, format);
			draw_begin(fresh, false, 0);
			draw_set_pipeline(udim_copy_pipe(format));
			draw_scaled_image(views[i], 0, 0, width, height);
			draw_set_pipeline(NULL);
			draw_end();
			util_udim_replace(views[i], fresh);
		}
		if (s != NULL) {
			udim_show(s, udim_cur);
		}
	}
	return true;
}

static bool udim_is_undo(slot_layer_t *l) {
	return l->ext != NULL && starts_with(l->ext, "_undo");
}

// The tiles held by the slot view swap their content with those of the layer view; a slot view without tiles is first
// copied to every tile of the layer (saved before the layer was tiled)
static void udim_swap_views(gpu_texture_t *layer_view, gpu_texture_t *slot_view) {
	udim_store_t *ls = udim_store_of(layer_view);
	if (ls == NULL || slot_view == NULL) {
		return;
	}
	udim_store_t *ss = udim_store_of(slot_view);
	if (ss == NULL) {
		ss        = udim_tile_view(slot_view, true, ls->count, ls->ids, true);
		ss->whole = true;
	}
	// A whole save swapped back after the table grew (an undo, or a redo that runs its operation again on every tile):
	// the new tiles of the layer, unchanged since, go to the slot as they are
	for (i32 j = 0; ss->whole && j < ls->count && ss->count < UDIM_MAX_TILES; ++j) {
		i32 t = ls->ids[j];
		if (udim_index(ss, t) < 0) {
			gpu_texture_t *like = ss->cur >= 0 ? ss->view : &ss->tiles[0];
			ss->ids[ss->count]   = t;
			ss->tiles[ss->count] = udim_new_content(like->width, like->height, like->format, ls->cur == t ? ls->view : &ls->tiles[j]);
			ss->count++;
		}
	}
	for (i32 i = 0; i < ss->count; ++i) {
		i32 t = ss->ids[i];
		i32 j = udim_index(ls, t);
		if (j < 0) {
			continue;
		}
		gpu_texture_t *a = ls->cur == t ? ls->view : &ls->tiles[j];
		gpu_texture_t *b = ss->cur == t ? ss->view : &ss->tiles[i];
		if (udim_busy(a) || udim_busy(b)) {
			continue;
		}
		gpu_texture_t tmp = *a;
		*a                = *b;
		*b                = tmp;
	}
}

bool util_udim_swap_layers(slot_layer_t *a, slot_layer_t *b) {
	slot_layer_t *slot  = udim_is_undo(a) ? a : udim_is_undo(b) ? b : NULL;
	slot_layer_t *layer = slot == a ? b : a;
	if (slot == NULL || udim_is_undo(layer) || slot_layer_is_group(layer) || slot->texpaint == NULL) {
		return false;
	}
	udim_disarm(); // Undo or redo: the paint step is over
	if (!util_udim_layer_tiled(layer)) {
		bool held = udim_store_of(slot->texpaint) != NULL || udim_store_of(slot->texpaint_nor) != NULL || udim_store_of(slot->texpaint_pack) != NULL;
		if (!held) {
			return false;
		}
		if (!util_udim_tile_layer(layer, true)) {
			util_udim_undo_reset(slot);
			return false;
		}
	}
	if (gpu_in_use) {
		return false;
	}
	udim_swap_views(layer->texpaint, slot->texpaint);
	if (slot_layer_is_layer(layer) && slot_layer_is_layer(slot)) {
		udim_swap_views(layer->texpaint_nor, slot->texpaint_nor);
		udim_swap_views(layer->texpaint_pack, slot->texpaint_pack);
	}
	// The rest of slot_layer_swap()
	gpu_texture_t *preview = layer->texpaint_preview;
	layer->texpaint_preview = slot->texpaint_preview;
	slot->texpaint_preview  = preview;
	if (a->texpaint_sculpt != NULL && b->texpaint_sculpt != NULL) {
		render_target_t *srt0          = any_map_get(render_path_render_targets, string("texpaint_sculpt%s", a->ext));
		render_target_t *srt1          = any_map_get(render_path_render_targets, string("texpaint_sculpt%s", b->ext));
		srt0->_image                   = b->texpaint_sculpt;
		srt1->_image                   = a->texpaint_sculpt;
		gpu_texture_t *texpaint_sculpt = a->texpaint_sculpt;
		a->texpaint_sculpt             = b->texpaint_sculpt;
		b->texpaint_sculpt             = texpaint_sculpt;
	}
	return true;
}

void util_udim_undo_reset(slot_layer_t *slot) {
	if (udim_arm_layer != NULL && slot->texpaint == udim_arm_slot_views[0]) {
		udim_disarm();
	}
	udim_untile_layer(slot);
}

// The view gets a content of its own for a tile: its texture when it has no tiles yet (a partial store then), a new
// cleared one otherwise. Tile 0: the texture of an undo slot waiting for the first tile of a paint step
static void udim_hold_tile(gpu_texture_t *view, i32 tile) {
	udim_store_t *s = udim_store_of(view);
	if (view == NULL) {
		return;
	}
	if (s == NULL) {
		s         = udim_register(view, true);
		s->ids[0] = tile;
		s->count  = 1;
		s->cur    = tile;
	}
	else if (udim_index(s, tile) < 0 && s->count < UDIM_MAX_TILES) {
		gpu_texture_t *like = s->cur >= 0 ? s->view : &s->tiles[0]; // The placeholder shown: every content is in tiles
		s->ids[s->count]    = tile;
		s->tiles[s->count]  = udim_new_content(like->width, like->height, like->format, NULL);
		s->count++;
		udim_show(s, udim_cur);
	}
}

static void udim_hold(gpu_texture_t *view) {
	udim_hold_tile(view, udim_cur);
}

// The undo slot is about to save a tile (of the context, or 0 for the first tile of a paint step): a texture for it
static void udim_undo_hold(slot_layer_t *slot, bool is_mask, i32 tile) {
	gpu_texture_t *views[3] = {slot->texpaint, is_mask ? NULL : slot->texpaint_nor, is_mask ? NULL : slot->texpaint_pack};
	for (i32 i = 0; i < 3; ++i) {
		if (views[i] != NULL) {
			udim_hold_tile(views[i], tile);
		}
	}
}

// The upstream copy of the tile of the context into the undo slot
static void udim_copy_tile_to_undo(i32 from_id, i32 to_id, bool is_mask) {
	udim_copying   = true;
	history_undo_i = to_id;
	history_copy_to_undo(from_id, to_id, is_mask);
	udim_copying = false;
}

bool util_udim_copy_to_undo(i32 from_id, i32 to_id, bool is_mask) {
	if (udim_copying || history_undo_layers == NULL || to_id >= history_undo_layers->length) {
		return false;
	}
	udim_disarm(); // A new step
	slot_layer_t *slot = history_undo_slot(to_id);
	util_udim_undo_reset(slot);
	slot_layer_t         *from   = NULL;
	slot_layer_t_array_t *layers = g_project->_->layers;
	for (i32 i = 0; i < layers->length; ++i) {
		if (layers->buffer[i]->id == from_id) {
			from = layers->buffer[i];
		}
	}
	if (from == NULL || !util_udim_layer_tiled(from) || gpu_in_use) {
		return false;
	}
	i32 n = util_udim_loop_begin(true);
	for (i32 k = 0; k < n; ++k) {
		util_udim_loop_tile(k);
		udim_undo_hold(slot, is_mask, udim_cur);
		udim_copy_tile_to_undo(from_id, to_id, is_mask);
	}
	util_udim_loop_end();
	gpu_texture_t *views[3] = {slot->texpaint, slot->texpaint_nor, slot->texpaint_pack};
	for (i32 i = 0; i < 3; ++i) {
		udim_store_t *s = udim_store_of(views[i]);
		if (s != NULL) {
			s->whole = true;
		}
	}
	return true;
}

// Copy on write of a paint step: the tile shown by a view of its layer goes to the undo slot when first drawn to
static void udim_save_tile(i32 v) {
	udim_store_t *ls = udim_store_of(udim_arm_views[v]);
	udim_store_t *ss = udim_store_of(udim_arm_slot_views[v]);
	if (ls == NULL || ss == NULL || ls->cur < 0 || udim_index(ss, ls->cur) >= 0 || ss->count >= UDIM_MAX_TILES) {
		return;
	}
	gpu_texture_t *draw_current = _draw_current; // Called from gpu_begin(), maybe that of draw_begin()
	u32            draw_color   = draw_get_color();
	i32            spare        = udim_index(ss, 0);
	if (spare >= 0) { // The texture of the slot, for the first tile of the step
		ss->ids[spare] = ls->cur;
		ss->cur        = ss->cur == 0 ? ls->cur : ss->cur;
		udim_show(ss, ls->cur);
		draw_begin(ss->view, false, 0);
		draw_set_pipeline(udim_copy_pipe(ss->view->format));
		draw_scaled_image(ls->view, 0, 0, ss->view->width, ss->view->height);
		draw_set_pipeline(NULL);
		draw_end();
	}
	else {
		ss->ids[ss->count]   = ls->cur;
		ss->tiles[ss->count] = udim_new_content(ls->view->width, ls->view->height, ls->view->format, ls->view);
		ss->count++;
	}
	_draw_current = draw_current;
	draw_set_color(draw_color);
	udim_show(ss, udim_cur); // The paint shader reads the slot (_texpaint_undo)
}

static void udim_target_beginning(gpu_texture_t **targets, i32 count) {
	static bool inside = false;
	if (udim_arm_layer == NULL || inside) {
		return;
	}
	inside = true;
	for (i32 i = 0; i < count; ++i) {
		for (i32 v = 0; v < 3; ++v) {
			if (targets[i] != NULL && targets[i] == udim_arm_views[v]) {
				udim_save_tile(v);
			}
		}
	}
	inside = false;
}

bool util_udim_paint_undo(slot_layer_t *layer, i32 to_id, bool is_mask) {
	util_udim_sync(); // The shaders follow the rule: no frame rendered yet since the table changed (scripts)
	if (history_undo_layers == NULL || to_id >= history_undo_layers->length || layer->texpaint_sculpt != NULL || !util_udim_layer_tiled(layer) ||
	    gpu_in_use) {
		return false;
	}
	udim_disarm();
	slot_layer_t *slot = history_undo_slot(to_id);
	util_udim_undo_reset(slot);
	udim_undo_hold(slot, is_mask, 0); // Nothing saved yet: a stroke that draws no tile has nothing to give back
	udim_arm_layer         = layer;
	udim_arm_views[0]      = layer->texpaint;
	udim_arm_views[1]      = is_mask ? NULL : layer->texpaint_nor;
	udim_arm_views[2]      = is_mask ? NULL : layer->texpaint_pack;
	udim_arm_slot_views[0] = slot->texpaint;
	udim_arm_slot_views[1] = is_mask ? NULL : slot->texpaint_nor;
	udim_arm_slot_views[2] = is_mask ? NULL : slot->texpaint_pack;
	gpu_target_beginning   = udim_target_beginning;
	history_undo_i         = (to_id + 1) % g_config->undo_steps; // As history_copy_to_undo()
	return true;
}

// Inside a loop over the tiles, at any depth: the code called paints and dilates the tile of the context only, a nested
// loop stays on it
static bool udim_in_tile_loop() {
	for (i32 d = 0; d < udim_loop_depth && d < 8; ++d) {
		if (udim_loop_tiled[d]) {
			return true;
		}
	}
	return false;
}

i32 util_udim_loop_begin(bool tiled) {
	if (!gpu_in_use) {
		udim_update_table();
	}
	i32  d      = udim_loop_depth < 8 ? udim_loop_depth : 7;
	bool nested = udim_in_tile_loop();
	udim_loop_depth++;
	udim_loop_saved[d] = udim_cur;
	udim_loop_tiled[d] = tiled && udim_table_count > 1 && !gpu_in_use && !nested;
	return udim_loop_tiled[d] ? udim_table_count : 1;
}

void util_udim_loop_tile(i32 k) {
	i32 d = udim_loop_depth <= 8 ? udim_loop_depth - 1 : 7;
	if (d >= 0 && udim_loop_tiled[d]) {
		util_udim_set_tile(udim_table[k]);
	}
}

void util_udim_loop_end() {
	if (udim_loop_depth == 0) {
		return;
	}
	udim_loop_depth--;
	i32 d = udim_loop_depth < 8 ? udim_loop_depth : 7;
	if (udim_loop_tiled[d]) {
		util_udim_set_tile(udim_loop_saved[d]);
	}
}

// Layer operations (lot D2c): layers_merge_layer() and layers_apply_mask() called back on each tile of the layer drawn to
bool util_udim_merge_layer(slot_layer_t *l0, slot_layer_t *l1, bool use_mask) {
	if (gpu_in_use || udim_in_tile_loop() || !util_udim_layer_tiled(l0)) {
		return false;
	}
	i32 n = util_udim_loop_begin(true);
	for (i32 k = 0; k < n; ++k) {
		util_udim_loop_tile(k);
		layers_merge_layer(l0, l1, use_mask);
	}
	util_udim_loop_end();
	return true;
}

bool util_udim_apply_mask(slot_layer_t *l, slot_layer_t *m) {
	if (gpu_in_use || udim_in_tile_loop() || !util_udim_layer_tiled(l)) {
		return false;
	}
	i32 n = util_udim_loop_begin(true);
	for (i32 k = 0; k < n; ++k) {
		util_udim_loop_tile(k);
		layers_apply_mask(l, m);
	}
	util_udim_loop_end();
	return true;
}

// layers_draw_mesh_materials(): the flatten of a tiled project is the one of the tile of the context. The override
// materials are drawn there by the objects of that tile, in their uvs (the atlas transform set to the identity, as for
// the raytrace override pass).
bool util_udim_draw_mesh_materials(char *context) {
	if (udim_store_count == 0 || udim_table_count < 2) {
		return false;
	}
	bool saved                         = render_path_raytrace_override_pass;
	render_path_raytrace_override_pass = true;
	_mesh_object_last_pipeline         = NULL;
	for (i32 i = 0; i < scene_meshes->length; ++i) {
		mesh_object_t *mesh = scene_meshes->buffer[i];
		if (udim_cur >= 0 && util_mesh_udim_tile(mesh->base->name) == udim_cur) {
			mesh_object_render(mesh, context, _render_path_bind_params);
		}
	}
	render_path_raytrace_override_pass = saved;
	render_path_end();
	return true;
}

void util_udim_draw_meshes(char *context) {
	if (udim_store_count == 0 || udim_table_count < 2) {
		render_path_draw_meshes(context);
		return;
	}
	i32 saved                  = udim_cur;
	_mesh_object_last_pipeline = NULL;
	for (i32 i = 0; i < scene_meshes->length; ++i) {
		mesh_object_t *mesh = scene_meshes->buffer[i];
		i32            tile = util_mesh_udim_tile(mesh->base->name);
		util_udim_set_tile(udim_table_index(tile) >= 0 ? tile : -1); // The textures are read at the draw
		mesh_object_render(mesh, context, _render_path_bind_params);
	}
	util_udim_set_tile(saved);
	render_path_end();
}

// Paint (lot D2b)

static bool udim_skips(mesh_object_t *m, char *context) {
	return m->skip_context != NULL && string_equals(m->skip_context, context);
}

// The objects the "paint" context draws: those of the merged mesh while it is shown, otherwise those not skipping it
static bool udim_paint_candidate(mesh_object_t *m) {
	mesh_object_t         *merged = g_context->merged_object;
	mesh_object_t_array_t *scope  = util_mesh_merged_scope();
	if (m == merged || !m->base->visible) {
		return false;
	}
	if (merged != NULL && merged->base->visible && scope != NULL) {
		return array_index_of((any_array_t *)scope, m) >= 0;
	}
	return !udim_skips(m, "paint");
}

// High poly bake: the object chosen and the other tiles of its mesh (the same name but for the tile)
static bool udim_high_poly(mesh_object_t *m) {
	mesh_object_t_array_t *objects = g_project->_->paint_objects;
	i32                    h       = g_context->bake_high_poly;
	if (h < 0 || h >= objects->length) {
		return false;
	}
	char *a  = m->base->name;
	char *b  = objects->buffer[h]->base->name;
	i32   la = string_length(a) - 5;
	return m == objects->buffer[h] ||
	       (util_mesh_udim_tile(a) > 0 && util_mesh_udim_tile(b) > 0 && la == string_length(b) - 5 && strncmp(a, b, la) == 0);
}

// 1: first pass of a high poly bake (its data from the high poly), 2: the next ones, 0: not a high poly bake
static i32 udim_high_poly_pass() {
	if (g_context->tool != TOOL_TYPE_BAKE || !render_path_paint_baking) {
		return 0;
	}
	bake_type_t t = g_context->bake_type;
	return t == BAKE_TYPE_NORMAL_OBJECT || t == BAKE_TYPE_POSITION                             ? 1
	       : t == BAKE_TYPE_NORMAL || t == BAKE_TYPE_HEIGHT || t == BAKE_TYPE_DERIVATIVE ? 2
	                                                                                      : 0;
}

// The objects baked from the high poly: those of the layer (the object it is limited to, or the merged mesh while shown),
// the high poly apart. Both passes see them: the first one selects the high poly as paint object.
static bool udim_high_poly_low(mesh_object_t *m) {
	if (udim_high_poly(m)) {
		return false;
	}
	i32 mask = context_layer_filter_used() ? g_context->layer_filter : context_object_mask_used() ? slot_layer_get_object_mask(g_context->layer) : 0;
	return mask > 0 ? m == g_project->_->paint_objects->buffer[mask - 1] && m->base->visible : udim_paint_candidate(m);
}

// Drawn by the paint loop on its tile. A high poly bake: the high poly then the objects baked from it, on the tiles of these
// only (the upstream merged mesh drew everything in both passes, the high poly over the low one).
static bool udim_paint_draws(mesh_object_t *m, i32 tile) {
	if (m == g_context->merged_object || util_mesh_udim_tile(m->base->name) != tile) {
		return false;
	}
	i32 pass = udim_high_poly_pass();
	if (pass == 0) {
		return udim_paint_candidate(m);
	}
	for (i32 i = 0; i < scene_meshes->length; ++i) {
		mesh_object_t *o = scene_meshes->buffer[i];
		if (util_mesh_udim_tile(o->base->name) == tile && udim_high_poly_low(o)) {
			return pass == 1 ? udim_high_poly(m) : udim_high_poly_low(m);
		}
	}
	return false;
}

// In the paint loop: the objects of the tile of the context, instead of the merged mesh
static bool udim_draw_tile_meshes() {
	if (!udim_painting) {
		return false;
	}
	_mesh_object_last_pipeline = NULL;
	for (i32 i = 0; i < scene_meshes->length; ++i) {
		mesh_object_t *m = scene_meshes->buffer[i];
		if (!udim_paint_draws(m, udim_cur)) {
			continue;
		}
		char *skip       = m->skip_context;
		bool  visible    = m->base->visible;
		m->skip_context  = NULL;
		m->base->visible = true; // A hidden tile of the high poly, as upstream shows the high poly for its pass
		mesh_object_render(m, "paint", _render_path_bind_params);
		m->skip_context  = skip;
		m->base->visible = visible;
	}
	render_path_end();
	return true;
}

static gpu_texture_t *udim_blend_view(i32 i) {
	render_target_t *rt = any_map_get(render_path_render_targets, i == 0 ? "texpaint_blend0" : "texpaint_blend1");
	return rt != NULL ? rt->_image : NULL;
}

void util_udim_blend_reset() {
	if (gpu_in_use) {
		return;
	}
	for (i32 i = 0; i < 2; ++i) {
		udim_store_t *s = udim_store_of(udim_blend_view(i));
		if (s != NULL) {
			udim_untile(s, udim_cur);
		}
	}
}

static void udim_dirty_add(i32 tile) {
	for (i32 i = 0; i < udim_dirty_count; ++i) {
		if (udim_dirty[i] == tile) {
			return;
		}
	}
	if (udim_dirty_count < UDIM_MAX_TILES) {
		udim_dirty[udim_dirty_count++] = tile;
	}
}

f32 uniforms_ext_f32_link(object_t *object, shader_data_t *mat, char *link); // uniforms.c, after this file in main.c

// Bounding box of the positions of a mesh, object space, computed once per vertex array
typedef struct {
	mesh_data_t *data;
	i16         *values;
	i32          length;
	vec4_t       min;
	vec4_t       max;
} udim_bounds_t;

static udim_bounds_t *udim_bounds       = NULL;
static i32            udim_bounds_count = 0;
static i32            udim_bounds_cap   = 0;

static udim_bounds_t *udim_mesh_bounds(mesh_data_t *data) {
	vertex_array_t *pos = mesh_data_get_vertex_array(data, "pos");
	if (pos == NULL) {
		return NULL;
	}
	udim_bounds_t *b = NULL;
	for (i32 i = 0; i < udim_bounds_count && b == NULL; ++i) {
		b = udim_bounds[i].data == data ? &udim_bounds[i] : NULL;
	}
	if (b != NULL && b->values == pos->values->buffer && b->length == pos->values->length) {
		return b;
	}
	if (b == NULL) {
		if (udim_bounds_count == udim_bounds_cap) {
			udim_bounds_cap = udim_bounds_cap == 0 ? 64 : udim_bounds_cap * 2;
			udim_bounds     = realloc(udim_bounds, udim_bounds_cap * sizeof(udim_bounds_t));
		}
		b = &udim_bounds[udim_bounds_count++];
	}
	b->data   = data;
	b->values = pos->values->buffer;
	b->length = pos->values->length;
	i16 lo[3] = {32767, 32767, 32767}; // mesh_data_calculate_aabb_min_max() keeps the origin in the box
	i16 hi[3] = {-32767, -32767, -32767};
	for (i32 i = 0; i + 2 < pos->values->length; i += 4) {
		for (i32 k = 0; k < 3; ++k) {
			i16 c = pos->values->buffer[i + k];
			lo[k] = c < lo[k] ? c : lo[k];
			hi[k] = c > hi[k] ? c : hi[k];
		}
	}
	f32 f  = data->scale_pos / 32767.0f;
	b->min = (vec4_t){lo[0] * f, lo[1] * f, lo[2] * f, 1.0};
	b->max = (vec4_t){hi[0] * f, hi[1] * f, hi[2] * f, 1.0};
	return b;
}

// Whether the brush can reach the tiles, by their objects: the stroke is a capsule of world radius R around the surface
// points under inp and inplast (make_brush.c). A point p within R of a point c of its axis is seen from the camera at
// most asin(R / |p|) from c: in the tangent plane (x / -z, y / -z), where the axis is the segment inp - inplast, at
// most tan(t + a) - tan(t) from it, t the angle of the stroke off the view axis. Aligned to the camera, the capsule is
// that segment widened by R / depth of the surface under inp, nearer than no object. Conservative: the box corners of
// each object, a square widening, 1.5 of margin; the other tools (fill, decal, text, particles, x-ray) paint every tile.
typedef struct {
	bool   cull;
	bool   ortho;
	bool   align;
	mat4_t v;
	mat4_t p;
	f32    radius;
	f32    ax, ay, bx, by; // Stroke in ndc
	f32    theta;          // Largest angle of the stroke off the view axis
	f32    align_margin;   // Tangent plane, camera aligned
} udim_brush_t;

static bool udim_corners_ndc(mesh_object_t *m, udim_brush_t *b, f32 *rect, f32 *dist, f32 *depth) {
	udim_bounds_t *bounds = udim_mesh_bounds(m->data);
	if (bounds == NULL) {
		return false;
	}
	f32 vmin[3] = {1e30, 1e30, 1e30};
	f32 vmax[3] = {-1e30, -1e30, -1e30};
	for (i32 k = 0; k < 8; ++k) {
		vec4_t c = {k & 1 ? bounds->max.x : bounds->min.x, k & 2 ? bounds->max.y : bounds->min.y, k & 4 ? bounds->max.z : bounds->min.z, 1.0};
		c        = vec4_apply_mat4(vec4_apply_mat4(c, m->base->transform->world), b->v);
		if (!b->ortho && c.z > -1e-4) {
			return false; // At or behind the camera
		}
		vmin[0]  = c.x < vmin[0] ? c.x : vmin[0];
		vmin[1]  = c.y < vmin[1] ? c.y : vmin[1];
		vmin[2]  = c.z < vmin[2] ? c.z : vmin[2];
		vmax[0]  = c.x > vmax[0] ? c.x : vmax[0];
		vmax[1]  = c.y > vmax[1] ? c.y : vmax[1];
		vmax[2]  = c.z > vmax[2] ? c.z : vmax[2];
		vec4_t n = vec4_apply_mat4(c, b->p);
		f32    x = n.x / n.w;
		f32    y = n.y / n.w;
		rect[0]  = k == 0 || x < rect[0] ? x : rect[0];
		rect[1]  = k == 0 || y < rect[1] ? y : rect[1];
		rect[2]  = k == 0 || x > rect[2] ? x : rect[2];
		rect[3]  = k == 0 || y > rect[3] ? y : rect[3];
	}
	f32 d = 0.0;
	for (i32 i = 0; i < 3; ++i) { // Camera to the box
		f32 e = vmin[i] > 0.0 ? vmin[i] : vmax[i] < 0.0 ? -vmax[i] : 0.0;
		d += e * e;
	}
	*dist  = sqrtf(d);
	*depth = -vmax[2];
	return true;
}

// Segment a - b of ndc against the rect widened by (mx, my), slabs
static bool udim_segment_hits(udim_brush_t *b, f32 *rect, f32 mx, f32 my) {
	f32 t0    = 0.0;
	f32 t1    = 1.0;
	f32 o[2]  = {b->ax, b->ay};
	f32 d[2]  = {b->bx - b->ax, b->by - b->ay};
	f32 lo[2] = {rect[0] - mx, rect[1] - my};
	f32 hi[2] = {rect[2] + mx, rect[3] + my};
	for (i32 i = 0; i < 2; ++i) {
		if (fabsf(d[i]) < 1e-9) {
			if (o[i] < lo[i] || o[i] > hi[i]) {
				return false;
			}
			continue;
		}
		f32 u0 = (lo[i] - o[i]) / d[i];
		f32 u1 = (hi[i] - o[i]) / d[i];
		t0     = fmaxf(t0, fminf(u0, u1));
		t1     = fminf(t1, fmaxf(u0, u1));
	}
	return t0 <= t1;
}

static bool udim_brush_reaches(mesh_object_t *m, udim_brush_t *b) {
	f32 rect[4];
	f32 dist;
	f32 depth;
	if (!b->cull || !udim_corners_ndc(m, b, rect, &dist, &depth)) {
		return true;
	}
	f32 margin = 0.0; // Tangent plane
	if (b->ortho) {
		margin = b->radius;
	}
	else {
		if (dist <= b->radius * 1.01) {
			return true;
		}
		f32 a = asinf(b->radius / dist);
		if (b->theta + a > 1.4) {
			return true;
		}
		margin = tanf(b->theta + a) - tanf(b->theta);
		if (b->align) {
			if (b->align_margin < 0.0) {
				return true;
			}
			margin = fmaxf(margin, b->align_margin);
		}
	}
	margin *= 1.5;
	return udim_segment_hits(b, rect, margin * fabsf(b->p.m[0]), margin * fabsf(b->p.m[5]));
}

static void udim_brush_init(udim_brush_t *b) {
	tool_type_t tool = g_context->tool;
	b->cull = (tool == TOOL_TYPE_BRUSH || tool == TOOL_TYPE_ERASER || tool == TOOL_TYPE_CLONE || tool == TOOL_TYPE_BLUR) && !g_context->xray &&
	          !context_is_decal() && scene_camera != NULL;
	if (!b->cull) {
		return;
	}
	b->v      = scene_camera->v;
	b->p      = scene_camera->p;
	b->ortho  = b->p.m[15] != 0.0;
	b->align  = context_is_brush_camera_align();
	b->radius = uniforms_ext_f32_link(NULL, NULL, "_brush_radius");
	b->ax     = g_context->paint_vec.x * 2.0 - 1.0;
	b->ay     = 1.0 - g_context->paint_vec.y * 2.0;
	b->bx     = g_context->last_paint_vec_x * 2.0 - 1.0;
	b->by     = 1.0 - g_context->last_paint_vec_y * 2.0;
	b->theta  = 0.0;
	if (b->ortho) {
		return;
	}
	f32 ends[4] = {b->ax, b->ay, b->bx, b->by};
	for (i32 i = 0; i < 2; ++i) {
		f32 tx   = (ends[i * 2] + b->p.m[8]) / b->p.m[0];
		f32 ty   = (ends[i * 2 + 1] + b->p.m[9]) / b->p.m[5];
		b->theta = fmaxf(b->theta, atanf(sqrtf(tx * tx + ty * ty)));
	}
	b->align_margin = 0.0;
	if (b->align) { // R / depth of the nearest object
		f32 nearest = 1e30;
		for (i32 i = 0; i < scene_meshes->length; ++i) {
			mesh_object_t *m = scene_meshes->buffer[i];
			f32            rect[4];
			f32            dist;
			f32            depth;
			if (udim_paint_candidate(m) && util_mesh_udim_tile(m->base->name) >= 1000) {
				if (!udim_corners_ndc(m, b, rect, &dist, &depth)) {
					nearest = 0.0;
					break;
				}
				nearest = fminf(nearest, depth);
			}
		}
		b->align_margin = nearest > 1e-4 ? b->radius / nearest : -1.0;
	}
}

// Face and uv island fills compare the triangles or islands of the tile under the brush: that tile, 0 for none, -1 every
// tile. The island map is that of the frame before (uniforms.c), with its tile.
static i32 udim_fill_tile() {
	if (g_context->tool != TOOL_TYPE_FILL) {
		return -1;
	}
	if (g_context->fill_type == FILL_TYPE_FACE) {
		i32 tile = util_udim_cursor_tile();
		return tile >= 0 ? tile : 0;
	}
	if (g_context->fill_type == FILL_TYPE_UV_ISLAND) {
		return util_uv_uvislandmap_cached && udim_island_tile >= 0 ? udim_island_tile : 0;
	}
	return -1;
}

bool util_udim_paint_tiles(bool dilation) {
	if (udim_painting || gpu_in_use) {
		return false;
	}
	util_udim_sync();
	if (g_context->paint2d || !util_udim_layer_tiled(g_context->layer)) {
		util_udim_blend_reset(); // Left by a stroke on a tiled layer
		return false;
	}
	udim_update_table();
	udim_brush_t brush;
	udim_brush_init(&brush);
	i32 only = udim_in_tile_loop() ? udim_cur : udim_fill_tile();
	i32 tiles[UDIM_MAX_TILES];
	i32 n = 0;
	for (i32 i = 0; i < udim_table_count; ++i) {
		if (only >= 0 && udim_table[i] != only) {
			continue;
		}
		for (i32 j = 0; j < scene_meshes->length; ++j) {
			mesh_object_t *m = scene_meshes->buffer[j];
			if (udim_paint_draws(m, udim_table[i]) && udim_brush_reaches(m, &brush)) {
				tiles[n++] = udim_table[i];
				break;
			}
		}
	}
	i32 saved     = udim_cur;
	udim_painting = true;
	for (i32 k = 0; k < n; ++k) {
		util_udim_set_tile(tiles[k]);
		if (udim_cur != tiles[k]) {
			continue;
		}
		udim_hold(udim_blend_view(0));
		udim_hold(udim_blend_view(1));
		udim_dirty_add(tiles[k]);
		if (g_context->tool == TOOL_TYPE_FILL && g_context->fill_type == FILL_TYPE_FACE) {
			util_uv_cache_triangle_map(); // Read by the pass
		}
		render_path_paint_commands_paint(dilation);
	}
	udim_painting = false;
	util_udim_set_tile(saved);
	return true;
}

bool util_udim_dilate(bool base, bool nor_pack) {
	static bool inside = false;
	if (inside || udim_painting || gpu_in_use || !util_udim_layer_tiled(g_context->layer) || udim_in_tile_loop()) {
		return false;
	}
	inside    = true;
	i32 saved = udim_cur;
	for (i32 i = 0; i < udim_dirty_count; ++i) {
		if (udim_table_index(udim_dirty[i]) >= 0) {
			util_udim_set_tile(udim_dirty[i]);
			render_path_paint_dilate(base, nor_pack);
		}
	}
	util_udim_set_tile(saved);
	if (nor_pack) {
		udim_dirty_count = 0;
	}
	inside = false;
	return true;
}

// As util_uv_cache_dilate_map() creates it
static void udim_dilate_pipe() {
	if (util_uv_pipe_dilate != NULL) {
		return;
	}
	util_uv_pipe_dilate                  = gpu_create_pipeline();
	util_uv_pipe_dilate->vertex_shader   = sys_get_shader("dilate_map.vert");
	util_uv_pipe_dilate->fragment_shader = sys_get_shader("dilate_map.frag");
	gpu_vertex_structure_t *vs           = ALLOC_INIT(gpu_vertex_structure_t, {0});
	gpu_vertex_structure_add(vs, "pos", GPU_VERTEX_DATA_I16_4X_NORM);
	gpu_vertex_structure_add(vs, "nor", GPU_VERTEX_DATA_I16_2X_NORM);
	gpu_vertex_structure_add(vs, "tex", GPU_VERTEX_DATA_I16_2X_NORM);
	util_uv_pipe_dilate->input_layout        = vs;
	util_uv_pipe_dilate->depth_write         = false;
	util_uv_pipe_dilate->depth_mode          = GPU_COMPARE_MODE_ALWAYS;
	util_uv_pipe_dilate->color_attachment[0] = GPU_TEXTURE_FORMAT_R8;
	gpu_pipeline_compile(util_uv_pipe_dilate);
}

// Maps of a tiled layer (dilate, triangles, uvs): tiled views, each tile built from the objects of the tile in their
// local uvs when first needed. Upstream uncaches a map when the objects change: then every tile again.
typedef struct {
	gpu_texture_t      **view;
	bool                *cached;
	gpu_texture_format_t format;
	bool                 valid[1000]; // Tile id - 1000: built
} udim_map_t;

static udim_map_t udim_dilate_map   = {&util_uv_dilatemap, &util_uv_dilatemap_cached, GPU_TEXTURE_FORMAT_R8};
static udim_map_t udim_triangle_map = {&util_uv_trianglemap, &util_uv_trianglemap_cached, GPU_TEXTURE_FORMAT_RGBA32};
static udim_map_t udim_uv_map       = {&util_uv_uvmap, &util_uv_uvmap_cached, GPU_TEXTURE_FORMAT_RGBA32};

// -1: the map of upstream (not a tiled layer), 1: the map of the tile of the context is in the view, 0: to draw, the view
// holds the tile
static i32 udim_map_begin(udim_map_t *m) {
	udim_store_t *s = udim_store_of(*m->view);
	if (gpu_in_use) {
		return s != NULL ? 1 : -1;
	}
	if (!util_udim_layer_tiled(g_context->layer) || udim_cur < 1000 || udim_cur >= 2000) {
		if (s != NULL) { // Back to the map of upstream
			udim_untile(s, udim_cur);
			*m->cached = false;
		}
		return -1;
	}
	gpu_texture_t *like = s == NULL || s->cur >= 0 ? *m->view : s->count > 0 ? &s->tiles[0] : NULL; // The view may show the placeholder
	if (like != NULL && (like->width != config_get_texture_res_x() || like->height != config_get_texture_res_y())) {
		gpu_delete_texture(*m->view); // Its tiles with it
		*m->view = NULL;
		s        = NULL;
	}
	if (*m->view == NULL) {
		*m->view = gpu_create_render_target(config_get_texture_res_x(), config_get_texture_res_y(), m->format);
	}
	if (s == NULL || !*m->cached) {
		memset(m->valid, 0, sizeof(m->valid));
		*m->cached = true;
	}
	if (s != NULL && s->cur == udim_cur && m->valid[udim_cur - 1000]) {
		return 1;
	}
	udim_hold(*m->view);
	m->valid[udim_cur - 1000] = true;
	return 0;
}

// The paint objects of the tile of the context, of the object mask or the layer filter
static bool udim_map_object(i32 i) {
	mesh_object_t_array_t *objects = g_project->_->paint_objects;
	i32                    mask    = context_object_mask_used() ? slot_layer_get_object_mask(g_context->layer) : 0;
	if (context_layer_filter_used()) {
		mask = g_context->layer_filter;
	}
	if (mask > 0 && mask <= objects->length && i != mask - 1) {
		return false;
	}
	return util_mesh_udim_tile(objects->buffer[i]->base->name) == udim_cur;
}

static i32 udim_bytes_tile = -1; // Tile of util_uv_dilate_bytes

bool util_udim_cache_dilate_map() {
	i32 state = udim_map_begin(&udim_dilate_map);
	if (state != 0) {
		return state > 0;
	}
	udim_dilate_pipe();
	mesh_object_t_array_t *objects = g_project->_->paint_objects;
	_gpu_begin(util_uv_dilatemap, NULL, NULL, GPU_CLEAR_COLOR, 0x00000000, 0.0);
	gpu_set_pipeline(util_uv_pipe_dilate);
	for (i32 i = 0; i < objects->length; ++i) {
		if (udim_map_object(i)) {
			gpu_set_vertex_buffer(objects->buffer[i]->data->_->vertex_buffer);
			gpu_set_index_buffer(objects->buffer[i]->data->_->index_buffer);
			gpu_draw();
		}
	}
	gpu_end();
	util_uv_dilate_bytes = NULL;
	return true;
}

// As util_uv_cache_triangle_map(), the colors going on from an object to the next
bool util_udim_cache_triangle_map() {
	i32 state = udim_map_begin(&udim_triangle_map);
	if (state != 0) {
		return state > 0;
	}
	mesh_object_t_array_t *objects = g_project->_->paint_objects;
	draw_begin(util_uv_trianglemap, true, 0xff000000);
	f32 f     = (1 / 32767.0) * util_uv_trianglemap->width;
	i32 color = 0xff000001;
	for (i32 o = 0; o < objects->length; ++o) {
		if (!udim_map_object(o)) {
			continue;
		}
		i16_array_t *texa = objects->buffer[o]->data->vertex_arrays->buffer[2]->values;
		u32_array_t *inda = objects->buffer[o]->data->index_array;
		for (i32 i = 0; i < inda->length / 3; ++i) {
			if (color == 0xffffffff) {
				color = 0xff000001;
			}
			color++;
			draw_set_color(color);
			f32 x1 = texa->buffer[inda->buffer[i * 3] * 2] * f;
			f32 x2 = texa->buffer[inda->buffer[i * 3 + 1] * 2] * f;
			f32 x3 = texa->buffer[inda->buffer[i * 3 + 2] * 2] * f;
			f32 y1 = texa->buffer[inda->buffer[i * 3] * 2 + 1] * f;
			f32 y2 = texa->buffer[inda->buffer[i * 3 + 1] * 2 + 1] * f;
			f32 y3 = texa->buffer[inda->buffer[i * 3 + 2] * 2 + 1] * f;
			draw_filled_triangle(x1, y1, x2, y2, x3, y3);
		}
	}
	draw_end();
	return true;
}

// As util_uv_cache_uv_map()
static void udim_draw_uv_map() {
	mesh_object_t_array_t *objects = g_project->_->paint_objects;
	draw_begin(util_uv_uvmap, true, 0x00000000);
	draw_set_color(0xffffffff);
	f32 strength = config_get_texture_res_x() > 2048 ? 2.0 : 1.0;
	f32 f        = (1 / 32767.0) * util_uv_uvmap->width;
	for (i32 o = 0; o < objects->length; ++o) {
		if (!udim_map_object(o)) {
			continue;
		}
		i16_array_t *texa = objects->buffer[o]->data->vertex_arrays->buffer[2]->values;
		u32_array_t *inda = objects->buffer[o]->data->index_array;
		for (i32 i = 0; i < inda->length / 3; ++i) {
			f32 x1 = texa->buffer[inda->buffer[i * 3] * 2] * f;
			f32 x2 = texa->buffer[inda->buffer[i * 3 + 1] * 2] * f;
			f32 x3 = texa->buffer[inda->buffer[i * 3 + 2] * 2] * f;
			f32 y1 = texa->buffer[inda->buffer[i * 3] * 2 + 1] * f;
			f32 y2 = texa->buffer[inda->buffer[i * 3 + 1] * 2 + 1] * f;
			f32 y3 = texa->buffer[inda->buffer[i * 3 + 2] * 2 + 1] * f;
			draw_line_aa(x1, y1, x2, y2, strength);
			draw_line_aa(x2, y2, x3, y3, strength);
			draw_line_aa(x3, y3, x1, y1, strength);
		}
	}
	draw_end();
}

bool util_udim_cache_uv_map() {
	if (gpu_in_use) {
		return udim_store_of(util_uv_uvmap) != NULL;
	}
	i32 saved = udim_cur;
	for (i32 i = -1; i < udim_table_count; ++i) { // The tile of the context, then the others
		if (i >= 0) {
			util_udim_set_tile(udim_table[i]);
		}
		i32 state = udim_map_begin(&udim_uv_map);
		if (state < 0) {
			return false;
		}
		if (state == 0) {
			udim_draw_uv_map();
		}
	}
	util_udim_set_tile(saved);
	return true;
}

// Pickers (lot D2b)

vec4_t uniforms_ext_vec4_link(object_t *object, shader_data_t *mat, char *link); // uniforms.c

static gpu_texture_t *udim_cursor_target = NULL;

f32 util_udim_tile_code(object_t *object) {
	i32 tile = util_udim_object_tile(object);
	return tile > 1000 && tile < 2000 ? tile - 1000 : 0.0;
}

// The texel of gbuffer2 under the brush, as the picker shaders read it (make_picking.c)
i32 util_udim_cursor_tile() {
	render_target_t *rt = render_path_render_targets != NULL ? any_map_get(render_path_render_targets, "gbuffer2") : NULL;
	if (udim_table_count < 2 || gpu_in_use || g_context->paint2d || rt == NULL || rt->_image == NULL) {
		return -1;
	}
	gpu_texture_t *gbuffer2 = rt->_image;
	vec4_t         inp      = uniforms_ext_vec4_link(NULL, NULL, "_input_brush");
	i32            x        = (i32)(inp.x * gbuffer2->width);
	i32            y        = (i32)(inp.y * gbuffer2->height);
	if (x < 0 || y < 0 || x >= gbuffer2->width || y >= gbuffer2->height) {
		return -1;
	}
	if (udim_cursor_target == NULL) {
		udim_cursor_target = gpu_create_render_target(1, 1, GPU_TEXTURE_FORMAT_RGBA128);
	}
	draw_begin(udim_cursor_target, false, 0);
	draw_set_pipeline(pipes_copy128);
	draw_sub_image(gbuffer2, x, y, 1, 1, 0, 0);
	draw_set_pipeline(NULL);
	draw_end();
	buffer_t *pixel = gpu_get_texture_pixels(udim_cursor_target);
	i32       tile  = 1000 + (i32)roundf(buffer_get_f32(pixel, 0));
	return udim_table_index(tile) >= 0 ? tile : -1;
}

i32 util_udim_pick_begin(bool show) {
	i32 saved   = udim_cur;
	i32 tile    = util_udim_cursor_tile();
	udim_picked = tile >= 0 ? tile : udim_cur;
	if (show && tile >= 0 && util_udim_layer_tiled(g_context->layer)) {
		util_udim_set_tile(tile);
	}
	return saved;
}

i32 util_udim_picked_tile() {
	return udim_picked;
}

i32 util_udim_island_begin() {
	i32 saved = udim_cur;
	if (!util_udim_layer_tiled(g_context->layer) || gpu_in_use) {
		udim_bytes_tile  = -1;
		udim_island_tile = -1;
		return saved;
	}
	i32 tile = util_udim_cursor_tile();
	if (tile >= 0) {
		util_udim_set_tile(tile);
	}
	if (udim_bytes_tile != udim_cur) { // Read again from the dilate map of this tile
		util_uv_dilate_bytes = NULL;
	}
	udim_bytes_tile  = udim_cur;
	udim_island_tile = udim_cur;
	return saved;
}

// 2d view (lot D2b): a tiled layer shows and paints the tile of the context, chosen there

static i32 udim_view2d_next = -1;

static void udim_view2d_set(void *_) { // Outside the ui pass
	util_udim_set_tile(udim_view2d_next);
	g_context->ddirty       = 2;
	ui_view2d_hwnd->redraws = 2;
}

bool util_udim_view2d_tile() {
	if (!util_udim_layer_tiled(g_context->layer) || udim_table_count < 2) {
		return false;
	}
	static char           labels[UDIM_MAX_TILES][8];
	static string_array_t names    = {0};
	static i32            position = 0;
	names.length                   = 0;
	for (i32 i = 0; i < udim_table_count; ++i) {
		snprintf(labels[i], sizeof(labels[i]), "%d", udim_table[i]);
		string_array_push(&names, labels[i]);
	}
	i32 k    = udim_table_index(udim_cur);
	position = k < 0 ? 0 : k;
	ui_combo(&position, &names, tr("Tile"), false, UI_ALIGN_LEFT, true);
	if (ui_item_changed() && position >= 0 && position < udim_table_count) {
		udim_view2d_next = udim_table[position];
		sys_notify_on_next_frame(&udim_view2d_set, NULL);
	}
	return true;
}

// Export (lot D2d)

static bool udim_export_set   = false; // A tile exported alone set in the context, until util_udim_export_end()
static i32  udim_export_saved = -1;

// The tile of an object mask, -1 for none (shared layer, texture set, object without a tile)
static i32 udim_mask_tile(slot_layer_t *l) {
	i32 mask = slot_layer_get_object_mask(l);
	if (mask <= 0 || mask > g_project->_->paint_objects->length) {
		return -1;
	}
	return util_mesh_udim_tile(g_project->_->paint_objects->buffer[mask - 1]->base->name);
}

void util_udim_export_tile(char *object_name, slot_layer_t *selected) {
	util_udim_sync(); // The layers follow the rule: no frame rendered yet since the table changed (scripts)
	if (udim_store_count == 0 || udim_table_count < 2) {
		return;
	}
	i32 tile = util_mesh_udim_tile(object_name);
	if (tile < 0 && selected != NULL) {
		tile = udim_mask_tile(selected);
	}
	if (tile < 0 || udim_table_index(tile) < 0) {
		return;
	}
	if (!udim_export_set) {
		udim_export_saved = udim_cur;
		udim_export_set   = true;
	}
	util_udim_set_tile(tile);
}

void util_udim_export_end() {
	if (udim_export_set) {
		util_udim_set_tile(udim_export_saved);
		udim_export_set = false;
	}
}

static gpu_texture_t *udim_atlas_a    = NULL;
static gpu_texture_t *udim_atlas_b    = NULL;
static gpu_texture_t *udim_atlas_c    = NULL;
static gpu_texture_t *udim_atlas_mask = NULL;
static gpu_texture_t *udim_atlas_temp = NULL; // Masks of a tile merged

static gpu_texture_t *udim_atlas_target(gpu_texture_t **t, gpu_texture_t *like) {
	if (*t != NULL && ((*t)->width != like->width || (*t)->height != like->height || (*t)->format != like->format)) {
		gpu_delete_texture(*t);
		*t = NULL;
	}
	if (*t == NULL) {
		*t = gpu_create_render_target(like->width, like->height, like->format);
	}
	return *t;
}

// The tiles of l in the atlas: every tile with a cell, or the tile of its object for a layer limited to one
static bool udim_atlas_has(slot_layer_t *l, i32 tile) {
	i32 own = udim_mask_tile(l);
	return util_mesh_udim_slot(tile) >= 0 && (own < 0 || own == tile);
}

// The content of the view on each tile of l, drawn into its cell of target (cleared)
static void udim_atlas_draw(slot_layer_t *l, gpu_texture_t *view, gpu_texture_t *target) {
	i32 saved  = udim_cur;
	i32 stride = util_mesh_atlas_stride();
	f32 cw     = target->width / (f32)stride;
	f32 ch     = target->height / (f32)stride;
	draw_begin(target, true, 0x00000000);
	draw_end();
	for (i32 i = 0; i < udim_table_count; ++i) {
		i32 tile = udim_table[i];
		if (!udim_atlas_has(l, tile)) {
			continue;
		}
		i32 slot = util_mesh_udim_slot(tile);
		util_udim_set_tile(tile);
		draw_begin(target, false, 0);
		draw_set_pipeline(udim_copy_pipe(target->format));
		draw_scaled_image(view, (slot % stride) * cw, (slot / stride) * ch, cw, ch);
		draw_set_pipeline(NULL);
		draw_end();
	}
	util_udim_set_tile(saved);
}

void util_udim_atlas_texture(slot_layer_t *l, gpu_texture_t *view, gpu_texture_t *target) {
	udim_atlas_draw(l, view, target);
}

slot_layer_t *util_udim_atlas_layer(slot_layer_t *l, gpu_texture_t **mask) {
	static slot_layer_t atlas;
	atlas               = *l;
	atlas.texpaint      = udim_atlas_target(&udim_atlas_a, l->texpaint);
	atlas.texpaint_nor  = udim_atlas_target(&udim_atlas_b, l->texpaint_nor);
	atlas.texpaint_pack = udim_atlas_target(&udim_atlas_c, l->texpaint_pack);
	udim_atlas_draw(l, l->texpaint, atlas.texpaint);
	udim_atlas_draw(l, l->texpaint_nor, atlas.texpaint_nor);
	udim_atlas_draw(l, l->texpaint_pack, atlas.texpaint_pack);
	slot_layer_t_array_t *masks = slot_layer_get_masks(l, true);
	if (mask != NULL && masks != NULL) {
		// The masks merged on each tile, as export_texture_run_layers() merges them on the tile of the context
		gpu_texture_t *temp   = udim_atlas_target(&udim_atlas_temp, *mask);
		gpu_texture_t *merged = udim_atlas_target(&udim_atlas_mask, *mask);
		i32            saved  = udim_cur;
		i32            stride = util_mesh_atlas_stride();
		f32            cw     = merged->width / (f32)stride;
		f32            ch     = merged->height / (f32)stride;
		draw_begin(merged, true, 0x00000000);
		draw_end();
		slot_layer_t *tl = ALLOC_INIT(slot_layer_t, {.texpaint = temp});
		for (i32 i = 0; i < udim_table_count; ++i) {
			i32 tile = udim_table[i];
			if (!udim_atlas_has(l, tile)) {
				continue;
			}
			i32 slot = util_mesh_udim_slot(tile);
			util_udim_set_tile(tile);
			draw_begin(temp, true, 0xffffffff);
			draw_end();
			for (i32 j = 0; j < masks->length; ++j) {
				layers_merge_layer(tl, masks->buffer[j], false);
			}
			draw_begin(merged, false, 0);
			draw_set_pipeline(udim_copy_pipe(merged->format));
			draw_scaled_image(temp, (slot % stride) * cw, (slot / stride) * ch, cw, ch);
			draw_set_pipeline(NULL);
			draw_end();
		}
		free(tl);
		util_udim_set_tile(saved);
		*mask = merged;
	}
	return &atlas;
}

bool util_udim_export_mask(slot_layer_t *l, char *file) {
	if (gpu_in_use || !util_udim_layer_tiled(l)) {
		return false;
	}
	char *base  = ends_with(file, ".png") ? substring(file, 0, string_length(file) - 4) : file;
	i32   saved = udim_cur;
	for (i32 i = 0; i < udim_table_count; ++i) {
		i32 tile = udim_table[i];
		if (!udim_atlas_has(l, tile)) {
			continue;
		}
		util_udim_set_tile(tile);
		iron_write_png(string("%s.%d.png", base, tile), gpu_get_texture_pixels(l->texpaint), l->texpaint->width, l->texpaint->height, 3); // RRR1
	}
	util_udim_set_tile(saved);
	return true;
}

// Raytrace (lot D2f)

static bool udim_flatten_atlas = false;

slot_layer_t *util_udim_flatten_atlas(bool height_to_normal) {
	udim_flatten_atlas = true;
	slot_layer_t *flat = layers_flatten(height_to_normal, NULL);
	udim_flatten_atlas = false;
	return flat;
}

slot_layer_t *util_udim_flatten_layer(slot_layer_t *l, gpu_texture_t **mask) {
	return udim_flatten_atlas && !gpu_in_use && util_udim_layer_tiled(l) ? util_udim_atlas_layer(l, mask) : l;
}

bool util_udim_bake_copy(char *target, char *bake) {
	render_target_t *rt  = any_map_get(render_path_render_targets, target);
	render_target_t *src = any_map_get(render_path_render_targets, bake);
	if (rt == NULL || src == NULL || udim_store_of(rt->_image) == NULL || udim_in_tile_loop()) {
		return false;
	}
	if (_render_path_current_image != NULL) {
		render_path_end();
	}
	if (gpu_in_use) {
		return false;
	}
	gpu_texture_t *view   = rt->_image;
	gpu_texture_t *from   = src->_image;
	i32            saved  = udim_cur;
	i32            stride = util_mesh_atlas_stride();
	f32            cw     = from->width / (f32)stride;
	f32            ch     = from->height / (f32)stride;
	for (i32 i = 0; i < udim_table_count; ++i) {
		i32 tile = udim_table[i];
		if (!udim_atlas_has(g_context->layer, tile)) {
			continue;
		}
		i32 slot = util_mesh_udim_slot(tile);
		util_udim_set_tile(tile);
		draw_begin(view, false, 0); // The tile saved in the undo slot of the step when first drawn to (copy on write)
		draw_set_pipeline(udim_copy_pipe(view->format));
		draw_scaled_sub_image(from, (slot % stride) * cw, (slot / stride) * ch, cw, ch, 0, 0, view->width, view->height);
		draw_set_pipeline(NULL);
		draw_end();
		udim_dirty_add(tile); // Dilated by render_path_paint_dilate()
	}
	util_udim_set_tile(saved);
	return true;
}

// .arm (lot D2e)

static void udim_free_blob(buffer_t *b) {
	if (b != NULL) {
		array_free(b);
		free(b);
	}
}

// The pixels of a texture, lz4, its readback let go: a tile out of the view would keep it
static buffer_t *udim_arm_blob(gpu_texture_t *t) {
	buffer_t *blob = lz4_encode(gpu_get_texture_pixels(t));
	array_free(t->buffer);
	free(t->buffer);
	t->buffer = NULL;
	return blob;
}

void util_udim_arm_save_layer(slot_layer_t *l, layer_data_t *d) {
	if (!util_udim_layer_tiled(l)) {
		return;
	}
	if (gpu_in_use) {
		iron_log("UDIM: layer \"%s\" saved with the tile of the context only, a pass is drawing", l->name);
		return;
	}
	gpu_texture_t             *views[3] = {l->texpaint, l->texpaint_nor, l->texpaint_pack};
	udim_store_t              *first    = udim_store_of(l->texpaint);
	layer_tile_data_t_array_t *tiles    = calloc(1, sizeof(layer_tile_data_t_array_t));
	tiles->buffer                       = calloc(UDIM_MAX_TILES, sizeof(layer_tile_data_t *));
	tiles->capacity                     = UDIM_MAX_TILES;
	for (i32 i = 0; i < udim_table_count; ++i) {
		i32 tile = udim_table[i];
		if (udim_index(first, tile) < 0) {
			continue;
		}
		layer_tile_data_t *t      = calloc(1, sizeof(layer_tile_data_t));
		buffer_t         **out[3] = {&t->texpaint, &t->texpaint_nor, &t->texpaint_pack};
		t->tile                   = tile;
		for (i32 v = 0; v < 3; ++v) {
			udim_store_t *s = udim_store_of(views[v]);
			if (s != NULL && udim_index(s, tile) >= 0) {
				udim_show(s, tile);
				*out[v] = udim_arm_blob(views[v]);
			}
		}
		tiles->buffer[tiles->length++] = t;
	}
	for (i32 v = 0; v < 3; ++v) {
		udim_store_t *s = udim_store_of(views[v]);
		if (s != NULL) {
			udim_show(s, udim_cur);
		}
	}

	// The textures read by upstream ArmorPaint: the atlas composite at the resolution of the project, or the tile of the
	// object of a layer limited to one, at its resolution (upstream holds such a layer in the space of its tile)
	buffer_t      **blobs[3]   = {&d->texpaint, &d->texpaint_nor, &d->texpaint_pack};
	gpu_texture_t **targets[3] = {&udim_atlas_a, &udim_atlas_b, &udim_atlas_c};
	i32             own        = udim_mask_tile(l);
	for (i32 v = 0; v < 3; ++v) {
		udim_store_t *s = udim_store_of(views[v]);
		if (s == NULL) {
			continue;
		}
		udim_free_blob(*blobs[v]);
		if (own >= 0 && udim_index(s, own) >= 0) {
			udim_show(s, own);
			*blobs[v] = udim_arm_blob(views[v]);
			udim_show(s, udim_cur);
		}
		else {
			gpu_texture_t *target = udim_atlas_target(targets[v], views[v]);
			udim_atlas_draw(l, views[v], target);
			*blobs[v] = udim_arm_blob(target);
		}
	}
	d->tiles = tiles;
}

void util_udim_arm_free_layer(layer_data_t *d) {
	layer_tile_data_t_array_t *tiles = d->tiles;
	if (tiles == NULL) {
		return;
	}
	for (i32 i = 0; i < tiles->length; ++i) {
		layer_tile_data_t *t = tiles->buffer[i];
		udim_free_blob(t->texpaint);
		udim_free_blob(t->texpaint_nor);
		udim_free_blob(t->texpaint_pack);
		free(t);
	}
	free(tiles->buffer);
	free(tiles);
	d->tiles = NULL;
}

i32 util_udim_tiles_size(layer_tile_data_t_array_t *tiles) {
	i32 size = 0;
	for (i32 i = 0; tiles != NULL && i < tiles->length; ++i) {
		layer_tile_data_t *t        = tiles->buffer[i];
		buffer_t          *blobs[3] = {t->texpaint, t->texpaint_nor, t->texpaint_pack};
		size += 128; // Keys and headers
		for (i32 v = 0; v < 3; ++v) {
			size += blobs[v] != NULL ? blobs[v]->length : 0;
		}
	}
	return size;
}

void util_udim_encode_tiles(layer_tile_data_t_array_t *tiles) {
	if (tiles == NULL) {
		armpack_encode_null();
		return;
	}
	armpack_encode_array(tiles->length);
	for (i32 i = 0; i < tiles->length; ++i) {
		layer_tile_data_t *t = tiles->buffer[i];
		armpack_encode_map(4);
		armpack_encode_string("tile");
		armpack_encode_i32(t->tile);
		armpack_encode_string("texpaint");
		armpack_encode_array_u8(t->texpaint);
		armpack_encode_string("texpaint_nor");
		armpack_encode_array_u8(t->texpaint_nor);
		armpack_encode_string("texpaint_pack");
		armpack_encode_array_u8(t->texpaint_pack);
	}
}

// The saved content of a texture of a layer (0 base, 1 nor, 2 pack) on a tile, NULL when not saved
static buffer_t *udim_arm_tile_blob(layer_data_t *ld, i32 tile, i32 v) {
	layer_tile_data_t_array_t *tiles = ld->tiles;
	for (i32 i = 0; tiles != NULL && i < tiles->length; ++i) {
		layer_tile_data_t *t = tiles->buffer[i];
		if (t->tile == tile) {
			return v == 0 ? t->texpaint : v == 1 ? t->texpaint_nor : t->texpaint_pack;
		}
	}
	return NULL;
}

// A tile of a view (shown) from its saved content, or, missing (a project of the atlas era), from the texture upstream
// loaded: the cell of the tile, upscaled as the atlas era exported it, or for a layer limited to a tile's object its
// texture on that tile and its background on the others
static void udim_arm_load_tile(gpu_texture_t *view, buffer_t *blob, gpu_texture_t *loaded, i32 tile, i32 own, gpu_texture_t **bg) {
	i32 w = view->width;
	i32 h = view->height;
	if (blob != NULL) {
		buffer_t      *pixels = lz4_decode(blob, w * h * gpu_texture_format_size(view->format));
		gpu_texture_t *t      = gpu_create_texture_from_bytes_raw(pixels, w, h, view->format);
		array_free(pixels);
		free(pixels);
		draw_begin(view, true, 0x00000000);
		draw_set_pipeline(g_project->is_bgra ? pipes_copy_bgra : pipes_copy); // As import_arm_run_project() loads a layer
		draw_image(t, 0, 0);
		draw_set_pipeline(NULL);
		draw_end();
		gpu_delete_texture(t);
		return;
	}
	if (own >= 0 && tile != own && *bg == NULL) {
		*bg = udim_background(loaded);
	}
	draw_begin(view, true, 0x00000000);
	draw_set_pipeline(pipes_copy);
	if (own >= 0) {
		if (tile == own) {
			draw_image(loaded, 0, 0);
		}
		else {
			draw_scaled_image(*bg, 0, 0, w, h);
		}
	}
	else if (util_mesh_udim_slot(tile) >= 0) { // As layers_udim_remap() cut a tile from the atlas
		i32 slot   = util_mesh_udim_slot(tile);
		i32 stride = util_mesh_atlas_stride();
		f32 cx     = (slot % stride) / (f32)stride;
		f32 cy     = (slot / stride) / (f32)stride;
		draw_scaled_sub_image(loaded, cx * w, cy * h, w / (f32)stride, h / (f32)stride, 0, 0, w, h);
	}
	draw_set_pipeline(NULL);
	draw_end();
}

static void udim_arm_load_layer(slot_layer_t *l, layer_data_t *ld) {
	gpu_texture_t *views[3]  = {l->texpaint, l->texpaint_nor, l->texpaint_pack};
	gpu_texture_t *loaded[3] = {NULL, NULL, NULL};
	for (i32 v = 0; v < 3; ++v) {
		bool missing = false;
		for (i32 i = 0; i < udim_table_count; ++i) {
			missing = missing || udim_arm_tile_blob(ld, udim_table[i], v) == NULL;
		}
		if (views[v] != NULL && missing) { // Kept aside: the view goes to a tile, drawn over
			loaded[v] = gpu_create_render_target(views[v]->width, views[v]->height, views[v]->format);
			draw_begin(loaded[v], false, 0);
			draw_set_pipeline(udim_copy_pipe(views[v]->format));
			draw_image(views[v], 0, 0);
			draw_set_pipeline(NULL);
			draw_end();
		}
	}
	util_udim_tile_layer(l, false);
	i32 own = udim_mask_tile(l);
	for (i32 v = 0; v < 3; ++v) {
		udim_store_t  *s  = udim_store_of(views[v]);
		gpu_texture_t *bg = NULL;
		for (i32 k = 0; s != NULL && k < s->count; ++k) {
			udim_show(s, s->ids[k]);
			udim_arm_load_tile(views[v], udim_arm_tile_blob(ld, s->ids[k], v), loaded[v], s->ids[k], own, &bg);
		}
		if (s != NULL) {
			udim_show(s, udim_cur);
		}
		if (bg != NULL) {
			gpu_delete_texture(bg);
		}
		if (loaded[v] != NULL) {
			gpu_delete_texture(loaded[v]);
		}
	}
}

void util_udim_arm_load(layer_data_t_array_t *datas) {
	if (datas == NULL) {
		return;
	}
	if (gpu_in_use) {
		iron_log("UDIM: project loaded with its layers on the tile of the context, a pass is drawing");
		return;
	}
	udim_update_table();
	if (udim_table_count < 2) {
		return;
	}
	slot_layer_t_array_t *layers   = g_project->_->layers;
	i32                   migrated = 0;
	for (i32 i = 0; i < layers->length && i < datas->length; ++i) {
		slot_layer_t *l = layers->buffer[i];
		if (slot_layer_is_group(l) || util_udim_layer_tiled(l) || !util_udim_layer_should_tile(l)) {
			continue;
		}
		migrated += datas->buffer[i]->tiles == NULL ? 1 : 0;
		udim_arm_load_layer(l, datas->buffer[i]);
	}
	if (migrated > 0) {
		iron_log("UDIM: %d layers of the atlas era cut into their tiles", migrated);
	}
}

char *util_udim_stats() {
	i32 partial  = 0;
	i32 contents = 0;
	for (i32 i = 0; i < udim_store_count; ++i) {
		partial += udim_stores[i]->partial ? 1 : 0;
		contents += udim_stores[i]->count;
	}
	char *table = "";
	for (i32 i = 0; i < udim_table_count; ++i) {
		table = string("%s%s%d", table, i > 0 ? "," : "", udim_table[i]);
	}
	return string("tile=%d table=%s stores=%d partial=%d contents=%d", udim_cur, table, udim_store_count, partial, contents);
}

char *util_udim_view_stats(gpu_texture_t *view) {
	udim_store_t *s = udim_store_of(view);
	if (s == NULL) {
		return view == NULL ? "null" : string("single %dx%d", view->width, view->height);
	}
	char *ids = "";
	for (i32 i = 0; i < s->count; ++i) {
		ids = string("%s%s%d", ids, i > 0 ? "," : "", s->ids[i]);
	}
	return string("cur=%d partial=%d tiles=%s %dx%d", s->cur, s->partial ? 1 : 0, ids, view->width, view->height);
}
