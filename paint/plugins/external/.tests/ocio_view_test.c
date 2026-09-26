// Banc du lot A2, hors de l'app, sous ASan/UBSan : LUT 3D du viewport (studio_ocio_lut.c) et transformation
// d'affichage du module (studio_ocio_display_transform). Lancé par ocio.sh.
//
//   ocio_view_test unit               half, shaper, disposition, interpolation exacte d'une LUT linéaire
//   ocio_view_test accuracy [<n>]     pour la configuration de $OCIO : chaque view du display par défaut et la view
//                                     par défaut des autres displays, LUT (n nœuds, défaut STUDIO_OCIO_LUT_SIZE)
//                                     comparée à OCIO exact après écrêtage à [0, 1] ; écarts en 1/255
//
// Tolérances de accuracy (mesurées sur les configurations ACES studio et CG d'OCIO 2.5.2, n = 129), vues SDR : gris
// ≤ 0.5/255, 99,9 % des pixels ≤ 4.5/255, maximum ≤ 18/255 (sRGB ACES 2.0 : 2.3 et 8.9 ; P3-D65 : 4.0 et 17.5 ;
// Rec.1886 : 3.4 et 14.2). Les écarts maximaux viennent des couleurs saturées au bord du gamut de la vue ACES 2.0
// (compression de gamut dépendante de la teinte), que l'interpolation d'une LUT lisse. Vues HDR (encodage
// hdr-video ou edr-video) : mesurées, sans tolérance ; le viewport est un affichage SDR 8 bits.

#include "../studio_ocio.h"
#include "../studio_ocio_lut.h"
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails;
static int checks;

void iron_log(const char *format, ...) {
	va_list ap;
	va_start(ap, format);
	if (getenv("OCIO_TEST_VERBOSE") != NULL) {
		fprintf(stderr, "  [log] ");
		vfprintf(stderr, format, ap);
		fprintf(stderr, "\n");
	}
	va_end(ap);
}

char *path_data(void) {
	return "./";
}

#define CHECK(cond, ...)                                                                                               \
	do {                                                                                                               \
		checks++;                                                                                                      \
		if (!(cond)) {                                                                                                 \
			fails++;                                                                                                   \
			fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__);                                                     \
			fprintf(stderr, __VA_ARGS__);                                                                              \
			fprintf(stderr, "\n");                                                                                     \
		}                                                                                                              \
	} while (0)

static uint64_t rng = 0x9e3779b97f4a7c15ull;

static float frand(void) { // [0, 1)
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (float)(rng >> 40) / (float)(1ull << 24);
}

// --- unit ---

static bool shaper_fn(float *rgb, int count, void *user) {
	for (int i = 0; i < count * 3; i++) {
		rgb[i] = studio_ocio_lut_shaper(rgb[i]);
	}
	return true;
}

static bool failing_fn(float *rgb, int count, void *user) {
	return false;
}

