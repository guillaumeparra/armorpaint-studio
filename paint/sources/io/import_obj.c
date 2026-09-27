
#include "../global.h"
// STUDIO-PATCH(udim-import)
#include "iron_udim.h"

void import_obj_run(char *path, bool replace_existing) {
	buffer_t *b = data_get_blob(path);
	if (b == NULL) { // STUDIO-PATCH(import-null-guards)
		console_error(string("Could not open file %s.", path));
		return;
	}
	import_obj_parse(b, replace_existing);
	data_delete_blob(path);
}

// STUDIO-PATCH(udim-import): udim split, see paint/plugins/external/PATCHES.md. Upstream read the file as a single
// object and gave each tile a copy of all the vertices. Now each object is split in its tiles, with the vertices of the
// tile only, and obj_parse cuts the triangles over several tiles. A uv out of the tiles 1001-1990, or more tiles than
// the layer atlas holds, refuses the file with a message and imports nothing. By material and tile (lot D1), each
// material gives one object per tile, gathered from all its parts.
static void _import_obj_udim_free(raw_mesh_t *part) {
	if (part->udims != NULL) {
		for (i32 i = 0; i < part->udims->length; ++i) {
			u32_array_t *a = part->udims->buffer[i];
			array_free(a);
			free(a);
		}
		array_free(part->udims);
		free(part->udims);
	}
	void *arrays[] = {part->posa, part->nora, part->texa, part->inda};
	for (i32 i = 0; i < 4; ++i) {
		if (arrays[i] != NULL) {
			array_free(arrays[i]);
			free(arrays[i]);
		}
	}
	free(part->name);
	free(part);
}

// The indices of a tile (u + 10 v) in the grid of a part, NULL out of it
static u32_array_t *_import_obj_udim_indices(raw_mesh_t *part, i32 tile) {
	i32 u = tile % UDIM_TILES_U;
	i32 v = tile / UDIM_TILES_U;
	return u < part->udims_u && v < part->udims_v ? part->udims->buffer[u + v * part->udims_u] : NULL;
}

// The triangles of one tile, with their own vertices (obj_parse never shares the corners of its triangles), from every
// part of the group, named after the first. The positions go to the largest scale of the parts. The indices taken are
// emptied: a later part of the group gives them no more. NULL if no part has the tile.
static raw_mesh_t *_import_obj_udim_tile(raw_mesh_t_array_t *group, i32 tile) {
	i32 length    = 0;
	f32 scale_pos = 0.0;
	for (i32 i = 0; i < group->length; ++i) {
		u32_array_t *a = _import_obj_udim_indices(group->buffer[i], tile);
		if (a != NULL && a->length > 0) {
			length += a->length;
			scale_pos = math_max(scale_pos, group->buffer[i]->scale_pos);
		}
	}
	if (length == 0) {
		return NULL;
	}
	raw_mesh_t *mesh = calloc(1, sizeof(raw_mesh_t));
	mesh->name       = string("%s.%d", group->buffer[0]->name, udim_tile_id(tile % UDIM_TILES_U, tile / UDIM_TILES_U));
	mesh->scale_pos  = scale_pos;
	mesh->scale_tex  = group->buffer[0]->scale_tex;
	mesh->posa       = i16_array_create(length * 4);
	mesh->nora       = i16_array_create(length * 2);
	mesh->texa       = i16_array_create(length * 2);
	mesh->inda       = u32_array_create(length);
	i32 n            = 0;
	for (i32 i = 0; i < group->length; ++i) {
		raw_mesh_t  *part  = group->buffer[i];
		u32_array_t *a     = _import_obj_udim_indices(part, tile);
		f32          scale = part->scale_pos / scale_pos;
		for (i32 j = 0; a != NULL && j < a->length; ++j, ++n) {
			u32 k = a->buffer[j];
			for (i32 c = 0; c < 3; ++c) {
				i16 p                         = part->posa->buffer[k * 4 + c];
				mesh->posa->buffer[n * 4 + c] = scale == 1.0 ? p : (i16)roundf(p * scale);
			}
			mesh->posa->buffer[n * 4 + 3] = part->posa->buffer[k * 4 + 3]; // Normal z
			memcpy(&mesh->nora->buffer[n * 2], &part->nora->buffer[k * 2], sizeof(i16) * 2);
			memcpy(&mesh->texa->buffer[n * 2], &part->texa->buffer[k * 2], sizeof(i16) * 2);
			mesh->inda->buffer[n] = n;
		}
		if (a != NULL) {
			a->length = 0;
		}
	}
	mesh->vertex_count = mesh->index_count = length;
	return mesh;
}

