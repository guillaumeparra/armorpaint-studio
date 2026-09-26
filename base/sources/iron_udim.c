// STUDIO-PATCH(udim-import): see iron_udim.h and paint/plugins/external/PATCHES.md

#include "iron_udim.h"

#include <math.h>
#include <string.h>

#define UDIM_VERT_FLOATS (int)(sizeof(udim_vert_t) / sizeof(float))
#define UDIM_POLY_MAX    16 // A triangle clipped by the 4 sides of a tile has 7 vertices at most

int udim_tile_id(int tu, int tv) {
	return 1001 + tu + tv * 10;
}

bool udim_uv_valid(float u, float v) {
	// Written so that nan fails every test
	return u >= -UDIM_EPS && u <= UDIM_TILES_U + UDIM_EPS && v >= -UDIM_EPS && v <= UDIM_TILES_V + UDIM_EPS;
}

// Tiles [t0, t1] covered by the interval [lo, hi]: a bound within UDIM_EPS of a border stays in the inner tile
static void udim_range(float lo, float hi, int max, int *t0, int *t1) {
	int a = (int)floorf(lo + UDIM_EPS);
	int b = (int)ceilf(hi - UDIM_EPS) - 1;
	if (b < a) { // Interval narrower than the tolerance, on a border: tile of its middle
		a = b = (int)floorf((lo + hi) * 0.5f);
	}
	*t0 = a < 0 ? 0 : a >= max ? max - 1 : a;
	*t1 = b < 0 ? 0 : b >= max ? max - 1 : b;
}

bool udim_triangle_tiles(const float *uv0, const float *uv1, const float *uv2, int *tu0, int *tv0, int *tu1, int *tv1) {
	if (!udim_uv_valid(uv0[0], uv0[1]) || !udim_uv_valid(uv1[0], uv1[1]) || !udim_uv_valid(uv2[0], uv2[1])) {
		return false;
	}
	udim_range(fminf(uv0[0], fminf(uv1[0], uv2[0])), fmaxf(uv0[0], fmaxf(uv1[0], uv2[0])), UDIM_TILES_U, tu0, tu1);
	udim_range(fminf(uv0[1], fminf(uv1[1], uv2[1])), fmaxf(uv0[1], fmaxf(uv1[1], uv2[1])), UDIM_TILES_V, tv0, tv1);
	return true;
}

static float udim_clamp01(float f) {
	return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}

static void udim_emit_local(const udim_vert_t *a, const udim_vert_t *b, const udim_vert_t *c, int tu, int tv, bool renormalize, udim_emit_t emit, void *user) {
	udim_vert_t tri[3] = {*a, *b, *c};
	for (int k = 0; k < 3; ++k) {
		tri[k].uv[0] = udim_clamp01(tri[k].uv[0] - tu);
		tri[k].uv[1] = udim_clamp01(tri[k].uv[1] - tv);
		if (renormalize) {
			float *n = tri[k].nor;
			float  l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			if (l > 0.0f) {
				n[0] /= l;
				n[1] /= l;
				n[2] /= l;
			}
		}
	}
	emit(tu, tv, tri, user);
}

// Sutherland-Hodgman: keeps the part of the polygon where side * (uv[axis] - bound) <= 0
static int udim_clip(const udim_vert_t *in, int n, udim_vert_t *out, int axis, float bound, float side) {
	int m = 0;
	for (int i = 0; i < n; ++i) {
		const udim_vert_t *a  = &in[i];
		const udim_vert_t *b  = &in[(i + 1) % n];
		float              da = side * (a->uv[axis] - bound);
		float              db = side * (b->uv[axis] - bound);
		if (da <= 0.0f) {
			out[m++] = *a;
		}
		if ((da < 0.0f && db > 0.0f) || (da > 0.0f && db < 0.0f)) {
			float        t  = da / (da - db);
			float       *o  = (float *)&out[m++];
			const float *pa = (const float *)a;
			const float *pb = (const float *)b;
			for (int k = 0; k < UDIM_VERT_FLOATS; ++k) {
				o[k] = pa[k] + (pb[k] - pa[k]) * t;
			}
			out[m - 1].uv[axis] = bound; // Exactly on the border
		}
	}
	return m;
}

static float udim_uv_area2(const udim_vert_t *a, const udim_vert_t *b, const udim_vert_t *c) {
	return (b->uv[0] - a->uv[0]) * (c->uv[1] - a->uv[1]) - (c->uv[0] - a->uv[0]) * (b->uv[1] - a->uv[1]);
}

int udim_split_triangle(const udim_vert_t tri[3], udim_emit_t emit, void *user) {
	int tu0, tv0, tu1, tv1;
	if (!udim_triangle_tiles(tri[0].uv, tri[1].uv, tri[2].uv, &tu0, &tv0, &tu1, &tv1)) {
		return 0;
	}
	// One tile, or no uv area to cut (collapsed uvs keep their geometry, in the tile of their middle)
	float area = fabsf(udim_uv_area2(&tri[0], &tri[1], &tri[2]));
	if ((tu0 == tu1 && tv0 == tv1) || area < 1e-12f) {
		float u  = (tri[0].uv[0] + tri[1].uv[0] + tri[2].uv[0]) / 3.0f;
		float v  = (tri[0].uv[1] + tri[1].uv[1] + tri[2].uv[1]) / 3.0f;
		int   tu = tu0 == tu1 ? tu0 : (int)floorf(u);
		int   tv = tv0 == tv1 ? tv0 : (int)floorf(v);
		tu       = tu < tu0 ? tu0 : tu > tu1 ? tu1 : tu;
		tv       = tv < tv0 ? tv0 : tv > tv1 ? tv1 : tv;
		udim_emit_local(&tri[0], &tri[1], &tri[2], tu, tv, false, emit, user);
		return 1;
	}

	int tiles = 0;
	for (int tv = tv0; tv <= tv1; ++tv) {
		for (int tu = tu0; tu <= tu1; ++tu) {
			udim_vert_t a[UDIM_POLY_MAX];
			udim_vert_t b[UDIM_POLY_MAX];
			int         n = 3;
			memcpy(a, tri, sizeof(udim_vert_t) * 3);
			n            = udim_clip(a, n, b, 0, (float)tu, -1.0f);
			n            = udim_clip(b, n, a, 0, (float)(tu + 1), 1.0f);
			n            = udim_clip(a, n, b, 1, (float)tv, -1.0f);
			n            = udim_clip(b, n, a, 1, (float)(tv + 1), 1.0f);
			bool emitted = false;
			for (int i = 2; i < n; ++i) { // Fan, same winding as the triangle
				if (fabsf(udim_uv_area2(&a[0], &a[i - 1], &a[i])) <= area * 1e-6f) {
					continue; // Sliver along a border
				}
				udim_emit_local(&a[0], &a[i - 1], &a[i], tu, tv, true, emit, user);
				emitted = true;
			}
			tiles += emitted ? 1 : 0;
		}
	}
	return tiles;
}
