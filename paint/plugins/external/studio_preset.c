#include "studio_preset.h"
#include "studio_fs.h"
#include "studio_json.h"
#include "studio_naming.h"
#include "studio_sha256.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	char  *err;
	size_t err_size;
	bool   failed;
} ctx_t;

static void fail(ctx_t *c, const char *format, ...) {
	if (c->failed) {
		return;
	}
	c->failed = true;
	if (c->err != NULL && c->err_size > 0) {
		va_list args;
		va_start(args, format);
		vsnprintf(c->err, c->err_size, format, args);
		va_end(args);
	}
}

// ---- Parcours JSON ---------------------------------------------------------------------------------------------------

static const char *ws(const char *p) {
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
		p++;
	}
	return p;
}

typedef struct {
	const char *p; // position dans l'objet ou le tableau
	bool        object;
} iter_t;

static iter_t iter_begin(studio_json_value_t v) {
	// Valeur absente : itérateur vide.
	return (iter_t){.p = v.start != NULL ? v.start + 1 : "", .object = v.type == STUDIO_JSON_OBJECT};
}

// Membre suivant : *key (malloc, objets seulement) et *value. false à la fin. Le texte a été validé en entier avant.
static bool iter_next(iter_t *it, char **key, studio_json_value_t *value) {
	const char *p = ws(it->p);
	if (*p == ',') {
		p = ws(p + 1);
	}
	if (*p == '}' || *p == ']' || *p == 0) {
		return false;
	}
	if (it->object) {
		studio_json_type_t t;
		const char        *end = studio_json_skip(p, &t);
		*key                   = studio_json_text((studio_json_value_t){.start = p, .len = (size_t)(end - p), .type = t});
		p                      = ws(end) + 1; // ':'
		p                      = ws(p);
	}
	studio_json_type_t t;
	const char        *end = studio_json_skip(p, &t);
	*value                 = (studio_json_value_t){.start = p, .len = (size_t)(end - p), .type = t};
	it->p                  = end;
	return true;
}

static char *text_of(studio_json_value_t v) {
	return studio_json_text(v);
}

// Chaîne attendue : malloc, NULL et erreur sinon.
static char *want_string(ctx_t *c, studio_json_value_t v, const char *where) {
	if (v.type != STUDIO_JSON_STRING) {
		fail(c, "%s: string expected", where);
		return NULL;
	}
	char *s = text_of(v);
	if (s == NULL) {
		fail(c, "out of memory");
	}
	return s;
}

static void check_naming(ctx_t *c, const char *naming, const char *where) {
	char err[400];
	if (naming != NULL && !studio_naming_check(naming, err, sizeof(err))) {
		fail(c, "%s: %s", where, err);
	}
}

static int want_int(ctx_t *c, studio_json_value_t v, const char *where, int min, int max) {
	long long n = studio_json_int(v, -1);
	if (v.type != STUDIO_JSON_NUMBER || memchr(v.start, '.', v.len) != NULL || n < min || n > max) {
		fail(c, "%s: integer between %d and %d expected", where, min, max);
		return min;
	}
	return (int)n;
}

static bool want_bool(ctx_t *c, studio_json_value_t v, const char *where) {
	if (v.type != STUDIO_JSON_BOOL) {
		fail(c, "%s: true or false expected", where);
		return false;
	}
	return v.start[0] == 't';
}

// ---- Valeurs ---------------------------------------------------------------------------------------------------------

static const char *format_names[] = {"png", "jpg", "tiff", "exr"};
static const char *color_names[]  = {"raw", "linear", "srgb", "ocio"};
static const char *exr_compressions[] = {"none", "rle", "zips", "zip", "piz", "pxr24", "b44", "b44a", "htj2k256", "htj2k32"};

const char *studio_format_name(studio_format_t format) {
	return format >= 0 && format <= STUDIO_FORMAT_EXR ? format_names[format] : "?";
}

