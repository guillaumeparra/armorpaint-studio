#pragma once

// Lot B2 : encodeurs PNG (8 et 16 bits) et TIFF (8 et 16 bits entiers, 32 bits flottants), en mémoire. Sans
// dépendance à ArmorPaint (libc et sdefl, le deflate d'iron) : testables hors de l'app (.tests/image_test.c), relus par
// libpng et libtiff.
//
// L'upstream n'écrit que du PNG et du JPG 8 bits (stb_image_write) : 16 et 32 bits imposaient l'EXR.

#include <stdbool.h>
#include <stddef.h>

typedef enum {
	STUDIO_TIFF_NONE    = 1,
	STUDIO_TIFF_LZW     = 5,
	STUDIO_TIFF_DEFLATE = 8, // Adobe Deflate (flux zlib)
} studio_tiff_compression_t;

// Exécute fn(ctx, job) pour chaque job de [0, jobs), éventuellement en parallèle : les jobs sont indépendants. Dans
// l'app, exr_parallel_for de tinyexr. NULL : en série.
typedef void (*studio_image_job_fn)(void *ctx, int job);
typedef void (*studio_image_parallel_fn)(int threads, int jobs, studio_image_job_fn fn, void *ctx);

typedef struct {
	// Échantillons entrelacés, lignes de haut en bas, sans marge : uint8 (bits 8), uint16 dans l'ordre de l'hôte
	// (bits 16) ou float (bits 32).
	const void *pixels;
	int         width;
	int         height;
	int         channels; // 1 (gris), 3 (RGB), 4 (RGBA, alpha non prémultiplié)
	int         bits;     // 8, 16 ; 32 (float) en TIFF seulement

	int                       level;            // deflate (PNG, TIFF deflate) : niveau sdefl, 0 (rapide) à 8
	studio_tiff_compression_t tiff_compression; // TIFF seulement
	const char               *software;         // texte "Software", NULL pour aucun

	int                      threads;
	studio_image_parallel_fn parallel;
} studio_image_t;

// Taille maximale des données PNG avant compression (lignes filtrées) : sdefl compresse en un bloc indexé par int.
// Au-delà (RGBA 16 bits en 16K), écrire en EXR ou en TIFF.
#define STUDIO_PNG_MAX_RAW ((size_t)0x7f000000)

// Encodent l'image en mémoire : *out (malloc, à libérer par l'appelant) et *size. false et err rempli en cas d'échec
// (paramètres invalides, image trop grande pour le format, mémoire).
bool studio_png_encode(const studio_image_t *img, void **out, size_t *size, char *err, size_t err_size);
bool studio_tiff_encode(const studio_image_t *img, void **out, size_t *size, char *err, size_t err_size);

// Octets des échantillons avant compression (lignes non filtrées), 0 si les dimensions débordent.
size_t studio_image_raw_size(int width, int height, int channels, int bits);
