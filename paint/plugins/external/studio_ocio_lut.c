#include "studio_ocio_lut.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Courbe ACEScct (S-2016-001) : cassure linéaire sous 2^-7, log2 au-dessus.
#define CCT_X_BRK 0.0078125f
#define CCT_Y_BRK 0.155251141552511f
#define CCT_A     10.5402377416545f
#define CCT_B     0.0729055341958355f

float studio_ocio_lut_shaper(float x) {
	return x <= CCT_X_BRK ? CCT_A * x + CCT_B : (log2f(x) + 9.72f) / 17.52f;
}

float studio_ocio_lut_shaper_inv(float y) {
	return y <= CCT_Y_BRK ? (y - CCT_B) / CCT_A : exp2f(y * 17.52f - 9.72f);
}

// cols : le plus petit entier dont le carré couvre n ; calcul entier, le shader reçoit cols et rows tels quels.
void studio_ocio_lut_layout(int n, int *cols, int *rows) {
	int c = 1;
	while (c * c < n) {
		c++;
	}
	*cols = c;
	*rows = (n + c - 1) / c;
}

// float → half, arrondi au plus proche pair (F. Giesen, float_to_half_fast3_rtne). Sans infini ni NaN en entrée : le
// bake les a déjà remplacés.
uint16_t studio_ocio_half_from_float(float f) {
	union {
		uint32_t u;
		float    f;
	} v = {.f = f}, f16max = {.u = (127 + 16) << 23}, denorm = {.u = ((127 - 15) + (23 - 10) + 1) << 23};
	uint32_t sign = v.u & 0x80000000u;
	uint16_t o;
	v.u ^= sign;
	if (v.u >= f16max.u) {
		o = v.u > (255u << 23) ? 0x7e00 : 0x7c00;
	}
	else if (v.u < (113u << 23)) { // sous-normal
		v.f += denorm.f;
		o = (uint16_t)(v.u - denorm.u);
	}
	else {
		uint32_t odd = (v.u >> 13) & 1;
		v.u += ((uint32_t)(15 - 127) << 23) + 0xfff;
		v.u += odd;
		o = (uint16_t)(v.u >> 13);
	}
	return (uint16_t)(o | (sign >> 16));
}

float studio_ocio_half_to_float(uint16_t h) {
	uint32_t s = (uint32_t)(h & 0x8000) << 16;
	uint32_t e = (h >> 10) & 0x1f;
	uint32_t m = h & 0x3ff;
	uint32_t bits;
	if (e == 0) {
		if (m == 0) {
			bits = s;
		}
		else {
			e = 127 - 15 + 1;
			while ((m & 0x400) == 0) {
				m <<= 1;
				e--;
			}
			bits = s | (e << 23) | ((m & 0x3ff) << 13);
		}
	}
	else if (e == 31) {
		bits = s | 0x7f800000u | (m << 13);
	}
	else {
		bits = s | ((e + 112) << 23) | (m << 13);
	}
	float f;
	memcpy(&f, &bits, sizeof(f));
	return f;
}

typedef struct {
	float             *rgb;
	int                begin, end; // nœuds
	studio_ocio_lut_fn fn;
	void              *user;
	bool               ok;
} job_t;

static void *run_job(void *p) {
	job_t *j = p;
	j->ok    = j->fn(j->rgb + (size_t)j->begin * 3, j->end - j->begin, j->user);
	return NULL;
}

