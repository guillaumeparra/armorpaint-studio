// Banc de test du lot B1, hors de l'app, sous ASan/UBSan : lancé par run.sh.
//   exr_test gen <dir>               génère les fichiers sources (tinyexr) et les fichiers enc_* (studio_exr_encode)
//   exr_test check <dir>             décode chaque cas et compare aux valeurs attendues
//   exr_test fuzz <file> <n> <seed>  mutations aléatoires : décodage tinyexr, puis repli DWA s'il est accepté
//   exr_test decode <file>           décode un seul fichier et imprime le résultat (rejouer un cas de fuzz)
//   exr_test upstream <file>         io_exr_parse seul (reproduit le débordement de pile upstream)
//   exr_test cmp <a.exr> <b.exr>     compare deux EXR canal par canal (Y de a contre R, G, B de b)
#include "../studio_exr_codec.h"
#include "stubs/stubs.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks   = 0;
static int failures = 0;

#define CHECK(cond, ...)                                                                                               \
	do {                                                                                                               \
		checks++;                                                                                                      \
		if (!(cond)) {                                                                                                 \
			failures++;                                                                                                \
			printf("  FAIL ");                                                                                         \
			printf(__VA_ARGS__);                                                                                       \
			printf("\n");                                                                                              \
		}                                                                                                              \
	} while (0)

static void *read_file(const char *path, size_t *size) {
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	*size = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	void *data = malloc(*size ? *size : 1);
	if (fread(data, 1, *size, f) != *size) {
		free(data);
		data = NULL;
	}
	fclose(f);
	return data;
}

static void write_file(const char *path, const void *data, size_t size) {
	FILE *f = fopen(path, "wb");
	fwrite(data, 1, size, f);
	fclose(f);
}

static float half_to_float(uint16_t h) {
	float f;
	exr_half_to_float(&h, &f, 1);
	return f;
}

static float round_half(float f) {
	uint16_t h;
	exr_float_to_half(&f, &h, 1);
	return half_to_float(h);
}

// ---- Génération ------------------------------------------------------------------------------------------------------

// Rôles : 0 R, 1 G, 2 B, 3 A, 4 Y, 5 et plus : autres canaux. ROLE_ZERO : canal à 0 (RY, BY).
#define ROLE_ZERO -1

static float pattern(int role, int x, int y) {
	if (role == ROLE_ZERO) {
		return 0.0f;
	}
	return sinf(x * 0.071f + role * 1.3f) * 1.5f + cosf(y * 0.043f - role) * 0.75f + 0.25f * role;
}

static uint32_t pattern_uint(int x, int y) {
	return (uint32_t)(x * 7 + y * 13);
}

typedef struct channel_def {
	const char    *name;
	int            role;
	exr_pixel_type type;
	int            sampling; // x et y ; fenêtre de données en (0, 0) si > 1
} channel_def_t;

#define BOX(x0, y0, x1, y1) ((exr_box2i){x0, y0, x1, y1})
static const exr_box2i DEFAULT_WINDOW = {0, 0, 508, 380}; // 509 x 381 : des chunks incomplets pour tous les codecs

static void gen(const char *dir, const char *file, exr_box2i dw, exr_box2i disp, const channel_def_t *defs, int n,
                exr_compression compression) {
	exr_part    part         = {0};
	exr_header *h            = &part.header;
	h->part_type             = EXR_PART_SCANLINE;
	h->compression           = compression;
	h->line_order            = EXR_LINEORDER_INCREASING_Y;
	h->data_window           = dw;
	h->display_window        = disp;
	h->pixel_aspect_ratio    = 1.0f;
	h->screen_window_width   = 1.0f;
	h->num_channels          = n;
	h->channels              = calloc((size_t)n, sizeof(exr_channel));
	part.width               = dw.max_x - dw.min_x + 1;
	part.height              = dw.max_y - dw.min_y + 1;
	part.images              = calloc((size_t)n, sizeof(void *));
	for (int c = 0; c < n; c++) {
		exr_channel *ch = &h->channels[c];
		strcpy(ch->name, defs[c].name);
		ch->pixel_type = defs[c].type;
		ch->x_sampling = ch->y_sampling = defs[c].sampling;
		int    s  = defs[c].sampling;
		int    nx = (part.width + s - 1) / s;
		int    ny = (part.height + s - 1) / s;
		size_t ps = defs[c].type == EXR_PIXEL_HALF ? 2 : 4;
		part.images[c] = malloc((size_t)nx * ny * ps);
		for (int sy = 0; sy < ny; sy++) {
			for (int sx = 0; sx < nx; sx++) {
				int    x = dw.min_x + sx * s;
				int    y = dw.min_y + sy * s;
				size_t i = (size_t)sy * nx + sx;
				float  v = pattern(defs[c].role, x, y);
				if (defs[c].type == EXR_PIXEL_HALF) {
					exr_float_to_half(&v, (uint16_t *)part.images[c] + i, 1);
				}
				else if (defs[c].type == EXR_PIXEL_FLOAT) {
					((float *)part.images[c])[i] = v;
				}
				else {
					((uint32_t *)part.images[c])[i] = pattern_uint(x, y);
				}
			}
		}
	}
	exr_image image = {.num_parts = 1, .parts = &part};
	void     *data;
	size_t    size;
	exr_result r = exr_save_to_memory(&data, &size, NULL, &image, compression);
	if (!EXR_OK(r)) {
		printf("  FAIL gen %s: %s\n", file, exr_result_string(r));
		failures++;
	}
	else {
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", dir, file);
		write_file(path, data, size);
		free(data);
	}
	exr_part_free(NULL, &part);
}

