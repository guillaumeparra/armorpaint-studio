#include "studio_exr.h"
#include "iron_gpu.h"
#include "iron_map.h"
#include "studio.h"
#include "studio_exr_codec.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// Cœur ArmorPaint / iron
buffer_t       *data_get_blob(char *file);
void            data_delete_blob(char *handle);
gpu_texture_t  *gpu_create_texture_from_bytes(buffer_t *data, int width, int height, int format);
int             iron_hardware_threads(void);
void            console_info(char *s);
void            console_error(char *s);
char           *config_get_sha(void);
extern char      *manifest_version;
extern any_map_t *import_texture_importers;
// paint/plugins/io_exr/io_exr.c : seul décodeur DWAA/DWAB disponible (tinyexr ne les lit pas).
void *io_exr_parse(uint8_t *buf, size_t buf_size);

static exr_compression studio_exr_compression   = EXR_COMPRESSION_ZIP;
static bool            studio_exr_legacy_writer = false;

#define STUDIO_EXR_MAX_METADATA 32
static char *studio_exr_metadata_names[STUDIO_EXR_MAX_METADATA];
static char *studio_exr_metadata_values[STUDIO_EXR_MAX_METADATA];
static int   studio_exr_metadata_count = 0;

// console_* passe le message à iron_log comme chaîne de format : les '%' d'un chemin doivent être doublés.
static void studio_exr_report(bool error, const char *format, ...) {
	char    message[1024];
	char    escaped[2048];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	size_t j = 0;
	for (size_t i = 0; message[i] != 0 && j + 2 < sizeof(escaped); i++) {
		if (message[i] == '%') {
			escaped[j++] = '%';
		}
		escaped[j++] = message[i];
	}
	escaped[j] = 0;
	if (error) {
		console_error(escaped);
	}
	else {
		console_info(escaped);
	}
}

// ---- Écriture --------------------------------------------------------------------------------------------------------

static FILE *studio_exr_fopen(const char *path) {
#ifdef _WIN32
	wchar_t wpath[1024];
	if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) == 0) {
		return NULL;
	}
	return _wfopen(wpath, L"wb");
#else
	return fopen(path, "wb");
#endif
}

static bool studio_exr_replace(const char *from, const char *to) {
#ifdef _WIN32
	wchar_t wfrom[1024];
	wchar_t wto[1024];
	if (MultiByteToWideChar(CP_UTF8, 0, from, -1, wfrom, 1024) == 0 || MultiByteToWideChar(CP_UTF8, 0, to, -1, wto, 1024) == 0) {
		return false;
	}
	return MoveFileExW(wfrom, wto, MOVEFILE_REPLACE_EXISTING) != 0;
#else
	return rename(from, to) == 0;
#endif
}

static void studio_exr_remove(const char *path) {
#ifdef _WIN32
	wchar_t wpath[1024];
	if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) != 0) {
		DeleteFileW(wpath);
	}
#else
	remove(path);
#endif
}

// Écrit dans <path>.tmp puis renomme : un lecteur (Nuke, un watcher de farm) ne voit jamais de fichier à moitié écrit,
// et un export raté ne détruit pas la version précédente.
static bool studio_exr_save(const char *path, const void *data, size_t size) {
	size_t len = strlen(path);
	char  *tmp = malloc(len + 5);
	if (tmp == NULL) {
		return false;
	}
	memcpy(tmp, path, len);
	memcpy(tmp + len, ".tmp", 5);
	bool  ok = false;
	FILE *f  = studio_exr_fopen(tmp);
	if (f != NULL) {
		ok = fwrite(data, 1, size, f) == size;
		ok = fclose(f) == 0 && ok;
		ok = ok && studio_exr_replace(tmp, path);
		if (!ok) {
			studio_exr_remove(tmp);
		}
	}
	free(tmp);
	return ok;
}

static const char *studio_exr_software(void) {
	static char software[256] = "";
	if (software[0] == 0) {
		snprintf(software, sizeof(software), "ArmorPaint %s (%s), studio %s, tinyexr", manifest_version, config_get_sha(), STUDIO_VERSION);
	}
	return software;
}

