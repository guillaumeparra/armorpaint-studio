#include "iron_array.h"
#include "iron_obj.h"
// STUDIO-PATCH(fbx-split)
#include "iron_udim.h"
#include "ufbx/ufbx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool         has_next          = false;
static int          current_node      = 0;
static float        scale_pos         = 1.0;
static ufbx_scene  *active_base_scene = NULL;
static ufbx_scene  *active_eval_scene = NULL;
static bool         active_tex1       = false;
static raw_mesh_t **mat_split_meshes  = NULL;
static int          mat_split_count   = 0;
static int          mat_split_idx     = 0;
extern int          plugins_split_by;
void                console_error(char *s); // STUDIO-PATCH(fbx-split)
void                console_info(char *s);
void                console_log(char *s);

static bool valid_mesh(ufbx_node *n) {
	return n->mesh != NULL && n->mesh->num_triangles > 0;
}

static bool mixed_uvs(ufbx_scene *scene) {
	bool found_tex    = false;
	bool found_no_tex = false;
	for (int i = 0; i < (int)scene->nodes.count; ++i) {
		ufbx_node *n = scene->nodes.data[i];
		if (valid_mesh(n)) {
			if (n->mesh->vertex_uv.exists) {
				found_tex = true;
			}
			else {
				found_no_tex = true;
			}
		}
	}
	return found_tex && found_no_tex;
}

static bool mixed_cols(ufbx_scene *scene) {
	bool found_col    = false;
	bool found_no_col = false;
	for (int i = 0; i < (int)scene->nodes.count; ++i) {
		ufbx_node *n = scene->nodes.data[i];
		if (valid_mesh(n)) {
			if (n->mesh->vertex_color.exists) {
				found_col = true;
			}
			else {
				found_no_col = true;
			}
		}
	}
	return found_col && found_no_col;
}

typedef struct {
	float *data;
	int    count;
	int    cap;
} fbuf_t;

static void fbuf_push(fbuf_t *b, float v) {
	if (b->count >= b->cap) {
		b->cap  = b->cap ? b->cap * 2 : 256;
		b->data = realloc(b->data, b->cap * sizeof(float));
	}
	b->data[b->count++] = v;
}

// STUDIO-PATCH(fbx-split): meshes of the material and udim splits, grown on demand. Upstream capped them at 64
// materials and 100 tiles and dropped the faces beyond without a message, see paint/plugins/external/PATCHES.md
typedef struct {
	char  *name;
	fbuf_t pf;
	fbuf_t nf;
	fbuf_t tf;
	fbuf_t t1f;
	fbuf_t cf;
} split_t;

static split_t *splits      = NULL;
static int      split_count = 0;
static int      split_cap   = 0;

// Takes the name
static int split_add(char *name) {
	if (split_count == split_cap) {
		split_cap = split_cap > 0 ? split_cap * 2 : 16;
		splits    = realloc(splits, sizeof(split_t) * split_cap);
	}
	splits[split_count] = (split_t){.name = name};
	return split_count++;
}

static char *split_name(const char *name, int tile) {
	size_t len = strlen(name) + 8;
	char  *s   = malloc(len);
	if (tile > 0) {
		snprintf(s, len, "%s.%d", name, tile);
	}
	else {
		snprintf(s, len, "%s", name);
	}
	return s;
}

