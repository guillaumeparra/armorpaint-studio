#include "studio_preflight.h"
#include "studio_fs.h"
#include "studio_image.h"
#include "studio_json.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- Texte -----------------------------------------------------------------------------------------------------------

typedef struct {
	char  *data;
	size_t len;
	size_t cap;
	bool   failed;
} sb_t;

static void sb_printf(sb_t *b, const char *format, ...) {
	if (b->failed) {
		return;
	}
	for (;;) {
		va_list args;
		va_start(args, format);
		int n = vsnprintf(b->data != NULL ? b->data + b->len : NULL, b->data != NULL ? b->cap - b->len : 0, format, args);
		va_end(args);
		if (n < 0) {
			b->failed = true;
			return;
		}
		if (b->data != NULL && b->len + (size_t)n < b->cap) {
			b->len += (size_t)n;
			return;
		}
		size_t cap  = (b->cap + (size_t)n + 1) * 2;
		char  *grow = realloc(b->data, cap);
		if (grow == NULL) {
			b->failed = true;
			return;
		}
		b->data = grow;
		b->cap  = cap;
	}
}

// Chaîne JSON échappée.
static void sb_quote(sb_t *b, const char *s) {
	char *q = studio_json_quote(s);
	if (q == NULL) {
		b->failed = true;
		return;
	}
	sb_printf(b, "%s", q);
	free(q);
}

static char *sb_take(sb_t *b) {
	if (b->failed) {
		free(b->data);
		return NULL;
	}
	if (b->data == NULL) {
		return strdup("");
	}
	return b->data;
}

void studio_format_size(uint64_t bytes, char *out, size_t out_size) {
	if (bytes >= (uint64_t)1 << 30) {
		snprintf(out, out_size, "%.1f GB", (double)bytes / (double)((uint64_t)1 << 30));
	}
	else if (bytes >= (uint64_t)1 << 20) {
		snprintf(out, out_size, "%.0f MB", (double)bytes / (double)(1 << 20));
	}
	else {
		snprintf(out, out_size, "%.0f KB", (double)bytes / 1024.0);
	}
}

// ---- Problèmes -------------------------------------------------------------------------------------------------------

static void add_issue(studio_report_t *report, bool blocking, bool note, const char *code, const char *format,
                      va_list args) {
	studio_issue_t *grow = realloc(report->issues, sizeof(studio_issue_t) * (size_t)(report->count + 1));
	if (grow == NULL) {
		return;
	}
	report->issues = grow;
	char message[1024];
	vsnprintf(message, sizeof(message), format, args);
	studio_issue_t *issue = &report->issues[report->count++];
	issue->blocking       = blocking;
	issue->note           = note;
	issue->code           = strdup(code);
	issue->message        = strdup(message);
	if (blocking) {
		report->blocking++;
	}
}

void studio_report_add(studio_report_t *report, bool blocking, const char *code, const char *format, ...) {
	va_list args;
	va_start(args, format);
	add_issue(report, blocking, false, code, format, args);
	va_end(args);
}

void studio_report_note(studio_report_t *report, const char *code, const char *format, ...) {
	va_list args;
	va_start(args, format);
	add_issue(report, false, true, code, format, args);
	va_end(args);
}

void studio_report_free(studio_report_t *report) {
	for (int i = 0; i < report->count; ++i) {
		free(report->issues[i].code);
		free(report->issues[i].message);
	}
	free(report->issues);
	memset(report, 0, sizeof(*report));
}

static int compare_tiles(const void *a, const void *b) {
	return atoi(*(const char *const *)a) - atoi(*(const char *const *)b);
}