const char *studio_format_ext(studio_format_t format) {
	static const char *exts[] = {"png", "jpg", "tif", "exr"};
	return format >= 0 && format <= STUDIO_FORMAT_EXR ? exts[format] : "";
}

const char *studio_color_name(studio_color_t color) {
	return color >= 0 && color <= STUDIO_COLOR_OCIO ? color_names[color] : "?";
}

const char *studio_preset_color(const studio_preset_texture_t *t) {
	return t->color == STUDIO_COLOR_OCIO ? t->color_space : studio_color_name(t->color);
}

bool studio_preset_texture_is_color(const studio_preset_texture_t *t, int (*is_data)(const char *colorspace)) {
	if (t->color == STUDIO_COLOR_RAW) {
		return false;
	}
	return t->color != STUDIO_COLOR_OCIO || is_data == NULL || is_data(t->color_space) != 1;
}

int studio_preset_override_color(studio_preset_t *p, const char *cs, int (*is_data)(const char *colorspace)) {
	int changed = 0;
	for (int i = 0; i < p->texture_count; ++i) {
		studio_preset_texture_t *t = &p->textures[i];
		if (!studio_preset_texture_is_color(t, is_data)) {
			continue;
		}
		char *name = strdup(cs);
		if (name == NULL) {
			return -1;
		}
		free(t->color_space);
		t->color_space = name;
		t->color       = STUDIO_COLOR_OCIO;
		changed++;
	}
	return changed;
}

int studio_preset_deflate_level(const char *compression) {
	if (compression != NULL && strcmp(compression, "fast") == 0) {
		return 1;
	}
	if (compression != NULL && strcmp(compression, "best") == 0) {
		return 8;
	}
	return 5;
}

static int find_name(const char *s, const char *const *names, int count) {
	for (int i = 0; s != NULL && i < count; ++i) {
		if (strcmp(s, names[i]) == 0) {
			return i;
		}
	}
	return -1;
}

static bool valid_compression(studio_format_t format, const char *s) {
	static const char *png[]  = {"fast", "default", "best"};
	static const char *tiff[] = {"none", "lzw", "deflate"};
	switch (format) {
	case STUDIO_FORMAT_PNG:
		return find_name(s, png, 3) >= 0;
	case STUDIO_FORMAT_TIFF:
		return find_name(s, tiff, 3) >= 0;
	case STUDIO_FORMAT_EXR:
		return find_name(s, exr_compressions, (int)(sizeof(exr_compressions) / sizeof(exr_compressions[0]))) >= 0;
	default:
		return false;
	}
}

static const char *compression_help(studio_format_t format) {
	switch (format) {
	case STUDIO_FORMAT_PNG:
		return "fast, default, best";
	case STUDIO_FORMAT_TIFF:
		return "none, lzw, deflate";
	case STUDIO_FORMAT_EXR:
		return "none, rle, zips, zip, piz, pxr24, b44, b44a, htj2k256, htj2k32";
	default:
		return "none (jpg has a quality instead)";
	}
}

bool studio_preset_bits_valid(studio_format_t format, int bits) {
	switch (format) {
	case STUDIO_FORMAT_PNG:
		return bits == 8 || bits == 16;
	case STUDIO_FORMAT_JPG:
		return bits == 8;
	case STUDIO_FORMAT_TIFF:
		return bits == 8 || bits == 16 || bits == 32;
	default:
		return bits == 16 || bits == 32;
	}
}

static const char *bits_help(studio_format_t format) {
	static const char *help[] = {"8 or 16", "8", "8, 16 or 32", "16 or 32"};
	return help[format];
}

// ---- Défauts ---------------------------------------------------------------------------------------------------------

typedef struct {
	int            format; // -1 : non fixé
	int            bits;   // 0 : non fixé
	studio_color_t color;
	char          *color_space; // color == STUDIO_COLOR_OCIO
	int            quality;
	int            resolution;
	char          *compression;          // pour les textures au format `format`
	char          *compressions[4];      // par format ("compression": {"exr": "zip", ...})
} defaults_t;