#define H EXR_PIXEL_HALF
#define F EXR_PIXEL_FLOAT
#define U EXR_PIXEL_UINT
#define N(a) (int)(sizeof(a) / sizeof(a[0]))

static void gen_encoded(const char *dir);

static void gen_all(const char *dir) {
	const exr_box2i     w      = DEFAULT_WINDOW;
	const exr_compression zip  = EXR_COMPRESSION_ZIP;
	channel_def_t       rgba[] = {{"A", 3, H, 1}, {"B", 2, H, 1}, {"G", 1, H, 1}, {"R", 0, H, 1}};
	channel_def_t       rgbaf[] = {{"A", 3, F, 1}, {"B", 2, F, 1}, {"G", 1, F, 1}, {"R", 0, F, 1}};
	channel_def_t       rgb[]  = {{"B", 2, H, 1}, {"G", 1, H, 1}, {"R", 0, H, 1}};
	channel_def_t       y[]    = {{"Y", 4, H, 1}};
	channel_def_t       r[]    = {{"R", 0, H, 1}};
	channel_def_t       rg[]   = {{"G", 1, H, 1}, {"R", 0, H, 1}};
	channel_def_t       layers[] = {{"a.Z", 5, F, 1},        {"diffuse.A", 3, H, 1}, {"diffuse.B", 2, H, 1},
	                                {"diffuse.G", 1, H, 1},  {"diffuse.R", 0, H, 1}, {"spec.R", 6, H, 1}};
	channel_def_t       many[] = {{"A", 3, H, 1},   {"B", 2, H, 1},   {"G", 1, H, 1},   {"N.X", 5, H, 1},
	                              {"N.Y", 6, H, 1}, {"N.Z", 7, H, 1}, {"R", 0, H, 1},   {"Z", 8, H, 1}};
	channel_def_t       lower[] = {{"b", 2, H, 1}, {"g", 1, H, 1}, {"r", 0, H, 1}};
	channel_def_t       uint[] = {{"id", 0, U, 1}};
	channel_def_t       yc[]   = {{"BY", ROLE_ZERO, H, 2}, {"RY", ROLE_ZERO, H, 2}, {"Y", 4, H, 1}};
	channel_def_t       sub[]  = {{"B", 2, H, 2}, {"G", 1, H, 1}, {"R", 0, H, 1}};

	gen(dir, "rgba_half.exr", w, w, rgba, N(rgba), zip);
	gen(dir, "rgba_float.exr", w, w, rgbaf, N(rgbaf), zip);
	gen(dir, "rgb_half.exr", w, w, rgb, N(rgb), zip);
	gen(dir, "y_half.exr", w, w, y, N(y), zip);
	gen(dir, "r_only.exr", w, w, r, N(r), zip);
	gen(dir, "rg_only.exr", w, w, rg, N(rg), zip);
	gen(dir, "lowercase.exr", w, w, lower, N(lower), zip);
	gen(dir, "layers.exr", w, w, layers, N(layers), zip);
	gen(dir, "many.exr", w, w, many, N(many), EXR_COMPRESSION_NONE);
	gen(dir, "uint.exr", w, w, uint, N(uint), zip);
	gen(dir, "yc.exr", w, w, yc, N(yc), zip);
	gen(dir, "subsampled.exr", w, w, sub, N(sub), zip);
	gen(dir, "window_inner.exr", BOX(100, 50, 300, 200), w, rgba, N(rgba), zip);
	gen(dir, "window_overscan.exr", BOX(-10, -10, 518, 390), w, rgba, N(rgba), zip);
	gen(dir, "window_offset.exr", BOX(1000, 1000, 1100, 1100), BOX(1000, 1000, 1100, 1100), rgba, N(rgba), zip);
	gen(dir, "window_disjoint.exr", BOX(600, 600, 700, 700), w, rgba, N(rgba), zip);
	gen(dir, "too_large.exr", BOX(0, 0, 16384, 0), BOX(0, 0, 16384, 0), y, N(y), zip);
	gen(dir, "display_invalid.exr", w, BOX(0, 0, 99999, 99999), rgba, N(rgba), zip);
	// dataWindow démesurée derrière une petite image : canal nul, donc quelques Ko une fois compressé.
	channel_def_t zero[] = {{"Y", ROLE_ZERO, H, 1}};
	gen(dir, "window_huge_area.exr", BOX(0, 0, 32767, 599), w, zero, N(zero), zip); // 19,7 M pixels > 4096²
	gen(dir, "window_huge_side.exr", BOX(-20000, 0, 20000, 0), w, zero, N(zero), zip); // 40 001 > 2 * 16384
	gen(dir, "window_overscan_ok.exr", BOX(-1000, -1000, 3095, 3095), BOX(0, 0, 2047, 2047), zero, N(zero), zip);

	char path[1024];
	snprintf(path, sizeof(path), "%s/garbage.exr", dir);
	write_file(path, "this is not an exr file", 23);
	snprintf(path, sizeof(path), "%s/empty.exr", dir);
	write_file(path, "", 0);

	gen_encoded(dir);
}