static raw_mesh_t *mat_build_raw(char *name, fbuf_t *pf, fbuf_t *nf, fbuf_t *tf, fbuf_t *t1f, fbuf_t *cf) {
	int   vertex_count = pf->count / 3;
	float sp           = 0.0f;
	for (int i = 0; i < pf->count; ++i) {
		float f = fabsf(pf->data[i]);
		if (f > sp)
			sp = f;
	}
	if (sp == 0.0f)
		sp = 1.0f;
	float inv = 1.0f / sp;

	short *posa = malloc(sizeof(short) * vertex_count * 4);
	short *nora = malloc(sizeof(short) * vertex_count * 2);
	for (int i = 0; i < vertex_count; ++i) {
		posa[i * 4]     = pf->data[i * 3] * 32767.0f * inv;
		posa[i * 4 + 1] = pf->data[i * 3 + 1] * 32767.0f * inv;
		posa[i * 4 + 2] = pf->data[i * 3 + 2] * 32767.0f * inv;
		nora[i * 2]     = nf->data[i * 3] * 32767.0f;
		nora[i * 2 + 1] = nf->data[i * 3 + 1] * 32767.0f;
		posa[i * 4 + 3] = nf->data[i * 3 + 2] * 32767.0f;
	}

	short *texa = NULL;
	if (tf->count > 0) {
		texa = malloc(sizeof(short) * vertex_count * 2);
		for (int i = 0; i < vertex_count; ++i) {
			texa[i * 2]     = tf->data[i * 2] * 32767.0f;
			texa[i * 2 + 1] = (1.0f - tf->data[i * 2 + 1]) * 32767.0f;
		}
	}

	short *texa1 = NULL; // STUDIO-PATCH(fbx-split): second uv set and vertex colors, as the object split
	if (t1f->count > 0) {
		texa1 = malloc(sizeof(short) * vertex_count * 2);
		for (int i = 0; i < vertex_count; ++i) {
			texa1[i * 2]     = t1f->data[i * 2] * 32767.0f;
			texa1[i * 2 + 1] = (1.0f - t1f->data[i * 2 + 1]) * 32767.0f;
		}
	}
	short *cola = NULL;
	if (cf->count > 0) {
		cola = malloc(sizeof(short) * vertex_count * 4);
		for (int i = 0; i < vertex_count * 4; ++i) {
			cola[i] = cf->data[i] * 32767.0f;
		}
	}

	uint32_t *inda = malloc(sizeof(uint32_t) * vertex_count);
	for (int i = 0; i < vertex_count; ++i)
		inda[i] = i;

	raw_mesh_t *raw = calloc(sizeof(raw_mesh_t), 1);
	raw->name       = malloc(strlen(name) + 1);
	strcpy(raw->name, name);

	raw->posa         = malloc(sizeof(i16_array_t));
	raw->posa->buffer = posa;
	raw->posa->length = raw->posa->capacity = vertex_count * 4;

	raw->nora         = malloc(sizeof(i16_array_t));
	raw->nora->buffer = nora;
	raw->nora->length = raw->nora->capacity = vertex_count * 2;

	if (texa) {
		raw->texa         = malloc(sizeof(i16_array_t));
		raw->texa->buffer = texa;
		raw->texa->length = raw->texa->capacity = vertex_count * 2;
	}

	if (texa1) {
		raw->texa1         = malloc(sizeof(i16_array_t));
		raw->texa1->buffer = texa1;
		raw->texa1->length = raw->texa1->capacity = vertex_count * 2;
	}

	if (cola) {
		raw->cola         = malloc(sizeof(i16_array_t));
		raw->cola->buffer = cola;
		raw->cola->length = raw->cola->capacity = vertex_count * 4;
	}

	raw->inda         = malloc(sizeof(u32_array_t));
	raw->inda->buffer = inda;
	raw->inda->length = raw->inda->capacity = vertex_count;

	raw->scale_pos = sp;
	raw->scale_tex = 1.0f;
	return raw;
}

// The split meshes, handed one by one by io_fbx_parse
static void split_finish() {
	mat_split_meshes = malloc(sizeof(raw_mesh_t *) * (split_count > 0 ? split_count : 1));
	mat_split_count  = split_count;
	for (int i = 0; i < split_count; ++i) {
		split_t *s          = &splits[i];
		mat_split_meshes[i] = mat_build_raw(s->name, &s->pf, &s->nf, &s->tf, &s->t1f, &s->cf);
	}
}

static void split_free() {
	for (int i = 0; i < split_count; ++i) {
		split_t *s = &splits[i];
		free(s->name);
		free(s->pf.data);
		free(s->nf.data);
		free(s->tf.data);
		free(s->t1f.data);
		free(s->cf.data);
	}
	free(splits);
	splits      = NULL;
	split_count = 0;
	split_cap   = 0;
}

static void build_material_split(ufbx_scene *scene) {
	bool force_tex = mixed_uvs(scene); // STUDIO-PATCH(fbx-split): uvs for all the faces or none, tf was read past its end

	uint32_t max_tris = 1;
	for (size_t ni = 0; ni < scene->nodes.count; ++ni) {
		ufbx_node *n = scene->nodes.data[ni];
		if (n->mesh && n->mesh->max_face_triangles > max_tris)
			max_tris = n->mesh->max_face_triangles;
	}
	uint32_t *indices = malloc(sizeof(uint32_t) * max_tris * 3);

	for (size_t ni = 0; ni < scene->nodes.count; ++ni) {
		ufbx_node *n = scene->nodes.data[ni];
		if (!n->mesh)
			continue;
		ufbx_mesh  *mesh       = n->mesh;
		ufbx_matrix normal_mat = ufbx_get_compatible_matrix_for_normals(n);
		bool        has_tex    = mesh->vertex_uv.exists;

		for (size_t fi = 0; fi < mesh->faces.count; ++fi) {
			ufbx_face face = mesh->faces.data[fi];

			uint32_t    midx     = (mesh->face_material.count > 0) ? mesh->face_material.data[fi] : 0;
			const char *mat_name = (midx < n->materials.count && n->materials.data[midx]) ? n->materials.data[midx]->name.data : n->name.data;

			int m = -1;
			for (int k = 0; k < split_count; ++k) {
				if (strcmp(splits[k].name, mat_name) == 0) {
					m = k;
					break;
				}
			}
			if (m == -1) {
				m = split_add(split_name(mat_name, 0));
			}
			split_t *s = &splits[m];

			uint32_t num_triangles = ufbx_triangulate_face(indices, max_tris * 3, mesh, face);
			for (uint32_t v_ix = 0; v_ix < num_triangles * 3; ++v_ix) {
				uint32_t  a = indices[v_ix];
				ufbx_vec3 v = ufbx_transform_position(&n->geometry_to_world, ufbx_get_vertex_vec3(&mesh->vertex_position, a));
				fbuf_push(&s->pf, v.x);
				fbuf_push(&s->pf, -v.z);
				fbuf_push(&s->pf, v.y);
				ufbx_vec3 nv = ufbx_transform_direction(&normal_mat, ufbx_get_vertex_vec3(&mesh->vertex_normal, a));
				fbuf_push(&s->nf, nv.x);
				fbuf_push(&s->nf, -nv.z);
				fbuf_push(&s->nf, nv.y);
				if (has_tex) {
					ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, a);
					fbuf_push(&s->tf, uv.x);
					fbuf_push(&s->tf, uv.y);
				}
				else if (force_tex) {
					fbuf_push(&s->tf, 0.0f);
					fbuf_push(&s->tf, 0.0f);
				}
			}
		}
	}

	free(indices);
	split_finish();
	split_free();
}

