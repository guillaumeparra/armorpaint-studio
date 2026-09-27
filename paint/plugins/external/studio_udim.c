#include "studio_udim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROJECT_RES_MAX 16384 // Largeur et hauteur maximales d'un projet (résolution Custom)
#define UDIM_TILES_MAX  64    // Tuiles tenues par les calques (UDIM_MAX_TILES, util_udim.h)

// "4096", ou "4096x2048" si ce n'est pas carré.
static void side(char *out, size_t size, int x, int y) {
	if (x == y) {
		snprintf(out, size, "%d", x);
	}
	else {
		snprintf(out, size, "%dx%d", x, y);
	}
}

char *studio_udim_hint(int tiles, int res_x, int res_y, int bits) {
	if (tiles < 2 || tiles > UDIM_TILES_MAX || res_x < 1 || res_y < 1 || res_x > PROJECT_RES_MAX || res_y > PROJECT_RES_MAX ||
	    (bits != 8 && bits != 16 && bits != 32)) {
		return NULL;
	}
	// Un calque : texpaint, texpaint_nor et texpaint_pack (slot_layer_alloc_textures), 4 canaux chacune, par tuile
	double bytes = (double)tiles * res_x * res_y * 4.0 * (bits / 8) * 3.0;
	char   size[32], memory[32], out[256];
	side(size, sizeof(size), res_x, res_y);
	if (bytes >= 1e9) {
		snprintf(memory, sizeof(memory), "%.1f GB", bytes / 1e9);
	}
	else {
		snprintf(memory, sizeof(memory), "%.0f MB", bytes / 1e6);
	}
	snprintf(out, sizeof(out), "UDIM: %d tiles, each with its own %s px textures: %s per %d bit layer.", tiles, size, memory, bits);
	return strdup(out);
}