// color_space : un mot-clé d'ArmorPaint, sinon un nom OCIO (s repris dans *name).
static void parse_color(ctx_t *c, char *s, const char *where, studio_color_t *color, char **name) {
	if (s == NULL) {
		return;
	}
	int i = find_name(s, color_names, STUDIO_COLOR_OCIO);
	if (i >= 0) {
		*color = (studio_color_t)i;
		free(s);
	}
	else if (s[0] == 0 || strcmp(s, "ocio") == 0) {
		fail(c, "%s: color_space '%s': raw, linear, srgb or an OCIO colorspace name", where, s);
		free(s);
	}
	else {
		*color = STUDIO_COLOR_OCIO;
		free(*name);
		*name = s;
	}
}

static void parse_defaults(ctx_t *c, studio_json_value_t v, defaults_t *d) {
	if (v.type != STUDIO_JSON_OBJECT) {
		fail(c, "defaults: object expected");
		return;
	}
	iter_t              it = iter_begin(v);
	char               *key;
	studio_json_value_t val;
	while (!c->failed && iter_next(&it, &key, &val)) {
		if (strcmp(key, "format") == 0) {
			char *s   = want_string(c, val, "defaults.format");
			d->format = find_name(s, format_names, 4);
			if (s != NULL && d->format < 0) {
				fail(c, "defaults.format: '%s' unknown (png, jpg, tiff, exr)", s);
			}
			free(s);
		}
		else if (strcmp(key, "bits") == 0) {
			d->bits = want_int(c, val, "defaults.bits", 8, 32);
		}
		else if (strcmp(key, "color_space") == 0) {
			parse_color(c, want_string(c, val, "defaults.color_space"), "defaults", &d->color, &d->color_space);
		}
		else if (strcmp(key, "quality") == 0) {
			d->quality = want_int(c, val, "defaults.quality", 1, 100);
		}
		else if (strcmp(key, "resolution") == 0) {
			d->resolution = want_int(c, val, "defaults.resolution", 0, STUDIO_PRESET_MAX_RESOLUTION);
		}
		else if (strcmp(key, "compression") == 0) {
			if (val.type == STUDIO_JSON_OBJECT) {
				iter_t              ci = iter_begin(val);
				char               *fmt;
				studio_json_value_t cv;
				while (!c->failed && iter_next(&ci, &fmt, &cv)) {
					int f = find_name(fmt, format_names, 4);
					if (f < 0 || f == STUDIO_FORMAT_JPG) {
						fail(c, "defaults.compression: '%s' is not png, tiff or exr", fmt);
					}
					else {
						free(d->compressions[f]);
						d->compressions[f] = want_string(c, cv, "defaults.compression");
					}
					free(fmt);
				}
			}
			else {
				free(d->compression);
				d->compression = want_string(c, val, "defaults.compression");
			}
		}
		else {
			fail(c, "defaults: unknown key '%s' (format, bits, compression, color_space, quality, resolution)", key);
		}
		free(key);
	}
}

// ---- Textures --------------------------------------------------------------------------------------------------------

static void parse_channels(ctx_t *c, studio_json_value_t v, studio_preset_texture_t *t, const char *where) {
	int n = studio_json_length(v);
	if (v.type != STUDIO_JSON_ARRAY || (n != 1 && n != 3 && n != 4)) {
		fail(c, "%s.channels: array of 1, 3 or 4 channels expected", where);
		return;
	}
	t->channels            = n;
	iter_t              it = iter_begin(v);
	studio_json_value_t val;
	char               *unused = NULL;
	for (int i = 0; !c->failed && iter_next(&it, &unused, &val); ++i) {
		if (val.type != STUDIO_JSON_STRING && val.type != STUDIO_JSON_NUMBER) {
			fail(c, "%s.channels[%d]: channel name, expression or number expected", where, i);
			return;
		}
		t->sources[i] = text_of(val);
		if (t->sources[i] == NULL) {
			fail(c, "out of memory");
			return;
		}
		char err[256];
		t->exprs[i] = studio_expr_compile(t->sources[i], err, sizeof(err));
		if (t->exprs[i] == NULL) {
			fail(c, "%s.channels[%d]: %s", where, i, err);
		}
	}
}

