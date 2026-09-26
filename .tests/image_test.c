// Banc du lot B2 (studio_image.c) : chaque image encodée est relue par libpng ou libtiff et comparée à la source.
#include "../studio_image.h"
#include <png.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tiffio.h>

unsigned char *iron_deflate_raw(unsigned char *data, int data_len, int *out_len, int quality);

static int failures = 0;

#define CHECK(c, ...)                                                                                                  \
	do {                                                                                                               \
		if (!(c)) {                                                                                                    \
			printf("  FAIL ");                                                                                         \
			printf(__VA_ARGS__);                                                                                       \
			printf("\n");                                                                                              \
			failures++;                                                                                                \
		}                                                                                                              \
	} while (0)

typedef struct {
	studio_image_job_fn fn;
	void               *ctx;
	int                 jobs;
	int                 next;
	pthread_mutex_t     lock;
} pool_t;

static void *pool_worker(void *arg) {
	pool_t *p = arg;
	for (;;) {
		pthread_mutex_lock(&p->lock);
		int job = p->next++;
		pthread_mutex_unlock(&p->lock);
		if (job >= p->jobs) {
			return NULL;
		}
		p->fn(p->ctx, job);
	}
}

static void parallel(int threads, int jobs, studio_image_job_fn fn, void *ctx) {
	pool_t    p = {.fn = fn, .ctx = ctx, .jobs = jobs};
	pthread_t t[8];
	threads = threads > 8 ? 8 : threads;
	pthread_mutex_init(&p.lock, NULL);
	for (int i = 0; i < threads; ++i) {
		pthread_create(&t[i], NULL, pool_worker, &p);
	}
	for (int i = 0; i < threads; ++i) {
		pthread_join(t[i], NULL);
	}
	pthread_mutex_destroy(&p.lock);
}