// ---- Vérifications ---------------------------------------------------------------------------------------------------

typedef struct expect {
	const char    *file;
	exr_result     result;
	int            role[4];  // source de R, G, B, A ; -1 : 0 pour R, G, B, 1 pour A ; -2 : pattern_uint
	exr_pixel_type src_type; // type des canaux sources (arrondi de la valeur attendue)
	bool           half;     // sortie attendue
	float          tolerance;
	exr_box2i      dw;
	exr_box2i      disp; // image attendue
	const char    *note; // sous-chaîne attendue dans la note
} expect_t;

static float expected_value(const expect_t *e, int k, int x, int y) {
	int role = e->role[k];
	if (role == -1) {
		return k == 3 ? 1.0f : 0.0f;
	}
	float v = role == -2 ? (float)pattern_uint(x, y) : pattern(role, x, y);
	if (e->src_type == EXR_PIXEL_HALF || e->half) {
		v = round_half(v);
	}
	return v;
}

static void compare_pixels(const char *label, const void *pixels, bool is_half, const expect_t *e) {
	int    w = e->disp.max_x - e->disp.min_x + 1;
	int    h = e->disp.max_y - e->disp.min_y + 1;
	int    bad = 0;
	double max_err = 0, sum_err = 0;
	char   first[256] = "";
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int  ax     = x + e->disp.min_x;
			int  ay     = y + e->disp.min_y;
			bool inside = ax >= e->dw.min_x && ax <= e->dw.max_x && ay >= e->dw.min_y && ay <= e->dw.max_y;
			for (int k = 0; k < 4; k++) {
				size_t i   = ((size_t)y * w + x) * 4 + k;
				float  got = is_half ? half_to_float(((const uint16_t *)pixels)[i]) : ((const float *)pixels)[i];
				float  exp = inside ? expected_value(e, k, ax, ay) : 0.0f;
				double err = fabs((double)got - exp);
				sum_err += err;
				if (err > max_err) {
					max_err = err;
				}
				if (err > e->tolerance || isnan(got)) {
					if (bad++ == 0) {
						snprintf(first, sizeof(first), "(%d,%d)[%d] got %g expected %g", ax, ay, k, got, exp);
					}
				}
			}
		}
	}
	CHECK(bad == 0, "%s: %d values off, first %s, max error %g", label, bad, first, max_err);
	if (e->tolerance > 0) {
		printf("  %-24s max error %.5f, mean %.6f\n", label, max_err, sum_err / ((double)w * h * 4));
	}
}

// ---- Encodeur --------------------------------------------------------------------------------------------------------

// Fichiers écrits par studio_exr_encode, pour chaque compression inscriptible, half et float, et chaque type
// d'export_texture_write_texture(). run.sh les fait relire par OpenEXR (exrmetrics --convert -z none -> *_ref.exr) :
// check_encoded() vérifie que la relecture OpenEXR, sans passer par le décodeur de tinyexr, rend nos valeurs.
static const exr_compression encoded_compressions[] = {
    EXR_COMPRESSION_NONE, EXR_COMPRESSION_RLE, EXR_COMPRESSION_ZIPS,     EXR_COMPRESSION_ZIP,     EXR_COMPRESSION_PIZ,
    EXR_COMPRESSION_PXR24, EXR_COMPRESSION_B44, EXR_COMPRESSION_B44A, EXR_COMPRESSION_HTJ2K256, EXR_COMPRESSION_HTJ2K32};
static const struct {
	int         type, off;
	int         role[4];
	const char *label;
} encoded_types[] = {{1, 0, {0, 1, 2, -1}, "rgb"}, {2, 1, {1, 1, 1, -1}, "y"}, {3, 0, {0, 1, 2, 3}, "rgba"}};
#define ENC_W 257 // chunks incomplets pour tous les codecs
#define ENC_H 131

static void encoded_name(char *out, size_t size, const char *dir, int c, int bits, int t, const char *suffix) {
	snprintf(out, size, "%s/enc_%s_%d_%s%s.exr", dir, studio_exr_compression_name(encoded_compressions[c]), bits,
	         encoded_types[t].label, suffix);
}

