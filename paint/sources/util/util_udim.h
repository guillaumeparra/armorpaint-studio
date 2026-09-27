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
