#pragma once
// Dans l'app, stdbool.h vient de l'unity build (io_exr.c ne l'inclut pas).
#include <stdbool.h>
// Stub de base/sources/iron_gpu.h pour compiler io_exr.c hors de l'app (le vrai tire les backends GPU).
typedef enum {
	GPU_TEXTURE_FORMAT_RGBA32,
	GPU_TEXTURE_FORMAT_RGBA64,
	GPU_TEXTURE_FORMAT_RGBA128,
} gpu_texture_format_t;