static void gen_encoded(const char *dir) {
	float    *pf = malloc((size_t)ENC_W * ENC_H * 4 * sizeof(float));
	uint16_t *ph = malloc((size_t)ENC_W * ENC_H * 4 * sizeof(uint16_t));
	for (int y = 0; y < ENC_H; y++) {
		for (int x = 0; x < ENC_W; x++) {
			for (int k = 0; k < 4; k++) {
				pf[((size_t)y * ENC_W + x) * 4 + k] = pattern(k, x, y);
			}
		}
	}
	exr_float_to_half(pf, ph, (size_t)ENC_W * ENC_H * 4);
	const char *names[]  = {"software", "asset"};
	const char *values[] = {"exr_test", "chair_A"};
	for (int c = 0; c < N(encoded_compressions); c++) {
		for (int bits = 16; bits <= 32; bits += 16) {
			for (int t = 0; t < N(encoded_types); t++) {
				studio_exr_encode_t in = {.pixels      = bits == 16 ? (const void *)ph : (const void *)pf,
				                          .pixels_size = (size_t)ENC_W * ENC_H * 4 * (bits / 8),
				                          .width       = ENC_W,
				                          .height      = ENC_H,
				                          .bits        = bits,
				                          .type        = encoded_types[t].type,
				                          .off         = encoded_types[t].off,
				                          .compression = encoded_compressions[c],
				                          .attr_count  = 2,
				                          .attr_names  = names,
				                          .attr_values = values};
				void      *out;
				size_t     out_size;
				exr_result r = studio_exr_encode(&in, &out, &out_size);
				char       path[1024];
				encoded_name(path, sizeof(path), dir, c, bits, t, "");
				if (!EXR_OK(r)) {
					printf("  FAIL encode %s: %s\n", path, exr_result_string(r));
					failures++;
					continue;
				}
				write_file(path, out, out_size);
				free(out);
			}
		}
	}
	free(pf);
	free(ph);
}

static bool decode_file(const char *path, studio_exr_image_t *img) {
	size_t size;
	void  *data = read_file(path, &size);
	char   err[256];
	exr_result r = data ? studio_exr_decode(data, size, img, err, sizeof(err)) : EXR_ERROR_IO;
	CHECK(EXR_OK(r), "%s: %s", path, data ? err : "cannot read");
	free(data);
	return EXR_OK(r);
}

static void check_encoded(const char *dir) {
	int files = 0, before = failures;
	for (int c = 0; c < N(encoded_compressions); c++) {
		exr_compression z = encoded_compressions[c];
		for (int bits = 16; bits <= 32; bits += 16) {
			// Pertes attendues : B44 sur les canaux half, PXR24 sur les canaux float (24 bits de mantisse).
			bool lossy = (bits == 16 && (z == EXR_COMPRESSION_B44 || z == EXR_COMPRESSION_B44A)) ||
			             (bits == 32 && z == EXR_COMPRESSION_PXR24);
			for (int t = 0; t < N(encoded_types); t++) {
				char path[1024], ref[1024], label[128];
				encoded_name(path, sizeof(path), dir, c, bits, t, "");
				encoded_name(ref, sizeof(ref), dir, c, bits, t, "_ref");
				snprintf(label, sizeof(label), "enc %s %d %s", studio_exr_compression_name(z), bits, encoded_types[t].label);
				studio_exr_image_t ours = {0}, theirs = {0};
				if (decode_file(path, &ours) && decode_file(ref, &theirs)) {
					files++;
					CHECK(ours.compression == (int)z, "%s: compression %s", label, studio_exr_compression_name(ours.compression));
					CHECK(ours.is_half == (bits == 16) && theirs.is_half == (bits == 16), "%s: pixel type", label);
					// OpenEXR relit exactement ce que relit tinyexr, pertes comprises.
					size_t bytes = (size_t)ENC_W * ENC_H * 4 * (bits / 8);
					CHECK(ours.width == ENC_W && ours.height == ENC_H && theirs.width == ENC_W && theirs.height == ENC_H &&
					          memcmp(ours.pixels, theirs.pixels, bytes) == 0,
					      "%s: OpenEXR and tinyexr decode differently", label);
					// Sans perte : les valeurs source, au bit près. Avec perte : une borne large, contre un canal vide ou décalé.
					expect_t e = {"", EXR_SUCCESS,
					              {encoded_types[t].role[0], encoded_types[t].role[1], encoded_types[t].role[2], encoded_types[t].role[3]},
					              bits == 16 ? H : F, bits == 16, lossy ? 0.25f : 0.0f,
					              BOX(0, 0, ENC_W - 1, ENC_H - 1), BOX(0, 0, ENC_W - 1, ENC_H - 1), NULL};
					compare_pixels(label, theirs.pixels, theirs.is_half, &e);

					size_t     size;
					void      *data = read_file(path, &size);
					exr_reader *rd   = NULL;
					char        asset[64] = "";
					if (data != NULL && EXR_OK(exr_reader_open_memory(data, size, NULL, &rd)) && EXR_OK(exr_reader_parse_header(rd))) {
						exr_header_get_string_attribute(exr_reader_part_header(rd, 0), "asset", asset, sizeof(asset), NULL);
					}
					CHECK(strcmp(asset, "chair_A") == 0, "%s: asset attribute \"%s\"", label, asset);
					exr_reader_close(rd);
					free(data);
				}
				studio_exr_image_free(&ours);
				studio_exr_image_free(&theirs);
			}
		}
	}
	if (failures == before) {
		printf("  ok   encode: %d files (%d compressions x half/float x RGB/Y/RGBA) read back identically by OpenEXR\n", files,
		       N(encoded_compressions));
	}
}

