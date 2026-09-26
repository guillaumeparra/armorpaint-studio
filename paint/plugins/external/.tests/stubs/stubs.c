// Symboles d'ArmorPaint utilisés par io_exr.c et iron_compress.c, remplacés pour le banc de test.
#include "stubs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

stub_texture_t stub_last_texture;
int            stub_quiet = 0;

buffer_t *buffer_create(uint32_t length) {
	buffer_t *b = calloc(1, sizeof(buffer_t));
	if (length > 0) {
		b->buffer = calloc(length, 1);
	}
	b->length = b->capacity = length;
	return b;
}

// Garde une copie des pixels pour que le test compare ce qu'io_exr_parse a décodé.
void *gpu_create_texture_from_bytes(void *data, int width, int height, int format) {
	buffer_t *b = data;
	free(stub_last_texture.pixels);
	stub_last_texture.pixels = malloc(b->length);
	memcpy(stub_last_texture.pixels, b->buffer, b->length);
	stub_last_texture.width  = width;
	stub_last_texture.height = height;
	stub_last_texture.format = format;
	return &stub_last_texture;
}

void console_info(char *s) {
	if (!stub_quiet) {
		fprintf(stderr, "  [io_exr] %s\n", s);
	}
}

// Copie de base/sources/iron_array.c (tronque, pas d'arrondi ; dénormaux à 0).
uint16_t float_to_half_fast(float value) {
	union {
		float    f;
		uint32_t u;
	} v           = {value};
	uint32_t sign = (v.u >> 16) & 0x8000;
	v.u &= 0x7FFFFFFF;
	if (v.u >= 0x47800000)
		return sign | 0x7C00;
	if (v.u < 0x38800000)
		return sign;
	return sign | ((v.u - 0x38000000) >> 13);
}
