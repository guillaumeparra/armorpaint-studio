#include "iron_obj.h"

#include "iron_alloc.h"
#include "iron_array.h"
#include "iron_math.h"
#include "iron_string.h"
#include <float.h> // STUDIO-PATCH(obj-float-exponent)
#include <math.h>
#include <stdlib.h>
#include <string.h>

// STUDIO-PATCH(obj-float-exponent): exponent found by read_float, applied once the whole mantissa is parsed
static int read_float_exp = 0;
static int read_exponent();

static raw_mesh_t *part = NULL;
static f32_array_t pos_temp;
static f32_array_t uv_temp;
static f32_array_t nor_temp;
static int32_t     va[512]; // STUDIO-PATCH(obj-global-indices): 0-based global indices, -1 = absent or out of range
static int32_t     ua[512];
static int32_t     na[512];
static int         vi = 0;
static uint8_t     buf[128];
static char        str[256];

static uint8_t     *bytes        = NULL;
static size_t       bytes_length = 0;
void                console_info(char *s);
static bool         check_uvmap         = true;
static bool         check_faces         = true; // STUDIO-PATCH(obj-global-indices)
bool                obj_parse_y_to_z_up = true;

static int read_int() {
	int bi = 0;
	while (part->pos < bytes_length) { // Read into buffer
		char c = bytes[part->pos];
		if (c == '/' || c == '\n' || c == '\r' || c == ' ' || c == '\t') {
			break;
		}
		if (bi < (int)sizeof(buf) - 1) { // STUDIO-PATCH(obj-global-indices)
			buf[bi++] = c;
		}
		part->pos++;
	}
	int64_t res = 0; // Parse buffer into int // STUDIO-PATCH(obj-global-indices): saturated instead of overflowing
	int     off = buf[0] == '-' ? 1 : 0;
	for (int i = off; i < bi; ++i) {
		res = res * 10 + (buf[i] - 48);
		if (res > INT32_MAX || res < -INT32_MAX) {
			res = INT32_MAX;
			break;
		}
	}
	return (int)(off > 0 ? -res : res);
}

static inline void skip_whitespace() {
	while (part->pos < bytes_length && (bytes[part->pos] == ' ' || bytes[part->pos] == '\t')) { // STUDIO-PATCH(obj-global-indices)
		part->pos++;
	}
}

// STUDIO-PATCH(obj-global-indices): OBJ indices are global to the file, 1-based, negative = relative to the
// end of the list read so far. Returns the 0-based index, or -1 when absent (0) or out of range.
static int32_t obj_index(int i, uint32_t count) {
	int64_t r = i > 0 ? (int64_t)i - 1 : (int64_t)count + i;
	return r >= 0 && r < count ? (int32_t)r : -1;
}

// STUDIO-PATCH(obj-global-indices): the corner syntax (v, v/t, v//n, v/t/n) is read from the face itself,
// not guessed from the vt / vn read so far, which now span every object of the file.
static void read_face() {
	while (true) {
		int v = read_int();
		int t = 0;
		int n = 0;
		if (part->pos < bytes_length && bytes[part->pos] == '/') {
			part->pos++;
			t = read_int(); // Empty in "v//n"
			if (part->pos < bytes_length && bytes[part->pos] == '/') {
				part->pos++;
				n = read_int();
			}
		}
		if (vi < 512) {
			va[vi] = obj_index(v, pos_temp.length / 3);
			ua[vi] = obj_index(t, uv_temp.length / 2); // Also drops the fake uv index of "f 1/1/1" without vt
			na[vi] = obj_index(n, nor_temp.length / 3);
		}
		vi++;
		skip_whitespace();
		if (part->pos >= bytes_length || bytes[part->pos] == '\n' || bytes[part->pos] == '\r') {
			break;
		}
	}
}