static void check_decode(const char *dir, const expect_t *e) {
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", dir, e->file);
	size_t size = 0;
	void  *data = read_file(path, &size);
	CHECK(data != NULL, "%s: missing", e->file);
	if (data == NULL) {
		return;
	}
	studio_exr_image_t img;
	char               err[256];
	exr_result         r = studio_exr_decode(data, size, &img, err, sizeof(err));
	if (e->result != EXR_SUCCESS) {
		CHECK(r == e->result, "%s: expected %s, got %s (%s)", e->file, exr_result_string(e->result), exr_result_string(r), err);
		printf("  ok   %-24s refused: %s\n", e->file, err);
	}
	else {
		CHECK(EXR_OK(r), "%s: %s (%s)", e->file, exr_result_string(r), err);
		if (EXR_OK(r)) {
			int w = e->disp.max_x - e->disp.min_x + 1;
			int h = e->disp.max_y - e->disp.min_y + 1;
			CHECK(img.width == w && img.height == h, "%s: size %dx%d, expected %dx%d", e->file, img.width, img.height, w, h);
			CHECK(img.is_half == e->half, "%s: is_half %d", e->file, img.is_half);
			if (e->note != NULL) {
				CHECK(strstr(img.note, e->note) != NULL, "%s: note \"%s\", expected \"%s\"", e->file, img.note, e->note);
			}
			if (img.width == w && img.height == h && img.is_half == e->half) {
				int before = failures;
				compare_pixels(e->file, img.pixels, img.is_half, e);
				if (failures == before) {
					printf("  ok   %-24s %dx%d %s %s%s%s\n", e->file, w, h, img.is_half ? "half" : "float",
					       studio_exr_compression_name(img.compression), img.note[0] ? ", " : "", img.note);
				}
			}
		}
	}
	studio_exr_image_free(&img);
	free(data);
}

// DWA : tinyexr refuse, le repli upstream doit être accepté (ou refusé) puis décoder comme OpenEXR (fichier _ref).
static void check_dwa(const char *dir, const char *name, bool expect_safe) {
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s.exr", dir, name);
	size_t size = 0;
	void  *data = read_file(path, &size);
	CHECK(data != NULL, "%s: missing", name);
	if (data == NULL) {
		return;
	}
	studio_exr_image_t img;
	char               err[256];
	exr_result         r = studio_exr_decode(data, size, &img, err, sizeof(err));
	CHECK(r == EXR_ERROR_UNSUPPORTED && (img.compression == EXR_COMPRESSION_DWAA || img.compression == EXR_COMPRESSION_DWAB),
	      "%s: tinyexr result %s, compression %d", name, exr_result_string(r), img.compression);
	char why[160] = "";
	bool safe     = studio_exr_dwa_legacy_safe(data, size, why, sizeof(why));
	CHECK(safe == expect_safe, "%s: legacy safe %d (%s)", name, safe, why);
	if (!safe) {
		printf("  ok   %-24s DWA fallback refused: %s\n", name, why);
		free(data);
		return;
	}

	uint8_t *copy = calloc(size + STUDIO_EXR_DWA_PAD, 1);
	memcpy(copy, data, size);
	void *texture = io_exr_parse(copy, size);
	free(copy);
	CHECK(texture != NULL && stub_last_texture.format == 1, "%s: io_exr_parse failed", name);

	// Référence : le même fichier décodé par OpenEXR (exrmetrics --convert -z none), lu par tinyexr.
	snprintf(path, sizeof(path), "%s/%s_ref.exr", dir, name);
	size_t ref_size = 0;
	void  *ref      = read_file(path, &ref_size);
	studio_exr_image_t ref_img;
	r = ref != NULL ? studio_exr_decode(ref, ref_size, &ref_img, err, sizeof(err)) : EXR_ERROR_IO;
	CHECK(EXR_OK(r) && ref_img.is_half, "%s: reference %s", name, err);
	if (texture != NULL && EXR_OK(r)) {
		CHECK(ref_img.width == stub_last_texture.width && ref_img.height == stub_last_texture.height, "%s: size", name);
		size_t   n   = (size_t)ref_img.width * ref_img.height * 4;
		double   max = 0, sum = 0;
		uint16_t *a  = stub_last_texture.pixels;
		uint16_t *b  = ref_img.pixels;
		for (size_t i = 0; i < n; i++) {
			double d = fabs((double)half_to_float(a[i]) - half_to_float(b[i]));
			sum += d;
			if (d > max) {
				max = d;
			}
		}
		// Écart entre le décodeur DWA upstream (DCT en float, conversions approchées) et OpenEXR.
		CHECK(max < 0.05, "%s: io_exr_parse vs OpenEXR max error %g", name, max);
		printf("  ok   %-24s DWA fallback: io_exr_parse vs OpenEXR max error %.5f, mean %.6f\n", name, max, sum / n);
		studio_exr_image_free(&ref_img);
	}
	free(ref);
	free(data);
}