static int mode_unit(void) {
	// half : aller-retour de chaque half fini, puis l'arrondi comparé au type _Float16 du compilateur.
	int bad = 0;
	for (uint32_t h = 0; h < 0x10000; h++) {
		if ((h & 0x7c00) == 0x7c00) {
			continue; // infini, NaN
		}
		float f = studio_ocio_half_to_float((uint16_t)h);
		bad += studio_ocio_half_from_float(f) != h;
	}
	CHECK(bad == 0, "half round trip: %d mismatches", bad);
#ifdef __FLT16_MAX__
	bad = 0;
	for (int i = 0; i < 2000000; i++) {
		uint32_t bits = (uint32_t)(rng >> 32);
		frand();
		float f;
		memcpy(&f, &bits, sizeof(f));
		if (!isfinite(f) || fabsf(f) > 65504.0f) {
			continue;
		}
		_Float16 ref = (_Float16)f;
		uint16_t rb;
		memcpy(&rb, &ref, sizeof(rb));
		bad += studio_ocio_half_from_float(f) != rb;
	}
	CHECK(bad == 0, "float -> half vs _Float16: %d mismatches", bad);
#endif

	// Shaper : inverses l'un de l'autre, continu à la cassure, [0, 1] ↔ [-0.0069, 222.86].
	float worst = 0.0f;
	for (int i = 0; i <= 100000; i++) {
		float y = (float)i / 100000.0f;
		float x = studio_ocio_lut_shaper_inv(y);
		worst   = fmaxf(worst, fabsf(studio_ocio_lut_shaper(x) - y));
	}
	CHECK(worst < 2e-6f, "shaper(inv(y)) - y: %g", worst);
	CHECK(fabsf(studio_ocio_lut_shaper(0.0078125f) - studio_ocio_lut_shaper(0.0078126f)) < 1e-5f, "shaper break");
	CHECK(fabsf(studio_ocio_lut_shaper_inv(1.0f) - 222.861f) < 0.01f, "shaper top: %g", studio_ocio_lut_shaper_inv(1.0f));
	CHECK(studio_ocio_lut_shaper(0.0f) > 0.07f && studio_ocio_lut_shaper_inv(0.0f) < 0.0f, "shaper toe");

	// Disposition : la grille couvre les n tranches sans rangée vide ; 129 tient dans une texture de 16384.
	for (int n = 2; n <= 256; n++) {
		int c, r;
		studio_ocio_lut_layout(n, &c, &r);
		CHECK(c * r >= n && c * (r - 1) < n && c >= r, "layout %d: %d x %d", n, c, r);
	}
	int c, r;
	studio_ocio_lut_layout(129, &c, &r);
	CHECK(c == 12 && r == 11, "layout 129: %d x %d", c, r);

	// Une LUT dont la sortie est la coordonnée mise en forme est linéaire par morceaux dans ses indices : l'interpolation
	// doit la restituer, à l'arrondi half près. Vérifie la disposition des tuiles et l'émulation du shader.
	int sizes[] = {2, 17, 33, 129};
	for (int k = 0; k < 4; k++) {
		studio_ocio_lut_t lut;
		bool              ok = studio_ocio_lut_bake(&lut, sizes[k], shaper_fn, NULL, k); // 0 thread : les cœurs
		CHECK(ok, "bake %d", sizes[k]);
		if (!ok) {
			continue;
		}
		float err = 0.0f;
		for (int i = 0; i < 200000; i++) {
			float in[3], out[3];
			for (int j = 0; j < 3; j++) {
				in[j] = studio_ocio_lut_shaper_inv(frand() * 1.1f - 0.05f); // un peu hors du domaine : bornes
			}
			studio_ocio_lut_sample(&lut, in, out);
			for (int j = 0; j < 3; j++) {
				float want = fminf(fmaxf(studio_ocio_lut_shaper(in[j]), 0.0f), 1.0f);
				err        = fmaxf(err, fabsf(out[j] - want));
			}
		}
		CHECK(err < 1e-3f, "linear LUT %d: max err %g", sizes[k], err);
		CHECK(lut.width == lut.cols * sizes[k] && lut.height == lut.rows * sizes[k], "size");
		studio_ocio_lut_free(&lut);
	}
	studio_ocio_lut_t lut;
	CHECK(!studio_ocio_lut_bake(&lut, 17, failing_fn, NULL, 4) && lut.texels == NULL, "failing transform");
	CHECK(!studio_ocio_lut_bake(&lut, 1, shaper_fn, NULL, 1), "n = 1");
	printf("  ok   unit (%d checks)\n", checks);
	return 0;
}

// --- accuracy ---

typedef struct {
	const char *display, *view;
} dv_t;

static const char *working;

static bool display_fn(float *rgb, int count, void *user) {
	const dv_t *dv = user;
	return studio_ocio_display_transform(rgb, count, 1, 3, working, dv->display, dv->view, NULL);
}

static int cmp_float(const void *a, const void *b) {
	float x = *(const float *)a, y = *(const float *)b;
	return (x > y) - (x < y);
}

#define SAMPLES 100000

// Écarts (en 1/255, après écrêtage à [0, 1]) de la LUT sur un jeu de pixels : max et 99,9e centile.
static void measure(const studio_ocio_lut_t *lut, const dv_t *dv, float *px, float *exact, float *errs, float *max,
                    float *p999) {
	memcpy(exact, px, sizeof(float) * 3 * SAMPLES);
	bool ok = studio_ocio_display_transform(exact, SAMPLES, 1, 3, working, dv->display, dv->view, NULL);
	CHECK(ok, "exact transform %s / %s", dv->display, dv->view);
	for (int i = 0; i < SAMPLES; i++) {
		float out[3];
		studio_ocio_lut_sample(lut, px + i * 3, out);
		float e = 0.0f;
		for (int c = 0; c < 3; c++) {
			float a = fminf(fmaxf(out[c], 0.0f), 1.0f), b = fminf(fmaxf(exact[i * 3 + c], 0.0f), 1.0f);
			e       = fmaxf(e, fabsf(a - b) * 255.0f);
		}
		errs[i] = isfinite(e) ? e : 1e9f;
	}
	qsort(errs, SAMPLES, sizeof(float), cmp_float);
	*max  = errs[SAMPLES - 1];
	*p999 = errs[(int)(SAMPLES * 0.999)];
}