// STUDIO-PATCH(obj-global-indices): a face is kept when all its positions exist. Its uvs and normals are kept
// only if all its corners have one, else they are marked absent (-1) for the whole face.
static bool check_face() {
	if (vi < 3 || vi > 512) {
		return false;
	}
	bool has_uv  = true;
	bool has_nor = true;
	for (int i = 0; i < vi; ++i) {
		if (va[i] < 0) {
			return false;
		}
		has_uv  = has_uv && ua[i] >= 0;
		has_nor = has_nor && na[i] >= 0;
	}
	for (int i = 0; i < vi; ++i) {
		if (!has_uv) {
			ua[i] = -1;
		}
		if (!has_nor) {
			na[i] = -1;
		}
	}
	return true;
}

static void push_corner(i32_array_t *pos_indices, i32_array_t *uv_indices, i32_array_t *nor_indices, int i) {
	i32_array_push(pos_indices, va[i]);
	i32_array_push(uv_indices, ua[i]);
	i32_array_push(nor_indices, na[i]);
}

static float read_float() {
	int bi = 0;
	while (part->pos < bytes_length) { // Read into buffer // STUDIO-PATCH(obj-global-indices)
		char c = bytes[part->pos];
		if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
			break;
		}
		if (c == 'E' || c == 'e') {
			part->pos++;
			read_float_exp = read_exponent(); // STUDIO-PATCH(obj-float-exponent)
			break;
		}
		if (bi < 9) {
			buf[bi++] = c;
		}
		part->pos++;
	}
	float   res = 0.0; // Parse buffer into float
	int64_t dot = 1;
	int64_t dec = 1;
	int     off = buf[0] == '-' ? 1 : 0;
	int     len = bi - 1;
	for (int i = 0; i < bi - off; ++i) {
		char c = buf[len - i];
		if (c == '.') {
			dot = dec;
			continue;
		}
		res += (c - 48) * dec;
		dec *= 10;
	}
	if (off > 0) {
		res /= -dot;
	}
	else {
		res /= dot;
	}
	// STUDIO-PATCH(obj-float-exponent): the mantissa parsed above, scaled in double and clamped to the float range
	// (no inf in the mesh). Without exponent the result is unchanged.
	if (read_float_exp != 0) {
		double d       = (double)res * pow(10.0, read_float_exp);
		read_float_exp = 0;
		return d > FLT_MAX ? FLT_MAX : d < -FLT_MAX ? -FLT_MAX : (float)d;
	}
	return res;
}

// STUDIO-PATCH(obj-float-exponent): "e-3", "E+2"... read up to the end of the token. Saturated at 99, past the
// float range: 10^99 times the mantissa (9 characters at most) stays finite in double, and there is no loop.
static int read_exponent() {
	int  exp    = 0;
	bool neg    = false;
	bool digits = true;
	if (part->pos < bytes_length && (bytes[part->pos] == '-' || bytes[part->pos] == '+')) {
		neg = bytes[part->pos++] == '-';
	}
	while (part->pos < bytes_length) {
		char c = bytes[part->pos];
		if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
			break;
		}
		if (digits && c >= '0' && c <= '9') {
			exp = exp * 10 + (c - 48);
			if (exp > 99) {
				exp = 99;
			}
		}
		else {
			digits = false;
		}
		part->pos++;
	}
	return neg ? -exp : exp;
}

static char *read_string() {
	size_t begin = part->pos;
	while (part->pos < bytes_length) { // STUDIO-PATCH(obj-global-indices)
		char c = bytes[part->pos];
		if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
			break;
		}
		part->pos++;
	}
	size_t len = part->pos - begin < sizeof(str) - 1 ? part->pos - begin : sizeof(str) - 1; // STUDIO-PATCH(obj-global-indices)
	for (int i = 0; i < len; ++i) {
		str[i] = bytes[begin + i];
	}
	str[len] = '\0';
	return str;
}

static void next_line() {
	while (part->pos < bytes_length) {
		char c = bytes[part->pos++];
		if (c == '\n') {
			break; // \n, \r\n
		}
	}
}