static void check_all(const char *dir) {
	const exr_box2i w       = DEFAULT_WINDOW;
	const expect_t  cases[] = {
        {"rgba_half.exr", EXR_SUCCESS, {0, 1, 2, 3}, H, true, 0, w, w, NULL},
        {"rgba_float.exr", EXR_SUCCESS, {0, 1, 2, 3}, F, false, 0, w, w, NULL},
        {"rgb_half.exr", EXR_SUCCESS, {0, 1, 2, -1}, H, true, 0, w, w, NULL},
        {"y_half.exr", EXR_SUCCESS, {4, 4, 4, -1}, H, true, 0, w, w, "gray from Y"},
        {"r_only.exr", EXR_SUCCESS, {0, 0, 0, -1}, H, true, 0, w, w, "gray from R"},
        {"rg_only.exr", EXR_SUCCESS, {0, 1, -1, -1}, H, true, 0, w, w, NULL},
        {"lowercase.exr", EXR_SUCCESS, {0, 1, 2, -1}, H, true, 0, w, w, NULL},
        {"layers.exr", EXR_SUCCESS, {0, 1, 2, 3}, H, true, 0, w, w, "layer \"diffuse\""},
        {"many.exr", EXR_SUCCESS, {0, 1, 2, 3}, H, true, 0, w, w, NULL},
        {"uint.exr", EXR_SUCCESS, {-2, -2, -2, -1}, U, false, 0, w, w, "gray from channel \"id\""},
        {"yc.exr", EXR_SUCCESS, {4, 4, 4, -1}, H, false, 2e-3f, w, w, "luminance/chroma"},
        {"subsampled.exr", EXR_ERROR_UNSUPPORTED},
        {"window_inner.exr", EXR_SUCCESS, {0, 1, 2, 3}, H, true, 0, BOX(100, 50, 300, 200), w, "placed in displayWindow"},
        {"window_overscan.exr", EXR_SUCCESS, {0, 1, 2, 3}, H, true, 0, BOX(-10, -10, 518, 390), w, NULL},
        {"window_offset.exr", EXR_SUCCESS, {0, 1, 2, 3}, H, true, 0, BOX(1000, 1000, 1100, 1100), BOX(1000, 1000, 1100, 1100), NULL},
        {"window_disjoint.exr", EXR_SUCCESS, {0, 1, 2, 3}, H, true, 0, BOX(600, 600, 700, 700), w, NULL},
        {"display_invalid.exr", EXR_SUCCESS, {0, 1, 2, 3}, H, true, 0, w, w, "displayWindow ignored"},
        {"too_large.exr", EXR_ERROR_LIMIT_EXCEEDED},
        {"window_huge_area.exr", EXR_ERROR_LIMIT_EXCEEDED},
        {"window_huge_side.exr", EXR_ERROR_LIMIT_EXCEEDED},
        {"window_overscan_ok.exr", EXR_SUCCESS, {-1, -1, -1, -1}, H, true, 0, BOX(-1000, -1000, 3095, 3095), BOX(0, 0, 2047, 2047), NULL},
        {"garbage.exr", EXR_ERROR_INVALID_FILE},
        {"empty.exr", EXR_ERROR_INVALID_FILE},
        // Convertis par OpenEXR (run.sh)
        {"rgba_none.exr", EXR_SUCCESS, {0, 1, 2, 3}, H, true, 0, w, w, NULL},
        {"rgba_rle.exr", EXR_SUCCESS, {0, 1, 2, 3}, H, true, 0, w, w, NULL},
        {"rgba_zips.exr", EXR_SUCCESS, {0, 1, 2, 3}, H, true, 0, w, w, NULL},
        {"rgba_piz.exr", EXR_SUCCESS, {0, 1, 2, 3}, H, true, 0, w, w, NULL},
        {"rgba_pxr24.exr", EXR_SUCCESS, {0, 1, 2, 3}, H, true, 0, w, w, NULL},
        {"rgba_htj2k256.exr", EXR_SUCCESS, {0, 1, 2, 3}, H, true, 0, w, w, NULL},
        {"rgba_htj2k32.exr", EXR_SUCCESS, {0, 1, 2, 3}, H, true, 0, w, w, NULL},
        // B44/B44A quantifient par blocs de 4x4 : l'erreur est relative au max du bloc (pattern jusqu'à ~3),
        // d'où une tolérance large face à la source sans perte. Le décodeur tinyexr est par ailleurs bit à bit
        // identique à OpenEXR (vérifié en comparant tinyexr(b44) à exrmetrics -z none) ; le contrôle d'erreur
        // moyenne ci-dessous reste serré et attrape une vraie erreur de décodage.
        {"rgba_b44.exr", EXR_SUCCESS, {0, 1, 2, 3}, H, true, 0.2f, w, w, NULL},
        {"rgba_b44a.exr", EXR_SUCCESS, {0, 1, 2, 3}, H, true, 0.2f, w, w, NULL},
        {"rgba_float_pxr24.exr", EXR_SUCCESS, {0, 1, 2, 3}, F, false, 1e-3f, w, w, NULL},
        {"tiled_one.exr", EXR_SUCCESS, {0, 1, 2, 3}, H, true, 0, w, w, NULL},
        {"tiled_mip.exr", EXR_SUCCESS, {0, 1, 2, 3}, H, true, 0, w, w, "mip level 0"},
        {"multipart.exr", EXR_SUCCESS, {4, 4, 4, -1}, H, true, 0, w, w, "part 1 of 2"},
    };
	// Erreur d'arrondi half de la valeur attendue quand la source est float et la sortie half (tolérance 0 sinon).
	for (int i = 0; i < N(cases); i++) {
		check_decode(dir, &cases[i]);
	}
	check_dwa(dir, "rgb_dwaa", true);
	check_dwa(dir, "rgb_dwab", true);
	check_dwa(dir, "y_dwaa", true);
	check_dwa(dir, "y_dwab", true);
	check_dwa(dir, "rgba_dwaa", false); // alpha : io_exr_parse ne sait pas le décoder

	check_encoded(dir);
}