static void _import_obj_udim_add(raw_mesh_t *mesh, bool *first, bool replace_existing) {
	if (*first && replace_existing) {
		import_mesh_make_mesh(mesh);
	}
	else {
		import_mesh_add_mesh(mesh);
	}
	*first = false;
	free(mesh); // The arrays and the name now belong to the mesh object
}

static void _import_obj_udim(buffer_t *b, bool replace_existing, bool by_material) {
	bool used[UDIM_TILES_U * UDIM_TILES_V] = {0}; // Tile u + 10 v

	raw_mesh_t_array_t *parts         = any_array_create_from_raw((void *[]){}, 0);
	i32                 tiles         = 0;
	i32                 cut           = 0;
	i32                 invalid       = 0;
	f32                 invalid_uv[2] = {0};
	char               *invalid_name  = NULL;
	i32                 no_uvs        = 0;
	char               *no_uvs_name   = NULL;
	u64                 pos           = 0;
	bool                has_next      = true;
	while (has_next) {
		raw_mesh_t *part = obj_parse(b, by_material ? 'u' : 'o', pos, true); // usemtl or o
		pos              = part->pos;
		has_next         = part->has_next;
		if (string_equals(part->name, "")) {
			free(part->name);
			part->name = string_copy("Object");
		}
		if (obj_udim_invalid > 0 && invalid == 0) {
			invalid_name  = string_copy(part->name);
			invalid_uv[0] = obj_udim_invalid_uv[0];
			invalid_uv[1] = obj_udim_invalid_uv[1];
		}
		invalid += obj_udim_invalid;
		cut += obj_udim_cut;
		if (part->inda->length == 0) { // No face, or only faces with invalid uvs
			_import_obj_udim_free(part);
			continue;
		}
		if (part->udims == NULL && no_uvs++ == 0) {
			no_uvs_name = string_copy(part->name);
		}
		for (i32 i = 0; part->udims != NULL && i < part->udims->length; ++i) {
			u32_array_t *a    = part->udims->buffer[i];
			i32          tile = udim_tile_id(i % part->udims_u, i / part->udims_u) - 1001;
			if (a->length > 0 && !used[tile]) {
				used[tile] = true;
				tiles++;
			}
		}
		any_array_push(parts, part);
	}

	char *error = NULL;
	if (invalid > 0) {
		error = string("UDIM import refused: uvs outside the tiles 1001-1990 in \"%s\" (%d triangles, first uv %g %g).", invalid_name, invalid, invalid_uv[0],
		               invalid_uv[1]);
	}
	else if (tiles > UDIM_MAX_TILES) {
		error = string("UDIM import refused: %d tiles, the layers hold %d at most.", tiles, UDIM_MAX_TILES);
	}
	else if (parts->length == 0) {
		error = strings_failed_to_read_mesh_data();
	}

	// The parts with uvs first: a first part without uvs makes ArmorPaint offer to unwrap the whole mesh, udim uvs included
	bool first   = true;
	i32  objects = 0;
	for (i32 pass = 0; pass < 2; ++pass) {
		for (i32 i = 0; i < parts->length; ++i) {
			raw_mesh_t *part = parts->buffer[i];
			if (part == NULL || (part->udims == NULL) != (pass == 1)) {
				continue;
			}
			parts->buffer[i] = NULL; // Freed or handed to the mesh object below, the second pass must not read it
			if (error != NULL) {
				_import_obj_udim_free(part);
			}
			else if (part->udims == NULL) { // No uvs: imported as is, without a tile
				_import_obj_udim_add(part, &first, replace_existing);
				objects++;
			}
			else {
				// By material, the later parts of the material bring their triangles too: an obj splits a material at each
				// object and at each "v" after faces (Maya repeats usemtl in each object)
				raw_mesh_t_array_t *group = any_array_create_from_raw((void *[]){part}, 1);
				for (i32 j = i + 1; by_material && j < parts->length; ++j) {
					raw_mesh_t *p = parts->buffer[j];
					if (p != NULL && p->udims != NULL && string_equals(p->name, part->name)) {
						any_array_push(group, p);
					}
				}
				for (i32 tile = 0; tile < UDIM_TILES_U * UDIM_TILES_V; ++tile) {
					raw_mesh_t *mesh = _import_obj_udim_tile(group, tile);
					if (mesh != NULL) {
						_import_obj_udim_add(mesh, &first, replace_existing);
						objects++;
					}
				}
				array_free(group);
				free(group);
				_import_obj_udim_free(part);
			}
		}
	}
	array_free(parts);
	free(parts);

	if (error != NULL) {
		console_error(error);
	}
	else {
		console_log(string("UDIM import: %d objects in %d tiles, %d triangles cut along the tile borders.", objects, tiles, cut));
		if (no_uvs > 0) {
			console_info(string("UDIM import: %d objects without uvs imported without a tile, first \"%s\".", no_uvs, no_uvs_name));
		}
	}
	free(invalid_name);
	free(no_uvs_name);
}