static void parse_texture(ctx_t *c, studio_json_value_t v, studio_preset_texture_t *t, const defaults_t *d, int index) {
	char where[64];
	snprintf(where, sizeof(where), "textures[%d]", index);
	if (v.type != STUDIO_JSON_OBJECT) {
		fail(c, "%s: object expected", where);
		return;
	}
	char *format = NULL, *color = NULL, *compression = NULL;
	int   bits = 0, quality = 0, resolution = -1;
	bool  has_channels = false;

	iter_t              it = iter_begin(v);
	char               *key;
	studio_json_value_t val;
	while (!c->failed && iter_next(&it, &key, &val)) {
		char sub[96];
		snprintf(sub, sizeof(sub), "%s.%s", where, key);
		if (strcmp(key, "name") == 0) {
			t->name = want_string(c, val, sub);
			if (t->name != NULL) {
				snprintf(where, sizeof(where), "textures[%d] '%.40s'", index, t->name);
			}
		}
		else if (strcmp(key, "suffix") == 0) {
			t->suffix = want_string(c, val, sub);
		}
		else if (strcmp(key, "naming") == 0) {
			t->naming = want_string(c, val, sub);
			check_naming(c, t->naming, sub);
		}
		else if (strcmp(key, "channels") == 0) {
			parse_channels(c, val, t, where);
			has_channels = true;
		}
		else if (strcmp(key, "format") == 0) {
			format = want_string(c, val, sub);
		}
		else if (strcmp(key, "bits") == 0) {
			bits = want_int(c, val, sub, 8, 32);
		}
		else if (strcmp(key, "compression") == 0) {
			compression = want_string(c, val, sub);
		}
		else if (strcmp(key, "quality") == 0) {
			quality = want_int(c, val, sub, 1, 100);
		}
		else if (strcmp(key, "color_space") == 0) {
			color = want_string(c, val, sub);
		}
		else if (strcmp(key, "resolution") == 0) {
			resolution = want_int(c, val, sub, 0, STUDIO_PRESET_MAX_RESOLUTION);
		}
		else {
			fail(c, "%s: unknown key '%s' (name, suffix, naming, channels, format, bits, compression, quality, "
			        "color_space, resolution)", where, key);
		}
		free(key);
	}

	if (!c->failed && (t->name == NULL || t->name[0] == 0)) {
		fail(c, "%s: name missing", where);
	}
	if (!c->failed && !has_channels) {
		fail(c, "%s: channels missing", where);
	}
	if (!c->failed) {
		int f = format != NULL ? find_name(format, format_names, 4) : d->format;
		if (f < 0) {
			fail(c, format != NULL ? "%s: format '%s' unknown (png, jpg, tiff, exr)" : "%s: format missing (and no "
			                                                                           "defaults.format)",
			     where, format);
		}
		t->format = f < 0 ? STUDIO_FORMAT_PNG : (studio_format_t)f;
	}
	if (!c->failed) {
		t->bits = bits != 0 ? bits : d->bits != 0 && studio_preset_bits_valid(t->format, d->bits) ? d->bits : 0;
		if (t->bits == 0) { // défaut du format quand defaults.bits ne s'y applique pas
			t->bits = t->format == STUDIO_FORMAT_EXR ? 16 : 8;
		}
		if (!studio_preset_bits_valid(t->format, t->bits)) {
			fail(c, "%s: %d bits not supported in %s (%s)", where, t->bits, format_names[t->format], bits_help(t->format));
		}
	}
	if (!c->failed) {
		const char *comp = compression != NULL                ? compression
		                   : d->compressions[t->format] != NULL ? d->compressions[t->format]
		                   : d->format == (int)t->format      ? d->compression
		                                                      : NULL;
		if (t->format == STUDIO_FORMAT_JPG) {
			if (compression != NULL) {
				fail(c, "%s: jpg has no compression setting, use quality", where);
			}
		}
		else if (comp != NULL && !valid_compression(t->format, comp)) {
			fail(c, "%s: compression '%s' not supported in %s (%s)", where, comp, format_names[t->format],
			     compression_help(t->format));
		}
		else if (comp != NULL) {
			t->compression = strdup(comp);
		}
		else if (t->format == STUDIO_FORMAT_PNG) {
			t->compression = strdup("default");
		}
		else if (t->format == STUDIO_FORMAT_TIFF) {
			t->compression = strdup("deflate");
		}
	}
	if (!c->failed) {
		t->quality = quality != 0 ? quality : d->quality != 0 ? d->quality : 90;
		if (quality != 0 && t->format != STUDIO_FORMAT_JPG) {
			fail(c, "%s: quality only applies to jpg", where);
		}
	}
	if (!c->failed) {
		t->color = d->color;
		if (color != NULL) {
			parse_color(c, color, where, &t->color, &t->color_space);
			color = NULL;
		}
		else if (d->color == STUDIO_COLOR_OCIO) {
			t->color_space = strdup(d->color_space);
			if (t->color_space == NULL) {
				fail(c, "out of memory");
			}
		}
	}
	t->resolution = resolution >= 0 ? resolution : d->resolution;
	if (!c->failed && t->format == STUDIO_FORMAT_JPG && t->channels == 4) {
		fail(c, "%s: jpg has no alpha, use 1 or 3 channels", where);
	}
	if (!c->failed && t->suffix == NULL) {
		t->suffix = strdup(t->name);
	}
	free(format);
	free(color);
	free(compression);
}

