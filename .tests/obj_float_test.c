// Banc du patch obj-float-exponent : appelle read_float() (statique, d'où l'#include du .c) sur des jetons de
// ligne v / vt / vn.
//
// Usage : obj_float_test exp      jetons à exposant comparés à strtof (borné à ±FLT_MAX), à 1 ulp près ; code de
//                                 retour = nombre d'écarts
//         obj_float_test plain    20 000 jetons décimaux sans exposant, pseudo-aléatoires, imprimés en %a : la sortie
//                                 doit être identique à celle de l'upstream (read_float inchangé sans exposant)

#include <float.h>
#include "iron_obj.c"
#include <stdio.h>

void console_info(char *s) {
	printf("[info] %s\n", s);
}

static raw_mesh_t mesh;

// Lit le jeton t (suivi d'un espace), vérifie que read_float() s'arrête à la fin du jeton
static float parse(const char *t, bool *boundary) {
	static char line[256];
	snprintf(line, sizeof(line), "%s 7", t);
	part         = &mesh;
	part->pos    = 0;
	bytes        = (uint8_t *)line;
	bytes_length = strlen(line);
	float f      = read_float();
	*boundary    = part->pos == strlen(t);
	return f;
}

static float expected(const char *t) {
	double d = strtod(t, NULL);
	return d > FLT_MAX ? FLT_MAX : d < -FLT_MAX ? -FLT_MAX : (float)d;
}

static int check_exp(void) {
	const char *tokens[] = {"1.5e-3", "-0.5e-2", "1E+2", "1e-45", "1e999", "-1e999", "1e-999", "0e999", "-0e5", "2.5E3",
	                        "-1.234567e-05", "6.02e23", "3.4e38", "3.5e38", "1e0", "1.5e+00", "-5.000000e-01",
	                        "7.450581e-09", "1.0e-20", "9.99e-39", "1e2147483648", "1e-2147483649", "1e99999999999",
	                        "-2.5e-7", "12345e-4", "1e", "1e+", "1e-", "1.5e-3x", "0.1E1"};
	int fails = 0;
	for (int i = 0; i < (int)(sizeof(tokens) / sizeof(tokens[0])); ++i) {
		bool  boundary;
		float got  = parse(tokens[i], &boundary);
		float want = expected(tokens[i]);
		bool  ok   = boundary && (got == want || nextafterf(want, INFINITY) == got || nextafterf(want, -INFINITY) == got);
		printf("  %s %-16s -> %.9g (attendu %.9g)%s\n", ok ? "✅" : "❌", tokens[i], got, want, boundary ? "" : " fin de jeton ratée");
		fails += ok ? 0 : 1;
	}
	return fails;
}

static int print_plain(void) {
	uint32_t seed = 7;
	char     t[32];
	for (int i = 0; i < 20000; ++i) {
		seed          = seed * 1664525 + 1013904223;
		int  digits   = 1 + (seed >> 8) % 12; // Au-delà de 9 caractères read_float tronque : identique aussi
		int  dot      = (seed >> 16) % (digits + 1);
		int  n        = 0;
		bool negative = (seed >> 28) & 1;
		if (negative) {
			t[n++] = '-';
		}
		for (int k = 0; k < digits; ++k) {
			seed = seed * 1664525 + 1013904223;
			if (k == dot && k > 0) {
				t[n++] = '.';
			}
			t[n++] = '0' + (seed >> 24) % 10;
		}
		t[n] = '\0';
		bool boundary;
		printf("%s %a %d\n", t, parse(t, &boundary), boundary);
	}
	return 0;
}

int main(int argc, char **argv) {
	return argc > 1 && strcmp(argv[1], "plain") == 0 ? print_plain() : check_exp();
}
