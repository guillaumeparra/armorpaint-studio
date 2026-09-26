#pragma once

#include "iron_armpack.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct raw_mesh {
	struct i16_array *posa;
	struct i16_array *nora;
	struct i16_array *texa;
	struct i16_array *texa1;
	struct i16_array *cola;
	struct u32_array *inda;
	int               vertex_count;
	int               index_count;
	float             scale_pos;
	float             scale_tex;
	char             *name;
	bool              has_next; // File contains multiple objects
	uint64_t          pos;
	struct any_array *udims;   // u32_array_t[] - Indices split per udim tile
	int               udims_u; // Number of horizontal udim tiles
	int               udims_v;
	void             *vertex_arrays; // vertex_array_t[]
	struct u32_array *index_array;
	struct u8_array  *blob; // Source file bytes
} raw_mesh_t;

extern bool obj_parse_y_to_z_up;
// STUDIO-PATCH(udim-import): udim split of the last obj_parse call, see paint/plugins/external/PATCHES.md
extern int   obj_udim_invalid;       // Triangles left out: a uv out of the tiles 1001-1990 (negative, u > 10, v > 99, nan)
extern float obj_udim_invalid_uv[2]; // The first of these uvs
extern int   obj_udim_cut;           // Triangles cut along a tile border

raw_mesh_t *obj_parse(buffer_t *file_bytes, char split_code, uint64_t start_pos, bool udim);
void        obj_destroy(raw_mesh_t *part);
