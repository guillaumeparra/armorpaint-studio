#include "studio_image.h"
#include "sdefl.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Les échantillons 16 et 32 bits sont lus dans l'ordre de l'hôte, et le TIFF est écrit petit-boutiste (« II ») : toutes
// les cibles d'ArmorPaint (x86_64, arm64) le sont.
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#error "studio_image: little-endian host expected"
#endif

#define IMAGE_MAX_SIDE (1 << 20)

static void fail(char *err, size_t err_size, const char *format, ...) {
	if (err == NULL || err_size == 0) {
		return;
	}
	va_list args;
	va_start(args, format);
	vsnprintf(err, err_size, format, args);
	va_end(args);
}

size_t studio_image_raw_size(int width, int height, int channels, int bits) {
	if (width <= 0 || height <= 0 || channels <= 0 || bits <= 0 || width > IMAGE_MAX_SIDE || height > IMAGE_MAX_SIDE) {
		return 0;
	}
	return (size_t)width * (size_t)height * (size_t)channels * (size_t)(bits / 8);
}

static bool check_image(const studio_image_t *img, bool allow_float, char *err, size_t err_size) {
	if (img == NULL || img->pixels == NULL) {
		fail(err, err_size, "no pixels");
		return false;
	}
	if (img->width <= 0 || img->height <= 0 || img->width > IMAGE_MAX_SIDE || img->height > IMAGE_MAX_SIDE) {
		fail(err, err_size, "invalid size %dx%d", img->width, img->height);
		return false;
	}
	if (img->channels != 1 && img->channels != 3 && img->channels != 4) {
		fail(err, err_size, "%d channels (1, 3 or 4 expected)", img->channels);
		return false;
	}
	if (img->bits != 8 && img->bits != 16 && !(allow_float && img->bits == 32)) {
		fail(err, err_size, "%d bits not supported", img->bits);
		return false;
	}
	return true;
}

static void run_jobs(const studio_image_t *img, int jobs, studio_image_job_fn fn, void *ctx) {
	if (img->parallel != NULL && jobs > 1) {
		img->parallel(img->threads > 0 ? img->threads : 1, jobs, fn, ctx);
		return;
	}
	for (int i = 0; i < jobs; ++i) {
		fn(ctx, i);
	}
}

// Flux zlib de n octets (malloc) dans *out, false si la mémoire manque ou si n dépasse ce que sdefl indexe.
static bool zlib_compress(const uint8_t *in, size_t n, int level, uint8_t **out, size_t *out_size) {
	*out      = NULL;
	*out_size = 0;
	if (n > STUDIO_PNG_MAX_RAW) {
		return false;
	}
	struct sdefl *s = malloc(sizeof(struct sdefl));
	// Borne calculée ici plutôt que par sdefl_bound(), qui sous-estimait les blocs stockés au-delà de 256 Ko (patch
	// sdefl-bound) : l'encodeur reste juste même si ce patch se perd. 5 octets par bloc stocké de 64 Ko et par bloc
	// sdefl de 256 Ko, en-tête zlib et adler32.
	uint8_t      *o = malloc(n + 5 * (n / 65535 + n / (256 * 1024) + 4) + 32);
	if (s == NULL || o == NULL) {
		free(s);
		free(o);
		return false;
	}
	memset(s, 0, sizeof(struct sdefl));
	level   = level < SDEFL_LVL_MIN ? SDEFL_LVL_MIN : level > SDEFL_LVL_MAX ? SDEFL_LVL_MAX : level;
	int len = zsdeflate(s, o, in, (int)n, level);
	free(s);
	if (len <= 0) {
		free(o);
		return false;
	}
	*out      = o;
	*out_size = (size_t)len;
	return true;
}

// ---- Tampon d'écriture -----------------------------------------------------------------------------------------------

typedef struct {
	uint8_t *data;
	size_t   len;
	size_t   cap;
	bool     failed;
} bytes_t;

static bool bytes_reserve(bytes_t *b, size_t n) {
	if (b->failed) {
		return false;
	}
	if (n > SIZE_MAX - b->len) {
		b->failed = true;
		return false;
	}
	if (b->len + n <= b->cap) {
		return true;
	}
	size_t cap = b->cap < 4096 ? 4096 : b->cap;
	while (cap < b->len + n) {
		cap = cap > SIZE_MAX / 2 ? b->len + n : cap * 2;
	}
	uint8_t *grow = realloc(b->data, cap);
	if (grow == NULL) {
		b->failed = true;
		return false;
	}
	b->data = grow;
	b->cap  = cap;
	return true;
}