// ---- Fuzz ------------------------------------------------------------------------------------------------------------

static void fuzz(const char *path, int iterations, unsigned seed) {
	size_t   size = 0;
	uint8_t *orig = read_file(path, &size);
	if (orig == NULL) {
		printf("  FAIL fuzz: %s missing\n", path);
		failures++;
		return;
	}
	srand(seed);
	const char *dump_path = getenv("FUZZ_DUMP");
	stub_quiet            = 1;
	int      decoded      = 0, refused = 0, dwa_runs = 0;
	uint8_t *work         = malloc(size);
	uint32_t interesting[] = {0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0xffff, 0x7fffffff, 0x80000000, 0xffffffff, 16384, 16385};
	for (int it = 0; it < iterations; it++) {
		memcpy(work, orig, size);
		size_t n    = size;
		int    kind = rand() % 10;
		size_t head = size < 1024 ? size : 1024;
		if (kind <= 5) {
			int flips = 1 + rand() % 4;
			for (int i = 0; i < flips; i++) {
				size_t p = rand() % 2 ? (size_t)rand() % head : (size_t)rand() % size;
				work[p] ^= (uint8_t)(1 + rand() % 255);
			}
		}
		else if (kind == 6) {
			n = (size_t)rand() % size;
		}
		else if (size >= 4) {
			size_t   p = (kind == 7 ? (size_t)rand() % head : (size_t)rand() % size);
			uint32_t v = interesting[rand() % N(interesting)];
			if (p + 4 > size) {
				p = size - 4;
			}
			memcpy(work + p, &v, 4);
		}

		alarm(20); // un décodeur qui boucle fait échouer le test
		uint8_t *exact = malloc(n ? n : 1);
		memcpy(exact, work, n);
		// FUZZ_DUMP=<fichier> : dernière entrée essayée, écrasée à chaque tour. Après un plantage, c'est le
		// cas à rejouer (exr_test decode <fichier>). Coûteux, donc sur demande : on relance la seule cible
		// qui a planté, avec la même graine.
		if (dump_path != NULL) {
			write_file(dump_path, exact, n);
		}
		studio_exr_image_t img;
		char               err[256];
		exr_result         r = studio_exr_decode(exact, n, &img, err, sizeof(err));
		if (EXR_OK(r)) {
			decoded++;
		}
		else {
			refused++;
		}
		if (r == EXR_ERROR_UNSUPPORTED && (img.compression == EXR_COMPRESSION_DWAA || img.compression == EXR_COMPRESSION_DWAB) &&
		    studio_exr_dwa_legacy_safe(exact, n, NULL, 0)) {
			uint8_t *padded = calloc(n + STUDIO_EXR_DWA_PAD, 1);
			memcpy(padded, exact, n);
			io_exr_parse(padded, n);
			free(padded);
			dwa_runs++;
		}
		studio_exr_image_free(&img);
		free(exact);
		alarm(0);
	}
	printf("  ok   fuzz %-20s %d mutations: %d decoded, %d refused, %d through io_exr_parse\n", strrchr(path, '/') + 1,
	       iterations, decoded, refused, dwa_runs);
	free(work);
	free(orig);
	stub_quiet = 0;
}

// ---- Comparaison d'exports -------------------------------------------------------------------------------------------

static float sample(const exr_part *p, int c, size_t i) {
	switch (p->header.channels[c].pixel_type) {
	case EXR_PIXEL_HALF:
		return half_to_float(((const uint16_t *)p->images[c])[i]);
	case EXR_PIXEL_FLOAT:
		return ((const float *)p->images[c])[i];
	default:
		return (float)((const uint32_t *)p->images[c])[i];
	}
}