const char *studio_exr_software_string(void) {
	return studio_exr_software();
}

bool studio_exr_encode_texture(const void *pixels, size_t pixels_size, int width, int height, int bits, int type, int off,
                               int compression, void **out, size_t *size, char *err, size_t err_size) {
	const char *names[STUDIO_EXR_MAX_METADATA + 1]  = {"software"};
	const char *values[STUDIO_EXR_MAX_METADATA + 1] = {studio_exr_software()};
	for (int i = 0; i < studio_exr_metadata_count; i++) {
		names[i + 1]  = studio_exr_metadata_names[i];
		values[i + 1] = studio_exr_metadata_values[i];
	}
	studio_exr_encode_t in = {
	    .pixels      = pixels,
	    .pixels_size = pixels_size,
	    .width       = width,
	    .height      = height,
	    .bits        = bits,
	    .type        = type,
	    .off         = off,
	    .compression = compression >= 0 ? (exr_compression)compression : studio_exr_compression,
	    .attr_count  = studio_exr_metadata_count + 1,
	    .attr_names  = names,
	    .attr_values = values,
	};
	exr_result r = studio_exr_encode(&in, out, size);
	if (!EXR_OK(r) && err != NULL && err_size > 0) {
		snprintf(err, err_size, "%s", exr_result_string(r));
	}
	return EXR_OK(r);
}

bool studio_exr_write_texture(char *path, buffer_t *pixels, int width, int height, int bits, int type, int off) {
	if (studio_exr_legacy_writer || (bits != 16 && bits != 32)) {
		return false;
	}
	void  *data = NULL;
	size_t size = 0;
	char   err[128];
	if (!studio_exr_encode_texture(pixels->buffer, pixels->length, width, height, bits, type, off, -1, &data, &size, err,
	                               sizeof(err))) {
		studio_exr_report(true, "EXR export failed (%s): %s", err, path);
	}
	else if (!studio_exr_save(path, data, size)) {
		studio_exr_report(true, "EXR export: cannot write %s", path);
	}
	free(data);
	return true;
}

// ---- Métadonnées -----------------------------------------------------------------------------------------------------

void studio_exr_set_metadata(const char *name, const char *value) {
	int i = 0;
	while (i < studio_exr_metadata_count && strcmp(studio_exr_metadata_names[i], name) != 0) {
		i++;
	}
	if (value == NULL) {
		if (i < studio_exr_metadata_count) {
			free(studio_exr_metadata_names[i]);
			free(studio_exr_metadata_values[i]);
			studio_exr_metadata_count--;
			studio_exr_metadata_names[i]  = studio_exr_metadata_names[studio_exr_metadata_count];
			studio_exr_metadata_values[i] = studio_exr_metadata_values[studio_exr_metadata_count];
		}
		return;
	}
	if (i == studio_exr_metadata_count) {
		if (studio_exr_metadata_count == STUDIO_EXR_MAX_METADATA) {
			studio_exr_report(true, "EXR metadata: more than %d attributes, \"%s\" ignored", STUDIO_EXR_MAX_METADATA, name);
			return;
		}
		studio_exr_metadata_names[i]  = strdup(name);
		studio_exr_metadata_values[i] = NULL;
		studio_exr_metadata_count++;
	}
	free(studio_exr_metadata_values[i]);
	studio_exr_metadata_values[i] = strdup(value);
}

void studio_exr_clear_metadata(void) {
	for (int i = 0; i < studio_exr_metadata_count; i++) {
		free(studio_exr_metadata_names[i]);
		free(studio_exr_metadata_values[i]);
	}
	studio_exr_metadata_count = 0;
}

// ---- Lecture ---------------------------------------------------------------------------------------------------------