static int compare_strings(const void *a, const void *b) {
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static bool is_data_texture(const studio_preset_texture_t *t) {
	uint32_t vars = 0;
	for (int i = 0; i < t->channels && i < 3; ++i) {
		vars |= studio_expr_vars(t->exprs[i]);
	}
	return vars != 0 && (vars & STUDIO_VAR_COLOR_MASK) == 0;
}

// Que des canaux de calque (layer('AO')) : couleur ou donnée, le preset seul ne le dit pas.
static bool layers_only(const studio_preset_texture_t *t) {
	uint32_t vars   = 0;
	int      layers = 0;
	for (int i = 0; i < t->channels && i < 3; ++i) {
		vars |= studio_expr_vars(t->exprs[i]);
		layers += studio_expr_layer_count(t->exprs[i]);
	}
	return vars == 0 && layers > 0;
}

static bool reads_var(const studio_preset_texture_t *t, int var) {
	for (int i = 0; i < t->channels; ++i) {
		if (studio_expr_vars(t->exprs[i]) & (1u << var)) {
			return true;
		}
	}
	return false;
}

// Une seule fois par texture du preset (les tuiles et objets répètent les mêmes textures).
static bool first_of_texture(const studio_plan_t *plan, int index) {
	for (int i = 0; i < index; ++i) {
		if (!plan->files[i].skip && plan->files[i].texture == plan->files[index].texture) {
			return false;
		}
	}
	return true;
}

// Résolutions standard du preset ("resolutions"), fichier par fichier : chaque texture a la sienne.
static void check_resolution(const studio_plan_t *plan, const studio_plan_file_t *f, studio_report_t *r) {
	const studio_preset_t *p  = plan->preset;
	bool                   ok = p->resolution_count == 0;
	for (int i = 0; i < p->resolution_count && !ok; ++i) {
		ok = p->resolutions[i].width == f->width && p->resolutions[i].height == f->height;
	}
	if (ok) {
		return;
	}
	char   list[256] = "";
	size_t len       = 0;
	for (int i = 0; i < p->resolution_count && len + 24 < sizeof(list); ++i) {
		len += (size_t)snprintf(list + len, sizeof(list) - len, "%s%dx%d", i > 0 ? ", " : "", p->resolutions[i].width,
		                        p->resolutions[i].height);
	}
	studio_report_add(r, true, "resolution", "%s: %dx%d is not a %s standard (%s): change its resolution, or Project > "
	                  "Texture resolution", f->texture->name, f->width, f->height, p->name, list);
}

static void check_textures(studio_plan_t *plan, studio_report_t *r) {
	for (int i = 0; i < plan->file_count; ++i) {
		studio_plan_file_t            *f = &plan->files[i];
		const studio_preset_texture_t *t = f->texture;
		if (f->skip) {
			continue;
		}
		size_t raw = studio_image_raw_size(f->width, f->height, t->channels, t->bits);
		if (t->format == STUDIO_FORMAT_PNG && (uint64_t)raw + (uint64_t)f->height > STUDIO_PNG_MAX_RAW) {
			studio_report_add(r, true, "png_too_large", "%s: %dx%d %d-bit PNG is over the %d MB the PNG encoder "
			                  "compresses at once, use TIFF or EXR", t->name, f->width, f->height, t->bits,
			                  (int)(STUDIO_PNG_MAX_RAW >> 20));
		}
		if (t->format == STUDIO_FORMAT_TIFF && (uint64_t)raw > 0xffff0000ull) {
			bool none = t->compression != NULL && strcmp(t->compression, "none") == 0;
			studio_report_add(r, none, "tiff_too_large", "%s: %dx%d %d-bit TIFF is over 4 GB uncompressed%s", t->name,
			                  f->width, f->height, t->bits,
			                  none ? " (no BigTIFF), use compression or EXR" : ": the write fails if compression does "
			                                                                  "not bring it under 4 GB");
		}
		if (!first_of_texture(plan, i)) {
			continue;
		}
		check_resolution(plan, f, r);
		if (f->width > plan->width || f->height > plan->height) {
			studio_report_note(r, "upscaled", "%s: %dx%d from %dx%d layers (upscaled)", t->name, f->width, f->height,
			                   plan->width, plan->height);
		}
		// Choix du preset : notes, pas des avertissements. Une profondeur supérieure à celle des calques n'est pas
		// signalée : les graphistes le savent.
		if (t->bits == 8 && reads_var(t, STUDIO_VAR_HEIGHT)) {
			studio_report_note(r, "height_8bit", "%s: height in 8 bits (256 levels): stepping in displacement, "
			                  "use 16 bits or more", t->name);
		}
		if (t->format == STUDIO_FORMAT_JPG && is_data_texture(t)) {
			studio_report_note(r, "jpg_data", "%s: data map in lossy JPG", t->name);
		}
		if (t->format == STUDIO_FORMAT_EXR && t->color == STUDIO_COLOR_SRGB) {
			studio_report_note(r, "exr_srgb", "%s: sRGB-encoded EXR, renderers expect linear values", t->name);
		}
		if (t->bits == 8 && t->color == STUDIO_COLOR_LINEAR && !is_data_texture(t) && !layers_only(t)) {
			studio_report_note(r, "linear_8bit", "%s: linear colour in 8 bits: banding in dark values, use "
			                  "srgb or 16 bits", t->name);
		}
	}
}

static void check_udim(studio_plan_t *plan, studio_report_t *r) {
	if (plan->tiles_without_slot > 0) {
		studio_report_add(r, true, "udim_tile_limit",
		                  "%d UDIM tiles past the 64 of the layers, from %s on: they are "
		                  "not painted",
		                  plan->tiles_without_slot, plan->first_without_slot != NULL ? plan->first_without_slot : "?");
	}
	if (plan->udim_mesh && !plan->per_tile) {
		studio_report_add(r, false, "udim_not_per_tile", "the mesh has UDIM tiles but the export is not per tile "
		                  "(Export > Layers: per UDIM tile): all tiles go into one texture set");
	}
	if (!plan->per_tile || plan->tile_count < 2) {
		return;
	}
	const char **tiles = malloc(sizeof(char *) * (size_t)plan->tile_count);
	if (tiles == NULL) {
		return;
	}
	memcpy(tiles, plan->tiles, sizeof(char *) * (size_t)plan->tile_count);
	qsort(tiles, (size_t)plan->tile_count, sizeof(char *), compare_tiles);
	char   missing[512] = "";
	size_t len          = 0;
	int    count        = 0;
	for (int i = 1; i < plan->tile_count; ++i) {
		for (int t = atoi(tiles[i - 1]) + 1; t < atoi(tiles[i]); ++t) {
			if (count++ < 12 && len + 8 < sizeof(missing)) {
				len += (size_t)snprintf(missing + len, sizeof(missing) - len, "%s%d", len > 0 ? ", " : "", t);
			}
		}
	}
	if (count > 0) {
		studio_report_add(r, false, "udim_gaps", "missing UDIM tiles between %s and %s: %s%s (%d)", tiles[0],
		                  tiles[plan->tile_count - 1], missing, count > 12 ? "..." : "", count);
	}
	free(tiles);
}

static void check_paths(studio_plan_t *plan, studio_report_t *r) {
	// Doublons : deux fichiers du plan au même chemin (modèle sans <UDIM> ou sans <channel>...).
	const char **paths = malloc(sizeof(char *) * (size_t)(plan->file_count > 0 ? plan->file_count : 1));
	if (paths == NULL) {
		return;
	}
	int count = 0;
	for (int i = 0; i < plan->file_count; ++i) {
		if (!plan->files[i].skip) {
			paths[count++] = plan->files[i].path;
		}
	}
	qsort(paths, (size_t)count, sizeof(char *), compare_strings);
	for (int i = 1; i < count; ++i) {
		if (strcmp(paths[i], paths[i - 1]) == 0 && (i < 2 || strcmp(paths[i], paths[i - 2]) != 0)) {
			studio_report_add(r, true, "duplicate_path", "several textures would be written to %s: add <channel>, "
			                  "<variant> or <UDIM> to the naming", paths[i]);
		}
	}
	free(paths);

	// Existants.
	int         existing = 0;
	const char *first    = NULL;
	for (int i = 0; i < plan->file_count; ++i) {
		plan->files[i].exists = studio_fs_exists(plan->files[i].path);
		if (plan->files[i].exists && !plan->files[i].skip) {
			first = first != NULL ? first : plan->files[i].path;
			existing++;
		}
	}
	if (existing > 0 && plan->preset->overwrite != STUDIO_OVERWRITE_ALLOW) {
		bool block = plan->preset->overwrite == STUDIO_OVERWRITE_BLOCK;
		studio_report_add(r, block, "exists", "%d file%s already exist%s%s (%s)%s", existing, existing > 1 ? "s" : "",
		                  existing > 1 ? "" : "s", block ? "" : " and will be replaced", first,
		                  block ? ": this preset never overwrites, export to a new version" : "");
	}

	// Droits et espace, par dossier.
	char **dirs      = calloc((size_t)(plan->file_count > 0 ? plan->file_count : 1), sizeof(char *));
	int    dir_count = 0;
	for (int i = 0; dirs != NULL && i < plan->file_count; ++i) {
		if (plan->files[i].skip) {
			continue;
		}
		char *dir = studio_fs_dirname(plan->files[i].path);
		bool  seen = false;
		for (int j = 0; dir != NULL && j < dir_count && !seen; ++j) {
			seen = strcmp(dirs[j], dir) == 0;
		}
		if (dir == NULL || seen) {
			free(dir);
			continue;
		}
		dirs[dir_count++] = dir;
		uint64_t largest = 0, total = 0;
		for (int j = i; j < plan->file_count; ++j) {
			char *other = plan->files[j].skip ? NULL : studio_fs_dirname(plan->files[j].path);
			if (other != NULL && strcmp(other, dir) == 0) {
				largest = plan->files[j].raw_size > largest ? plan->files[j].raw_size : largest;
				total += plan->files[j].raw_size;
			}
			free(other);
		}
		if (!studio_fs_can_write_in(dir[0] != 0 ? dir : ".")) {
			studio_report_add(r, true, "not_writable", "cannot write in %s", dir[0] != 0 ? dir : ".");
			continue;
		}
		int64_t space = studio_fs_free_space(dir[0] != 0 ? dir : ".");
		char    a[32], b[32];
		if (space >= 0 && (uint64_t)space < largest) {
			studio_format_size((uint64_t)space, a, sizeof(a));
			studio_format_size(largest, b, sizeof(b));
			studio_report_add(r, true, "disk_full", "%s free in %s, the largest texture takes up to %s", a, dir, b);
		}
		else if (space >= 0 && (uint64_t)space < total) {
			studio_format_size((uint64_t)space, a, sizeof(a));
			studio_format_size(total, b, sizeof(b));
			studio_report_add(r, false, "disk_low", "%s free in %s for up to %s of textures (uncompressed estimate)",
			                  a, dir, b);
		}
	}
	for (int i = 0; i < dir_count; ++i) {
		free(dirs[i]);
	}
	free(dirs);
}

void studio_preflight_run(studio_plan_t *plan, studio_report_t *report) {
	if (plan->naming_error != NULL) {
		studio_report_add(report, true, "naming", "%s", plan->naming_error);
	}
	if (plan->layer_count == 0) {
		studio_report_add(report, true, "no_layers", "no layer to export (all hidden, or none selected)");
	}
	else if (plan->file_count == 0) {
		studio_report_add(report, true, "no_files", "nothing to export (%s mode found no object, tile or atlas)",
		                  plan->mode != NULL ? plan->mode : "this");
	}
	else {
		int selected = 0;
		for (int i = 0; i < plan->file_count; ++i) {
			selected += plan->files[i].skip ? 0 : 1;
		}
		if (selected == 0) {
			studio_report_add(report, true, "no_selection", "no file selected");
		}
	}
	if (plan->hidden_layers > 0) {
		studio_report_note(report, "hidden_layers", "%d hidden layer%s not exported", plan->hidden_layers,
		                  plan->hidden_layers > 1 ? "s" : "");
	}
	report->total_size = 0;
	for (int i = 0; i < plan->file_count; ++i) {
		report->total_size += plan->files[i].skip ? 0 : plan->files[i].raw_size;
	}
	check_textures(plan, report);
	check_udim(plan, report);
	check_paths(plan, report);
}

// ---- Rendu -----------------------------------------------------------------------------------------------------------

static const char *channels_name(int channels) {
	return channels == 1 ? "Y" : channels == 3 ? "RGB" : "RGBA";
}

char *studio_report_text(const studio_plan_t *plan, const studio_report_t *report) {
	sb_t b = {0};
	char total[32];
	studio_format_size(report->total_size, total, sizeof(total));
	sb_printf(&b, "Preset %s: %d file%s, %dx%d, up to %s uncompressed%s\n", plan->preset->name, plan->file_count,
	          plan->file_count > 1 ? "s" : "", plan->width, plan->height, total,
	          report->blocking > 0 ? " - NOT EXPORTED" : "");
	if (plan->destination != NULL) {
		sb_printf(&b, "Destination: %s\n", plan->destination);
	}
	if (plan->color_override != NULL) {
		sb_printf(&b, "Colour textures in %s (chosen at export, overrides the preset)\n", plan->color_override);
	}
	for (int pass = 0; pass < 2; ++pass) { // bloquants d'abord
		for (int i = 0; i < report->count; ++i) {
			const studio_issue_t *issue = &report->issues[i];
			if (issue->blocking == (pass == 0)) {
				sb_printf(&b, "%s %s\n", issue->blocking ? "[blocking]" : issue->note ? "[note]    " : "[warning] ",
				          issue->message);
			}
		}
	}
	if (plan->file_count > 0) {
		sb_printf(&b, "Files:\n");
	}
	for (int i = 0; i < plan->file_count; ++i) {
		const studio_plan_file_t      *f = &plan->files[i];
		const studio_preset_texture_t *t = f->texture;
		char                           size[32];
		studio_format_size(f->raw_size, size, sizeof(size));
		sb_printf(&b, "  %s  (%dx%d %s %d-bit %s%s%s, %s, <= %s%s)\n", f->path, f->width, f->height,
		          studio_format_name(t->format), t->bits,
		          channels_name(t->channels), t->compression != NULL ? " " : "",
		          t->compression != NULL ? t->compression : "", studio_preset_color(t), size,
		          f->skip ? ", not selected" : f->exists ? ", replaces existing" : "");
	}
	if (plan->manifest_path != NULL) {
		sb_printf(&b, "Manifest: %s\n", plan->manifest_path);
	}
	return sb_take(&b);
}

char *studio_report_json(const studio_plan_t *plan, const studio_report_t *report) {
	sb_t b = {0};
	sb_printf(&b, "{\"ok\": %s, \"preset\": ", report->blocking == 0 ? "true" : "false");
	sb_quote(&b, plan->preset->name);
	sb_printf(&b, ", \"color_space_override\": ");
	if (plan->color_override != NULL) {
		sb_quote(&b, plan->color_override);
	}
	else {
		sb_printf(&b, "null");
	}
	sb_printf(&b, ", \"destination\": ");
	sb_quote(&b, plan->destination != NULL ? plan->destination : "");
	sb_printf(&b, ", \"width\": %d, \"height\": %d, \"source_bits\": %d, \"mode\": ", plan->width, plan->height,
	          plan->source_bits);
	sb_quote(&b, plan->mode != NULL ? plan->mode : "");
	sb_printf(&b, ", \"total_size\": %llu, \"manifest\": ", (unsigned long long)report->total_size);
	if (plan->manifest_path != NULL) {
		sb_quote(&b, plan->manifest_path);
	}
	else {
		sb_printf(&b, "null");
	}
	sb_printf(&b, ", \"files\": [");
	for (int i = 0; i < plan->file_count; ++i) {
		const studio_plan_file_t      *f = &plan->files[i];
		const studio_preset_texture_t *t = f->texture;
		sb_printf(&b, "%s{\"path\": ", i > 0 ? ", " : "");
		sb_quote(&b, f->path);
		sb_printf(&b, ", \"texture\": ");
		sb_quote(&b, t->name);
		sb_printf(&b, ", \"variant\": ");
		sb_quote(&b, f->variant);
		sb_printf(&b, ", \"udim\": ");
		sb_quote(&b, f->udim);
		sb_printf(&b, ", \"sequence\": ");
		if (f->sequence != NULL) {
			sb_quote(&b, f->sequence);
		}
		else {
			sb_printf(&b, "null");
		}
		sb_printf(&b, ", \"width\": %d, \"height\": %d, \"format\": \"%s\", \"bits\": %d, \"channels\": %d, "
		          "\"compression\": ", f->width, f->height, studio_format_name(t->format), t->bits, t->channels);
		if (t->compression != NULL) {
			sb_quote(&b, t->compression);
		}
		else {
			sb_printf(&b, "null");
		}
		sb_printf(&b, ", \"color_space\": ");
		sb_quote(&b, studio_preset_color(t)); // nom OCIO : texte libre
		sb_printf(&b, ", \"size_estimate\": %llu, \"exists\": %s, \"skip\": %s}", (unsigned long long)f->raw_size,
		          f->exists ? "true" : "false", f->skip ? "true" : "false");
	}
	sb_printf(&b, "], \"issues\": [");
	for (int i = 0; i < report->count; ++i) {
		sb_printf(&b, "%s{\"level\": \"%s\", \"code\": ", i > 0 ? ", " : "",
		          report->issues[i].blocking ? "blocking" : report->issues[i].note ? "note" : "warning");
		sb_quote(&b, report->issues[i].code);
		sb_printf(&b, ", \"message\": ");
		sb_quote(&b, report->issues[i].message);
		sb_printf(&b, "}");
	}
	sb_printf(&b, "]}");
	return sb_take(&b);
}
