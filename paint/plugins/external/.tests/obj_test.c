// Banc du patch obj-global-indices : lit un OBJ avec obj_parse() (base/sources/iron_obj.c) comme le fait
// import_obj_parse() — un appel par part tant que has_next — et imprime chaque part décodée (positions dépackées,
// normales et uvs en int16). Deux sorties identiques = même maillage importé.
//
// Usage : obj_test <fichier.obj> [o|u] [udim]     o : split par objet (défaut), u : par matériau (usemtl)
//
// Patch udim-import : en udim, chaque part (un objet) imprime aussi ses triangles écartés et coupés quand il y en a,
// et avec OBJ_TEST_TILES=1 les indices de chaque tuile. Les compteurs sont faibles : l'upstream ne les a pas, ils y
// restent à 0 et la sortie d'un fichier propre ne change pas.

#include "iron_array.h"
#include "iron_obj.h"
#include <stdio.h>
#include <stdlib.h>

__attribute__((weak)) int   obj_udim_invalid = 0;
__attribute__((weak)) float obj_udim_invalid_uv[2];
__attribute__((weak)) int   obj_udim_cut = 0;

void console_info(char *s) {
	printf("[info] %s\n", s);
}

int main(int argc, char **argv) {
	char  split = argc > 2 ? argv[2][0] : 'o';
	bool  udim  = argc > 3;
	FILE *f     = fopen(argv[1], "rb");
	if (f == NULL) {
		return 2;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buffer_t b = {0};
	b.buffer   = malloc(n > 0 ? n : 1); // Taille exacte : ASan voit toute lecture au-delà de la fin du fichier
	b.length   = n;
	fread(b.buffer, 1, n, f);
	fclose(f);

	raw_mesh_t *part = obj_parse(&b, split, 0, udim);
	while (true) {
		printf("PART %s idx=%d scale=%g tex=%d\n", part->name, part->inda->length, part->scale_pos, part->texa != NULL);
		for (int i = 0; i < (int)part->inda->length; ++i) {
			int16_t *p = part->posa->buffer + i * 4;
			int16_t *q = part->nora->buffer + i * 2;
			printf("  v %.4f %.4f %.4f n %d %d %d", p[0] / 32767.0 * part->scale_pos, p[1] / 32767.0 * part->scale_pos,
			       p[2] / 32767.0 * part->scale_pos, q[0], q[1], p[3]);
			if (part->texa != NULL) {
				printf(" uv %d %d", part->texa->buffer[i * 2], part->texa->buffer[i * 2 + 1]);
			}
			printf("\n");
		}
		if (part->udims != NULL) {
			for (int t = 0; t < part->udims->length; ++t) {
				printf("  udim %d: %d\n", t, ((u32_array_t *)part->udims->buffer[t])->length);
			}
			if (getenv("OBJ_TEST_TILES") != NULL) {
				for (int t = 0; t < part->udims->length; ++t) {
					u32_array_t *a = part->udims->buffer[t];
					if (a->length == 0) {
						continue;
					}
					printf("  tile %d:", 1001 + t % part->udims_u + 10 * (t / part->udims_u));
					for (int i = 0; i < (int)a->length; ++i) {
						printf(" %u", a->buffer[i]);
					}
					printf("\n");
				}
			}
		}
		if (obj_udim_invalid > 0 || obj_udim_cut > 0) {
			printf("  udim invalid=%d uv=%g %g cut=%d\n", obj_udim_invalid, obj_udim_invalid > 0 ? obj_udim_invalid_uv[0] : 0.0,
			       obj_udim_invalid > 0 ? obj_udim_invalid_uv[1] : 0.0, obj_udim_cut);
		}
		if (!part->has_next) {
			break;
		}
		part = obj_parse(&b, split, part->pos, udim); // STUDIO-PATCH(udim-import) : un objet par part en udim aussi
	}
	return 0;
}