// STUDIO-PATCH(fbx-split): one mesh per node and tile, named <node>.<tile>, instead of all the nodes merged per tile.
// Triangles over several tiles are cut along the borders (iron_udim.c); upstream took the tile of the uv barycenter
// cast with (int) (u = -0.5 in 1001, u >= 10 in the next row) and stretched the triangle into it. A uv out of the tiles
// 1001-1990, or more tiles than the layer atlas holds, refuses the file with a message. Nodes without uvs keep their
// own mesh, without a tile.
typedef struct {
	int         tiles[UDIM_TILES_U * UDIM_TILES_V]; // Split of each tile for the current node, -1 if none yet
	bool       *used;                               // Tiles of the file
	int        *used_count;
	const char *name;
	bool        tex1;
	bool        col;
} udim_node_t;

static void udim_push(split_t *s, const udim_vert_t *v, bool uv, bool tex1, bool col) {
	for (int k = 0; k < 3; ++k) {
		fbuf_push(&s->pf, v->pos[k]);
		fbuf_push(&s->nf, v->nor[k]);
	}
	if (uv) {
		fbuf_push(&s->tf, v->uv[0]);
		fbuf_push(&s->tf, v->uv[1]);
	}
	if (tex1) {
		fbuf_push(&s->t1f, v->uv1[0]);
		fbuf_push(&s->t1f, v->uv1[1]);
	}
	for (int k = 0; col && k < 4; ++k) {
		fbuf_push(&s->cf, v->col[k]);
	}
}

static void udim_emit(int tu, int tv, const udim_vert_t tri[3], void *user) {
	udim_node_t *u    = user;
	int          tile = tu + tv * UDIM_TILES_U;
	if (u->tiles[tile] < 0) {
		u->tiles[tile] = split_add(split_name(u->name, udim_tile_id(tu, tv)));
		if (!u->used[tile]) {
			u->used[tile] = true;
			(*u->used_count)++;
		}
	}
	for (int k = 0; k < 3; ++k) {
		udim_push(&splits[u->tiles[tile]], &tri[k], true, u->tex1, u->col);
	}
}