static uint32_t rng = 1234567;
static uint32_t rnd(void) {
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

// Contenu : dégradés (filtres utiles), bruit (incompressible) et aplats (LZW jusqu'au Clear), selon le mode.
static void *make_pixels(int w, int h, int c, int bits, int mode) {
	size_t n = (size_t)w * h * c;
	void  *p = malloc(n * (bits / 8));
	for (size_t i = 0; i < n; ++i) {
		size_t   px = i / c, x = px % w, y = px / w;
		uint32_t v  = mode == 0 ? (uint32_t)(x * 7 + y * 3 + (i % c) * 50) : mode == 1 ? rnd() : (uint32_t)((x / 64) * 40);
		if (bits == 8) {
			((uint8_t *)p)[i] = (uint8_t)v;
		}
		else if (bits == 16) {
			((uint16_t *)p)[i] = (uint16_t)(mode == 1 ? v : v * 257);
		}
		else {
			((float *)p)[i] = mode == 1 ? (float)(v % 100000) / 7777.0f - 3.0f : (float)v / 255.0f;
		}
	}
	return p;
}

static int test_png(int w, int h, int c, int bits, int mode, bool threads, const char *dir) {
	void          *pixels = make_pixels(w, h, c, bits, mode);
	studio_image_t img    = {.pixels = pixels, .width = w, .height = h, .channels = c, .bits = bits, .level = 5,
	                         .software = "studio test", .threads = 4, .parallel = threads ? parallel : NULL};
	void          *data   = NULL;
	size_t         size   = 0;
	char           err[256];
	if (!studio_png_encode(&img, &data, &size, err, sizeof(err))) {
		CHECK(0, "png encode %dx%d c%d b%d: %s", w, h, c, bits, err);
		free(pixels);
		return 1;
	}
	char path[1024];
	snprintf(path, sizeof(path), "%s/png_%dx%d_c%d_b%d_m%d.png", dir, w, h, c, bits, mode);
	FILE *f = fopen(path, "wb");
	fwrite(data, 1, size, f);
	fclose(f);
	free(data);

	png_image pi = {.version = PNG_IMAGE_VERSION};
	int       ok = png_image_begin_read_from_file(&pi, path);
	CHECK(ok, "libpng open %s: %s", path, pi.message);
	if (ok) {
		// Format natif : gris / RGB / RGBA, 16 bits en « linear » (libpng rend alors des uint16 hôte sans conversion
		// pourvu qu'on ne demande pas de changement de colorimétrie).
		pi.format = (c == 1 ? 0 : c == 3 ? PNG_FORMAT_FLAG_COLOR : PNG_FORMAT_FLAG_COLOR | PNG_FORMAT_FLAG_ALPHA) |
		            (bits == 16 ? PNG_FORMAT_FLAG_LINEAR : 0);
		png_image_finish_read(&pi, NULL, NULL, 0, NULL);
		png_image_free(&pi);
	}
	// png_image transforme les 16 bits (alpha prémultiplié, gamma) : lecture bas niveau pour une comparaison exacte.
	f               = fopen(path, "rb");
	png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_infop   inf = png_create_info_struct(png);
	int         bad = 0;
	if (setjmp(png_jmpbuf(png))) {
		CHECK(0, "libpng error on %s", path);
		bad = 1;
	}
	else {
		png_init_io(png, f);
		png_read_info(png, inf);
		CHECK((int)png_get_image_width(png, inf) == w && (int)png_get_image_height(png, inf) == h, "png size");
		CHECK(png_get_bit_depth(png, inf) == bits, "png depth");
		CHECK(png_get_channels(png, inf) == c, "png channels");
		if (bits == 16) {
			png_set_swap(png);
		}
		size_t   rb  = (size_t)w * c * bits / 8;
		uint8_t *row = malloc(rb);
		for (int y = 0; y < h && !bad; ++y) {
			png_read_row(png, row, NULL);
			if (memcmp(row, (uint8_t *)pixels + (size_t)y * rb, rb) != 0) {
				CHECK(0, "png pixels differ at row %d (%s)", y, path);
				bad = 1;
			}
		}
		free(row);
		png_text *text = NULL;
		int       nt   = 0;
		png_read_end(png, inf);
		png_get_text(png, inf, &text, &nt);
		CHECK(nt == 1 && strcmp(text[0].key, "Software") == 0 && strcmp(text[0].text, "studio test") == 0, "png tEXt");
	}
	png_destroy_read_struct(&png, &inf, NULL);
	fclose(f);
	free(pixels);
	return bad;
}

static int test_tiff(int w, int h, int c, int bits, int mode, studio_tiff_compression_t comp, bool threads, const char *dir) {
	void          *pixels = make_pixels(w, h, c, bits, mode);
	studio_image_t img    = {.pixels = pixels, .width = w, .height = h, .channels = c, .bits = bits, .level = 5,
	                         .tiff_compression = comp, .software = "studio test", .threads = 4,
	                         .parallel = threads ? parallel : NULL};
	void          *data   = NULL;
	size_t         size   = 0;
	char           err[256];
	if (!studio_tiff_encode(&img, &data, &size, err, sizeof(err))) {
		CHECK(0, "tiff encode: %s", err);
		free(pixels);
		return 1;
	}
	char path[1024];
	snprintf(path, sizeof(path), "%s/tif_%dx%d_c%d_b%d_m%d_z%d.tif", dir, w, h, c, bits, mode, (int)comp);
	FILE *f = fopen(path, "wb");
	fwrite(data, 1, size, f);
	fclose(f);
	free(data);

	int   bad = 0;
	TIFF *t   = TIFFOpen(path, "r");
	CHECK(t != NULL, "libtiff open %s", path);
	if (t == NULL) {
		free(pixels);
		return 1;
	}
	uint32_t tw = 0, th = 0;
	uint16_t spp = 0, bps = 0, fmt = 1, cmp = 0;
	char    *soft = NULL;
	TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &tw);
	TIFFGetField(t, TIFFTAG_IMAGELENGTH, &th);
	TIFFGetField(t, TIFFTAG_SAMPLESPERPIXEL, &spp);
	TIFFGetField(t, TIFFTAG_BITSPERSAMPLE, &bps);
	TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLEFORMAT, &fmt);
	TIFFGetField(t, TIFFTAG_COMPRESSION, &cmp);
	TIFFGetField(t, TIFFTAG_SOFTWARE, &soft);
	CHECK((int)tw == w && (int)th == h && spp == c && bps == bits, "tiff header %s", path);
	CHECK(fmt == (bits == 32 ? 3 : 1), "tiff sample format");
	CHECK(cmp == comp, "tiff compression");
	CHECK(soft != NULL && strcmp(soft, "studio test") == 0, "tiff software");
	tsize_t  sl  = TIFFScanlineSize(t);
	size_t   rb  = (size_t)w * c * bits / 8;
	uint8_t *row = malloc(sl);
	CHECK((size_t)sl == rb, "tiff scanline size");
	for (uint32_t y = 0; y < th && !bad; ++y) {
		if (TIFFReadScanline(t, row, y, 0) < 0 || memcmp(row, (uint8_t *)pixels + y * rb, rb) != 0) {
			CHECK(0, "tiff pixels differ at row %u (%s)", y, path);
			bad = 1;
		}
	}
	free(row);
	TIFFClose(t);
	free(pixels);
	return bad;
}

