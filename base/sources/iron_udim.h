#pragma once

// STUDIO-PATCH(udim-import): udim tiles of the mesh importers (iron_obj.c, plugins/io_fbx), see
// paint/plugins/external/PATCHES.md
//
// Tile (u, v) covers the uvs [u, u + 1] x [v, v + 1], its id is 1001 + u + 10 * v. The importers accept u in 0-9 and
// v in 0-98 (ids 1001-1990, the ".1xxx" names read by util_mesh_udim_tile). A uv on a tile border, within UDIM_EPS,
// belongs to the tile of its triangle. A triangle over several tiles is cut along the borders, one part per tile.

#include <stdbool.h>

#define UDIM_TILES_U   10
#define UDIM_TILES_V   99
#define UDIM_EPS       1e-4f
#define UDIM_MAX_TILES 64 // Atlas slots of paint/sources/util/util_mesh.c (ATLAS_MAX_SLOTS)

typedef struct udim_vert {
	float pos[3];
	float nor[3];
	float uv[2];
	float uv1[2];
	float col[4];
} udim_vert_t;

// Receives one triangle of the split, its uvs local to tile (tu, tv), in [0, 1]
typedef void (*udim_emit_t)(int tu, int tv, const udim_vert_t tri[3], void *user);

int  udim_tile_id(int tu, int tv);
bool udim_uv_valid(float u, float v);
// Range of tiles covered by the triangle uvs, false when one of them is not valid
bool udim_triangle_tiles(const float *uv0, const float *uv1, const float *uv2, int *tu0, int *tv0, int *tu1, int *tv1);
// Cuts the triangle along the tile borders and emits its parts, normals renormalized. Returns the number of tiles it
// was emitted in (1: not cut), 0 when a uv is not valid (nothing emitted).
int udim_split_triangle(const udim_vert_t tri[3], udim_emit_t emit, void *user);
