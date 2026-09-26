#include "studio_exr_codec.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_message(char *buf, size_t size, const char *format, ...) {
	if (buf == NULL || size == 0) {
		return;
	}
	va_list args;
	va_start(args, format);
	vsnprintf(buf, size, format, args);
	va_end(args);
}

static int ascii_lower(int c) {
	return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static bool equals_nocase(const char *a, const char *b) {
	while (*a != 0 && ascii_lower((unsigned char)*a) == ascii_lower((unsigned char)*b)) {
		a++;
		b++;
	}
	return ascii_lower((unsigned char)*a) == ascii_lower((unsigned char)*b);
}

// ---- Compressions ----------------------------------------------------------------------------------------------------

static const char *compression_names[] = {"none", "rle",  "zips", "zip",      "piz",     "pxr24", "b44",
                                          "b44a", "dwaa", "dwab", "htj2k256", "htj2k32", "zstd"};
#define COMPRESSION_COUNT (int)(sizeof(compression_names) / sizeof(compression_names[0]))

const char *studio_exr_compression_name(int compression) {
	return compression >= 0 && compression < COMPRESSION_COUNT ? compression_names[compression] : "?";
}

int studio_exr_compression_from_name(const char *name) {
	for (int i = 0; i < COMPRESSION_COUNT; i++) {
		if (equals_nocase(name, compression_names[i])) {
			return i;
		}
	}
	return -1;
}

// ---- Écriture --------------------------------------------------------------------------------------------------------

exr_result studio_exr_encode(const studio_exr_encode_t *in, void **out, size_t *out_size) {
	*out      = NULL;
	*out_size = 0;
	if (in->width <= 0 || in->height <= 0 || (in->bits != 16 && in->bits != 32) || in->type < 1 || in->type > 3 ||
	    (in->type == 2 && (in->off < 0 || in->off > 3))) {
		return EXR_ERROR_INVALID_ARGUMENT;
	}
	size_t bpc  = (size_t)in->bits / 8;
	size_t npix = (size_t)in->width * (size_t)in->height;
	if (npix > SIZE_MAX / (4 * bpc) || in->pixels_size < npix * 4 * bpc) {
		return EXR_ERROR_INVALID_ARGUMENT;
	}

	// Canaux dans l'ordre des noms, celui d'OpenEXR (A, B, G, R) ; src = composante RGBA lue dans les pixels.
	static const char *rgba_names[4] = {"R", "G", "B", "A"};
	int                src[4];
	int                count = 0;
	if (in->type == 2) {
		src[count++] = in->off;
	}
	else {
		if (in->type == 3) {
			src[count++] = 3;
		}
		src[count++] = 2;
		src[count++] = 1;
		src[count++] = 0;
	}

	exr_part    part         = {0};
	exr_header *hdr          = &part.header;
	hdr->part_type           = EXR_PART_SCANLINE;
	hdr->compression         = in->compression;
	hdr->line_order          = EXR_LINEORDER_INCREASING_Y;
	hdr->data_window         = (exr_box2i){0, 0, in->width - 1, in->height - 1};
	hdr->display_window      = hdr->data_window;
	hdr->pixel_aspect_ratio  = 1.0f;
	hdr->screen_window_width = 1.0f;
	hdr->num_channels        = count;
	hdr->channels            = calloc((size_t)count, sizeof(exr_channel));
	part.width               = in->width;
	part.height              = in->height;
	part.images              = calloc((size_t)count, sizeof(void *));

	exr_result r = EXR_ERROR_OUT_OF_MEMORY;
	if (hdr->channels == NULL || part.images == NULL) {
		goto done;
	}
	for (int c = 0; c < count; c++) {
		exr_channel *ch = &hdr->channels[c];
		strcpy(ch->name, in->type == 2 ? "Y" : rgba_names[src[c]]);
		ch->pixel_type = in->bits == 16 ? EXR_PIXEL_HALF : EXR_PIXEL_FLOAT;
		ch->x_sampling = 1;
		ch->y_sampling = 1;
		part.images[c] = malloc(npix * bpc);
		if (part.images[c] == NULL) {
			goto done;
		}
		if (bpc == 2) {
			const uint16_t *s = (const uint16_t *)in->pixels + src[c];
			uint16_t       *d = part.images[c];
			for (size_t i = 0; i < npix; i++) {
				d[i] = s[i * 4];
			}
		}
		else {
			const float *s = (const float *)in->pixels + src[c];
			float       *d = part.images[c];
			for (size_t i = 0; i < npix; i++) {
				d[i] = s[i * 4];
			}
		}
	}
	for (int i = 0; i < in->attr_count; i++) {
		r = exr_header_set_string_attribute(NULL, hdr, in->attr_names[i], in->attr_values[i]);
		if (!EXR_OK(r)) {
			goto done;
		}
	}

	exr_image image = {.num_parts = 1, .parts = &part};
	r               = exr_save_to_memory(out, out_size, NULL, &image, in->compression);

done:
	exr_part_free(NULL, &part);
	return r;
}

// ---- Lecture ---------------------------------------------------------------------------------------------------------

// Composante de sortie : un plan (ou un tampon entrelacé, stride en éléments) ou une constante (p == NULL).
typedef struct source {
	const void    *p;
	exr_pixel_type type;
	int            stride;
	float          value;
} source_t;

// Cherche R, G, B, A, Y dans le calque `prefix` ("" = racine, sinon "calque." point compris) : nom exact d'abord,
// puis insensible à la casse. Retourne true si le calque a au moins R, G, B ou Y.
static bool pick_layer(const exr_header *h, const char *prefix, int idx[5]) {
	static const char *wanted[5] = {"R", "G", "B", "A", "Y"};
	size_t             len       = strlen(prefix);
	for (int k = 0; k < 5; k++) {
		idx[k] = -1;
	}
	for (int pass = 0; pass < 2; pass++) {
		for (int c = 0; c < h->num_channels; c++) {
			const char *name = h->channels[c].name;
			if (strncmp(name, prefix, len) != 0 || strchr(name + len, '.') != NULL) {
				continue;
			}
			for (int k = 0; k < 5; k++) {
				if (idx[k] < 0 && (pass == 0 ? strcmp(name + len, wanted[k]) == 0 : equals_nocase(name + len, wanted[k]))) {
					idx[k] = c;
				}
			}
		}
	}
	return idx[0] >= 0 || idx[1] >= 0 || idx[2] >= 0 || idx[4] >= 0;
}

static bool has_channel(const exr_header *h, const char *name) {
	for (int c = 0; c < h->num_channels; c++) {
		if (strcmp(h->channels[c].name, name) == 0) {
			return true;
		}
	}
	return false;
}

static void add_note(studio_exr_image_t *out, const char *format, ...) {
	size_t used = strlen(out->note);
	if (used + 3 >= sizeof(out->note)) {
		return;
	}
	if (used > 0) {
		strcpy(out->note + used, "; ");
		used += 2;
	}
	va_list args;
	va_start(args, format);
	vsnprintf(out->note + used, sizeof(out->note) - used, format, args);
	va_end(args);
}

// Convertit n éléments d'une source en float.
static const float *source_row_float(const source_t *s, size_t start, int n, float *tmp) {
	if (s->type == EXR_PIXEL_FLOAT && s->stride == 1) {
		return (const float *)s->p + start;
	}
	if (s->type == EXR_PIXEL_FLOAT) {
		const float *p = (const float *)s->p + start * (size_t)s->stride;
		for (int i = 0; i < n; i++) {
			tmp[i] = p[(size_t)i * s->stride];
		}
	}
	else if (s->type == EXR_PIXEL_HALF) { // plans uniquement : stride 1
		exr_half_to_float((const uint16_t *)s->p + start, tmp, (size_t)n);
	}
	else {
		const uint32_t *p = (const uint32_t *)s->p + start;
		for (int i = 0; i < n; i++) {
			tmp[i] = (float)p[i];
		}
	}
	return tmp;
}

exr_result studio_exr_decode(const void *data, size_t size, studio_exr_image_t *out, char *err, size_t err_size) {
	memset(out, 0, sizeof(*out));
	out->compression = -1;
	set_message(err, err_size, "");

	exr_context *ctx   = NULL;
	exr_reader  *rd    = NULL;
	exr_part     part  = {0};
	float       *yc    = NULL;
	float       *tmpf  = NULL;
	uint16_t    *tmph  = NULL;

	exr_result r = exr_context_create(NULL, &ctx);
	if (!EXR_OK(r)) {
		goto done;
	}
	// Plans décodés (toutes les voies de la part) : 4 Gio couvrent du 16K RGBA float. Le fichier est déjà en mémoire.
	exr_context_set_max_image_bytes(ctx, (uint64_t)4 << 30);
	exr_context_set_max_file_bytes(ctx, UINT32_MAX);
	r = exr_reader_open_memory_ctx(ctx, data, size, NULL, &rd);
	if (EXR_OK(r)) {
		r = exr_reader_parse_header(rd);
	}
	if (!EXR_OK(r)) {
		set_message(err, err_size, "invalid header");
		goto done;
	}

	int num_parts = exr_reader_num_parts(rd);
	int pi        = -1;
	for (int i = 0; i < num_parts; i++) {
		const exr_header *h = exr_reader_part_header(rd, i);
		if (h != NULL && (h->part_type == EXR_PART_SCANLINE || h->part_type == EXR_PART_TILED)) {
			pi = i;
			break;
		}
	}
	if (pi < 0) {
		r = EXR_ERROR_UNSUPPORTED;
		set_message(err, err_size, "deep data only");
		goto done;
	}
	const exr_header *h = exr_reader_part_header(rd, pi);
	out->compression    = h->compression;
	if (h->compression == EXR_COMPRESSION_DWAA || h->compression == EXR_COMPRESSION_DWAB) {
		r = EXR_ERROR_UNSUPPORTED;
		set_message(err, err_size, "%s compression is not supported", studio_exr_compression_name(h->compression));
		goto done;
	}
	if (num_parts > 1) {
		add_note(out, "part %d of %d", pi + 1, num_parts);
	}
	if (h->tiled && h->level_mode != EXR_TILE_ONE_LEVEL) {
		add_note(out, "mip level 0");
	}

	// Choix des canaux : la racine, sinon le premier calque (ordre des noms) qui a R, G, B ou Y.
	int  idx[5];
	bool found = pick_layer(h, "", idx);
	if (!found) {
		char tried[EXR_MAX_NAME] = "";
		int  tries               = 0;
		for (int c = 0; c < h->num_channels && !found && tries < 64; c++) {
			const char *name = h->channels[c].name;
			const char *dot  = strrchr(name, '.');
			size_t      len  = dot != NULL ? (size_t)(dot - name + 1) : 0;
			if (dot == NULL || (len == strlen(tried) && strncmp(name, tried, len) == 0)) {
				continue;
			}
			memcpy(tried, name, len);
			tried[len] = 0;
			tries++;
			found = pick_layer(h, tried, idx);
			if (found) {
				add_note(out, "layer \"%.*s\"", (int)len - 1, tried);
			}
		}
	}
	if (!found) {
		for (int k = 0; k < 5; k++) {
			idx[k] = -1;
		}
	}
	int  rgb    = (idx[0] >= 0) + (idx[1] >= 0) + (idx[2] >= 0);
	int  gray   = -1; // canal lu en niveaux de gris
	// Y/RY/BY à la racine : luminance/chroma, reconstruit en couleur par tinyexr si le format est conforme.
	bool yc_try = idx[4] >= 0 && rgb == 0 && strchr(h->channels[idx[4]].name, '.') == NULL && has_channel(h, "RY") &&
	              has_channel(h, "BY");
	if (!found) {
		gray = 0;
		add_note(out, "gray from channel \"%s\"", h->channels[0].name);
	}
	else if (rgb == 0) {
		gray = idx[4];
	}
	else if (rgb == 1) {
		gray = idx[0] >= 0 ? idx[0] : idx[1] >= 0 ? idx[1] : idx[2];
		add_note(out, "gray from %s", h->channels[gray].name);
	}
	int used[4] = {gray >= 0 ? gray : idx[0], gray >= 0 ? gray : idx[1], gray >= 0 ? gray : idx[2], idx[3]};

	// Géométrie : la fenêtre d'affichage, sauf si elle est invalide ou trop grande.
	exr_box2i dw   = h->data_window;
	exr_box2i disp = h->display_window;
	int64_t   ow   = (int64_t)disp.max_x - disp.min_x + 1;
	int64_t   oh   = (int64_t)disp.max_y - disp.min_y + 1;
	if (ow < 1 || oh < 1 || ow > STUDIO_EXR_MAX_DIM || oh > STUDIO_EXR_MAX_DIM) {
		disp = dw;
		ow   = (int64_t)dw.max_x - dw.min_x + 1;
		oh   = (int64_t)dw.max_y - dw.min_y + 1;
		add_note(out, "displayWindow ignored, dataWindow used");
	}
	else if (memcmp(&dw, &disp, sizeof(dw)) != 0) {
		add_note(out, "dataWindow (%d,%d)-(%d,%d) placed in displayWindow", dw.min_x, dw.min_y, dw.max_x, dw.max_y);
	}
	if (ow < 1 || oh < 1 || ow > STUDIO_EXR_MAX_DIM || oh > STUDIO_EXR_MAX_DIM) {
		r = EXR_ERROR_LIMIT_EXCEEDED;
		set_message(err, err_size, "image too large (%lldx%lld, max %d)", (long long)ow, (long long)oh, STUDIO_EXR_MAX_DIM);
		goto done;
	}
	r = exr_reader_read_part(rd, pi, &part);
	if (!EXR_OK(r)) {
		set_message(err, err_size, "%s", exr_result_string(r));
		goto done;
	}

	source_t src[4];
	bool     is_half = true;
	if (yc_try && exr_part_is_luminance_chroma(&part)) {
		int yw, yh;
		r = exr_part_yc_to_rgba_float(NULL, &part, &yc, &yw, &yh);
		if (!EXR_OK(r)) {
			set_message(err, err_size, "luminance/chroma: %s", exr_result_string(r));
			goto done;
		}
		for (int k = 0; k < 4; k++) {
			src[k] = (source_t){.p = yc + k, .type = EXR_PIXEL_FLOAT, .stride = 4};
		}
		is_half = false;
		add_note(out, "luminance/chroma");
	}
	else {
		if (gray >= 0 && gray == idx[4]) {
			add_note(out, "gray from Y");
		}
		for (int k = 0; k < 4; k++) {
			if (used[k] < 0) {
				src[k] = (source_t){.p = NULL, .value = k == 3 ? 1.0f : 0.0f};
				continue;
			}
			const exr_channel *ch = &part.header.channels[used[k]];
			if (ch->x_sampling != 1 || ch->y_sampling != 1) {
				r = EXR_ERROR_UNSUPPORTED;
				set_message(err, err_size, "subsampled channel \"%s\"", ch->name);
				goto done;
			}
			src[k] = (source_t){.p = part.images[used[k]], .type = ch->pixel_type, .stride = 1};
			if (ch->pixel_type != EXR_PIXEL_HALF) {
				is_half = false;
			}
		}
	}
	// buffer_t (iron) a une longueur sur 32 bits.
	if (!is_half && (uint64_t)ow * (uint64_t)oh * 16 > UINT32_MAX) {
		is_half = true;
		add_note(out, "converted to half");
	}

	size_t bpc   = is_half ? 2 : 4;
	size_t opix  = (size_t)ow * (size_t)oh;
	out->pixels  = calloc(opix * 4, bpc); // 0 hors dataWindow, alpha compris
	out->width   = (int)ow;
	out->height  = (int)oh;
	out->is_half = is_half;
	if (out->pixels == NULL) {
		r = EXR_ERROR_OUT_OF_MEMORY;
		goto done;
	}

	int64_t x0 = dw.min_x > disp.min_x ? dw.min_x : disp.min_x;
	int64_t x1 = dw.max_x < disp.max_x ? dw.max_x : disp.max_x;
	int64_t y0 = dw.min_y > disp.min_y ? dw.min_y : disp.min_y;
	int64_t y1 = dw.max_y < disp.max_y ? dw.max_y : disp.max_y;
	if (x0 <= x1 && y0 <= y1) {
		int n = (int)(x1 - x0 + 1);
		tmpf  = malloc((size_t)n * sizeof(float));
		tmph  = malloc((size_t)n * sizeof(uint16_t));
		if (tmpf == NULL || tmph == NULL) {
			r = EXR_ERROR_OUT_OF_MEMORY;
			goto done;
		}
		for (int64_t y = y0; y <= y1; y++) {
			size_t start = (size_t)(y - dw.min_y) * (size_t)part.width + (size_t)(x0 - dw.min_x);
			size_t dst   = ((size_t)(y - disp.min_y) * (size_t)ow + (size_t)(x0 - disp.min_x)) * 4;
			for (int k = 0; k < 4; k++) {
				const source_t *s = &src[k];
				if (is_half) {
					uint16_t *d = (uint16_t *)out->pixels + dst + k;
					if (s->p == NULL) {
						uint16_t v = s->value == 1.0f ? 0x3c00 : 0;
						for (int i = 0; i < n; i++) {
							d[(size_t)i * 4] = v;
						}
						continue;
					}
					const uint16_t *row;
					if (s->type == EXR_PIXEL_HALF) {
						row = (const uint16_t *)s->p + start;
					}
					else {
						exr_float_to_half(source_row_float(s, start, n, tmpf), tmph, (size_t)n);
						row = tmph;
					}
					for (int i = 0; i < n; i++) {
						d[(size_t)i * 4] = row[i];
					}
				}
				else {
					float *d = (float *)out->pixels + dst + k;
					if (s->p == NULL) {
						for (int i = 0; i < n; i++) {
							d[(size_t)i * 4] = s->value;
						}
						continue;
					}
					const float *row = source_row_float(s, start, n, tmpf);
					for (int i = 0; i < n; i++) {
						d[(size_t)i * 4] = row[i];
					}
				}
			}
		}
	}
	r = EXR_SUCCESS;

done:
	if (!EXR_OK(r)) {
		free(out->pixels);
		out->pixels = NULL;
	}
	free(tmpf);
	free(tmph);
	free(yc);
	exr_part_free(NULL, &part);
	exr_reader_close(rd);
	exr_context_destroy(ctx);
	return r;
}

void studio_exr_image_free(studio_exr_image_t *img) {
	free(img->pixels);
	img->pixels = NULL;
}

// ---- Repli DWAA/DWAB vers io_exr_parse -------------------------------------------------------------------------------

#ifdef WITH_COMPRESS

int zsinflate(void *out, int cap, const void *in, int size); // base/sources/libs/sinfl.h, compilé par iron_compress.c

static uint32_t rd_u32(const uint8_t *p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t rd_u64(const uint8_t *p) {
	return (uint64_t)rd_u32(p) | (uint64_t)rd_u32(p + 4) << 32;
}

// Chaîne terminée par NUL d'au plus 255 caractères dans [*pos, end) : la taille des tampons name[256] d'io_exr_parse.
static bool read_name(const uint8_t *b, size_t end, size_t *pos, char out[256]) {
	for (size_t i = 0; i < 256 && *pos + i < end; i++) {
		if (b[*pos + i] == 0) {
			memcpy(out, b + *pos, i);
			out[i] = 0;
			*pos += i + 1;
			return true;
		}
	}
	return false;
}

// Rejoue get_bits (io_exr.c) en comptant les octets lus.
typedef struct bit_sim {
	const uint8_t *p;
	size_t         avail;
	size_t         used;
	uint64_t       c;
	int            lc;
} bit_sim_t;

static uint32_t bit_sim_get(bit_sim_t *b, int n) {
	while (b->lc < n) {
		b->c = (b->c << 8) | (b->used < b->avail ? b->p[b->used] : 0);
		b->used++;
		b->lc += 8;
	}
	b->lc -= n;
	return (uint32_t)(b->c >> b->lc) & ((1u << n) - 1);
}

// huf_decode (io_exr.c) lit un en-tête de 20 octets, la table de codes, puis au plus nbits + 8 bits de données
// (un code de répétition entamé au dernier bit). On rejoue la table et on exige que table + ceil(nbits / 8) tiennent
// dans les `avail` octets ; l'octet de plus possible est couvert par STUDIO_EXR_DWA_PAD.
static bool huf_fits(const uint8_t *src, size_t avail) {
	if (avail < 20) {
		return false;
	}
	uint32_t  im    = rd_u32(src);
	uint32_t  iM    = rd_u32(src + 4);
	uint32_t  nbits = rd_u32(src + 12);
	bit_sim_t b     = {.p = src + 20, .avail = avail - 20};
	for (uint32_t i = im; i <= iM && i < 65537; i++) {
		uint32_t l = bit_sim_get(&b, 6);
		if (l == 63) {
			i += bit_sim_get(&b, 8) + 6 - 1;
		}
		else if (l >= 59) {
			i += l - 59 + 2 - 1;
		}
		if (b.used > b.avail) {
			return false;
		}
	}
	return (uint64_t)b.used + ((uint64_t)nbits + 7) / 8 <= b.avail;
}

// iron_inflate (io_exr) rappelle zsinflate avec une capacité doublée tant qu'il échoue : un flux que zsinflate ne
// décode pas le fait boucler sans fin. On n'accepte que les flux que zsinflate décode à la taille attendue.
// zsinflate lit jusqu'à 8 octets après la fin du flux (sinfl_refill) : il décode ici une copie suivie de
// STUDIO_EXR_DWA_PAD octets à zéro, comme io_exr_parse ensuite.
static bool zlib_decodes_to(const uint8_t *src, uint64_t size, uint64_t expected) {
	if (size < 6 || size > (1u << 28) || expected == 0 || expected > (1u << 28)) {
		return false;
	}
	uint8_t *in  = calloc((size_t)size + STUDIO_EXR_DWA_PAD, 1);
	void    *tmp = malloc((size_t)expected + 1);
	int      n   = -1;
	if (in != NULL && tmp != NULL) {
		memcpy(in, src, (size_t)size);
		n = zsinflate(tmp, (int)expected + 1, in, (int)size);
	}
	free(in);
	free(tmp);
	return n >= 0 && (uint64_t)n == expected;
}

// Rejoue les lectures de dwa_decode (io_exr.c) sur un chunk de `len` octets.
static bool dwa_chunk_ok(const uint8_t *c, uint64_t len, int64_t width, int lines, int num_channels) {
	if (len < 88) {
		return false;
	}
	uint64_t h[11]; // version, unknown uncompressed / compressed, ac, dc, rle compressed / uncompressed / raw, ac count,
	                // dc count, ac compression
	for (int i = 0; i < 11; i++) {
		h[i] = rd_u64(c + i * 8);
	}
	uint64_t p = 88;
	if (h[0] >= 2) { // règles de canaux : taille sur 16 bits, taille comprise
		if (len - p < 2) {
			return false;
		}
		uint64_t rules = (uint64_t)c[p] | (uint64_t)c[p + 1] << 8;
		if (rules < 2 || rules > len - p) {
			return false;
		}
		p += rules;
	}
	// dwa_decode lit num_channels * blocs coefficients DC, et alloue h[8] coefficients AC (63 au plus par bloc).
	uint64_t dct = (uint64_t)num_channels * (uint64_t)((width + 7) / 8) * (uint64_t)((lines + 7) / 8);
	if (h[9] != dct || h[8] > 64 * dct || h[10] > 1) {
		return false;
	}
	if (h[2] > len - p) {
		return false;
	}
	p += h[2];
	if (h[3] > len - p) {
		return false;
	}
	if (h[10] == 0 ? !huf_fits(c + p, (size_t)(len - p)) : !zlib_decodes_to(c + p, h[3], h[8] * 2)) {
		return false;
	}
	p += h[3];
	if (h[4] > len - p || !zlib_decodes_to(c + p, h[4], h[9] * 2)) {
		return false;
	}
	p += h[4];
	return h[5] <= len - p;
}

bool studio_exr_dwa_legacy_safe(const void *data, size_t size, char *why, size_t why_size) {
	const uint8_t *b = data;
	if (size < 8 || rd_u32(b) != 20000630) {
		set_message(why, why_size, "not an EXR file");
		return false;
	}
	// Version 2, sans drapeau : scanline, une part, noms courts, pas de deep. C'est ce qu'io_exr_parse suppose.
	if (rd_u32(b + 4) != 2) {
		set_message(why, why_size, "tiled, multi-part, deep or long names");
		return false;
	}

	size_t  pos = 8;
	int     num_channels = 0, chlists = 0, compression = -1;
	bool    has_data_window = false;
	int64_t width = 0, height = 0;
	while (true) {
		char name[256];
		char type[256];
		if (!read_name(b, size, &pos, name)) {
			set_message(why, why_size, "invalid header");
			return false;
		}
		if (name[0] == 0) {
			break;
		}
		if (!read_name(b, size, &pos, type) || size - pos < 4) {
			set_message(why, why_size, "invalid header");
			return false;
		}
		uint32_t attr_size = rd_u32(b + pos);
		pos += 4;
		if (attr_size > size - pos) {
			set_message(why, why_size, "invalid header");
			return false;
		}
		size_t attr_end = pos + attr_size;

		if (strcmp(name, "channels") == 0 && strcmp(type, "chlist") == 0) {
			if (++chlists > 1) {
				set_message(why, why_size, "several channel lists");
				return false;
			}
			size_t cp = pos;
			while (true) {
				char ch[256];
				if (!read_name(b, attr_end, &cp, ch)) {
					set_message(why, why_size, "invalid channel list");
					return false;
				}
				if (ch[0] == 0) {
					break;
				}
				if (attr_end - cp < 16) {
					set_message(why, why_size, "invalid channel list");
					return false;
				}
				if (num_channels == 4) {
					set_message(why, why_size, "more than 4 channels");
					return false;
				}
				uint32_t pixel_type = rd_u32(b + cp);
				uint32_t xs         = rd_u32(b + cp + 8);
				uint32_t ys         = rd_u32(b + cp + 12);
				if (pixel_type != EXR_PIXEL_HALF || xs != 1 || ys != 1 || ch[1] != 0 || strchr("RGBYrgby", ch[0]) == NULL) {
					set_message(why, why_size, "channel \"%s\" (only half R, G, B, Y channels)", ch);
					return false;
				}
				num_channels++;
				cp += 16;
			}
		}
		else if (strcmp(name, "dataWindow") == 0 || strcmp(name, "displayWindow") == 0) {
			if (attr_size < 16) {
				set_message(why, why_size, "invalid %s", name);
				return false;
			}
			if (strcmp(name, "dataWindow") == 0) {
				width           = (int64_t)(int32_t)rd_u32(b + pos + 8) - (int32_t)rd_u32(b + pos) + 1;
				height          = (int64_t)(int32_t)rd_u32(b + pos + 12) - (int32_t)rd_u32(b + pos + 4) + 1;
				has_data_window = true;
			}
		}
		else if (strcmp(name, "compression") == 0) {
			if (attr_size < 1) {
				set_message(why, why_size, "invalid compression");
				return false;
			}
			compression = b[pos];
		}
		pos = attr_end;
	}

	if (compression != EXR_COMPRESSION_DWAA && compression != EXR_COMPRESSION_DWAB) {
		set_message(why, why_size, "not DWAA/DWAB");
		return false;
	}
	if (num_channels == 0) {
		set_message(why, why_size, "no channel");
		return false;
	}
	if (!has_data_window || width < 1 || height < 1 || width > STUDIO_EXR_MAX_DIM || height > STUDIO_EXR_MAX_DIM) {
		set_message(why, why_size, "invalid or too large dataWindow");
		return false;
	}

	int      lines_per_chunk = compression == EXR_COMPRESSION_DWAA ? 32 : 256;
	uint64_t num_chunks      = (uint64_t)(height + lines_per_chunk - 1) / lines_per_chunk;
	if (num_chunks > (size - pos) / 8) {
		set_message(why, why_size, "truncated offset table");
		return false;
	}
	for (uint64_t k = 0; k < num_chunks; k++) {
		uint64_t offset = rd_u64(b + pos + k * 8);
		// io_exr_parse lit l'offset sur 32 bits, puis la taille du chunk à offset + 4.
		if (offset > UINT32_MAX || offset > size || size - offset < 8) {
			set_message(why, why_size, "chunk %llu: invalid offset", (unsigned long long)k);
			return false;
		}
		uint64_t len   = rd_u32(b + offset + 4);
		int      lines = (int)(height - (int64_t)k * lines_per_chunk);
		if (lines > lines_per_chunk) {
			lines = lines_per_chunk;
		}
		if (len > size - offset - 8 || !dwa_chunk_ok(b + offset + 8, len, width, lines, num_channels)) {
			set_message(why, why_size, "chunk %llu: invalid DWA data", (unsigned long long)k);
			return false;
		}
	}
	return true;
}

#else

bool studio_exr_dwa_legacy_safe(const void *data, size_t size, char *why, size_t why_size) {
	(void)data;
	(void)size;
	set_message(why, why_size, "DWA decoder not built (WITH_COMPRESS)");
	return false;
}

#endif