bool studio_ocio_lut_bake(studio_ocio_lut_t *lut, int n, studio_ocio_lut_fn fn, void *user, int threads) {
	memset(lut, 0, sizeof(*lut));
	if (n < 2 || fn == NULL) {
		return false;
	}
	int    total = n * n * n;
	float *rgb   = malloc(sizeof(float) * 3 * (size_t)total);
	float *axis  = malloc(sizeof(float) * (size_t)n);
	if (rgb == NULL || axis == NULL) {
		free(rgb);
		free(axis);
		return false;
	}
	for (int i = 0; i < n; i++) {
		axis[i] = studio_ocio_lut_shaper_inv((float)i / (float)(n - 1));
	}
	for (int b = 0, k = 0; b < n; b++) {
		for (int g = 0; g < n; g++) {
			for (int r = 0; r < n; r++, k++) {
				rgb[k * 3 + 0] = axis[r];
				rgb[k * 3 + 1] = axis[g];
				rgb[k * 3 + 2] = axis[b];
			}
		}
	}
	free(axis);

	if (threads <= 0) {
		long c  = sysconf(_SC_NPROCESSORS_ONLN);
		threads = c > 0 ? (int)c : 1;
	}
	if (threads > 64) {
		threads = 64;
	}
	job_t     jobs[64];
	pthread_t tids[64];
	bool      started[64] = {false};
	for (int t = 0; t < threads; t++) {
		jobs[t] = (job_t){.rgb = rgb, .begin = (int)((long)total * t / threads), .end = (int)((long)total * (t + 1) / threads),
		                  .fn = fn, .user = user};
		started[t] = t > 0 && pthread_create(&tids[t], NULL, run_job, &jobs[t]) == 0;
	}
	run_job(&jobs[0]);
	bool ok = jobs[0].ok;
	for (int t = 1; t < threads; t++) {
		if (started[t]) {
			pthread_join(tids[t], NULL);
		}
		else { // thread refusé : la tranche est faite ici
			run_job(&jobs[t]);
		}
		ok = ok && jobs[t].ok;
	}
	if (!ok) {
		free(rgb);
		return false;
	}

	studio_ocio_lut_layout(n, &lut->cols, &lut->rows);
	lut->n      = n;
	lut->width  = lut->cols * n;
	lut->height = lut->rows * n;
	lut->texels = calloc((size_t)lut->width * lut->height * 4, sizeof(uint16_t));
	if (lut->texels == NULL) {
		free(rgb);
		memset(lut, 0, sizeof(*lut));
		return false;
	}
	for (int b = 0, k = 0; b < n; b++) {
		int x0 = (b % lut->cols) * n;
		int y0 = (b / lut->cols) * n;
		for (int g = 0; g < n; g++) {
			uint16_t *row = lut->texels + ((size_t)(y0 + g) * lut->width + x0) * 4;
			for (int r = 0; r < n; r++, k++) {
				for (int c = 0; c < 3; c++) {
					float v = rgb[k * 3 + c];
					v       = isfinite(v) ? fminf(fmaxf(v, -65504.0f), 65504.0f) : 0.0f;
					row[r * 4 + c] = studio_ocio_half_from_float(v);
				}
				row[r * 4 + 3] = 0x3c00; // 1.0
			}
		}
	}
	free(rgb);
	return true;
}

void studio_ocio_lut_free(studio_ocio_lut_t *lut) {
	free(lut->texels);
	memset(lut, 0, sizeof(*lut));
}

// Filtrage bilinéaire d'une texture comme le GPU (texels aux centres, coordonnée en texels).
static void bilinear(const studio_ocio_lut_t *lut, float x, float y, float out[3]) {
	float fx = x - 0.5f, fy = y - 0.5f;
	int   ix = (int)floorf(fx), iy = (int)floorf(fy);
	float tx = fx - (float)ix, ty = fy - (float)iy;
	for (int c = 0; c < 3; c++) {
		float v[2][2];
		for (int j = 0; j < 2; j++) {
			for (int i = 0; i < 2; i++) {
				int px  = ix + i < 0 ? 0 : (ix + i >= lut->width ? lut->width - 1 : ix + i);
				int py  = iy + j < 0 ? 0 : (iy + j >= lut->height ? lut->height - 1 : iy + j);
				v[j][i] = studio_ocio_half_to_float(lut->texels[((size_t)py * lut->width + px) * 4 + c]);
			}
		}
		float top = v[0][0] + (v[0][1] - v[0][0]) * tx;
		float bot = v[1][0] + (v[1][1] - v[1][0]) * tx;
		out[c]    = top + (bot - top) * ty;
	}
}

void studio_ocio_lut_sample(const studio_ocio_lut_t *lut, const float in[3], float out[3]) {
	float n = (float)lut->n;
	float s[3];
	for (int c = 0; c < 3; c++) {
		s[c] = fminf(fmaxf(studio_ocio_lut_shaper(in[c]), 0.0f), 1.0f) * (n - 1.0f);
	}
	float b0 = floorf(s[2]);
	float b1 = fminf(b0 + 1.0f, n - 1.0f);
	float fb = s[2] - b0;
	float c0[3], c1[3];
	// Tuile de la tranche b : rangée floor((b + 0.5) / cols), robuste à une division approchée sur le GPU.
	float cols = (float)lut->cols;
	float ty0 = floorf((b0 + 0.5f) / cols), ty1 = floorf((b1 + 0.5f) / cols);
	bilinear(lut, (b0 - ty0 * cols) * n + s[0] + 0.5f, ty0 * n + s[1] + 0.5f, c0);
	bilinear(lut, (b1 - ty1 * cols) * n + s[0] + 0.5f, ty1 * n + s[1] + 0.5f, c1);
	for (int c = 0; c < 3; c++) {
		out[c] = c0[c] + (c1[c] - c0[c]) * fb;
	}
}