static bool parse_resolution(studio_json_value_t v, studio_resolution_t *r) {
	if (v.type == STUDIO_JSON_NUMBER) {
		long long n = studio_json_int(v, 0);
		r->width = r->height = n > 0 && n <= 65536 ? (int)n : 0;
		return r->width > 0;
	}
	if (v.type != STUDIO_JSON_STRING) {
		return false;
	}
	char *s = text_of(v);
	int   w = 0, h = 0;
	char  tail = 0;
	bool  ok   = s != NULL && sscanf(s, "%dx%d%c", &w, &h, &tail) == 2 && w > 0 && h > 0 && w <= 65536 && h <= 65536;
	free(s);
	r->width  = w;
	r->height = h;
	return ok;
}

studio_preset_t *studio_preset_parse(const char *json, const char *name, char *err, size_t err_size) {
	ctx_t c = {.err = err, .err_size = err_size};
	if (err != NULL && err_size > 0) {
		err[0] = 0;
	}
	studio_json_type_t type;
	const char        *end = studio_json_skip(json, &type);
	if (end == NULL || *ws(end) != 0 || type != STUDIO_JSON_OBJECT) {
		fail(&c, "not a valid JSON object");
		return NULL;
	}
	studio_preset_t *p = calloc(1, sizeof(studio_preset_t));
	if (p == NULL) {
		fail(&c, "out of memory");
		return NULL;
	}
	p->name     = strdup(name != NULL ? name : "");
	p->manifest = true;
	studio_sha256_hex(json, strlen(json), p->sha256);

	defaults_t          d        = {.format = -1};
	studio_json_value_t root     = studio_json_find(json, "");
	studio_json_value_t textures = {0};
	iter_t              it       = iter_begin(root);
	char               *key;
	studio_json_value_t val;
	while (!c.failed && iter_next(&it, &key, &val)) {
		if (strcmp(key, "version") == 0) {
			if (want_int(&c, val, "version", 1, 1000) != 1 && !c.failed) {
				fail(&c, "version %s not supported (1)", "> 1");
			}
		}
		else if (strcmp(key, "description") == 0) {
			p->description = want_string(&c, val, "description");
		}
		else if (strcmp(key, "naming") == 0) {
			p->naming = want_string(&c, val, "naming");
			check_naming(&c, p->naming, "naming");
		}
		else if (strcmp(key, "defaults") == 0) {
			parse_defaults(&c, val, &d);
		}
		else if (strcmp(key, "resolutions") == 0) {
			int n = studio_json_length(val);
			if (val.type != STUDIO_JSON_ARRAY || n < 0) {
				fail(&c, "resolutions: array expected");
				break;
			}
			p->resolutions = calloc((size_t)(n > 0 ? n : 1), sizeof(studio_resolution_t));
			iter_t              ri = iter_begin(val);
			studio_json_value_t rv;
			char               *unused = NULL;
			for (int i = 0; p->resolutions != NULL && !c.failed && iter_next(&ri, &unused, &rv); ++i) {
				if (!parse_resolution(rv, &p->resolutions[i])) {
					fail(&c, "resolutions[%d]: size (4096) or \"WIDTHxHEIGHT\" expected", i);
				}
				p->resolution_count = i + 1;
			}
		}
		else if (strcmp(key, "overwrite") == 0) {
			static const char *modes[] = {"warn", "allow", "block"};
			char              *s       = want_string(&c, val, "overwrite");
			int                i       = find_name(s, modes, 3);
			if (s != NULL && i < 0) {
				fail(&c, "overwrite: '%s' unknown (allow, warn, block)", s);
			}
			p->overwrite = i < 0 ? STUDIO_OVERWRITE_WARN : (studio_overwrite_t)i;
			free(s);
		}
		else if (strcmp(key, "manifest") == 0) {
			p->manifest = want_bool(&c, val, "manifest");
		}
		else if (strcmp(key, "textures") == 0) {
			textures = val;
		}
		else {
			fail(&c, "unknown key '%s' (version, description, naming, defaults, resolutions, overwrite, manifest, "
			         "textures)", key);
		}
		free(key);
	}

	int n = textures.type == STUDIO_JSON_ARRAY ? studio_json_length(textures) : -1;
	if (!c.failed && n <= 0) {
		fail(&c, "textures: non-empty array expected");
	}
	if (!c.failed) {
		p->textures = calloc((size_t)n, sizeof(studio_preset_texture_t));
		if (p->textures == NULL) {
			fail(&c, "out of memory");
		}
	}
	iter_t              ti = iter_begin(textures);
	studio_json_value_t tv;
	char               *unused = NULL;
	for (int i = 0; !c.failed && i < n && iter_next(&ti, &unused, &tv); ++i) {
		p->texture_count = i + 1;
		parse_texture(&c, tv, &p->textures[i], &d, i);
		for (int j = 0; !c.failed && j < i; ++j) {
			if (strcmp(p->textures[j].name, p->textures[i].name) == 0) {
				fail(&c, "textures[%d]: name '%s' already used by textures[%d]", i, p->textures[i].name, j);
			}
		}
	}
	if (!c.failed && p->naming == NULL) {
		p->naming = strdup(STUDIO_PRESET_DEFAULT_NAMING);
	}
	if (!c.failed && (p->name == NULL || p->naming == NULL)) {
		fail(&c, "out of memory");
	}
	free(d.compression);
	free(d.color_space);
	for (int i = 0; i < 4; ++i) {
		free(d.compressions[i]);
	}
	if (c.failed) {
		studio_preset_free(p);
		return NULL;
	}
	return p;
}

