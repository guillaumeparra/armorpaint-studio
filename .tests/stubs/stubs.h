#pragma once
#include "iron_array.h"
#include <stddef.h>

typedef struct stub_texture {
	void *pixels;
	int   width;
	int   height;
	int   format;
} stub_texture_t;

extern stub_texture_t stub_last_texture;
extern int            stub_quiet;

void *io_exr_parse(uint8_t *buf, size_t buf_size);