// STUDIO-PATCH(udim-import): the tile of a triangle was the one of its uv barycenter, cast with (int): negative uvs
// indexed num[] before its start, u >= 10 ran into the next row, and a triangle over two tiles was folded into one.
// Now each triangle is cut along the tile borders (iron_udim.c), its corners get uvs local to their tile, and a uv
// out of the tiles 1001-1990 is counted in obj_udim_invalid (the importer refuses the file). Include and globals here,
// out of the lines of obj-global-indices and obj-float-exponent at the top of the file (context of their .patch).
#include "iron_udim.h"

int   obj_udim_invalid = 0;
float obj_udim_invalid_uv[2];
int   obj_udim_cut = 0;

typedef struct {
	i32_array_t  pos; // Corners after the cut, into pos_temp / uv_temp / nor_temp
	i32_array_t  uv;
	i32_array_t  nor;
	i32_array_t *tiles; // Tile u + v * UDIM_TILES_U of each triangle
	bool         has_nor;
} obj_udim_t;

static void obj_udim_push_uv(obj_udim_t *s, float u, float v) {
	i32_array_push(&s->uv, uv_temp.length / 2);
	f32_array_push(&uv_temp, u);
	f32_array_push(&uv_temp, v);
}

// A part of a cut triangle: new corners, appended to the temp arrays until the part is packed
static void obj_udim_emit(int tu, int tv, const udim_vert_t tri[3], void *user) {
	obj_udim_t *s = user;
	for (int k = 0; k < 3; ++k) {
		i32_array_push(&s->pos, pos_temp.length / 3);
		f32_array_push(&pos_temp, tri[k].pos[0]);
		f32_array_push(&pos_temp, tri[k].pos[1]);
		f32_array_push(&pos_temp, tri[k].pos[2]);
		obj_udim_push_uv(s, tri[k].uv[0], tri[k].uv[1]);
		if (s->has_nor) {
			i32_array_push(&s->nor, nor_temp.length / 3);
			f32_array_push(&nor_temp, tri[k].nor[0]);
			f32_array_push(&nor_temp, tri[k].nor[1]);
			f32_array_push(&nor_temp, tri[k].nor[2]);
		}
	}
	i32_array_push(s->tiles, tu + tv * UDIM_TILES_U);
}

static void obj_udim_swap(i32_array_t *a, i32_array_t *b) {
	i32_array_t t = *a;
	*a            = *b;
	*b            = t;
}

static void obj_udim_cut_triangles(i32_array_t *pos_indices, i32_array_t *uv_indices, i32_array_t *nor_indices, i32_array_t *tiles) {
	obj_udim_t s = {.tiles = tiles, .has_nor = nor_indices->length > 0};
	for (int i = 0; i + 2 < pos_indices->length; i += 3) {
		udim_vert_t tri[3] = {0};
		for (int k = 0; k < 3; ++k) {
			memcpy(tri[k].pos, &pos_temp.buffer[pos_indices->buffer[i + k] * 3], sizeof(float) * 3);
			memcpy(tri[k].uv, &uv_temp.buffer[uv_indices->buffer[i + k] * 2], sizeof(float) * 2);
			if (s.has_nor) {
				memcpy(tri[k].nor, &nor_temp.buffer[nor_indices->buffer[i + k] * 3], sizeof(float) * 3);
			}
		}
		int tu0, tv0, tu1, tv1;
		if (!udim_triangle_tiles(tri[0].uv, tri[1].uv, tri[2].uv, &tu0, &tv0, &tu1, &tv1)) {
			if (obj_udim_invalid++ == 0) {
				int k                  = udim_uv_valid(tri[0].uv[0], tri[0].uv[1]) ? udim_uv_valid(tri[1].uv[0], tri[1].uv[1]) ? 2 : 1 : 0;
				obj_udim_invalid_uv[0] = tri[k].uv[0];
				obj_udim_invalid_uv[1] = tri[k].uv[1];
			}
			continue;
		}
		if (tu0 == tu1 && tv0 == tv1) { // In one tile: same positions and normals, uvs made local
			for (int k = 0; k < 3; ++k) {
				i32_array_push(&s.pos, pos_indices->buffer[i + k]);
				if (s.has_nor) {
					i32_array_push(&s.nor, nor_indices->buffer[i + k]);
				}
				float u = tri[k].uv[0] - tu0;
				float v = tri[k].uv[1] - tv0;
				obj_udim_push_uv(&s, u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u, v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v);
			}
			i32_array_push(tiles, tu0 + tv0 * UDIM_TILES_U);
		}
		else if (udim_split_triangle(tri, obj_udim_emit, &s) > 1) {
			obj_udim_cut++;
		}
	}
	obj_udim_swap(pos_indices, &s.pos);
	obj_udim_swap(uv_indices, &s.uv);
	obj_udim_swap(nor_indices, &s.nor);
	array_free(&s.pos);
	array_free(&s.uv);
	array_free(&s.nor);
}