static void *studio_exr_import_dwa(char *path, buffer_t *blob) {
	char why[160];
	if (!studio_exr_dwa_legacy_safe(blob->buffer, blob->length, why, sizeof(why))) {
		studio_exr_report(true, "EXR import: DWAA/DWAB not supported for this file (%s), convert it to ZIP or PIZ: %s", why, path);
		return NULL;
	}
	// Copie avec STUDIO_EXR_DWA_PAD octets à zéro : le décodeur Huffman upstream peut lire un octet de trop.
	uint8_t *copy = malloc((size_t)blob->length + STUDIO_EXR_DWA_PAD);
	if (copy == NULL) {
		return NULL;
	}
	memcpy(copy, blob->buffer, blob->length);
	memset(copy + blob->length, 0, STUDIO_EXR_DWA_PAD);
	void *texture = io_exr_parse(copy, blob->length);
	free(copy);
	if (texture != NULL) {
		studio_exr_report(false, "EXR import: DWA read by the ArmorPaint decoder (dataWindow only): %s", path);
	}
	return texture;
}

static void *studio_exr_import(char *path) {
	buffer_t *blob = data_get_blob(path);
	if (blob == NULL || blob->buffer == NULL) {
		studio_exr_report(true, "EXR import: cannot read %s", path);
		return NULL;
	}

	void              *texture = NULL;
	studio_exr_image_t image;
	char               err[256];
	exr_result         r = studio_exr_decode(blob->buffer, blob->length, &image, err, sizeof(err));
	if (EXR_OK(r)) {
		size_t   bytes = (size_t)image.width * (size_t)image.height * (image.is_half ? 8 : 16);
		buffer_t b     = {.buffer = image.pixels, .length = (uint32_t)bytes, .capacity = (uint32_t)bytes};
		// Copie synchrone vers le GPU sur tous les backends : les pixels peuvent être libérés juste après.
		texture = gpu_create_texture_from_bytes(&b, image.width, image.height,
		                                        image.is_half ? GPU_TEXTURE_FORMAT_RGBA64 : GPU_TEXTURE_FORMAT_RGBA128);
		if (image.note[0] != 0) {
			studio_exr_report(false, "EXR import (%s): %s", image.note, path);
		}
	}
	else if (image.compression == EXR_COMPRESSION_DWAA || image.compression == EXR_COMPRESSION_DWAB) {
		texture = studio_exr_import_dwa(path, blob);
	}
	else {
		studio_exr_report(true, "EXR import failed (%s): %s", err, path);
	}
	studio_exr_image_free(&image);
	data_delete_blob(path);
	return texture;
}

// ---- Configuration ---------------------------------------------------------------------------------------------------

void studio_exr_init(void) {
	const char *compression = getenv("STUDIO_EXR_COMPRESSION");
	if (compression != NULL && compression[0] != 0) {
		int c = studio_exr_compression_from_name(compression);
		if (c < 0 || c == EXR_COMPRESSION_DWAA || c == EXR_COMPRESSION_DWAB || c == EXR_COMPRESSION_ZSTD) {
			studio_exr_report(true, "STUDIO_EXR_COMPRESSION=%s not supported (none, rle, zips, zip, piz, pxr24, b44, b44a, htj2k256, htj2k32), zip used", compression);
		}
		else {
			studio_exr_compression = (exr_compression)c;
		}
	}

	const char *writer       = getenv("STUDIO_EXR_WRITER");
	studio_exr_legacy_writer = writer != NULL && strcmp(writer, "legacy") == 0;

	int         threads = iron_hardware_threads();
	const char *env     = getenv("STUDIO_EXR_THREADS");
	if (env != NULL && atoi(env) > 0) {
		threads = atoi(env);
	}
	exr_set_num_threads(threads);

	const char *reader = getenv("STUDIO_EXR_READER");
	if (reader == NULL || strcmp(reader, "legacy") != 0) {
		any_map_set(import_texture_importers, "exr", studio_exr_import);
	}

	iron_log("studio: exr writer %s (%s), reader %s, %d threads", studio_exr_legacy_writer ? "legacy" : "tinyexr",
	         studio_exr_compression_name(studio_exr_compression), reader != NULL && strcmp(reader, "legacy") == 0 ? "legacy" : "tinyexr",
	         threads);
}
