// Banc du lot B2 (studio_image.c) : chaque image encodée est relue par libpng ou libtiff et comparée à la source.
// Patch tiff-lzw : les TIFF 8 et 16 bits sont aussi relus par l'importeur d'ArmorPaint (plugins/io_tiff/io_tiff.c).
#include "../studio_image.h"
#include "stubs.h"
#include "iron_image.h" // iron_encode_png, iron_encode_jpg (base/sources/iron_image.c, WITH_IMAGE_WRITE)
#include <png.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tiffio.h>

unsigned char *iron_deflate_raw(unsigned char *data, int data_len, int *out_len, int quality);
void          *io_tiff_parse(uint8_t *buf, size_t len);

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

static uint32_t le32(const uint8_t *p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_le32(uint8_t *p, uint32_t v) {
	for (int i = 0; i < 4; ++i) {
		p[i] = (uint8_t)(v >> (8 * i));
	}
}

// StripByteCounts (279) d'un TIFF de studio_tiff_encode divisés par deux : bandes déclarées plus courtes que leurs lignes.
static void halve_strip_counts(uint8_t *tif) {
	uint32_t ifd = le32(tif + 4);
	int      n   = tif[ifd] | tif[ifd + 1] << 8;
	for (int i = 0; i < n; ++i) {
		uint8_t *e = tif + ifd + 2 + 12 * i;
		if ((e[0] | e[1] << 8) != 279) {
			continue;
		}
		uint32_t count = le32(e + 4);
		uint8_t *v     = count == 1 ? e + 8 : tif + le32(e + 8);
		for (uint32_t k = 0; k < count; ++k) {
			put_le32(v + 4 * k, le32(v + 4 * k) / 2);
		}
	}
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

	int bad = 0;
	// Importeur d'ArmorPaint : RGBA 8 bits (gris recopié, alpha 255 sans 4e canal, 16 bits divisés par 257). Pas de
	// flottants : il ne connaît pas le prédicteur 3.
	if (bits != 32) {
		const uint8_t  *p8  = pixels;
		const uint16_t *p16 = pixels;
		stub_quiet          = 1;
		bool ok             = io_tiff_parse(data, size) != NULL && stub_last_texture.width == w && stub_last_texture.height == h;
		for (int i = 0; ok && i < w * h; ++i) {
			uint8_t want[4] = {0, 0, 0, 255};
			for (int k = 0; k < 4; ++k) {
				int src = c == 1 ? (k < 3 ? 0 : -1) : k < c ? k : -1;
				if (src >= 0) {
					want[k] = bits == 8 ? p8[(size_t)i * c + src] : (uint8_t)(p16[(size_t)i * c + src] / 257);
				}
			}
			ok = memcmp((uint8_t *)stub_last_texture.pixels + (size_t)i * 4, want, 4) == 0;
		}
		CHECK(ok, "io_tiff pixels differ (%s)", path);
		bad |= !ok;
	}
	free(data);

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

	// Patch encode-image-overflow : iron_encode_png et iron_encode_jpg (le paquet d'assets d'un .arm, Pack Assets)
	// écrivaient le fichier dans les w * h * 4 octets des pixels. Une petite image ou du bruit donnent un fichier plus
	// gros : ASan s'arrête ici sans le patch. Le PNG relu par libpng doit redonner les pixels.
	{
		int sizes[][2] = {{1, 1}, {8, 4}, {16, 16}, {64, 64}};
		for (int s = 0; s < 4; ++s) {
			int      w = sizes[s][0], h = sizes[s][1], n = w * h * 4;
			buffer_t px = {.buffer = malloc(n), .length = n, .capacity = n};
			for (int i = 0; i < n; ++i) {
				px.buffer[i] = (uint8_t)rnd();
			}
			uint8_t *src = malloc(n); // IRON_BGRA échange R et B dans px pendant l'encodage
			memcpy(src, px.buffer, n);
			buffer_t *png = iron_encode_png(&px, w, h, 0);
			png_image pi  = {.version = PNG_IMAGE_VERSION};
			uint8_t  *got = malloc(n);
			bool      ok  = png_image_begin_read_from_memory(&pi, png->buffer, png->length);
			pi.format     = PNG_FORMAT_RGBA;
			ok            = ok && pi.width == (uint32_t)w && pi.height == (uint32_t)h && png_image_finish_read(&pi, NULL, got, 0, NULL);
			png_image_free(&pi);
			CHECK(ok && memcmp(got, src, n) == 0, "iron_encode_png %dx%d noise (%d bytes of pixels, %u of png)", w, h, n, png->length);
			memcpy(px.buffer, src, n);
			buffer_t *jpg = iron_encode_jpg(&px, w, h, 0, 80);
			CHECK(jpg->length > 4 && jpg->buffer[0] == 0xff && jpg->buffer[1] == 0xd8 && jpg->buffer[jpg->length - 2] == 0xff &&
			          jpg->buffer[jpg->length - 1] == 0xd9,
			      "iron_encode_jpg %dx%d noise (%u bytes)", w, h, jpg->length);
			free(png->buffer);
			free(png);
			free(jpg->buffer);
			free(jpg);
			free(got);
			free(src);
			free(px.buffer);
			count += 2;
		}
	}

	// Patch tiff-lzw : bandes LZW et non compressées abîmées (octets changés, ou taille déclarée trop courte : 1 cas sur 4).
	// L'importeur d'ArmorPaint lisait first[-1] et débordait sa pile de décodage sur un code invalide, et au-delà de la
	// bande quand elle était trop courte : ASan s'arrête ici sans le patch. Il doit s'arrêter proprement, image partielle.
	// Les bandes suivent l'en-tête (8 octets) ; les tables et l'IFD, à la fin, ne sont pas touchés.
	{
		int                       mutations = 0;
		studio_tiff_compression_t comps[]   = {STUDIO_TIFF_LZW, STUDIO_TIFF_NONE};
		for (int z = 0; z < 2; ++z) {
			for (int ci = 0; ci < 3; ++ci) {
				int            ch     = ci == 0 ? 1 : ci == 1 ? 3 : 4;
				void          *pixels = make_pixels(300, 70, ch, 8, 0);
				studio_image_t img    = {.pixels = pixels, .width = 300, .height = 70, .channels = ch, .bits = 8, .level = 5,
				                         .tiff_compression = comps[z], .software = "studio test"};
				void          *data   = NULL;
				size_t         size   = 0;
				char           e[256];
				if (!studio_tiff_encode(&img, &data, &size, e, sizeof(e))) {
					CHECK(0, "tiff encode for mutations: %s", e);
					free(pixels);
					continue;
				}
				uint8_t *copy = malloc(size);
				for (int m = 0; m < 400; ++m) {
					memcpy(copy, data, size);
					if (m % 4 == 3) {
						halve_strip_counts(copy);
					}
					else {
						for (int k = 0, n = 1 + rnd() % 8; k < n; ++k) {
							copy[8 + rnd() % (size - 512)] = (uint8_t)rnd();
						}
					}
					io_tiff_parse(copy, size);
					mutations++;
				}
				free(copy);
				free(data);
				free(pixels);
			}
		}
		stub_quiet = 0;
		printf("  io_tiff : %d mutations sans erreur ASan\n", mutations);
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