static void build_udim_split(ufbx_scene *scene) {
	bool any_col = false;
	for (size_t ni = 0; ni < scene->nodes.count; ++ni) {
		ufbx_node *n = scene->nodes.data[ni];
		any_col      = any_col || (valid_mesh(n) && n->mesh->vertex_color.exists);
	}

	uint32_t max_tris = 1;
	for (size_t ni = 0; ni < scene->nodes.count; ++ni) {
		ufbx_node *n = scene->nodes.data[ni];
		if (n->mesh && n->mesh->max_face_triangles > max_tris)
			max_tris = n->mesh->max_face_triangles;
	}
	uint32_t *indices = malloc(sizeof(uint32_t) * max_tris * 3);

	bool  used[UDIM_TILES_U * UDIM_TILES_V] = {0};
	int   tiles                             = 0;
	int   cut                               = 0;
	int   invalid                           = 0;
	float invalid_uv[2]                     = {0};
	char *invalid_name                      = NULL;
	int   no_uvs                            = 0;
	char *no_uvs_name                       = NULL;

	// The nodes with uvs first: a first mesh without uvs makes ArmorPaint offer to unwrap the whole mesh, udim uvs included
	for (size_t step = 0; step < scene->nodes.count * 2; ++step) {
		ufbx_node *n = scene->nodes.data[step % scene->nodes.count];
		if (!valid_mesh(n) || n->mesh->vertex_uv.exists != (step < scene->nodes.count))
			continue;
		ufbx_mesh  *mesh       = n->mesh;
		ufbx_matrix normal_mat = ufbx_get_compatible_matrix_for_normals(n);
		bool        has_tex    = mesh->vertex_uv.exists;
		bool        has_col    = mesh->vertex_color.exists;
		udim_node_t u          = {.used = used, .used_count = &tiles, .name = n->name.length > 0 ? n->name.data : "Mesh", .tex1 = active_tex1, .col = any_col};
		memset(u.tiles, -1, sizeof(u.tiles));
		int no_uv = -1;
		if (!has_tex) {
			no_uv = split_add(split_name(u.name, 0));
			if (no_uvs++ == 0) {
				no_uvs_name = split_name(u.name, 0);
			}
		}

		for (size_t fi = 0; fi < mesh->faces.count; ++fi) {
			uint32_t num_triangles = ufbx_triangulate_face(indices, max_tris * 3, mesh, mesh->faces.data[fi]);
			for (uint32_t t = 0; t < num_triangles; ++t) {
				udim_vert_t tri[3] = {0};
				for (int k = 0; k < 3; ++k) {
					uint32_t     a  = indices[t * 3 + k];
					udim_vert_t *vt = &tri[k];
					ufbx_vec3    v  = ufbx_transform_position(&n->geometry_to_world, ufbx_get_vertex_vec3(&mesh->vertex_position, a));
					vt->pos[0]      = v.x;
					vt->pos[1]      = -v.z;
					vt->pos[2]      = v.y;
					v               = ufbx_transform_direction(&normal_mat, ufbx_get_vertex_vec3(&mesh->vertex_normal, a));
					float l         = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z); // Unit length, as the cut ones
					l               = l > 0.0f ? 1.0f / l : 0.0f;
					vt->nor[0]      = v.x * l;
					vt->nor[1]      = -v.z * l;
					vt->nor[2]      = v.y * l;
					if (has_tex) {
						ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, a);
						vt->uv[0]    = uv.x;
						vt->uv[1]    = uv.y;
					}
					if (u.tex1) {
						ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->uv_sets.data[1].vertex_uv, a);
						vt->uv1[0]   = uv.x;
						vt->uv1[1]   = uv.y;
					}
					if (has_col) {
						ufbx_vec4 c = ufbx_get_vertex_vec4(&mesh->vertex_color, a);
						vt->col[0]  = c.x;
						vt->col[1]  = c.y;
						vt->col[2]  = c.z;
						vt->col[3]  = c.w;
					}
				}
				if (!has_tex) {
					for (int k = 0; k < 3; ++k) {
						udim_push(&splits[no_uv], &tri[k], false, u.tex1, any_col);
					}
					continue;
				}
				int r = udim_split_triangle(tri, udim_emit, &u);
				if (r == 0 && invalid++ == 0) {
					int k         = udim_uv_valid(tri[0].uv[0], tri[0].uv[1]) ? udim_uv_valid(tri[1].uv[0], tri[1].uv[1]) ? 2 : 1 : 0;
					invalid_uv[0] = tri[k].uv[0];
					invalid_uv[1] = tri[k].uv[1];
					invalid_name  = split_name(u.name, 0);
				}
				cut += r > 1 ? 1 : 0;
			}
		}
	}

	free(indices);

	char msg[512];
	if (invalid > 0) {
		snprintf(msg, sizeof(msg), "UDIM import refused: uvs outside the tiles 1001-1990 in \"%s\" (%d triangles, first uv %g %g).", invalid_name, invalid,
		         invalid_uv[0], invalid_uv[1]);
		console_error(msg);
	}
	else if (tiles > UDIM_MAX_TILES) {
		snprintf(msg, sizeof(msg), "UDIM import refused: %d tiles, the layers hold %d at most.", tiles, UDIM_MAX_TILES);
		console_error(msg);
	}
	else {
		split_finish();
		snprintf(msg, sizeof(msg), "UDIM import: %d objects in %d tiles, %d triangles cut along the tile borders.", split_count, tiles, cut);
		console_log(msg);
		if (no_uvs > 0) {
			snprintf(msg, sizeof(msg), "UDIM import: %d objects without uvs imported without a tile, first \"%s\".", no_uvs, no_uvs_name);
			console_info(msg);
		}
	}
	split_free();
	free(invalid_name);
	free(no_uvs_name);
}