// Indices of each tile, in a udims_u x udims_v grid as upstream. The corners are not shared (inda is the identity).
static void obj_udim_tiles(i32_array_t *tiles) {
	int tiles_u = 1;
	int tiles_v = 1;
	for (int i = 0; i < tiles->length; ++i) {
		int tu  = tiles->buffer[i] % UDIM_TILES_U;
		int tv  = tiles->buffer[i] / UDIM_TILES_U;
		tiles_u = tu + 1 > tiles_u ? tu + 1 : tiles_u;
		tiles_v = tv + 1 > tiles_v ? tv + 1 : tiles_v;
	}
	uint32_t *num = calloc(tiles_u * tiles_v, sizeof(uint32_t));
	for (int i = 0; i < tiles->length; ++i) {
		tiles->buffer[i] = tiles->buffer[i] % UDIM_TILES_U + tiles->buffer[i] / UDIM_TILES_U * tiles_u;
		num[tiles->buffer[i]] += 3;
	}
	part->udims   = any_array_create(tiles_u * tiles_v);
	part->udims_u = tiles_u;
	part->udims_v = tiles_v;
	for (int i = 0; i < tiles_u * tiles_v; ++i) {
		part->udims->buffer[i] = u32_array_create(num[i]);
		num[i]                 = 0;
	}
	for (int i = 0; i < tiles->length; ++i) {
		u32_array_t *a = part->udims->buffer[tiles->buffer[i]];
		for (int k = 0; k < 3; ++k) {
			a->buffer[num[tiles->buffer[i]]++] = i * 3 + k;
		}
	}
	free(num);
}

static bool pnpoly(float v0x, float v0y, float v1x, float v1y, float v2x, float v2y, float px, float py) {
	// https://wrf.ecse.rpi.edu//Research/Short_Notes/pnpoly.html
	bool c = false;
	if (((v0y > py) != (v2y > py)) && (px < (v2x - v0x) * (py - v0y) / (v2y - v0y) + v0x)) {
		c = !c;
	}
	if (((v1y > py) != (v0y > py)) && (px < (v0x - v1x) * (py - v1y) / (v0y - v1y) + v1x)) {
		c = !c;
	}
	if (((v2y > py) != (v1y > py)) && (px < (v1x - v2x) * (py - v2y) / (v1y - v2y) + v2x)) {
		c = !c;
	}
	return c;
}

static vec4_t calc_normal(vec4_t a, vec4_t b, vec4_t c) {
	vec4_t cb = vec4_sub(c, b);
	vec4_t ab = vec4_sub(a, b);
	cb        = vec4_cross(cb, ab);
	cb        = vec4_norm(cb);
	return cb;
}