static int find_channel(const exr_part *p, const char *name) {
	for (int c = 0; c < p->header.num_channels; c++) {
		if (strcmp(p->header.channels[c].name, name) == 0) {
			return c;
		}
	}
	return -1;
}

static int cmp(const char *a_path, const char *b_path) {
	size_t     as, bs;
	void      *ad = read_file(a_path, &as), *bd = read_file(b_path, &bs);
	exr_image  a = {0}, b = {0};
	exr_result ra = ad ? exr_load_from_memory(ad, as, NULL, &a) : EXR_ERROR_IO;
	exr_result rb = bd ? exr_load_from_memory(bd, bs, NULL, &b) : EXR_ERROR_IO;
	if (!EXR_OK(ra) || !EXR_OK(rb)) {
		printf("FAIL load %s / %s\n", exr_result_string(ra), exr_result_string(rb));
		return 1;
	}
	exr_part *pa = &a.parts[0], *pb = &b.parts[0];
	if (pa->width != pb->width || pa->height != pb->height ||
	    memcmp(&pa->header.data_window, &pb->header.data_window, sizeof(exr_box2i)) != 0) {
		printf("FAIL geometry %dx%d vs %dx%d\n", pa->width, pa->height, pb->width, pb->height);
		return 1;
	}
	size_t n      = (size_t)pa->width * pa->height;
	int    status = 0;
	char   summary[512] = "";
	for (int cb = 0; cb < pb->header.num_channels; cb++) {
		const char *name = pb->header.channels[cb].name;
		int         ca   = find_channel(pa, name);
		const char *as_  = name;
		if (ca < 0 && strlen(name) == 1 && strchr("RGB", name[0]) != NULL) {
			ca  = find_channel(pa, "Y");
			as_ = "Y";
		}
		if (ca < 0) {
			printf("FAIL channel %s missing\n", name);
			status = 1;
			continue;
		}
		size_t bad = 0;
		double max = 0;
		for (size_t i = 0; i < n; i++) {
			double d = fabs((double)sample(pa, ca, i) - sample(pb, cb, i));
			if (d > 0) {
				bad++;
			}
			if (d > max) {
				max = d;
			}
		}
		if (bad > 0) {
			status = 1;
		}
		snprintf(summary + strlen(summary), sizeof(summary) - strlen(summary), " %s%s%s=%s", as_, strcmp(as_, name) ? "~" : "",
		         strcmp(as_, name) ? name : "", bad ? "DIFF" : "same");
		if (bad) {
			printf("  %s: %zu values differ, max %g\n", name, bad, max);
		}
	}
	for (int c = 0; c < pa->header.num_channels; c++) {
		const char *name = pa->header.channels[c].name;
		if (find_channel(pb, name) < 0 && strcmp(name, "Y") != 0) {
			snprintf(summary + strlen(summary), sizeof(summary) - strlen(summary), " +%s", name);
		}
	}
	printf("%s%s\n", status ? "DIFF" : "same", summary);
	exr_image_free(&a);
	exr_image_free(&b);
	free(ad);
	free(bd);
	return status;
}

int main(int argc, char **argv) {
	if (argc >= 3 && strcmp(argv[1], "gen") == 0) {
		gen_all(argv[2]);
	}
	else if (argc >= 3 && strcmp(argv[1], "check") == 0) {
		check_all(argv[2]);
	}
	else if (argc >= 5 && strcmp(argv[1], "fuzz") == 0) {
		fuzz(argv[2], atoi(argv[3]), (unsigned)atoi(argv[4]));
	}
	else if (argc >= 3 && strcmp(argv[1], "decode") == 0) {
		size_t             size;
		void              *data = read_file(argv[2], &size);
		studio_exr_image_t img;
		char               err[256];
		exr_result         r = data ? studio_exr_decode(data, size, &img, err, sizeof(err)) : EXR_ERROR_IO;
		printf("studio_exr_decode: %s (%s) compression %s%s%s\n", exr_result_string(r), EXR_OK(r) ? "" : err,
		       studio_exr_compression_name(img.compression), img.note[0] ? ", " : "", img.note);
		studio_exr_image_free(&img);
		free(data);
		return 0;
	}
	else if (argc >= 3 && strcmp(argv[1], "upstream") == 0) {
		size_t size;
		void  *data = read_file(argv[2], &size);
		void  *tex  = data ? io_exr_parse(data, size) : NULL;
		printf("io_exr_parse: %s\n", tex ? "texture" : "NULL");
		return 0;
	}
	else if (argc >= 4 && strcmp(argv[1], "cmp") == 0) {
		return cmp(argv[2], argv[3]);
	}
	else {
		fprintf(stderr,
		        "usage: exr_test gen|check <dir> | fuzz <file> <n> <seed> | decode <file> | upstream <file> | cmp <a> <b>\n");
		return 2;
	}
	if (strcmp(argv[1], "check") == 0) {
		printf("%d checks, %d failures\n", checks, failures);
	}
	return failures > 0;
}