void io_fbx_parse_mesh(raw_mesh_t *raw, ufbx_mesh *mesh, ufbx_matrix *to_world, ufbx_matrix *to_world_unscaled, bool force_tex, bool force_col) {
	uint32_t  indices_size = mesh->max_face_triangles * 3;
	uint32_t *indices      = (uint32_t *)malloc(sizeof(uint32_t) * indices_size);

	bool has_tex  = mesh->vertex_uv.exists;
	bool has_tex1 = active_tex1 && mesh->uv_sets.count > 1;
	bool has_col  = mesh->vertex_color.exists;

	int    numtri  = mesh->num_triangles;
	float *posa32  = (float *)malloc(sizeof(float) * numtri * 3 * 3);
	float *nora32  = (float *)malloc(sizeof(float) * numtri * 3 * 3);
	float *texa32  = (has_tex || force_tex) ? (float *)malloc(sizeof(float) * numtri * 3 * 2) : NULL;
	float *texa132 = has_tex1 ? (float *)malloc(sizeof(float) * numtri * 3 * 2) : NULL;
	float *cola32  = (has_col || force_col) ? (float *)malloc(sizeof(float) * numtri * 3 * 4) : NULL;
	int    pi      = 0;
	int    ni      = 0;
	int    ti      = 0;
	int    ti1     = 0;
	int    ci      = 0;

	for (int j = 0; j < (int)mesh->faces.count; ++j) {
		ufbx_face face          = mesh->faces.data[j];
		uint32_t  num_triangles = ufbx_triangulate_face(indices, indices_size, mesh, face);
		for (uint32_t v_ix = 0; v_ix < num_triangles * 3; v_ix++) {
			uint32_t a = indices[v_ix];

			ufbx_vec3 v = ufbx_transform_position(to_world, ufbx_get_vertex_vec3(&mesh->vertex_position, a));
			// posa32[pi++] = v.x;
			// posa32[pi++] = v.y;
			// posa32[pi++] = v.z;
			posa32[pi++] = v.x;
			posa32[pi++] = -v.z;
			posa32[pi++] = v.y;

			v = ufbx_transform_direction(to_world_unscaled, ufbx_get_vertex_vec3(&mesh->vertex_normal, a));
			// nora32[ni++] = v.x;
			// nora32[ni++] = v.y;
			// nora32[ni++] = v.z;
			nora32[ni++] = v.x;
			nora32[ni++] = -v.z;
			nora32[ni++] = v.y;

			if (has_tex) {
				texa32[ti++] = ufbx_get_vertex_vec2(&mesh->vertex_uv, a).x;
				texa32[ti++] = ufbx_get_vertex_vec2(&mesh->vertex_uv, a).y;
			}
			else if (force_tex) {
				texa32[ti++] = 0;
				texa32[ti++] = 0;
			}

			if (has_tex1) {
				texa132[ti1++] = ufbx_get_vertex_vec2(&mesh->uv_sets.data[1].vertex_uv, a).x;
				texa132[ti1++] = ufbx_get_vertex_vec2(&mesh->uv_sets.data[1].vertex_uv, a).y;
			}

			if (has_col) {
				cola32[ci++] = ufbx_get_vertex_vec4(&mesh->vertex_color, a).x;
				cola32[ci++] = ufbx_get_vertex_vec4(&mesh->vertex_color, a).y;
				cola32[ci++] = ufbx_get_vertex_vec4(&mesh->vertex_color, a).z;
				cola32[ci++] = ufbx_get_vertex_vec4(&mesh->vertex_color, a).w;
			}
			else if (force_col) {
				cola32[ci++] = 0;
				cola32[ci++] = 0;
				cola32[ci++] = 0;
				cola32[ci++] = 0;
			}
		}
	}

	free(indices);

	int vertex_count = pi / 3;
	int index_count  = vertex_count;

	uint32_t *inda = malloc(sizeof(uint32_t) * index_count);
	for (int i = 0; i < index_count; ++i) {
		inda[i] = i;
	}

	// Pack positions to (-1, 1) range
	float hx = 0.0;
	float hy = 0.0;
	float hz = 0.0;
	for (int i = 0; i < vertex_count; ++i) {
		float f = fabsf(posa32[i * 3]);
		if (hx < f)
			hx = f;
		f = fabsf(posa32[i * 3 + 1]);
		if (hy < f)
			hy = f;
		f = fabsf(posa32[i * 3 + 2]);
		if (hz < f)
			hz = f;
	}
	float _scale_pos = fmax(hx, fmax(hy, hz));
	if (_scale_pos > scale_pos)
		scale_pos = _scale_pos;
	float inv = 1 / scale_pos;

	// Pack into 16bit
	short *posa = malloc(sizeof(short) * vertex_count * 4);
	for (int i = 0; i < vertex_count; ++i) {
		posa[i * 4]     = posa32[i * 3] * 32767 * inv;
		posa[i * 4 + 1] = posa32[i * 3 + 1] * 32767 * inv;
		posa[i * 4 + 2] = posa32[i * 3 + 2] * 32767 * inv;
	}

	short *nora = malloc(sizeof(short) * vertex_count * 2);
	if (nora32 != NULL) {
		for (int i = 0; i < vertex_count; ++i) {
			nora[i * 2]     = nora32[i * 3] * 32767;
			nora[i * 2 + 1] = nora32[i * 3 + 1] * 32767;
			posa[i * 4 + 3] = nora32[i * 3 + 2] * 32767;
		}
		free(nora32);
	}

	free(posa32);

	short *texa = NULL;
	if (texa32 != NULL) {
		texa = malloc(sizeof(short) * vertex_count * 2);
		for (int i = 0; i < vertex_count; ++i) {
			texa[i * 2]     = texa32[i * 2] * 32767;
			texa[i * 2 + 1] = (1.0 - texa32[i * 2 + 1]) * 32767;
		}
		free(texa32);
	}

	short *texa1 = NULL;
	if (texa132 != NULL) {
		texa1 = malloc(sizeof(short) * vertex_count * 2);
		for (int i = 0; i < vertex_count; ++i) {
			texa1[i * 2]     = texa132[i * 2] * 32767;
			texa1[i * 2 + 1] = (1.0 - texa132[i * 2 + 1]) * 32767;
		}
		free(texa132);
	}

	short *cola = NULL;
	if (cola32 != NULL) {
		cola = malloc(sizeof(short) * vertex_count * 4);
		for (int i = 0; i < vertex_count; ++i) {
			cola[i * 4]     = cola32[i * 4] * 32767;
			cola[i * 4 + 1] = cola32[i * 4 + 1] * 32767;
			cola[i * 4 + 2] = cola32[i * 4 + 2] * 32767;
			cola[i * 4 + 3] = cola32[i * 4 + 3] * 32767;
		}
		free(cola32);
	}

	raw->posa         = (i16_array_t *)malloc(sizeof(i16_array_t));
	raw->posa->buffer = posa;
	raw->posa->length = raw->posa->capacity = vertex_count * 4;

	raw->nora         = (i16_array_t *)malloc(sizeof(i16_array_t));
	raw->nora->buffer = nora;
	raw->nora->length = raw->nora->capacity = vertex_count * 2;

	if (texa != NULL) {
		raw->texa         = (i16_array_t *)malloc(sizeof(i16_array_t));
		raw->texa->buffer = texa;
		raw->texa->length = raw->texa->capacity = vertex_count * 2;
	}

	if (texa1 != NULL) {
		raw->texa1         = (i16_array_t *)malloc(sizeof(i16_array_t));
		raw->texa1->buffer = texa1;
		raw->texa1->length = raw->texa1->capacity = vertex_count * 2;
	}

	if (cola != NULL) {
		raw->cola         = (i16_array_t *)malloc(sizeof(i16_array_t));
		raw->cola->buffer = cola;
		raw->cola->length = raw->cola->capacity = vertex_count * 4;
	}

	raw->inda         = (u32_array_t *)malloc(sizeof(u32_array_t));
	raw->inda->buffer = inda;
	raw->inda->length = raw->inda->capacity = index_count;

	raw->scale_pos = scale_pos;
	raw->scale_tex = 1.0;
}