void import_obj_parse(buffer_t *b, bool replace_existing) {
	split_type_t i          = g_context->split_by;
	bool         is_udim    = i == SPLIT_TYPE_UDIM || i == SPLIT_TYPE_MATERIAL_UDIM; // STUDIO-PATCH(udim-import)
	i32          split_code = (i == SPLIT_TYPE_OBJECT || is_udim) ? 'o' : 'u'; // usemtl

	if (is_udim) { // STUDIO-PATCH(udim-import)
		_import_obj_udim(b, replace_existing, i == SPLIT_TYPE_MATERIAL_UDIM);
	}
	else {
		raw_mesh_t_array_t *parts     = any_array_create_from_raw((void *[]){}, 0);
		raw_mesh_t_array_t *discarded = any_array_create_from_raw((void *[]){}, 0);
		raw_mesh_t         *part      = obj_parse(b, split_code, 0, false);
		any_array_push(parts, part);
		while (part->has_next) {
			part = obj_parse(b, split_code, part->pos, false);
			// This part does not contain faces (may contain lines only)
			if (part->inda->length == 0) {
				any_array_push(discarded, part);
				continue;
			}
			any_array_push(parts, part);
		}
		if (g_context->split_by == SPLIT_TYPE_MATERIAL) {
			i16_array_t *posa0;
			i16_array_t *posa1;
			i16_array_t *nora0;
			i16_array_t *nora1;
			i16_array_t *texa0;
			i16_array_t *texa1;
			u32_array_t *inda0;
			u32_array_t *inda1;
			// Merge to single object per material
			for (i32 i = 0; i < parts->length; ++i) {
				i32 j = i + 1;
				while (j < parts->length) {
					char *iname = parts->buffer[i]->name;
					char *jname = parts->buffer[j]->name;
					if (string_equals(iname, jname)) {
						posa0    = parts->buffer[i]->posa;
						posa1    = parts->buffer[j]->posa;
						nora0    = parts->buffer[i]->nora;
						nora1    = parts->buffer[j]->nora;
						texa0    = parts->buffer[i]->texa != NULL ? parts->buffer[i]->texa : NULL;
						texa1    = parts->buffer[j]->texa != NULL ? parts->buffer[j]->texa : NULL;
						inda0    = parts->buffer[i]->inda;
						inda1    = parts->buffer[j]->inda;
						i32 voff = math_floor(posa0->length / 4.0);
						// Repack merged positions
						f32_array_t *posa32 = f32_array_create(math_floor(posa0->length / 4.0) * 3 + math_floor(posa1->length / 4.0) * 3);
						for (i32 k = 0; k < math_floor(posa0->length / 4.0); ++k) {
							posa32->buffer[k * 3]     = posa0->buffer[k * 4] / 32767.0 * parts->buffer[i]->scale_pos;
							posa32->buffer[k * 3 + 1] = posa0->buffer[k * 4 + 1] / 32767.0 * parts->buffer[i]->scale_pos;
							posa32->buffer[k * 3 + 2] = posa0->buffer[k * 4 + 2] / 32767.0 * parts->buffer[i]->scale_pos;
						}
						for (i32 k = 0; k < math_floor(posa1->length / 4.0); ++k) {
							posa32->buffer[voff * 3 + k * 3]     = posa1->buffer[k * 4] / 32767.0 * parts->buffer[j]->scale_pos;
							posa32->buffer[voff * 3 + k * 3 + 1] = posa1->buffer[k * 4 + 1] / 32767.0 * parts->buffer[j]->scale_pos;
							posa32->buffer[voff * 3 + k * 3 + 2] = posa1->buffer[k * 4 + 2] / 32767.0 * parts->buffer[j]->scale_pos;
						}
						f32 scale_pos = 0.0;
						for (i32 k = 0; k < posa32->length; ++k) {
							f32 f = math_abs(posa32->buffer[k]);
							if (scale_pos < f) {
								scale_pos = f;
							}
						}
						f32          inv  = 32767 * (1 / (float)scale_pos);
						i16_array_t *posa = i16_array_create(posa0->length + posa1->length);
						for (i32 k = 0; k < math_floor(posa->length / 4.0); ++k) {
							posa->buffer[k * 4]     = math_floor(posa32->buffer[k * 3] * inv);
							posa->buffer[k * 4 + 1] = math_floor(posa32->buffer[k * 3 + 1] * inv);
							posa->buffer[k * 4 + 2] = math_floor(posa32->buffer[k * 3 + 2] * inv);
						}
						for (i32 k = 0; k < math_floor(posa0->length / 4.0); ++k) {
							posa->buffer[k * 4 + 3] = posa0->buffer[k * 4 + 3];
						}
						for (i32 k = 0; k < math_floor(posa1->length / 4.0); ++k) {
							posa->buffer[posa0->length + k * 4 + 3] = posa1->buffer[k * 4 + 3];
						}
						// Merge normals and uvs
						i16_array_t *nora = i16_array_create(nora0->length + nora1->length);
						i16_array_t *texa = (texa0 != NULL && texa1 != NULL) ? i16_array_create(texa0->length + texa1->length) : NULL;
						u32_array_t *inda = u32_array_create(inda0->length + inda1->length);

						for (i32 k = 0; k < nora0->length; ++k) {
							nora->buffer[k] = nora0->buffer[k];
						}
						for (i32 k = 0; k < nora1->length; ++k) {
							nora->buffer[k + nora0->length] = nora1->buffer[k];
						}

						if (texa != NULL) {
							for (i32 k = 0; k < texa0->length; ++k) {
								texa->buffer[k] = texa0->buffer[k];
							}
							for (i32 k = 0; k < texa1->length; ++k) {
								texa->buffer[k + texa0->length] = texa1->buffer[k];
							}
						}

						for (i32 k = 0; k < inda0->length; ++k) {
							inda->buffer[k] = inda0->buffer[k];
						}
						for (i32 k = 0; k < inda1->length; ++k) {
							inda->buffer[k + inda0->length] = inda1->buffer[k] + voff;
						}

						array_free(posa32);
						free(posa32);
						array_free(posa0);
						free(posa0);
						array_free(nora0);
						free(nora0);
						array_free(inda0);
						free(inda0);
						if (texa0 != NULL) {
							array_free(texa0);
							free(texa0);
						}
						array_free(posa1);
						free(posa1);
						array_free(nora1);
						free(nora1);
						array_free(inda1);
						free(inda1);
						if (texa1 != NULL) {
							array_free(texa1);
							free(texa1);
						}
						raw_mesh_t *jpart = parts->buffer[j];
						free(jpart->name);
						free(jpart);

						parts->buffer[i]->posa      = posa;
						parts->buffer[i]->nora      = nora;
						parts->buffer[i]->texa      = texa;
						parts->buffer[i]->inda      = inda;
						parts->buffer[i]->scale_pos = scale_pos;
						array_splice(parts, j, 1);
					}
					else {
						j++;
					}
				}
			}
		}

		replace_existing ? import_mesh_make_mesh(parts->buffer[0]) : import_mesh_add_mesh(parts->buffer[0]);
		for (i32 i = 1; i < parts->length; ++i) {
			import_mesh_add_mesh(parts->buffer[i]);
		}

		for (i32 i = 0; i < parts->length; ++i) {
			free(parts->buffer[i]);
		}
		array_free(parts);
		free(parts);
		for (i32 i = 0; i < discarded->length; ++i) {
			raw_mesh_t *d = discarded->buffer[i];
			array_free(d->posa);
			free(d->posa);
			array_free(d->nora);
			free(d->nora);
			if (d->texa != NULL) {
				array_free(d->texa);
				free(d->texa);
			}
			array_free(d->inda);
			free(d->inda);
			free(d->name);
			free(d);
		}
		array_free(discarded);
		free(discarded);
	}
}