int main(int argc, char **argv) {
	const char *dir = argc > 1 ? argv[1] : ".";
	int         sizes[][2] = {{1, 1}, {3, 2}, {17, 33}, {256, 256}, {1000, 700}};
	int         chans[]    = {1, 3, 4};
	int         count      = 0;
	for (int s = 0; s < 5; ++s) {
		for (int ci = 0; ci < 3; ++ci) {
			for (int mode = 0; mode < 3; ++mode) {
				for (int bits = 8; bits <= 16; bits += 8) {
					test_png(sizes[s][0], sizes[s][1], chans[ci], bits, mode, s % 2 == 0, dir);
					count++;
				}
				for (int bits = 8; bits <= 32; bits *= 2) {
					studio_tiff_compression_t comps[] = {STUDIO_TIFF_NONE, STUDIO_TIFF_LZW, STUDIO_TIFF_DEFLATE};
					for (int z = 0; z < 3; ++z) {
						test_tiff(sizes[s][0], sizes[s][1], chans[ci], bits, mode, comps[z], s % 2 == 1, dir);
						count++;
					}
				}
			}
		}
	}
	// Grande image unie (LZW : plusieurs Clear par bande ; deflate : bandes multiples en parallèle).
	test_tiff(4096, 512, 4, 16, 2, STUDIO_TIFF_LZW, true, dir);
	test_tiff(4096, 512, 4, 16, 1, STUDIO_TIFF_LZW, true, dir);
	test_png(4096, 512, 4, 16, 1, true, dir);
	count += 3;

	// Patch sdefl-bound : iron_deflate_raw (le compresseur zlib de stb_image_write, donc de tout export PNG upstream)
	// allouait sdefl_bound() octets, trop peu pour des données incompressibles au-delà de 256 Ko. ASan s'arrête ici
	// sans le patch.
	{
		int      n   = 8 << 20;
		uint8_t *raw = malloc(n);
		for (int i = 0; i < n; ++i) {
			raw[i] = (uint8_t)rnd();
		}
		int            len = 0;
		unsigned char *z   = iron_deflate_raw(raw, n, &len, 0);
		CHECK(z != NULL && len > n, "iron_deflate_raw on noise (%d -> %d)", n, len);
		free(z);
		free(raw);
	}

	// Erreurs.
	studio_image_t bad = {.pixels = "x", .width = 0, .height = 1, .channels = 3, .bits = 8};
	void          *o;
	size_t         n;
	char           err[128] = "";
	CHECK(!studio_png_encode(&bad, &o, &n, err, sizeof(err)) && err[0], "png w=0 refused");
	bad.width = 1;
	bad.bits  = 32;
	CHECK(!studio_png_encode(&bad, &o, &n, err, sizeof(err)), "png 32 bits refused");
	bad.channels = 2;
	bad.bits     = 8;
	CHECK(!studio_tiff_encode(&bad, &o, &n, err, sizeof(err)), "tiff 2 channels refused");
	CHECK(studio_image_raw_size(16384, 16384, 4, 16) == (size_t)16384 * 16384 * 8, "raw size");
	CHECK(studio_image_raw_size(-1, 2, 4, 8) == 0, "raw size invalid");
	printf("  %d images, %d failures\n", count, failures);
	return failures != 0;
}