void *io_fbx_parse(char *buf, size_t size) {
	if (active_base_scene == NULL) {
		ufbx_load_opts opts = {.generate_missing_normals = true};
		active_base_scene = active_eval_scene = ufbx_load_memory(buf, size, &opts, NULL);
		if (active_base_scene == NULL) { // STUDIO-PATCH(import-null-guards): not an fbx, see external/PATCHES.md
			return NULL;
		}
		active_tex1                           = true;
		for (size_t i = 0; i < active_eval_scene->nodes.count; ++i) {
			ufbx_node *n = active_eval_scene->nodes.data[i];
			if (valid_mesh(n) && n->mesh->uv_sets.count < 2) {
				active_tex1 = false;
				break;
			}
		}
		if (plugins_split_by == 1 /* SPLIT_TYPE_MATERIAL */) {
			build_material_split(active_eval_scene);
			mat_split_idx = 0;
		}
		else if (plugins_split_by == 2 /* SPLIT_TYPE_UDIM */) {
			build_udim_split(active_eval_scene);
			mat_split_idx = 0;
		}
	}

	if (plugins_split_by == 1 /* SPLIT_TYPE_MATERIAL */ || plugins_split_by == 2 /* SPLIT_TYPE_UDIM */) {
		if (mat_split_count == 0) { // STUDIO-PATCH(fbx-split): no triangle, or udim file refused: read past the array
			ufbx_free_scene(active_base_scene);
			active_base_scene = NULL;
			active_eval_scene = NULL;
			active_tex1       = false;
			free(mat_split_meshes);
			mat_split_meshes = NULL;
			return NULL;
		}
		raw_mesh_t *raw = mat_split_meshes[mat_split_idx++];
		raw->has_next   = (mat_split_idx < mat_split_count);
		if (!raw->has_next) {
			ufbx_free_scene(active_base_scene);
			active_base_scene = NULL;
			active_eval_scene = NULL;
			current_node      = 0;
			active_tex1       = false;
			free(mat_split_meshes);
			mat_split_meshes = NULL;
			mat_split_count  = 0;
			mat_split_idx    = 0;
		}
		return raw;
	}

	raw_mesh_t *raw = (raw_mesh_t *)calloc(sizeof(raw_mesh_t), 1);

	bool force_tex = mixed_uvs(active_eval_scene);
	bool force_col = mixed_cols(active_eval_scene);

	for (; current_node < (int)active_eval_scene->nodes.count; ++current_node) {
		ufbx_node *n = active_eval_scene->nodes.data[current_node];
		if (valid_mesh(n)) {
			raw->name = malloc(strlen(n->name.data) + 1);
			strcpy(raw->name, n->name.data);
			ufbx_matrix normal_mat = ufbx_get_compatible_matrix_for_normals(n);
			io_fbx_parse_mesh(raw, n->mesh, &n->geometry_to_world, &normal_mat, force_tex, force_col);
			break;
		}
	}
	current_node++;

	has_next = false;
	for (int i = current_node; i < (int)active_eval_scene->nodes.count; ++i) {
		ufbx_node *n = active_eval_scene->nodes.data[i];
		if (valid_mesh(n)) {
			has_next = true;
			break;
		}
	}

	if (!has_next) {
		ufbx_free_scene(active_base_scene);
		active_base_scene = NULL;
		active_eval_scene = NULL;
		current_node      = 0;
		active_tex1       = false;
	}

	raw->has_next = has_next;
	return raw;
}

