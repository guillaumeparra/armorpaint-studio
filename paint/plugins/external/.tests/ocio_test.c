// Banc du lot A1, hors de l'app, sous ASan/UBSan : studio_ocio.c et l'ABI de libstudio_ocio (studio_ocio_lib.h).
// Lancé par ocio.sh, qui construit deux variantes : liaison statique (celle de l'app sur macOS) et dlopen.
//
//   ocio_test ref <référence>          compare à PyOpenColorIO (ocio_ref.py) pour la configuration de $OCIO
//   ocio_test errors                   entrées invalides, au niveau du module et de l'ABI
//   ocio_test threads                  transformations parallèles, cache sous contention et éviction
//   ocio_test reload <config>          rechargements successifs (défaut, <config>, invalide, défaut)
//   ocio_test unavailable <extrait>    OCIO indisponible : status contient <extrait>, rien ne plante
//
// Tolérance de ref : 0 ULP par défaut, OCIO_TEST_ULPS pour la relâcher (voir ocio.sh, « tolérance »).

#include "../studio_ocio.h"
#include "../studio_ocio_lib.h"
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
static int checks;
static int log_count;

void iron_log(const char *format, ...) {
	va_list ap;
	va_start(ap, format);
	if (getenv("OCIO_TEST_VERBOSE") != NULL) {
		fprintf(stderr, "  [log] ");
		vfprintf(stderr, format, ap);
		fprintf(stderr, "\n");
	}
	va_end(ap);
	log_count++;
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

static bool same(const char *a, const char *b) {
	return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static const char *s(const char *a) {
	return a != NULL ? a : "(null)";
}

// ── ref ────────────────────────────────────────────────────────────────────────────────────────────────────────────

#define MAX_PX 64
static float pixels[MAX_PX * 4];
static int   npixels;
static long  max_ulps_seen;
static long  tolerance;
static int   exact;
static int   compared;

static long ulps(float a, float b) {
	if (isnan(a) || isnan(b)) {
		return isnan(a) && isnan(b) ? 0 : LONG_MAX;
	}
	int32_t ia, ib;
	memcpy(&ia, &a, 4);
	memcpy(&ib, &b, 4);
	// Ordre lexicographique des flottants : négatifs retournés, pour que la différence compte les flottants entre a et b.
	int64_t oa = ia < 0 ? (int64_t)INT32_MIN - ia : ia;
	int64_t ob = ib < 0 ? (int64_t)INT32_MIN - ib : ib;
	int64_t d  = oa > ob ? oa - ob : ob - oa;
	return (long)d;
}

static int parse_floats(char *line, float *out, int max) {
	int   n = 0;
	char *p = line;
	while (n < max) {
		char *end;
		float v = strtof(p, &end);
		if (end == p) {
			break;
		}
		out[n++] = v;
		p        = end;
	}
	return n;
}

// Compare got (RGBA) à expected ; channels 3 : compare RGB d'un calcul fait en RGB.
static void compare(const char *what, const float *got, const float *expected, int channels) {
	for (int i = 0; i < npixels; i++) {
		for (int c = 0; c < 3; c++) {
			long u = ulps(got[i * channels + c], expected[i * 4 + c]);
			compared++;
			exact += u == 0;
			if (u > max_ulps_seen && u != LONG_MAX) {
				max_ulps_seen = u;
			}
			CHECK(u <= tolerance, "%s pixel %d channel %d: %.9g, expected %.9g (%ld ulps)", what, i, c,
			      got[i * channels + c], expected[i * 4 + c], u);
		}
		if (channels == 4) {
			CHECK(got[i * 4 + 3] == pixels[i * 4 + 3], "%s pixel %d: alpha changed", what, i);
		}
	}
}

static studio_ocio_lib_config_t *lib_config;

static void ref_xform(char **f, int nf) {
	float expected[MAX_PX * 4];
	CHECK(nf == 4 && parse_floats(f[3], expected, MAX_PX * 4) == npixels * 4, "bad xform line");
	float rgba[MAX_PX * 4];
	float rgb[MAX_PX * 3];
	memcpy(rgba, pixels, sizeof(float) * npixels * 4);
	for (int i = 0; i < npixels; i++) {
		memcpy(rgb + i * 3, pixels + i * 4, sizeof(float) * 3);
	}
	char what[512];
	snprintf(what, sizeof(what), "%s -> %s", f[1], f[2]);
	CHECK(studio_ocio_transform(rgba, npixels, 1, 4, f[1], f[2]), "%s: transform failed", what);
	compare(what, rgba, expected, 4);
	CHECK(studio_ocio_transform(rgb, npixels, 1, 3, f[1], f[2]), "%s: RGB transform failed", what);
	compare(what, rgb, expected, 3);
}

static void ref_display_xform(char **f, int nf) {
	float expected[MAX_PX * 4];
	CHECK(nf == 7 && parse_floats(f[6], expected, MAX_PX * 4) == npixels * 4, "bad display_xform line");
	studio_ocio_lib_proc_t *p = studio_ocio_lib_proc_display(lib_config, f[1], f[2], f[3], f[5], atoi(f[4]));
	char                    what[512];
	snprintf(what, sizeof(what), "%s -> %s/%s look '%s'%s", f[1], f[2], f[3], f[5], atoi(f[4]) ? " inverse" : "");
	CHECK(p != NULL, "%s: %s", what, studio_ocio_lib_error());
	if (p == NULL) {
		return;
	}
	float rgba[MAX_PX * 4];
	memcpy(rgba, pixels, sizeof(float) * npixels * 4);
	CHECK(studio_ocio_lib_apply(p, rgba, npixels, 1, 4, 0) == 1, "%s: apply failed", what);
	compare(what, rgba, expected, 4);
	studio_ocio_lib_proc_free(p);
}

// Les listes de la configuration, dans l'ordre de la référence.
typedef struct {
	const char *kind;
	int (*count)(void);
	const char *(*name)(int);
	int seen;
} list_t;

static int mode_ref(const char *path) {
	const char *t = getenv("OCIO_TEST_ULPS");
	tolerance     = t != NULL ? atol(t) : 0;
	studio_ocio_init();
	CHECK(studio_ocio_ready(), "not ready: %s", studio_ocio_status());
	if (!studio_ocio_ready()) {
		return 1;
	}
	lib_config = studio_ocio_lib_config_load(studio_ocio_config_source());
	CHECK(lib_config != NULL, "lib config: %s", studio_ocio_lib_error());

	FILE *fh = fopen(path, "r");
	if (fh == NULL) {
		fprintf(stderr, "cannot read %s\n", path);
		return 1;
	}
	list_t lists[] = {{"cs", studio_ocio_colorspace_count, studio_ocio_colorspace_name, 0},
	                  {"display", studio_ocio_display_count, studio_ocio_display_name, 0},
	                  {"look", studio_ocio_look_count, studio_ocio_look_name, 0},
	                  {"role", studio_ocio_role_count, studio_ocio_role_name, 0}};
	char        display[256] = "";
	int         view_index   = 0;
	static char line[1 << 16];
	while (fgets(line, sizeof(line), fh) != NULL) {
		line[strcspn(line, "\n")] = '\0';
		char *f[8];
		int   nf = 0;
		for (char *p = line; nf < 8;) {
			f[nf++]   = p;
			char *tab = strchr(p, '\t');
			if (tab == NULL) {
				break;
			}
			*tab = '\0';
			p    = tab + 1;
		}
		if (strcmp(f[0], "version") == 0) {
			CHECK(same(studio_ocio_version(), f[1]), "version %s, reference %s", studio_ocio_version(), f[1]);
		}
		else if (strcmp(f[0], "config_name") == 0) {
			CHECK(same(studio_ocio_config_name(), f[1]), "config name %s, expected %s", s(studio_ocio_config_name()), f[1]);
		}
		else if (strcmp(f[0], "pixels") == 0) {
			npixels = parse_floats(f[1], pixels, MAX_PX * 4) / 4;
		}
		else if (strcmp(f[0], "resolve") == 0) {
			CHECK(same(studio_ocio_resolve(f[1]), f[2]), "resolve %s: %s, expected %s", f[1], s(studio_ocio_resolve(f[1])), f[2]);
		}
		else if (strcmp(f[0], "default_display") == 0) {
			CHECK(same(studio_ocio_display_default(), f[1]), "default display %s", s(studio_ocio_display_default()));
		}
		else if (strcmp(f[0], "default_view") == 0) {
			CHECK(same(studio_ocio_view_default(f[1]), f[2]), "default view of %s: %s", f[1], s(studio_ocio_view_default(f[1])));
		}
		else if (strcmp(f[0], "view") == 0) {
			if (strcmp(display, f[1]) != 0) {
				snprintf(display, sizeof(display), "%s", f[1]);
				view_index = 0;
			}
			CHECK(same(studio_ocio_view_name(f[1], view_index), f[2]), "view %d of %s: %s, expected %s", view_index, f[1],
			      s(studio_ocio_view_name(f[1], view_index)), f[2]);
			view_index++;
			CHECK(studio_ocio_view_count(f[1]) >= view_index, "view count of %s", f[1]);
		}
		else if (strcmp(f[0], "from_path") == 0) {
			CHECK(same(studio_ocio_colorspace_from_path(f[1]), f[2]), "from_path %s: %s, expected %s", f[1],
			      s(studio_ocio_colorspace_from_path(f[1])), f[2]);
		}
		else if (strcmp(f[0], "xform") == 0) {
			ref_xform(f, nf);
		}
		else if (strcmp(f[0], "display_xform") == 0) {
			ref_display_xform(f, nf);
		}
		else {
			for (int i = 0; i < 4; i++) {
				list_t *l = &lists[i];
				if (strcmp(f[0], l->kind) != 0) {
					continue;
				}
				CHECK(same(l->name(l->seen), f[1]), "%s %d: %s, expected %s", l->kind, l->seen, s(l->name(l->seen)), f[1]);
				if (i == 0) { // cs : isData, encoding, famille
					CHECK(studio_ocio_is_data(f[1]) == atoi(f[2]), "is_data %s", f[1]);
					CHECK(same(studio_ocio_encoding(f[1]), f[3]), "encoding %s: %s, expected %s", f[1], s(studio_ocio_encoding(f[1])), f[3]);
					CHECK(same(studio_ocio_family(f[1]), nf > 4 ? f[4] : ""), "family %s: %s", f[1], s(studio_ocio_family(f[1])));
				}
				l->seen++;
			}
		}
	}
	fclose(fh);
	for (int i = 0; i < 4; i++) {
		CHECK(lists[i].count() == lists[i].seen, "%s: %d in the config, %d in the reference", lists[i].kind,
		      lists[i].count(), lists[i].seen);
	}
	printf("  %s: %d colorspaces, %d values compared, %d exact, max %ld ulps (tolerance %ld)\n", s(studio_ocio_config_name()),
	       lists[0].seen, compared, exact, max_ulps_seen, tolerance);
	studio_ocio_lib_config_free(lib_config);
	studio_ocio_shutdown();
	return 0;
}

// ── errors ─────────────────────────────────────────────────────────────────────────────────────────────────────────

static bool has_error(void) {
	return studio_ocio_lib_error()[0] != '\0';
}

static int mode_errors(void) {
	studio_ocio_init();
	CHECK(studio_ocio_ready(), "not ready: %s", studio_ocio_status());
	float px[8] = {0.18f, 0.18f, 0.18f, 1, 1, 1, 1, 1};

	// Module
	CHECK(!studio_ocio_transform(NULL, 1, 1, 4, "ACEScg", "sRGB - Texture"), "NULL pixels accepted");
	CHECK(!studio_ocio_transform(px, 0, 1, 4, "ACEScg", "sRGB - Texture"), "width 0 accepted");
	CHECK(!studio_ocio_transform(px, 1, -1, 4, "ACEScg", "sRGB - Texture"), "height -1 accepted");
	CHECK(!studio_ocio_transform(px, 2, 1, 2, "ACEScg", "sRGB - Texture"), "2 channels accepted");
	CHECK(!studio_ocio_transform(px, 1, 1, 5, "ACEScg", "sRGB - Texture"), "5 channels accepted");
	CHECK(!studio_ocio_transform(px, 1, 1, 4, NULL, "sRGB - Texture"), "NULL src accepted");
	CHECK(!studio_ocio_transform(px, 1, 1, 4, "ACEScg", NULL), "NULL dst accepted");
	int before = log_count;
	CHECK(!studio_ocio_transform(px, 1, 1, 4, "nope", "ACEScg"), "unknown src accepted");
	CHECK(!studio_ocio_transform(px, 1, 1, 4, "nope", "ACEScg"), "unknown src accepted");
	CHECK(log_count == before + 1, "unknown pair logged %d times, expected once", log_count - before);
	CHECK(px[0] == 0.18f && px[3] == 1, "pixels changed by a failed transform");
	CHECK(studio_ocio_transform(px, 1, 1, 4, "ACEScg", "lin_ap1"), "alias to itself failed");
	CHECK(px[0] == 0.18f, "identity changed the pixels");
	CHECK(studio_ocio_transform(px, 1, 1, 4, "scene_linear", "ACEScg"), "role to itself failed");
	CHECK(studio_ocio_transform_id("ACEScg", "sRGB - Texture") != NULL, "no transform id");
	CHECK(studio_ocio_transform_id("nope", "ACEScg") == NULL, "transform id of an unknown pair");
	CHECK(studio_ocio_transform_id(NULL, "ACEScg") == NULL, "transform id with NULL");
	CHECK(studio_ocio_resolve(NULL) == NULL && studio_ocio_resolve("") == NULL && studio_ocio_resolve("nope") == NULL,
	      "resolve of an invalid name");
	CHECK(studio_ocio_is_data("nope") == -1 && studio_ocio_is_data(NULL) == -1, "is_data of an invalid name");
	CHECK(studio_ocio_is_data("Raw") == 1 && studio_ocio_is_data("ACEScg") == 0, "is_data");
	CHECK(studio_ocio_encoding(NULL) == NULL && studio_ocio_family("nope") == NULL, "encoding/family of an invalid name");
	CHECK(studio_ocio_colorspace_name(-1) == NULL && studio_ocio_colorspace_name(studio_ocio_colorspace_count()) == NULL,
	      "colorspace index out of range");
	CHECK(studio_ocio_view_count(NULL) == 0 && studio_ocio_view_count("nope") == 0, "views of an invalid display");
	CHECK(studio_ocio_view_name("nope", 0) == NULL && studio_ocio_view_default(NULL) == NULL, "view of an invalid display");
	CHECK(studio_ocio_colorspace_from_path(NULL) == NULL, "from_path(NULL)");
	float special[8] = {NAN, INFINITY, -INFINITY, 1, 1e38f, -1e38f, 1e-45f, 0};
	CHECK(studio_ocio_transform(special, 2, 1, 4, "ACEScg", "sRGB - Texture"), "NaN/Inf rejected");
	CHECK(studio_ocio_transform(special, 2, 1, 4, "ACEScg", "ACEScct"), "NaN/Inf rejected (log)");

	// ABI
	studio_ocio_lib_config_t *c = studio_ocio_lib_config_load("ocio://studio-config-latest");
	CHECK(c != NULL && !has_error(), "built-in config: %s", studio_ocio_lib_error());
	CHECK(studio_ocio_lib_config_load(NULL) == NULL && has_error(), "load(NULL)");
	CHECK(studio_ocio_lib_config_load("") == NULL && has_error(), "load(\"\")");
	CHECK(studio_ocio_lib_config_load("/nonexistent/config.ocio") == NULL && has_error(), "missing file");
	CHECK(studio_ocio_lib_config_load("/tmp") == NULL && has_error(), "directory");
	CHECK(studio_ocio_lib_config_load("ocio://no-such-config") == NULL && has_error(), "unknown built-in");
	CHECK(studio_ocio_lib_config_name(NULL) == NULL && has_error(), "name(NULL)");
	CHECK(studio_ocio_lib_config_description(NULL) == NULL && has_error(), "description(NULL)");
	CHECK(studio_ocio_lib_count(NULL, 0) == -1 && has_error(), "count(NULL)");
	CHECK(studio_ocio_lib_count(c, 4) == -1 && has_error(), "count(unknown list)");
	CHECK(studio_ocio_lib_count(c, -1) == -1 && has_error(), "count(-1)");
	CHECK(studio_ocio_lib_name(c, 0, -1) == NULL && has_error(), "name(-1)");
	CHECK(studio_ocio_lib_name(c, 0, studio_ocio_lib_count(c, 0)) == NULL && has_error(), "name(n)");
	CHECK(studio_ocio_lib_name(c, 9, 0) == NULL && has_error(), "name(unknown list)");
	CHECK(studio_ocio_lib_name(c, 0, 0) != NULL && !has_error(), "error not cleared by a successful call");
	CHECK(studio_ocio_lib_resolve(c, NULL) == NULL && has_error(), "resolve(NULL)");
	CHECK(studio_ocio_lib_resolve(NULL, "ACEScg") == NULL && has_error(), "resolve on NULL config");
	CHECK(strstr(studio_ocio_lib_error(), "null config") != NULL, "message: %s", studio_ocio_lib_error());
	CHECK(studio_ocio_lib_resolve(c, "nope") == NULL && strstr(studio_ocio_lib_error(), "nope") != NULL,
	      "unknown colorspace not named in the message: %s", studio_ocio_lib_error());
	CHECK(studio_ocio_lib_is_data(c, NULL) == -1 && studio_ocio_lib_encoding(c, "") == NULL && studio_ocio_lib_family(NULL, "x") == NULL,
	      "colorspace attributes with invalid arguments");
	CHECK(studio_ocio_lib_colorspace_from_path(c, NULL) == NULL && has_error(), "from_path(NULL)");
	CHECK(studio_ocio_lib_default_display(NULL) == NULL && has_error(), "default_display(NULL)");
	CHECK(studio_ocio_lib_default_view(c, NULL) == NULL && has_error(), "default_view(NULL)");
	CHECK(studio_ocio_lib_view_count(c, "") == -1 && has_error(), "view_count(\"\")");
	CHECK(studio_ocio_lib_view_name(c, "sRGB - Display", 99) == NULL && has_error(), "view_name(99)");
	CHECK(studio_ocio_lib_proc_colorspace(c, "ACEScg", "nope") == NULL && strstr(studio_ocio_lib_error(), "nope") != NULL,
	      "unknown dst: %s", studio_ocio_lib_error());
	CHECK(studio_ocio_lib_proc_colorspace(NULL, "ACEScg", "Raw") == NULL && has_error(), "proc on NULL config");
	CHECK(studio_ocio_lib_proc_display(c, "ACEScg", "nope", "Raw", NULL, 0) == NULL && has_error(), "unknown display");
	CHECK(studio_ocio_lib_proc_display(c, "ACEScg", "sRGB - Display", "nope", NULL, 0) == NULL && has_error(), "unknown view");
	CHECK(studio_ocio_lib_proc_display(c, "ACEScg", "sRGB - Display", "Raw", "no-such-look", 0) == NULL && has_error(),
	      "unknown look");
	CHECK(studio_ocio_lib_proc_display(c, "ACEScg", "", "Raw", NULL, 0) == NULL && has_error(), "empty display");
	CHECK(studio_ocio_lib_proc_is_noop(NULL) == -1 && studio_ocio_lib_proc_cache_id(NULL) == NULL, "NULL processor");
	studio_ocio_lib_proc_free(NULL);
	studio_ocio_lib_config_free(NULL);

	studio_ocio_lib_proc_t *p = studio_ocio_lib_proc_colorspace(c, "ACEScg", "sRGB - Texture");
	CHECK(p != NULL && studio_ocio_lib_proc_is_noop(p) == 0, "processor");
	studio_ocio_lib_proc_t *id = studio_ocio_lib_proc_colorspace(c, "ACEScg", "lin_ap1");
	CHECK(id != NULL && studio_ocio_lib_proc_is_noop(id) == 1, "alias processor is not a no-op");
	float img[4 * 4] = {0};
	CHECK(studio_ocio_lib_apply(p, NULL, 1, 1, 4, 0) == 0 && has_error(), "apply(NULL)");
	CHECK(studio_ocio_lib_apply(NULL, img, 1, 1, 4, 0) == 0 && has_error(), "apply on NULL proc");
	CHECK(studio_ocio_lib_apply(p, img, 1, 1, 2, 0) == 0 && has_error(), "apply 2 channels");
	CHECK(studio_ocio_lib_apply(p, img, 0, 1, 4, 0) == 0 && has_error(), "apply width 0");
	CHECK(studio_ocio_lib_apply(p, img, 1, -5, 4, 0) == 0 && has_error(), "apply height -5");
	CHECK(studio_ocio_lib_apply(p, img, 2, 1, 4, 16) == 0 && has_error(), "stride smaller than a row");
	CHECK(studio_ocio_lib_apply(p, img, LONG_MAX, 1, 4, 0) == 0 && strstr(studio_ocio_lib_error(), "wide") != NULL,
	      "huge width: %s", studio_ocio_lib_error());
	CHECK(studio_ocio_lib_apply(p, img, 1, LONG_MAX / 8, 4, 16) == 0 && strstr(studio_ocio_lib_error(), "large") != NULL,
	      "huge height: %s", studio_ocio_lib_error());
	CHECK(img[0] == 0, "rejected apply wrote pixels");
	// Stride : deux lignes d'un pixel espacées de 2 pixels ; le pixel du milieu n'est pas touché.
	float strided[12] = {0.18f, 0.18f, 0.18f, 1, 7, 7, 7, 7, 0.18f, 0.18f, 0.18f, 1};
	CHECK(studio_ocio_lib_apply(p, strided, 1, 2, 4, 32) == 1, "strided apply: %s", studio_ocio_lib_error());
	CHECK(strided[4] == 7 && strided[7] == 7 && strided[0] == strided[8] && strided[0] != 0.18f, "strided apply wrote the gap");
	studio_ocio_lib_proc_free(p);
	studio_ocio_lib_proc_free(id);
	studio_ocio_lib_config_free(c);
	studio_ocio_shutdown();
	CHECK(!studio_ocio_ready() && studio_ocio_colorspace_count() == 0, "still ready after shutdown");
	CHECK(!studio_ocio_transform(px, 1, 1, 4, "ACEScg", "sRGB - Texture"), "transform after shutdown");
	return 0;
}

// ── threads ────────────────────────────────────────────────────────────────────────────────────────────────────────

#define TW 512
#define TH 512
#define NT 8
static float *image;

typedef struct {
	int         index;
	const char *src;
	const char *dst;
	int         ok;
} job_t;

static void *band(void *arg) {
	job_t *j    = arg;
	int    rows = TH / NT;
	j->ok       = studio_ocio_transform(image + (size_t)j->index * rows * TW * 4, TW, rows, 4, j->src, j->dst);
	// Contention sur le cache : chaque thread demande des couples différents, plus de 64 au total (éviction).
	int n = studio_ocio_colorspace_count();
	for (int k = 0; k < 24; k++) {
		const char *a = studio_ocio_colorspace_name((j->index * 7 + k) % n);
		const char *b = studio_ocio_colorspace_name((j->index * 3 + k * 5 + 1) % n);
		float       px[4] = {0.5f, 0.25f, 0.125f, 1};
		if (!studio_ocio_transform(px, 1, 1, 4, a, b) || studio_ocio_transform_id(a, b) == NULL) {
			// Certains couples display → display n'ont pas de sens retour et échouent : pas une erreur ici.
		}
	}
	return NULL;
}

static int mode_threads(void) {
	studio_ocio_init();
	CHECK(studio_ocio_ready(), "not ready: %s", studio_ocio_status());
	size_t n = (size_t)TW * TH * 4;
	image    = malloc(n * sizeof(float));
	float *one = malloc(n * sizeof(float));
	srand(1);
	for (size_t i = 0; i < n; i++) {
		image[i] = (float)rand() / RAND_MAX * 21.0f - 1.0f;
	}
	memcpy(one, image, n * sizeof(float));
	const char *src = "ACEScg", *dst = "ACEScct";
	CHECK(studio_ocio_transform(one, TW, TH, 4, src, dst), "single-threaded transform");
	pthread_t t[NT];
	job_t     jobs[NT];
	for (int i = 0; i < NT; i++) {
		jobs[i] = (job_t){.index = i, .src = src, .dst = dst};
		pthread_create(&t[i], NULL, band, &jobs[i]);
	}
	for (int i = 0; i < NT; i++) {
		pthread_join(t[i], NULL);
		CHECK(jobs[i].ok, "band %d failed", i);
	}
	CHECK(memcmp(one, image, n * sizeof(float)) == 0, "parallel bands differ from a single-threaded transform");
	printf("  %d threads x %d rows: identical to one call; cache under contention, %d colorspaces\n", NT, TH / NT,
	       studio_ocio_colorspace_count());
	free(image);
	free(one);
	studio_ocio_shutdown();
	return 0;
}

// ── reload ─────────────────────────────────────────────────────────────────────────────────────────────────────────

static int mode_reload(const char *other) {
	float px[4] = {0.18f, 0.18f, 0.18f, 1};
	studio_ocio_init();
	CHECK(studio_ocio_ready(), "default: %s", studio_ocio_status());
	CHECK(studio_ocio_transform(px, 1, 1, 4, "ACEScg", "sRGB - Texture"), "default transform");
	char first[256];
	snprintf(first, sizeof(first), "%s", s(studio_ocio_config_name()));

	CHECK(studio_ocio_reload(other), "%s: %s", other, studio_ocio_status());
	CHECK(same(studio_ocio_config_source(), other), "source %s", studio_ocio_config_source());
	CHECK(!same(studio_ocio_config_name(), first), "same config name after reload");
	CHECK(strstr(studio_ocio_status(), "explicit") != NULL, "status: %s", studio_ocio_status());
	float q[4] = {0.18f, 0.18f, 0.18f, 1};
	CHECK(studio_ocio_transform(q, 1, 1, 4, "scene_linear", "data"), "transform after reload");
	CHECK(q[0] == 0.18f, "scene_linear -> data changed the pixel (%.9g)", q[0]);

	CHECK(!studio_ocio_reload("/nonexistent/config.ocio"), "invalid config accepted");
	CHECK(!studio_ocio_ready() && strstr(studio_ocio_status(), "/nonexistent/config.ocio") != NULL, "status: %s",
	      studio_ocio_status());
	CHECK(!studio_ocio_transform(q, 1, 1, 4, "scene_linear", "data"), "transform without config");
	CHECK(studio_ocio_config_source()[0] == '\0', "source kept after a failed reload");

	CHECK(studio_ocio_reload(NULL), "back to default: %s", studio_ocio_status());
	CHECK(same(studio_ocio_config_name(), first), "default config %s, first was %s", s(studio_ocio_config_name()), first);
	for (int i = 0; i < 20; i++) { // rechargements en boucle : ni fuite ni processeur périmé (ASan)
		CHECK(studio_ocio_reload(i % 2 ? other : NULL), "reload %d", i);
		float r[4] = {0.5f, 0.5f, 0.5f, 1};
		CHECK(studio_ocio_transform(r, 1, 1, 4, "scene_linear", "data"), "transform %d", i);
	}
	studio_ocio_shutdown();
	printf("  reload: %s <-> %s, invalid config refused, 20 reloads\n", first, other);
	return 0;
}

// ── unavailable ────────────────────────────────────────────────────────────────────────────────────────────────────

static int mode_unavailable(const char *expect) {
	studio_ocio_init();
	CHECK(!studio_ocio_ready(), "ready although unavailable");
	CHECK(strstr(studio_ocio_status(), expect) != NULL, "status '%s' lacks '%s'", studio_ocio_status(), expect);
	float px[4] = {0.18f, 0.18f, 0.18f, 1};
	CHECK(!studio_ocio_transform(px, 1, 1, 4, "ACEScg", "sRGB - Texture"), "transform while unavailable");
	CHECK(studio_ocio_transform_id("ACEScg", "sRGB - Texture") == NULL, "transform id while unavailable");
	CHECK(studio_ocio_colorspace_count() == 0 && studio_ocio_colorspace_name(0) == NULL, "colorspaces while unavailable");
	CHECK(studio_ocio_display_default() == NULL && studio_ocio_view_count("x") == 0, "displays while unavailable");
	CHECK(studio_ocio_resolve("ACEScg") == NULL && studio_ocio_is_data("Raw") == -1, "resolve while unavailable");
	CHECK(studio_ocio_config_name() == NULL && studio_ocio_config_source()[0] == '\0', "config while unavailable");
	CHECK(!studio_ocio_reload(NULL), "second reload succeeded");
	CHECK(strstr(studio_ocio_status(), expect) != NULL, "status after reload '%s'", studio_ocio_status());
	printf("  %s\n", studio_ocio_status());
	studio_ocio_shutdown();
	return 0;
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: see ocio_test.c\n");
		return 2;
	}
	int r = 2;
	if (strcmp(argv[1], "ref") == 0 && argc == 3) {
		r = mode_ref(argv[2]);
	}
	else if (strcmp(argv[1], "errors") == 0) {
		r = mode_errors();
	}
	else if (strcmp(argv[1], "threads") == 0) {
		r = mode_threads();
	}
	else if (strcmp(argv[1], "reload") == 0 && argc == 3) {
		r = mode_reload(argv[2]);
	}
	else if (strcmp(argv[1], "unavailable") == 0 && argc == 3) {
		r = mode_unavailable(argv[2]);
	}
	if (r == 2) {
		fprintf(stderr, "usage: see ocio_test.c\n");
		return 2;
	}
	if (fails > 0) {
		fprintf(stderr, "  %d/%d checks failed\n", fails, checks);
		return 1;
	}
	printf("  ok   %s (%d checks)\n", argv[1], checks);
	return r;
}
