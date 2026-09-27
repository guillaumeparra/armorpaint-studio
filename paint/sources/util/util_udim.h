#pragma once

// STUDIO-PATCH(udim-core): UDIM of lot D1, see paint/plugins/external/PATCHES.md and ARMORPAINT_UDIM.md
//
// A UDIM mesh keeps one paint object per tile, named "<object>.<tile>" (body.1001, body.1002...), as the upstream
// format has it: the .arm stays readable by upstream ArmorPaint and by older studio builds. The shared layers hold every
// tile in its own cell of the layer atlas (util_mesh.c).

// Material uvs of the udim layers: those of the DCC, tex + (tile u, -tile v) with the v flip of the stored uvs, continuous
// from a tile to its neighbours. util_udim_tile_uv() gives (tile u, -tile v) of a tile object, for the layers of a single
// tile (tex + tile uv); util_udim_offset() the same relative to its atlas cell, for the shared layers (atlas_uv * stride
// + offset). Both zero for the merged mesh, the 2d plane and the objects without a tile, util_udim_offset() also for the
// tiles without a cell.
vec2_t util_udim_tile_uv(object_t *object);
vec2_t util_udim_offset(object_t *object);

// Draws the "paint" context. The udim layers are painted object by object, each object with its offset, instead of
// through the merged mesh; the merged mesh stays for the uv, triangle and dilate maps, the raytracer, the bake and the
// pickers, whose shaders read the layers in atlas space.
void util_udim_draw_paint_meshes();

// util_mesh.c: the objects of the merged mesh
mesh_object_t_array_t *util_mesh_merged_scope();

// Tile context (lot D2). Each tiled layer texture is a stable view (the gpu_texture_t pointer held by the layer, its
// render target and the undo swaps) whose content, the gpu_texture_t struct itself, is that of the tile of the context.
// util_udim_set_tile() puts the content of a tile in every view; the others wait in the store of the view, keyed by
// tile id. Code written for a single texture works on the tile of the context; the layer operations loop over the tiles.
// Layers and masks are tiled when the udim table is active, on their udim uvs (uv map 0), unless limited to an object
// without a tile; they hold every tile of the table. Undo slots hold the tiles they saved.
// The functions doing gpu work run outside a render pass (!gpu_in_use), they do nothing otherwise.
#define UDIM_MAX_TILES 64

void  util_udim_set_tile(i32 tile); // -1: no tile, the tiled views show an empty 1x1 texture (objects without a tile)
i32   util_udim_tile();
i32   util_udim_tile_count();       // Tiles of the table, 0 when udim is off
i32   util_udim_tile_at(i32 i);
bool  util_udim_layer_tiled(slot_layer_t *l);
bool  util_udim_layer_should_tile(slot_layer_t *l);
void  util_udim_sync();             // Layers tiled or not by the rule, stores following the table
bool  util_udim_tile_layer(slot_layer_t *l, bool background); // Tiled by the rule now, the new tiles cleared or given texel (0, 0)
void  util_udim_replace(gpu_texture_t *view, gpu_texture_t *fresh); // Content of fresh into the view, the old one deleted
bool  util_udim_resize_layer(slot_layer_t *l, i32 width, i32 height); // Every tile of a tiled layer, in place
bool  util_udim_swap_layers(slot_layer_t *a, slot_layer_t *b); // slot_layer_swap of a tiled layer and an undo slot
bool  util_udim_copy_to_undo(i32 from_id, i32 to_id, bool is_mask); // history_copy_to_undo of a tiled layer
void  util_udim_undo_reset(slot_layer_t *slot);              // Undo slot reused: back to a single texture
void  util_udim_draw_meshes(char *context);                  // render_path_draw_meshes, each object with its tile
char *util_udim_stats();                                     // Stores and their tiles, for the benches
char *util_udim_view_stats(gpu_texture_t *view);

// Loop over the tiles of a layer operation: n = util_udim_loop_begin(tiled) gives the tiles to go through (1 when not
// tiled), util_udim_loop_tile(k) sets the k-th, util_udim_loop_end() puts back the tile of the context.
i32  util_udim_loop_begin(bool tiled);
void util_udim_loop_tile(i32 k);
void util_udim_loop_end();

// util_mesh.c: the tile table, sorted, and its length (1 for a single tile, udim active from 2)
i32 util_mesh_udim_tile_table(i32 **tiles);