// 'o' for object split, 'g' for groups, 'u'semtl for materials
raw_mesh_t *obj_parse(buffer_t *file_bytes, char split_code, uint64_t start_pos, bool udim) {
	bytes        = file_bytes->buffer;
	bytes_length = file_bytes->length;

	part            = calloc(1, sizeof(raw_mesh_t));
	part->scale_pos = 1.0;
	part->scale_tex = 1.0;
	part->pos       = start_pos;
	part->udims_u   = 1;
	part->udims_v   = 1;
	if (start_pos == 0) { // STUDIO-PATCH(udim-import): no name left over from the previous file for a first object without "o"
		str[0] = '\0';
	}
	obj_udim_invalid = 0; // STUDIO-PATCH(udim-import): counted per part
	obj_udim_cut     = 0;
	part->name       = string_copy(str);

	i32_array_t pos_indices = {0};
	i32_array_t uv_indices  = {0};
	i32_array_t nor_indices = {0};

	bool reading_faces  = false;
	bool reading_object = false;
	check_uvmap         = true;

	// STUDIO-PATCH(obj-global-indices): v / vt / vn are kept from one part to the next, whatever the split: a
	// face may use data declared under a previous object. They are freed by the last part.
	if (start_pos == 0) {
		array_free(&pos_temp);
		array_free(&uv_temp);
		array_free(&nor_temp);
		check_faces = true;
	}
	uint32_t pos_start = pos_temp.length; // First coordinate declared in this part

	while (part->pos < bytes_length) {
		char c0 = bytes[part->pos++];
		if (reading_object && reading_faces && (c0 == 'v' || c0 == split_code)) {
			part->pos--;
			part->has_next = true;
			break;
		}

		if (c0 == 'v') {
			char c1 = part->pos < bytes_length ? bytes[part->pos++] : '\0'; // STUDIO-PATCH(obj-global-indices)
			if (c1 == ' ') {
				skip_whitespace(); // Some exporters put additional space directly after "v"
				f32_array_push(&pos_temp, read_float());
				skip_whitespace();
				f32_array_push(&pos_temp, read_float());
				skip_whitespace();
				f32_array_push(&pos_temp, read_float());
			}
			else if (c1 == 't') {
				skip_whitespace();
				f32_array_push(&uv_temp, read_float());
				skip_whitespace();
				f32_array_push(&uv_temp, read_float());
			}
			else if (c1 == 'n') {
				skip_whitespace();
				f32_array_push(&nor_temp, read_float());
				skip_whitespace();
				f32_array_push(&nor_temp, read_float());
				skip_whitespace();
				f32_array_push(&nor_temp, read_float());
			}
		}
		else if (c0 == 'f') {
			skip_whitespace();
			reading_faces = true;
			vi            = 0;
			read_face();

			if (!check_face()) { // STUDIO-PATCH(obj-global-indices)
				if (check_faces) {
					check_faces = false;
					console_info("Warning: Skipped faces with invalid vertex indices");
				}
			}
			else if (vi <= 4) { // Convex, fan triangulation
				for (int i = 2; i < vi; ++i) {
					push_corner(&pos_indices, &uv_indices, &nor_indices, 0);
					push_corner(&pos_indices, &uv_indices, &nor_indices, i - 1);
					push_corner(&pos_indices, &uv_indices, &nor_indices, i);
				}
				if (check_uvmap && ua[0] == 0 && ua[1] == 0 && ua[2] == 0) { // Blender points faces with no UV to 0.0 (vt index 0)
					check_uvmap = false;
					console_info("Warning: Mesh is not fully UV unwrapped");
				}
			}
			else { // Convex or concave, ear clipping
				float nx = 0.0;
				float ny = 0.0;
				float nz = 0.0;
				if (na[0] >= 0) {
					nx = nor_temp.buffer[na[0] * 3];
					ny = nor_temp.buffer[na[0] * 3 + 1];
					nz = nor_temp.buffer[na[0] * 3 + 2];
				}
				else {
					vec4_t n = calc_normal((vec4_t){pos_temp.buffer[va[0] * 3], pos_temp.buffer[va[0] * 3 + 1], pos_temp.buffer[va[0] * 3 + 2], 1.0f},
					                       (vec4_t){pos_temp.buffer[va[1] * 3], pos_temp.buffer[va[1] * 3 + 1], pos_temp.buffer[va[1] * 3 + 2], 1.0f},
					                       (vec4_t){pos_temp.buffer[va[2] * 3], pos_temp.buffer[va[2] * 3 + 1], pos_temp.buffer[va[2] * 3 + 2], 1.0f});
					nx       = n.x;
					ny       = n.y;
					nz       = n.z;
				}
				float nxabs = (float)fabs(nx);
				float nyabs = (float)fabs(ny);
				float nzabs = (float)fabs(nz);
				bool  flip  = nx + ny + nz > 0;
				int   axis  = nxabs > nyabs && nxabs > nzabs ? 0 : nyabs > nxabs && nyabs > nzabs ? 1 : 2;
				int   axis0 = axis == 0 ? (flip ? 2 : 1) : axis == 1 ? (flip ? 0 : 2) : (flip ? 1 : 0);
				int   axis1 = axis == 0 ? (flip ? 1 : 2) : axis == 1 ? (flip ? 2 : 0) : (flip ? 0 : 1);

				int loops = 0;
				int i     = -1;
				while (vi > 3 && loops++ < vi) {
					i         = (i + 1) % vi;
					int   i1  = (i + 1) % vi;
					int   i2  = (i + 2) % vi;
					int   vi0 = va[i] * 3;
					int   vi1 = va[i1] * 3;
					int   vi2 = va[i2] * 3;
					float v0x = pos_temp.buffer[vi0 + axis0];
					float v0y = pos_temp.buffer[vi0 + axis1];
					float v1x = pos_temp.buffer[vi1 + axis0];
					float v1y = pos_temp.buffer[vi1 + axis1];
					float v2x = pos_temp.buffer[vi2 + axis0];
					float v2y = pos_temp.buffer[vi2 + axis1];

					float e0x   = v0x - v1x; // Not an interior vertex
					float e0y   = v0y - v1y;
					float e1x   = v2x - v1x;
					float e1y   = v2y - v1y;
					float cross = e0x * e1y - e0y * e1x;
					if (cross <= 0) {
						continue;
					}

					bool overlap = false; // Other vertex found inside this triangle
					for (int j = 0; j < vi - 3; ++j) {
						int   j0 = va[(i + 3 + j) % vi] * 3;
						float px = pos_temp.buffer[j0 + axis0];
						float py = pos_temp.buffer[j0 + axis1];
						if (pnpoly(v0x, v0y, v1x, v1y, v2x, v2y, px, py)) {
							overlap = true;
							break;
						}
					}
					if (overlap) {
						continue;
					}

					push_corner(&pos_indices, &uv_indices, &nor_indices, i); // Found ear
					push_corner(&pos_indices, &uv_indices, &nor_indices, i1);
					push_corner(&pos_indices, &uv_indices, &nor_indices, i2);

					for (int j = ((i + 1) % vi); j < vi - 1; ++j) { // Consume vertex
						va[j] = va[j + 1];
						ua[j] = ua[j + 1];
						na[j] = na[j + 1];
					}
					vi--;
					i--;
					loops = 0;
				}
				push_corner(&pos_indices, &uv_indices, &nor_indices, 0); // Last one
				push_corner(&pos_indices, &uv_indices, &nor_indices, 1);
				push_corner(&pos_indices, &uv_indices, &nor_indices, 2);
			}
		}
		else if (c0 == split_code) {
			if (split_code == 'u') {
				part->pos += 5; // "u"semtl
			}
			skip_whitespace();
			reading_object = true; // STUDIO-PATCH(udim-import): udim tiles are split per object too
			free(part->name);
			part->name = string_copy(read_string());
		}
		else if (c0 == '\n') { // Empty line
			continue;
		}
		next_line();
	}

	// STUDIO-PATCH(obj-global-indices): indices stay global, no per-object offset. Faces without uv point to vt 0,
	// like the padding upstream did. Normals are used only if every face has them, else they are computed.
	int uv_missing = 0;
	for (int i = 0; i < uv_indices.length; ++i) {
		if (uv_indices.buffer[i] < 0) {
			uv_indices.buffer[i] = 0;
			uv_missing++;
		}
	}
	if (uv_missing == uv_indices.length) {
		uv_indices.length = 0;
	}
	else if (uv_missing > 0) {
		console_info("Warning: Mesh is not fully UV unwrapped");
	}
	for (int i = 0; i < nor_indices.length; ++i) {
		if (nor_indices.buffer[i] < 0) {
			nor_indices.length = 0;
			break;
		}
	}

	// Pack positions to (-1, 1) range
	// STUDIO-PATCH(obj-global-indices): positions declared in this part (all of them for 'u', as upstream), plus
	// the earlier ones its faces use.
	part->scale_pos    = 0.0;
	uint32_t scale_from = split_code == 'u' ? 0 : pos_start;
	for (int i = scale_from; i < pos_temp.length; ++i) {
		float f = (float)fabs(pos_temp.buffer[i]);
		if (part->scale_pos < f) {
			part->scale_pos = f;
		}
	}
	for (int i = 0; i < pos_indices.length; ++i) {
		for (int j = pos_indices.buffer[i] * 3; j < pos_indices.buffer[i] * 3 + 3 && j < scale_from; ++j) {
			float f = (float)fabs(pos_temp.buffer[j]);
			if (part->scale_pos < f) {
				part->scale_pos = f;
			}
		}
	}
	float inv = 32767 * (1 / part->scale_pos);

	part->posa         = calloc(sizeof(i16_array_t), 1);
	// STUDIO-PATCH(udim-import): corners cut along the tile borders, appended to the temp arrays for the packing below.
	// Cut after the scale: the new corners lie between the old ones. Here, out of the lines of obj-global-indices.
	i32_array_t udim_tiles   = {0};
	uint32_t    udim_pos_len = pos_temp.length;
	uint32_t    udim_uv_len  = uv_temp.length;
	uint32_t    udim_nor_len = nor_temp.length;
	if (udim && uv_indices.length > 0) {
		obj_udim_cut_triangles(&pos_indices, &uv_indices, &nor_indices, &udim_tiles);
	}
	part->posa->length = part->posa->capacity = pos_indices.length * 4;
	part->posa->buffer                        = malloc(part->posa->capacity * sizeof(int16_t));

	part->inda         = calloc(sizeof(u32_array_t), 1);
	part->inda->length = part->inda->capacity = pos_indices.length;
	part->inda->buffer                        = malloc(part->inda->capacity * sizeof(uint32_t));

	part->vertex_count = pos_indices.length;
	part->index_count  = pos_indices.length;
	int inda_length    = pos_indices.length;
	for (int i = 0; i < pos_indices.length; ++i) {
		float *p                      = &pos_temp.buffer[pos_indices.buffer[i] * 3];
		part->posa->buffer[i * 4]     = (int)(p[0] * inv);
		part->posa->buffer[i * 4 + 1] = (int)((obj_parse_y_to_z_up ? -p[2] : p[1]) * inv);
		part->posa->buffer[i * 4 + 2] = (int)((obj_parse_y_to_z_up ? p[1] : p[2]) * inv);
		part->inda->buffer[i]         = i;
	}

	if (nor_indices.length > 0) {
		part->nora         = calloc(sizeof(i16_array_t), 1);
		part->nora->length = part->nora->capacity = nor_indices.length * 2;
		part->nora->buffer                        = malloc(part->nora->capacity * sizeof(int16_t));

		for (int i = 0; i < pos_indices.length; ++i) {
			float *n                      = &nor_temp.buffer[nor_indices.buffer[i] * 3];
			part->nora->buffer[i * 2]     = (int)(n[0] * 32767);
			part->nora->buffer[i * 2 + 1] = (int)((obj_parse_y_to_z_up ? -n[2] : n[1]) * 32767);
			part->posa->buffer[i * 4 + 3] = (int)((obj_parse_y_to_z_up ? n[1] : n[2]) * 32767);
		}
	}
	else {
		// Calc normals
		part->nora         = calloc(sizeof(i16_array_t), 1);
		part->nora->length = part->nora->capacity = inda_length * 2;
		part->nora->buffer                        = malloc(part->nora->capacity * sizeof(int16_t));

		for (int i = 0; i < (int)(inda_length / 3); ++i) {
			int    i1                  = part->inda->buffer[i * 3];
			int    i2                  = part->inda->buffer[i * 3 + 1];
			int    i3                  = part->inda->buffer[i * 3 + 2];
			vec4_t n                   = calc_normal((vec4_t){part->posa->buffer[i1 * 4], part->posa->buffer[i1 * 4 + 1], part->posa->buffer[i1 * 4 + 2], 1.0},
			                                         (vec4_t){part->posa->buffer[i2 * 4], part->posa->buffer[i2 * 4 + 1], part->posa->buffer[i2 * 4 + 2], 1.0},
			                                         (vec4_t){part->posa->buffer[i3 * 4], part->posa->buffer[i3 * 4 + 1], part->posa->buffer[i3 * 4 + 2], 1.0});
			part->nora->buffer[i1 * 2] = (int)(n.x * 32767);
			part->nora->buffer[i1 * 2 + 1] = (int)(n.y * 32767);
			part->posa->buffer[i1 * 4 + 3] = (int)(n.z * 32767);
			part->nora->buffer[i2 * 2]     = (int)(n.x * 32767);
			part->nora->buffer[i2 * 2 + 1] = (int)(n.y * 32767);
			part->posa->buffer[i2 * 4 + 3] = (int)(n.z * 32767);
			part->nora->buffer[i3 * 2]     = (int)(n.x * 32767);
			part->nora->buffer[i3 * 2 + 1] = (int)(n.y * 32767);
			part->posa->buffer[i3 * 4 + 3] = (int)(n.z * 32767);
		}
	}

	if (uv_indices.length > 0) {
		if (udim) { // STUDIO-PATCH(udim-import): see obj_udim_cut_triangles
			obj_udim_tiles(&udim_tiles);
		}

		part->texa         = calloc(sizeof(i16_array_t), 1);
		part->texa->length = part->texa->capacity = uv_indices.length * 2;
		part->texa->buffer                        = malloc(part->texa->capacity * sizeof(int16_t));

		for (int i = 0; i < uv_indices.length; ++i) {
			float uvx = uv_temp.buffer[uv_indices.buffer[i] * 2];
			if (uvx > 1.0) {
				uvx = uvx - (int)(uvx);
			}
			float uvy = uv_temp.buffer[uv_indices.buffer[i] * 2 + 1];
			if (uvy > 1.0) {
				uvy = uvy - (int)(uvy);
			}
			part->texa->buffer[i * 2]     = (int)(uvx * 32767);
			part->texa->buffer[i * 2 + 1] = (int)((1.0 - uvy) * 32767);
		}
	}

	// STUDIO-PATCH(udim-import): the cut corners are dropped, the next part reads the file's own v / vt / vn
	array_free(&udim_tiles);
	pos_temp.length = udim_pos_len;
	uv_temp.length  = udim_uv_len;
	nor_temp.length = udim_nor_len;

	bytes = NULL;
	if (!part->has_next) {
		array_free(&pos_temp);
		array_free(&uv_temp);
		array_free(&nor_temp);
	}

	array_free(&pos_indices);
	array_free(&uv_indices);
	array_free(&nor_indices);

	return part;
}

void obj_destroy(raw_mesh_t *part) {
	// if (part->udims != NULL) {
	// 	for (int i = 0; i < part->udims_u * part->udims_v; ++i) {
	// 		free(part->udims[i]);
	// 	}
	// 	free(part->udims);
	// }

	free(part->posa);
	free(part->nora);
	free(part->texa);
	free(part->inda);
	free(part);
}