void *io_fbx_parse_skinned(char *buf, size_t size, int frame) {
	if (active_base_scene == NULL) {
		ufbx_load_opts load_opts = {.generate_missing_normals = true};
		active_base_scene        = ufbx_load_memory(buf, size, &load_opts, NULL);
		if (active_base_scene == NULL)
			return NULL;

		double fps  = active_base_scene->settings.frames_per_second > 0.0 ? active_base_scene->settings.frames_per_second : 30.0;
		double time = (double)frame / fps;
		if (active_base_scene->anim_stacks.count > 0) {
			ufbx_evaluate_opts eval_opts = {.evaluate_skinning = true};
			active_eval_scene            = ufbx_evaluate_scene(active_base_scene, active_base_scene->anim_stacks.data[0]->anim, time, &eval_opts, NULL);
			if (active_eval_scene == NULL) {
				ufbx_free_scene(active_base_scene);
				active_base_scene = NULL;
				return NULL;
			}
		}
		else {
			active_eval_scene = active_base_scene;
		}

		// Pre-scan: disable tex1 if any mesh lacks a second uv set
		active_tex1 = true;
		for (size_t i = 0; i < active_eval_scene->nodes.count; ++i) {
			ufbx_node *n = active_eval_scene->nodes.data[i];
			if (valid_mesh(n) && n->mesh->uv_sets.count < 2) {
				active_tex1 = false;
				break;
			}
		}
	}

	// Find next mesh node
	ufbx_node *mesh_node = NULL;
	for (; current_node < (int)active_eval_scene->nodes.count; ++current_node) {
		ufbx_node *n = active_eval_scene->nodes.data[current_node];
		if (valid_mesh(n)) {
			mesh_node = n;
			break;
		}
	}
	current_node++;

	if (mesh_node == NULL) {
		if (active_eval_scene != active_base_scene)
			ufbx_free_scene(active_eval_scene);
		ufbx_free_scene(active_base_scene);
		active_base_scene = NULL;
		active_eval_scene = NULL;
		current_node      = 0;
		has_next          = false;
		active_tex1       = false;
		return calloc(sizeof(raw_mesh_t), 1);
	}

	ufbx_mesh  *mesh            = mesh_node->mesh;
	ufbx_matrix normal_to_world = ufbx_get_compatible_matrix_for_normals(mesh_node);

	uint32_t  indices_size = mesh->max_face_triangles * 3;
	uint32_t *indices      = (uint32_t *)malloc(sizeof(uint32_t) * indices_size);

	bool   force_tex = mixed_uvs(active_eval_scene);
	bool   has_tex   = mesh->vertex_uv.exists;
	bool   has_tex1  = active_tex1 && mesh->uv_sets.count > 1;
	int    numtri    = mesh->num_triangles;
	float *posa32    = (float *)malloc(sizeof(float) * numtri * 3 * 3);
	float *nora32    = (float *)malloc(sizeof(float) * numtri * 3 * 3);
	float *texa32    = (has_tex || force_tex) ? (float *)malloc(sizeof(float) * numtri * 3 * 2) : NULL;
	float *texa132   = has_tex1 ? (float *)malloc(sizeof(float) * numtri * 3 * 2) : NULL;
	int    pi = 0, ni = 0, ti = 0, ti1 = 0;

	for (int j = 0; j < mesh->faces.count; ++j) {
		ufbx_face face          = mesh->faces.data[j];
		uint32_t  num_triangles = ufbx_triangulate_face(indices, indices_size, mesh, face);
		for (uint32_t v_ix = 0; v_ix < num_triangles * 3; v_ix++) {
			uint32_t a = indices[v_ix];

			// skinned_position/normal are world-space when skinned_is_local == false
			// (evaluated skinned mesh), or local-space when skinned_is_local == true
			ufbx_vec3 v = ufbx_get_vertex_vec3(&mesh->skinned_position, a);
			if (mesh->skinned_is_local)
				v = ufbx_transform_position(&mesh_node->geometry_to_world, v);
			posa32[pi++] = v.x;
			posa32[pi++] = -v.z;
			posa32[pi++] = v.y;

			ufbx_vec3 n = ufbx_get_vertex_vec3(&mesh->skinned_normal, a);
			if (mesh->skinned_is_local)
				n = ufbx_transform_direction(&normal_to_world, n);
			float nlen = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
			if (nlen > 1e-6f) {
				n.x /= nlen;
				n.y /= nlen;
				n.z /= nlen;
			}
			nora32[ni++] = n.x;
			nora32[ni++] = -n.z;
			nora32[ni++] = n.y;

			if (has_tex) {
				texa32[ti++] = ufbx_get_vertex_vec2(&mesh->vertex_uv, a).x;
				texa32[ti++] = ufbx_get_vertex_vec2(&mesh->vertex_uv, a).y;
			}
			else if (force_tex) {
				texa32[ti++] = 0;
				texa32[ti++] = 0;
			}

			if (has_tex1) {
				texa132[ti1++] = ufbx_get_vertex_vec2(&mesh->uv_sets.data[1].vertex_uv, a).x;
				texa132[ti1++] = ufbx_get_vertex_vec2(&mesh->uv_sets.data[1].vertex_uv, a).y;
			}
		}
	}

	free(indices);

	int vertex_count = pi / 3;
	int index_count  = vertex_count;

	uint32_t *inda = malloc(sizeof(uint32_t) * index_count);
	for (int i = 0; i < index_count; ++i)
		inda[i] = i;

	// Pack positions to (-1, 1) range
	float hx = 0.0, hy = 0.0, hz = 0.0;
	for (int i = 0; i < vertex_count; ++i) {
		float f = fabsf(posa32[i * 3]);
		if (hx < f)
			hx = f;
		f = fabsf(posa32[i * 3 + 1]);
		if (hy < f)
			hy = f;
		f = fabsf(posa32[i * 3 + 2]);
		if (hz < f)
			hz = f;
	}
	float _scale_pos = fmax(hx, fmax(hy, hz));
	if (_scale_pos > scale_pos)
		scale_pos = _scale_pos;
	float inv = 1 / scale_pos;

	// Pack into 16bit
	short *posa = malloc(sizeof(short) * vertex_count * 4);
	for (int i = 0; i < vertex_count; ++i) {
		posa[i * 4]     = posa32[i * 3] * 32767 * inv;
		posa[i * 4 + 1] = posa32[i * 3 + 1] * 32767 * inv;
		posa[i * 4 + 2] = posa32[i * 3 + 2] * 32767 * inv;
	}
	free(posa32);

	short *nora = malloc(sizeof(short) * vertex_count * 2);
	for (int i = 0; i < vertex_count; ++i) {
		nora[i * 2]     = nora32[i * 3] * 32767;
		nora[i * 2 + 1] = nora32[i * 3 + 1] * 32767;
		posa[i * 4 + 3] = nora32[i * 3 + 2] * 32767;
	}
	free(nora32);

	short *texa = NULL;
	if (texa32 != NULL) {
		texa = malloc(sizeof(short) * vertex_count * 2);
		for (int i = 0; i < vertex_count; ++i) {
			texa[i * 2]     = texa32[i * 2] * 32767;
			texa[i * 2 + 1] = (1.0 - texa32[i * 2 + 1]) * 32767;
		}
		free(texa32);
	}

	short *texa1 = NULL;
	if (texa132 != NULL) {
		texa1 = malloc(sizeof(short) * vertex_count * 2);
		for (int i = 0; i < vertex_count; ++i) {
			texa1[i * 2]     = texa132[i * 2] * 32767;
			texa1[i * 2 + 1] = (1.0 - texa132[i * 2 + 1]) * 32767;
		}
		free(texa132);
	}

	raw_mesh_t *raw = (raw_mesh_t *)calloc(sizeof(raw_mesh_t), 1);
	raw->name       = malloc(strlen(mesh_node->name.data) + 1);
	strcpy(raw->name, mesh_node->name.data);

	raw->posa         = (i16_array_t *)malloc(sizeof(i16_array_t));
	raw->posa->buffer = posa;
	raw->posa->length = raw->posa->capacity = vertex_count * 4;

	raw->nora         = (i16_array_t *)malloc(sizeof(i16_array_t));
	raw->nora->buffer = nora;
	raw->nora->length = raw->nora->capacity = vertex_count * 2;

	if (texa != NULL) {
		raw->texa         = (i16_array_t *)malloc(sizeof(i16_array_t));
		raw->texa->buffer = texa;
		raw->texa->length = raw->texa->capacity = vertex_count * 2;
	}

	if (texa1 != NULL) {
		raw->texa1         = (i16_array_t *)malloc(sizeof(i16_array_t));
		raw->texa1->buffer = texa1;
		raw->texa1->length = raw->texa1->capacity = vertex_count * 2;
	}

	raw->inda         = (u32_array_t *)malloc(sizeof(u32_array_t));
	raw->inda->buffer = inda;
	raw->inda->length = raw->inda->capacity = index_count;

	raw->scale_pos = scale_pos;
	raw->scale_tex = 1.0;

	// Check for more mesh nodes
	has_next = false;
	for (int i = current_node; i < (int)active_eval_scene->nodes.count; ++i) {
		ufbx_node *n = active_eval_scene->nodes.data[i];
		if (valid_mesh(n)) {
			has_next = true;
			break;
		}
	}

	if (!has_next) {
		if (active_eval_scene != active_base_scene)
			ufbx_free_scene(active_eval_scene);
		ufbx_free_scene(active_base_scene);
		active_base_scene = NULL;
		active_eval_scene = NULL;
		current_node      = 0;
		active_tex1       = false;
		scale_pos         = 1.0;
	}

	raw->has_next = has_next;
	return raw;
}
