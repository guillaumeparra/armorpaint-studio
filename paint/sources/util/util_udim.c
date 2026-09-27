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

void util_udim_draw_paint_meshes() {
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