studio_preset_t *studio_preset_load(const char *path, const char *name, char *err, size_t err_size) {
	size_t size = 0;
	char  *json = studio_fs_read(path, &size);
	if (json == NULL) {
		if (err != NULL && err_size > 0) {
			snprintf(err, err_size, "cannot read %s", path);
		}
		return NULL;
	}
	if (strlen(json) != size) {
		free(json);
		if (err != NULL && err_size > 0) {
			snprintf(err, err_size, "%s: NUL byte in file", path);
		}
		return NULL;
	}
	studio_preset_t *p = studio_preset_parse(json, name, err, err_size);
	free(json);
	if (p != NULL) {
		p->path = strdup(path);
	}
	return p;
}

void studio_preset_free(studio_preset_t *p) {
	if (p == NULL) {
		return;
	}
	for (int i = 0; i < p->texture_count; ++i) {
		studio_preset_texture_t *t = &p->textures[i];
		free(t->name);
		free(t->suffix);
		free(t->naming);
		free(t->compression);
		free(t->color_space);
		for (int j = 0; j < 4; ++j) {
			free(t->sources[j]);
			studio_expr_free(t->exprs[j]);
		}
	}
	free(p->textures);
	free(p->resolutions);
	free(p->name);
	free(p->path);
	free(p->description);
	free(p->naming);
	free(p);
}

