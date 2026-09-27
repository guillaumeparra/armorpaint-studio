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
bool  util_udim_clear(gpu_texture_t *view, u32 color);       // Every tile of a tiled texture cleared, false when not tiled
bool  util_udim_tile_like(gpu_texture_t *view, gpu_texture_t *from); // view made from a tiled texture: tiled as it, each tile copied
// A node preview baked on the mesh (Curvature Texture): a tile per tile of the layer. True when the layer is tiled, the
// tile of the context then held; false otherwise, the preview a single texture again.
bool  util_udim_tile_preview(gpu_texture_t *image);
bool  util_udim_swap_layers(slot_layer_t *a, slot_layer_t *b); // slot_layer_swap of a tiled layer and an undo slot
bool  util_udim_copy_to_undo(i32 from_id, i32 to_id, bool is_mask); // history_copy_to_undo of a tiled layer
void  util_udim_undo_reset(slot_layer_t *slot);              // Undo slot reused: back to a single texture
void  util_udim_draw_meshes(char *context);                  // render_path_draw_meshes, each object with its tile
char *util_udim_stats();                                     // Stores and their tiles, for the benches
char *util_udim_view_stats(gpu_texture_t *view);

// Loop over the tiles of a layer operation: n = util_udim_loop_begin(tiled) gives the tiles to go through (1 when not
// tiled, or inside another loop over the tiles), util_udim_loop_tile(k) sets the k-th, util_udim_loop_end() puts back
// the tile of the context.
i32  util_udim_loop_begin(bool tiled);
void util_udim_loop_tile(i32 k);
void util_udim_loop_end();

// Paint (lot D2b). render_path_paint_commands_paint() paints a tiled layer tile by tile, each with the objects of the
// tile in their local uvs: util_udim_paint_tiles() sets each tile and calls it back. A paint step (history_paint) saves
// the tile of the context in its undo slot, then each tile of the layer when first drawn to (copy on write, from
// gpu_begin): its slot holds the tiles the step changed, undo and redo swap them. texpaint_blend0/1 get a content per
// tile painted, freed at the end of the stroke; the dilation goes through the tiles painted, each with its dilate map.
bool util_udim_paint_undo(slot_layer_t *layer, i32 to_id, bool is_mask); // history_paint() of a tiled layer
bool util_udim_paint_tiles(bool dilation);       // Painted tile by tile, false for the upstream path
void util_udim_blend_reset();                    // Before texpaint_blend0/1 are cleared
bool util_udim_dilate(bool base, bool nor_pack); // render_path_paint_dilate() outside the loop, of the tiles painted
bool util_udim_cache_dilate_map();               // util_uv_cache_dilate_map(): the map of the tile of the context
bool util_udim_cache_triangle_map();             // util_uv_cache_triangle_map(): the same
bool util_udim_cache_uv_map();                   // util_uv_cache_uv_map(): every tile, the viewport wireframe reads them all

// Pickers (lot D2b). The gbuffer writes the tile of each object (gbuffer2.r, tile - 1000, 0 without one), read under the
// brush: the layer and mask pickers run on that tile, the uv islands are those of that tile, the face and uv island
// fills paint that tile only.
f32  util_udim_tile_code(object_t *object); // _udim_tile_code
i32  util_udim_cursor_tile();               // Tile under the brush, -1 on an object without one or the 2d view
i32  util_udim_pick_begin(bool show);       // Before a picker pass: tile under the brush picked, shown when show; tile to put back
i32  util_udim_picked_tile();               // Tile of the last pick
i32  util_udim_island_begin();              // util_uv_cache_uv_island_map(): the tile under the brush shown; tile to put back

// Layer operations (lot D2c). Merge down, masks applied and group merge go through layers_merge_layer() and
// layers_apply_mask(): on a tiled layer, each called back on every tile. layers_flatten() flattens the tile of the
// context, the tiled layers read there; its callers go through the tiles they need.
bool util_udim_merge_layer(slot_layer_t *l0, slot_layer_t *l1, bool use_mask); // layers_merge_layer() into a tiled l0
bool util_udim_apply_mask(slot_layer_t *l, slot_layer_t *m);                    // layers_apply_mask() on a tiled l
bool util_udim_draw_mesh_materials(char *context);                               // Flatten: mesh material overrides of the tile

// 2d view (lot D2b): a tiled layer shows and paints the tile of the context, the 2d plane with the material uvs of that
// tile. util_udim_view2d_tile() draws the combo choosing it, false when the layer is not tiled.
bool util_udim_view2d_tile();

// util_mesh.c: the tile table, sorted, and its length (1 for a single tile, udim active from 2)
i32 util_mesh_udim_tile_table(i32 **tiles);