static void bytes_put(bytes_t *b, const void *data, size_t n) {
	if (n > 0 && bytes_reserve(b, n)) {
		memcpy(b->data + b->len, data, n);
		b->len += n;
	}
}

static void bytes_u8(bytes_t *b, uint8_t v) {
	bytes_put(b, &v, 1);
}

static void bytes_be32(bytes_t *b, uint32_t v) {
	uint8_t p[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
	bytes_put(b, p, 4);
}

static void bytes_le16(bytes_t *b, uint16_t v) {
	uint8_t p[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
	bytes_put(b, p, 2);
}

static void bytes_le32(bytes_t *b, uint32_t v) {
	uint8_t p[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
	bytes_put(b, p, 4);
}

// ---- PNG -------------------------------------------------------------------------------------------------------------

static uint32_t png_crc_table[256];
static bool     png_crc_ready = false;

static void png_crc_init(void) {
	// Idempotent : deux threads qui l'exécutent en même temps écrivent les mêmes valeurs.
	if (png_crc_ready) {
		return;
	}
	for (uint32_t n = 0; n < 256; ++n) {
		uint32_t c = n;
		for (int k = 0; k < 8; ++k) {
			c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
		}
		png_crc_table[n] = c;
	}
	png_crc_ready = true;
}

static uint32_t png_crc(uint32_t crc, const uint8_t *p, size_t n) {
	crc ^= 0xffffffffu;
	for (size_t i = 0; i < n; ++i) {
		crc = png_crc_table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
	}
	return crc ^ 0xffffffffu;
}

static void png_chunk(bytes_t *b, const char *type, const uint8_t *data, size_t n) {
	bytes_be32(b, (uint32_t)n);
	bytes_put(b, type, 4);
	bytes_put(b, data, n);
	uint32_t crc = png_crc(0, (const uint8_t *)type, 4);
	crc          = png_crc(crc, data, n);
	bytes_be32(b, crc);
}

typedef struct {
	const studio_image_t *img;
	size_t                row_bytes; // sans l'octet de filtre
	int                   bpp;       // octets par pixel, pour les filtres
	int                   rows_per_job;
	uint8_t              *filtered;  // height * (1 + row_bytes)
	bool                  failed;
} png_job_t;

// Ligne y en octets PNG (16 bits : gros-boutiste).
static void png_raw_row(const studio_image_t *img, int y, size_t row_bytes, uint8_t *dst) {
	if (img->bits == 8) {
		memcpy(dst, (const uint8_t *)img->pixels + (size_t)y * row_bytes, row_bytes);
		return;
	}
	const uint16_t *src = (const uint16_t *)img->pixels + (size_t)y * (row_bytes / 2);
	for (size_t i = 0; i < row_bytes / 2; ++i) {
		dst[i * 2]     = (uint8_t)(src[i] >> 8);
		dst[i * 2 + 1] = (uint8_t)src[i];
	}
}

static inline uint8_t png_paeth(int a, int b, int c) {
	int p  = a + b - c;
	int pa = abs(p - a);
	int pb = abs(p - b);
	int pc = abs(p - c);
	if (pa <= pb && pa <= pc) {
		return (uint8_t)a;
	}
	return (uint8_t)(pb <= pc ? b : c);
}

static inline uint8_t png_predict(int filter, const uint8_t *cur, const uint8_t *prev, size_t i, int bpp) {
	int a = i >= (size_t)bpp ? cur[i - bpp] : 0;
	int b = prev[i];
	int c = i >= (size_t)bpp ? prev[i - bpp] : 0;
	switch (filter) {
	case 1:
		return (uint8_t)a;
	case 2:
		return (uint8_t)b;
	case 3:
		return (uint8_t)((a + b) >> 1);
	case 4:
		return png_paeth(a, b, c);
	default:
		return 0;
	}
}

// Filtre adaptatif (heuristique de libpng) : pour chaque ligne, le filtre dont la somme des écarts signés est minimale.
static void png_filter_job(void *ctx, int job) {
	png_job_t *p   = ctx;
	size_t     rb  = p->row_bytes;
	uint8_t   *cur = malloc(rb);
	uint8_t   *prv = calloc(rb, 1);
	if (cur == NULL || prv == NULL) {
		free(cur);
		free(prv);
		p->failed = true;
		return;
	}
	int y0 = job * p->rows_per_job;
	int y1 = y0 + p->rows_per_job > p->img->height ? p->img->height : y0 + p->rows_per_job;
	if (y0 > 0) {
		png_raw_row(p->img, y0 - 1, rb, prv);
	}
	for (int y = y0; y < y1; ++y) {
		png_raw_row(p->img, y, rb, cur);
		int      best     = 0;
		uint64_t best_sum = UINT64_MAX;
		for (int f = 0; f < 5; ++f) {
			uint64_t sum = 0;
			for (size_t i = 0; i < rb && sum < best_sum; ++i) {
				int8_t d = (int8_t)(uint8_t)(cur[i] - png_predict(f, cur, prv, i, p->bpp));
				sum += (uint64_t)(d < 0 ? -d : d);
			}
			if (sum < best_sum) {
				best_sum = sum;
				best     = f;
			}
		}
		uint8_t *out = p->filtered + (size_t)y * (rb + 1);
		out[0]       = (uint8_t)best;
		for (size_t i = 0; i < rb; ++i) {
			out[i + 1] = (uint8_t)(cur[i] - png_predict(best, cur, prv, i, p->bpp));
		}
		uint8_t *swap = prv;
		prv           = cur;
		cur           = swap;
	}
	free(cur);
	free(prv);
}

bool studio_png_encode(const studio_image_t *img, void **out, size_t *size, char *err, size_t err_size) {
	*out  = NULL;
	*size = 0;
	if (!check_image(img, false, err, err_size)) {
		return false;
	}
	png_crc_init();
	size_t rb       = (size_t)img->width * (size_t)img->channels * (size_t)(img->bits / 8);
	size_t filtered = (rb + 1) * (size_t)img->height;
	if (filtered > STUDIO_PNG_MAX_RAW) {
		fail(err, err_size, "PNG too large (%zu MB before compression, %zu MB max): use EXR or TIFF", filtered >> 20,
		     STUDIO_PNG_MAX_RAW >> 20);
		return false;
	}
	png_job_t job = {.img = img, .row_bytes = rb, .bpp = img->channels * img->bits / 8, .rows_per_job = 64};
	job.filtered  = malloc(filtered);
	if (job.filtered == NULL) {
		fail(err, err_size, "out of memory");
		return false;
	}
	run_jobs(img, (img->height + job.rows_per_job - 1) / job.rows_per_job, png_filter_job, &job);
	uint8_t *z      = NULL;
	size_t   z_size = 0;
	bool     ok     = !job.failed && zlib_compress(job.filtered, filtered, img->level, &z, &z_size);
	free(job.filtered);
	if (!ok) {
		fail(err, err_size, "PNG compression failed (out of memory)");
		return false;
	}

	bytes_t       b         = {0};
	const uint8_t sig[8]    = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	uint8_t       ihdr[13]  = {0};
	const uint8_t types[5]  = {0, 0, 0, 2, 6}; // gris, -, -, RGB, RGBA
	uint32_t      w         = (uint32_t)img->width;
	uint32_t      h         = (uint32_t)img->height;
	uint8_t       dims[8]   = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
	                           (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h};
	memcpy(ihdr, dims, 8);
	ihdr[8]  = (uint8_t)img->bits;
	ihdr[9]  = types[img->channels];
	ihdr[10] = 0; // deflate
	ihdr[11] = 0; // filtres adaptatifs
	ihdr[12] = 0; // pas d'entrelacement
	bytes_put(&b, sig, 8);
	png_chunk(&b, "IHDR", ihdr, sizeof(ihdr));
	if (img->software != NULL && img->software[0] != 0) {
		bytes_t text = {0};
		bytes_put(&text, "Software", 9); // mot-clé et son NUL
		bytes_put(&text, img->software, strlen(img->software));
		png_chunk(&b, "tEXt", text.data, text.len);
		b.failed = b.failed || text.failed;
		free(text.data);
	}
	const size_t idat_max = (size_t)1 << 20;
	for (size_t off = 0; off < z_size; off += idat_max) {
		png_chunk(&b, "IDAT", z + off, z_size - off < idat_max ? z_size - off : idat_max);
	}
	png_chunk(&b, "IEND", NULL, 0);
	free(z);
	if (b.failed) {
		free(b.data);
		fail(err, err_size, "out of memory");
		return false;
	}
	*out  = b.data;
	*size = b.len;
	return true;
}

// ---- LZW (TIFF) ------------------------------------------------------------------------------------------------------

// LZW de TIFF 6.0 : codes de 9 à 12 bits, poids fort d'abord, Clear 256, EOI 257, changement de taille anticipé d'un
// code (« early change »), comme libtiff : la largeur augmente quand la prochaine entrée dépasse la largeur courante.

#define LZW_CLEAR     256
#define LZW_EOI       257
#define LZW_FIRST     258
#define LZW_FULL      4094 // libtiff : CODE_MAX - 1
#define LZW_HASH_BITS 14
#define LZW_HASH_SIZE (1 << LZW_HASH_BITS)

typedef struct {
	bytes_t  out;
	uint64_t bits;
	int      count;
} lzw_writer_t;

static void lzw_put(lzw_writer_t *w, unsigned code, int nbits) {
	w->bits = (w->bits << nbits) | code;
	w->count += nbits;
	while (w->count >= 8) {
		bytes_u8(&w->out, (uint8_t)(w->bits >> (w->count - 8)));
		w->count -= 8;
	}
}

static void lzw_flush(lzw_writer_t *w) {
	if (w->count > 0) {
		bytes_u8(&w->out, (uint8_t)(w->bits << (8 - w->count)));
		w->count = 0;
	}
}

static bool lzw_compress(const uint8_t *in, size_t n, uint8_t **out, size_t *out_size) {
	int32_t  *keys  = malloc(sizeof(int32_t) * LZW_HASH_SIZE);
	uint16_t *codes = malloc(sizeof(uint16_t) * LZW_HASH_SIZE);
	if (keys == NULL || codes == NULL) {
		free(keys);
		free(codes);
		return false;
	}
	memset(keys, 0xff, sizeof(int32_t) * LZW_HASH_SIZE);
	lzw_writer_t w = {0};
	bytes_reserve(&w.out, n / 2 + 64);
	int nbits    = 9;
	int maxcode  = (1 << nbits) - 1;
	int free_ent = LZW_FIRST;
	lzw_put(&w, LZW_CLEAR, nbits);
	if (n > 0) {
		unsigned ent = in[0];
		for (size_t i = 1; i < n; ++i) {
			int32_t  key  = (int32_t)((ent << 8) | in[i]);
			uint32_t slot = ((uint32_t)key * 2654435761u) >> (32 - LZW_HASH_BITS);
			while (keys[slot] != -1 && keys[slot] != key) {
				slot = (slot + 1) & (LZW_HASH_SIZE - 1);
			}
			if (keys[slot] == key) {
				ent = codes[slot];
				continue;
			}
			lzw_put(&w, ent, nbits);
			keys[slot]  = key;
			codes[slot] = (uint16_t)free_ent++;
			if (free_ent == LZW_FULL) {
				lzw_put(&w, LZW_CLEAR, nbits);
				memset(keys, 0xff, sizeof(int32_t) * LZW_HASH_SIZE);
				free_ent = LZW_FIRST;
				nbits    = 9;
				maxcode  = (1 << nbits) - 1;
			}
			else if (free_ent > maxcode) {
				nbits++;
				maxcode = (1 << nbits) - 1;
			}
			ent = in[i];
		}
		// Le décodeur ajoute encore une entrée pour ce dernier code : EOI suit la largeur qui en résulte.
		lzw_put(&w, ent, nbits);
		free_ent++;
		if (free_ent == LZW_FULL) {
			lzw_put(&w, LZW_CLEAR, nbits);
			nbits = 9;
		}
		else if (free_ent > maxcode) {
			nbits++;
		}
	}
	lzw_put(&w, LZW_EOI, nbits);
	lzw_flush(&w);
	free(keys);
	free(codes);
	if (w.out.failed) {
		free(w.out.data);
		return false;
	}
	*out      = w.out.data;
	*out_size = w.out.len;
	return true;
}

// ---- TIFF ------------------------------------------------------------------------------------------------------------

typedef struct {
	const studio_image_t *img;
	size_t                row_bytes;
	int                   rows_per_strip;
	int                   strips;
	uint8_t             **data;  // données de chaque bande (malloc), ou pointeur dans pixels sans compression
	size_t               *sizes; // octets de chaque bande
	bool                 *owned;
	bool                  failed;
} tiff_job_t;

// Prédicteur 2 (différence horizontale, entiers) ou 3 (flottants : octets regroupés par poids, du fort au faible, puis
// différence horizontale d'octets), en place sur une ligne. Voir libtiff (tif_predict.c, horDiff*, fpDiff).
static void tiff_predict_row(uint8_t *row, const studio_image_t *img, size_t row_bytes, uint8_t *scratch) {
	int    stride  = img->channels;
	size_t samples = (size_t)img->width * (size_t)img->channels;
	if (img->bits == 8) {
		for (size_t i = samples - 1; i >= (size_t)stride; --i) {
			row[i] = (uint8_t)(row[i] - row[i - stride]);
		}
	}
	else if (img->bits == 16) {
		uint16_t *s = (uint16_t *)row;
		for (size_t i = samples - 1; i >= (size_t)stride; --i) {
			s[i] = (uint16_t)(s[i] - s[i - stride]);
		}
	}
	else {
		memcpy(scratch, row, row_bytes);
		for (size_t i = 0; i < samples; ++i) {
			for (int k = 0; k < 4; ++k) {
				row[(size_t)(3 - k) * samples + i] = scratch[i * 4 + (size_t)k];
			}
		}
		for (size_t i = row_bytes - 1; i >= (size_t)stride; --i) {
			row[i] = (uint8_t)(row[i] - row[i - stride]);
		}
	}
}

static void tiff_strip_job(void *ctx, int job) {
	tiff_job_t           *t     = ctx;
	const studio_image_t *img   = t->img;
	int                   y0    = job * t->rows_per_strip;
	int                   rows  = y0 + t->rows_per_strip > img->height ? img->height - y0 : t->rows_per_strip;
	size_t                n     = t->row_bytes * (size_t)rows;
	const uint8_t        *src   = (const uint8_t *)img->pixels + (size_t)y0 * t->row_bytes;
	uint8_t              *strip = NULL;

	if (img->tiff_compression == STUDIO_TIFF_NONE) {
		t->data[job]  = (uint8_t *)src;
		t->sizes[job] = n;
		t->owned[job] = false;
		return;
	}
	strip            = malloc(n);
	uint8_t *scratch = img->bits == 32 ? malloc(t->row_bytes) : NULL;
	if (strip == NULL || (img->bits == 32 && scratch == NULL)) {
		free(strip);
		free(scratch);
		t->failed = true;
		return;
	}
	memcpy(strip, src, n);
	for (int r = 0; r < rows; ++r) {
		tiff_predict_row(strip + (size_t)r * t->row_bytes, img, t->row_bytes, scratch);
	}
	free(scratch);
	uint8_t *z      = NULL;
	size_t   z_size = 0;
	bool     ok     = img->tiff_compression == STUDIO_TIFF_LZW ? lzw_compress(strip, n, &z, &z_size)
	                                                           : zlib_compress(strip, n, img->level, &z, &z_size);
	free(strip);
	if (!ok) {
		t->failed = true;
		return;
	}
	t->data[job]  = z;
	t->sizes[job] = z_size;
	t->owned[job] = true;
}

typedef struct {
	uint16_t tag;
	uint16_t type;
	uint32_t count;
	uint32_t value; // valeur (alignée à gauche) ou décalage
} tiff_entry_t;

enum { TIFF_ASCII = 2, TIFF_SHORT = 3, TIFF_LONG = 4, TIFF_RATIONAL = 5 };

static void tiff_pad(bytes_t *b) {
	if (b->len & 1) {
		bytes_u8(b, 0);
	}
}

bool studio_tiff_encode(const studio_image_t *img, void **out, size_t *size, char *err, size_t err_size) {
	*out  = NULL;
	*size = 0;
	if (!check_image(img, true, err, err_size)) {
		return false;
	}
	if (img->tiff_compression != STUDIO_TIFF_NONE && img->tiff_compression != STUDIO_TIFF_LZW &&
	    img->tiff_compression != STUDIO_TIFF_DEFLATE) {
		fail(err, err_size, "unknown TIFF compression %d", (int)img->tiff_compression);
		return false;
	}
	tiff_job_t t     = {.img = img};
	t.row_bytes      = (size_t)img->width * (size_t)img->channels * (size_t)(img->bits / 8);
	size_t target    = (size_t)256 << 10;
	t.rows_per_strip = t.row_bytes >= target ? 1 : (int)(target / t.row_bytes);
	t.rows_per_strip = t.rows_per_strip > img->height ? img->height : t.rows_per_strip;
	t.strips         = (img->height + t.rows_per_strip - 1) / t.rows_per_strip;
	t.data           = calloc((size_t)t.strips, sizeof(uint8_t *));
	t.sizes          = calloc((size_t)t.strips, sizeof(size_t));
	t.owned          = calloc((size_t)t.strips, sizeof(bool));
	bytes_t b        = {0};
	bool    ok       = t.data != NULL && t.sizes != NULL && t.owned != NULL;
	if (ok) {
		run_jobs(img, t.strips, tiff_strip_job, &t);
		ok = !t.failed;
	}
	if (!ok) {
		fail(err, err_size, "TIFF compression failed (out of memory)");
	}

	uint64_t total = 8;
	for (int i = 0; ok && i < t.strips; ++i) {
		total += t.sizes[i];
	}
	// Tables, résolution, texte et IFD : largement sous 1 Mo pour 16K lignes.
	if (ok && total + ((uint64_t)t.strips * 8) + 4096 + (img->software ? strlen(img->software) : 0) > 0xffffffffu) {
		fail(err, err_size, "TIFF larger than 4 GB (%llu MB): use compression or EXR", (unsigned long long)(total >> 20));
		ok = false;
	}
	if (ok) {
		bytes_reserve(&b, (size_t)total + (size_t)t.strips * 8 + 1024);
		bytes_put(&b, "II", 2);
		bytes_le16(&b, 42);
		bytes_le32(&b, 0); // décalage de l'IFD, écrit à la fin
		uint32_t *offsets = malloc(sizeof(uint32_t) * (size_t)t.strips);
		ok                = offsets != NULL;
		for (int i = 0; ok && i < t.strips; ++i) {
			offsets[i] = (uint32_t)b.len;
			bytes_put(&b, t.data[i], t.sizes[i]);
		}

		int      n          = img->channels;
		bool     is_float   = img->bits == 32;
		uint16_t predictor  = img->tiff_compression == STUDIO_TIFF_NONE ? 1 : is_float ? 3 : 2;
		uint32_t bps_off    = 0;
		uint32_t format_off = 0;
		uint32_t strip_off  = 0;
		uint32_t count_off  = 0;
		uint32_t res_off    = 0;
		uint32_t soft_off   = 0;
		size_t   soft_len   = img->software != NULL ? strlen(img->software) + 1 : 0;
		if (ok && n > 2) {
			tiff_pad(&b);
			bps_off = (uint32_t)b.len;
			for (int i = 0; i < n; ++i) {
				bytes_le16(&b, (uint16_t)img->bits);
			}
			format_off = (uint32_t)b.len;
			for (int i = 0; i < n; ++i) {
				bytes_le16(&b, is_float ? 3 : 1);
			}
		}
		if (ok && t.strips > 1) {
			tiff_pad(&b);
			strip_off = (uint32_t)b.len;
			for (int i = 0; i < t.strips; ++i) {
				bytes_le32(&b, offsets[i]);
			}
			count_off = (uint32_t)b.len;
			for (int i = 0; i < t.strips; ++i) {
				bytes_le32(&b, (uint32_t)t.sizes[i]);
			}
		}
		if (ok) {
			tiff_pad(&b);
			res_off = (uint32_t)b.len;
			bytes_le32(&b, 72); // 72 ppp, en x puis en y
			bytes_le32(&b, 1);
			bytes_le32(&b, 72);
			bytes_le32(&b, 1);
			if (soft_len > 4) {
				soft_off = (uint32_t)b.len;
				bytes_put(&b, img->software, soft_len);
			}
		}

		tiff_entry_t e[20];
		int          count = 0;
#define TIFF_ENTRY(t_, ty_, c_, v_) e[count++] = (tiff_entry_t){.tag = (t_), .type = (ty_), .count = (c_), .value = (v_)}
		TIFF_ENTRY(256, TIFF_LONG, 1, (uint32_t)img->width);
		TIFF_ENTRY(257, TIFF_LONG, 1, (uint32_t)img->height);
		TIFF_ENTRY(258, TIFF_SHORT, (uint32_t)n, n > 2 ? bps_off : (uint32_t)img->bits);
		TIFF_ENTRY(259, TIFF_SHORT, 1, (uint32_t)img->tiff_compression);
		TIFF_ENTRY(262, TIFF_SHORT, 1, n == 1 ? 1u : 2u); // BlackIsZero, RGB
		TIFF_ENTRY(273, TIFF_LONG, (uint32_t)t.strips, t.strips > 1 ? strip_off : (ok ? offsets[0] : 0));
		TIFF_ENTRY(277, TIFF_SHORT, 1, (uint32_t)n);
		TIFF_ENTRY(278, TIFF_LONG, 1, (uint32_t)t.rows_per_strip);
		TIFF_ENTRY(279, TIFF_LONG, (uint32_t)t.strips, t.strips > 1 ? count_off : (uint32_t)t.sizes[0]);
		TIFF_ENTRY(282, TIFF_RATIONAL, 1, res_off);
		TIFF_ENTRY(283, TIFF_RATIONAL, 1, res_off + 8);
		TIFF_ENTRY(284, TIFF_SHORT, 1, 1); // entrelacé
		TIFF_ENTRY(296, TIFF_SHORT, 1, 2); // pouces
		if (soft_len > 0) {
			uint32_t inline_text = 0;
			if (soft_len <= 4) {
				memcpy(&inline_text, img->software, soft_len);
			}
			TIFF_ENTRY(305, TIFF_ASCII, (uint32_t)soft_len, soft_len > 4 ? soft_off : inline_text);
		}
		if (predictor != 1) {
			TIFF_ENTRY(317, TIFF_SHORT, 1, predictor);
		}
		if (n == 4) {
			TIFF_ENTRY(338, TIFF_SHORT, 1, 2); // alpha non prémultiplié
		}
		TIFF_ENTRY(339, TIFF_SHORT, (uint32_t)n, n > 2 ? format_off : (is_float ? 3u : 1u));
#undef TIFF_ENTRY

		tiff_pad(&b);
		uint32_t ifd = (uint32_t)b.len;
		bytes_le16(&b, (uint16_t)count);
		for (int i = 0; i < count; ++i) {
			bytes_le16(&b, e[i].tag);
			bytes_le16(&b, e[i].type);
			bytes_le32(&b, e[i].count);
			// Une valeur SHORT en ligne occupe les deux premiers octets du champ (deux si count vaut 2 ; n > 2 va dans
			// une table). Le texte en ligne est déjà dans l'ordre des octets.
			if (e[i].type == TIFF_SHORT && e[i].count <= 2 && !(e[i].tag == 258 && n > 2) && !(e[i].tag == 339 && n > 2)) {
				bytes_le16(&b, (uint16_t)e[i].value);
				bytes_le16(&b, 0);
			}
			else {
				bytes_le32(&b, e[i].value);
			}
		}
		bytes_le32(&b, 0); // pas d'IFD suivant
		if (!b.failed) {
			b.data[4] = (uint8_t)ifd;
			b.data[5] = (uint8_t)(ifd >> 8);
			b.data[6] = (uint8_t)(ifd >> 16);
			b.data[7] = (uint8_t)(ifd >> 24);
		}
		free(offsets);
		if (ok && b.failed) {
			fail(err, err_size, "out of memory");
			ok = false;
		}
	}

	for (int i = 0; t.data != NULL && i < t.strips; ++i) {
		if (t.owned[i]) {
			free(t.data[i]);
		}
	}
	free(t.data);
	free(t.sizes);
	free(t.owned);
	if (!ok) {
		free(b.data);
		return false;
	}
	*out  = b.data;
	*size = b.len;
	return true;
}
