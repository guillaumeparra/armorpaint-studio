// Banc du patch fbx-split : lit un FBX avec io_fbx_parse() (paint/plugins/io_fbx/io_fbx.c) comme import_mesh_run()
// — un appel par mesh tant que has_next — et imprime chaque mesh (positions dépackées, normales, uvs, second jeu
// d'uv et couleurs en int16) ainsi que les messages de l'importeur.
//
// Usage : fbx_test <fichier.fbx> [0|1|2|3]   0 : split par objet (défaut), 1 : par matériau, 2 : par tuile UDIM,
//                                            3 : par matériau et par tuile UDIM
// Compilé avec -DIO_FBX='"<io_fbx.c>"' : l'arbre courant ou l'upstream (voir fbx.sh).

#include IO_FBX

int plugins_split_by = 0;

void console_error(char *s) {
	printf("[error] %s\n", s);
}

void console_info(char *s) {
	printf("[info] %s\n", s);
}

void console_log(char *s) {
	printf("[log] %s\n", s);
}

static void print_i16(const char *label, i16_array_t *a, int i, int n) {
	if (a != NULL) {
		printf(" %s", label);
		for (int k = 0; k < n; ++k) {
			printf(" %d", a->buffer[i * n + k]);
		}
	}
}

int main(int argc, char **argv) {
	plugins_split_by = argc > 2 ? atoi(argv[2]) : 0;
	FILE *f          = fopen(argv[1], "rb");
	if (f == NULL) {
		return 2;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc(n > 0 ? n : 1);
	fread(buf, 1, n, f);
	fclose(f);

	while (true) {
		raw_mesh_t *m = io_fbx_parse(buf, n);
		if (m == NULL) {
			printf("NULL\n");
			break;
		}
		if (m->posa == NULL) {
			printf("EMPTY\n");
			break;
		}
		int count = m->inda->length;
		printf("MESH %s idx=%d scale=%g tex=%d tex1=%d col=%d\n", m->name, count, m->scale_pos, m->texa != NULL, m->texa1 != NULL, m->cola != NULL);
		for (int i = 0; i < count; ++i) {
			int      k = m->inda->buffer[i];
			int16_t *p = m->posa->buffer + k * 4;
			int16_t *q = m->nora->buffer + k * 2;
			printf("  v %.4f %.4f %.4f n %d %d %d", p[0] / 32767.0 * m->scale_pos, p[1] / 32767.0 * m->scale_pos, p[2] / 32767.0 * m->scale_pos, q[0], q[1],
			       p[3]);
			print_i16("uv", m->texa, k, 2);
			print_i16("uv1", m->texa1, k, 2);
			print_i16("col", m->cola, k, 4);
			printf("\n");
		}
		if (!m->has_next) {
			break;
		}
	}
	return 0;
}