int studio_preset_bits_options(studio_format_t format, int out[3]) {
	int n = 0;
	for (int bits = 8; bits <= 32; bits *= 2) {
		if (studio_preset_bits_valid(format, bits)) {
			out[n++] = bits;
		}
	}
	return n;
}

void studio_preset_output_size(const studio_preset_texture_t *t, int width, int height, int *out_width, int *out_height) {
	*out_width  = width;
	*out_height = height;
	int longest = width > height ? width : height;
	if (t->resolution <= 0 || longest <= 0 || t->resolution == longest) {
		return;
	}
	// Le plus grand côté prend la résolution, l'autre suit les proportions du projet (arrondi, au moins 1 pixel).
	double scale = (double)t->resolution / (double)longest;
	int    w     = (int)((double)width * scale + 0.5);
	int    h     = (int)((double)height * scale + 0.5);
	*out_width   = w > 0 ? w : 1;
	*out_height  = h > 0 ? h : 1;
}

bool studio_preset_apply_overrides(studio_preset_t *p, const char *json, char *err, size_t err_size) {
	ctx_t c = {.err = err, .err_size = err_size};
	if (err != NULL && err_size > 0) {
		err[0] = 0;
	}
	if (json == NULL || json[0] == 0) {
		return true;
	}
	studio_json_type_t type;
	const char        *end = studio_json_skip(json, &type);
	if (end == NULL || *ws(end) != 0 || type != STUDIO_JSON_OBJECT) {
		fail(&c, "overrides: JSON object expected");
		return false;
	}
	iter_t              it = iter_begin(studio_json_find(json, ""));
	char               *name;
	studio_json_value_t v;
	while (!c.failed && iter_next(&it, &name, &v)) {
		studio_preset_texture_t *t = NULL;
		for (int i = 0; i < p->texture_count && t == NULL; ++i) {
			t = strcmp(p->textures[i].name, name) == 0 ? &p->textures[i] : NULL;
		}
		if (v.type != STUDIO_JSON_OBJECT) {
			fail(&c, "overrides.%s: object expected", name);
		}
		iter_t              oi = iter_begin(v);
		char               *key;
		studio_json_value_t ov;
		while (!c.failed && v.type == STUDIO_JSON_OBJECT && iter_next(&oi, &key, &ov)) {
			char where[160];
			snprintf(where, sizeof(where), "overrides.%.64s.%.32s", name, key);
			if (strcmp(key, "bits") == 0) {
				int bits = want_int(&c, ov, where, 8, 32);
				if (!c.failed && t != NULL && !studio_preset_bits_valid(t->format, bits)) {
					fail(&c, "%s: %d bits not supported in %s (%s)", name, bits, format_names[t->format],
					     bits_help(t->format));
				}
				if (!c.failed && t != NULL) {
					t->bits = bits;
				}
			}
			else if (strcmp(key, "resolution") == 0) {
				int res = want_int(&c, ov, where, 0, STUDIO_PRESET_MAX_RESOLUTION);
				if (!c.failed && t != NULL) {
					t->resolution = res;
				}
			}
			else {
				fail(&c, "overrides.%s: unknown key '%s' (bits, resolution)", name, key);
			}
			free(key);
		}
		free(name); // texture absente du preset : ignorée (réglage d'une autre version du preset)
	}
	return !c.failed;
}