static int mode_accuracy(int n) {
	CHECK(studio_ocio_reload(NULL), "config: %s", studio_ocio_status());
	working = studio_ocio_working_space();
	CHECK(working != NULL, "working space: %s", studio_ocio_working_space_error());
	if (fails > 0) {
		return 1;
	}
	printf("  %s, LUT %d^3\n", studio_ocio_config_name(), n);

	// Jeux de pixels (espace de travail) : gris HDR, cube aléatoire saturé, log-uniforme sur 19 stops, albédos éclairés.
	enum { SETS = 4 };
	const char *set_names[SETS] = {"greys", "cube", "log", "albedo"};
	float      *sets[SETS];
	for (int k = 0; k < SETS; k++) {
		sets[k] = malloc(sizeof(float) * 3 * SAMPLES);
	}
	for (int i = 0; i < SAMPLES; i++) {
		float g   = 16.0f * (float)i / (float)(SAMPLES - 1);
		float lit = frand() * 3.0f;
		for (int c = 0; c < 3; c++) {
			sets[0][i * 3 + c] = g;
			float u            = frand();
			sets[1][i * 3 + c] = u * u * u * 4.0f;
			sets[2][i * 3 + c] = exp2f(frand() * 19.0f - 12.0f);
			sets[3][i * 3 + c] = powf(frand(), 2.2f) * lit;
		}
	}
	float *exact = malloc(sizeof(float) * 3 * SAMPLES);
	float *errs  = malloc(sizeof(float) * SAMPLES);

	// Views : toutes celles du display par défaut, la view par défaut des autres displays.
	dv_t        dvs[256];
	int         count = 0;
	const char *d0    = studio_ocio_display_default();
	for (int i = 0; i < studio_ocio_view_count(d0) && count < 256; i++) {
		dvs[count++] = (dv_t){d0, studio_ocio_view_name(d0, i)};
	}
	for (int i = 0; i < studio_ocio_display_count() && count < 256; i++) {
		const char *d = studio_ocio_display_name(i);
		if (strcmp(d, d0) != 0) {
			dvs[count++] = (dv_t){d, studio_ocio_view_default(d)};
		}
	}

	for (int k = 0; k < count; k++) {
		studio_ocio_lut_t lut;
		struct timespec   t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		bool ok = studio_ocio_lut_bake(&lut, n, display_fn, &dvs[k], 0);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		CHECK(ok, "bake %s / %s", dvs[k].display, dvs[k].view);
		if (!ok) {
			continue;
		}
		int hdr = studio_ocio_view_is_hdr(dvs[k].display, dvs[k].view);
		CHECK(hdr >= 0, "encoding of %s / %s", dvs[k].display, dvs[k].view);
		double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
		printf("    %-24.24s %-38.38s %5.0f ms", dvs[k].display, dvs[k].view, ms);
		float worst_max = 0.0f, worst_p = 0.0f;
		for (int s = 0; s < SETS; s++) {
			float mx, p;
			measure(&lut, &dvs[k], sets[s], exact, errs, &mx, &p);
			printf(" | %s %.2f/%.2f", set_names[s], mx, p);
			if (s == 0 && !hdr) {
				CHECK(mx <= 0.5f, "%s / %s: greys max %.2f/255", dvs[k].display, dvs[k].view, mx);
			}
			worst_max = fmaxf(worst_max, mx);
			worst_p   = fmaxf(worst_p, p);
		}
		printf("%s\n", hdr ? " (HDR)" : "");
		if (!hdr) {
			CHECK(worst_p <= 4.5f, "%s / %s: 99.9%% %.2f/255", dvs[k].display, dvs[k].view, worst_p);
			CHECK(worst_max <= 18.0f, "%s / %s: max %.2f/255", dvs[k].display, dvs[k].view, worst_max);
		}
		studio_ocio_lut_free(&lut);
	}
	CHECK(count > 1, "views: %d", count);

	// Le module refuse un display ou une view inconnus, sans planter, et un look inconnu aussi.
	float px[3] = {0.18f, 0.18f, 0.18f};
	CHECK(!studio_ocio_display_transform(px, 1, 1, 3, working, "no such display", "x", NULL), "unknown display");
	CHECK(!studio_ocio_display_transform(px, 1, 1, 3, working, d0, "no such view", NULL), "unknown view");
	CHECK(!studio_ocio_display_transform(px, 1, 1, 3, working, d0, studio_ocio_view_default(d0), "no such look"),
	      "unknown look");
	CHECK(studio_ocio_display_transform_id(working, d0, studio_ocio_view_default(d0), NULL) != NULL, "transform id");
	CHECK(studio_ocio_display_transform_id(working, d0, "no such view", NULL) == NULL, "transform id of an unknown view");

	for (int k = 0; k < SETS; k++) {
		free(sets[k]);
	}
	free(exact);
	free(errs);
	studio_ocio_shutdown();
	printf("  ok   accuracy (%d views, %d checks)\n", count, checks);
	return fails > 0;
}

int main(int argc, char **argv) {
	int r = 2;
	if (argc >= 2 && strcmp(argv[1], "unit") == 0) {
		r = mode_unit();
	}
	else if (argc >= 2 && strcmp(argv[1], "accuracy") == 0) {
		r = mode_accuracy(argc > 2 ? atoi(argv[2]) : STUDIO_OCIO_LUT_SIZE);
	}
	if (r == 2) {
		fprintf(stderr, "usage: see ocio_view_test.c\n");
		return 2;
	}
	if (fails > 0) {
		fprintf(stderr, "  %d/%d checks failed\n", fails, checks);
		return 1;
	}
	return 0;
}
