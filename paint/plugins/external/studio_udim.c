#include "studio_udim.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROJECT_RES_MAX 16384 // Largeur et hauteur maximales d'un projet (résolution Custom)

// "4096", ou "4096x2048" si ce n'est pas carré.
static void side(char *out, size_t size, int x, int y) {
	if (x == y) {
		snprintf(out, size, "%d", x);
	}
	else {
		snprintf(out, size, "%dx%d", x, y);
	}
}

// Ajoute à out le projet qu'il faut pour target px par tuile.
static void advise(char *out, size_t size, int target, int stride, int bits) {
	size_t len = strlen(out);
	int    res = target * stride;
	if (res > PROJECT_RES_MAX) {
		snprintf(out + len, size - len, " For %d px per tile: out of reach, %d px at most (project at %d px).", target, PROJECT_RES_MAX / stride,
		         PROJECT_RES_MAX);
		return;
	}
	// Un calque : texpaint, texpaint_nor et texpaint_pack (slot_layer_alloc_textures), 4 canaux chacune
	double bytes  = (double)res * res * 4.0 * (bits / 8) * 3.0;
	bool   custom = res != 2048 && res != 4096 && res != 8192 && res != 16384;
	char   memory[32];
	if (bytes >= 1e9) {
		snprintf(memory, sizeof(memory), "%.1f GB", bytes / 1e9);
	}
	else {
		snprintf(memory, sizeof(memory), "%.0f MB", bytes / 1e6);
	}
	const char *undo = res < 8192 ? "" : res == 8192 ? ", 4 undo steps" : ", 1 undo step";
	snprintf(out + len, size - len, " For %d px per tile: project at %d px (%s%s per %d bit layer%s).", target, res, custom ? "custom, " : "", memory, bits,
	         undo);
}

char *studio_udim_hint(int tiles, int stride, int res_x, int res_y, int bits) {
	if (tiles < 2 || stride < 2 || stride > 64 || tiles > stride * stride || res_x < 1 || res_y < 1 || res_x > PROJECT_RES_MAX || res_y > PROJECT_RES_MAX ||
	    (bits != 8 && bits != 16 && bits != 32)) {
		return NULL;
	}
	char project[32], tile[32], out[512];
	side(project, sizeof(project), res_x, res_y);
	side(tile, sizeof(tile), res_x / stride, res_y / stride);
	snprintf(out, sizeof(out), "UDIM: %d tiles, %dx%d grid in the %s px layers: %s px per tile.", tiles, stride, stride, project, tile);
	int smallest = (res_x < res_y ? res_x : res_y) / stride;
	if (smallest < 2048) {
		advise(out, sizeof(out), 2048, stride, bits);
	}
	if (smallest < 4096) {
		advise(out, sizeof(out), 4096, stride, bits);
	}
	return strdup(out);
}
