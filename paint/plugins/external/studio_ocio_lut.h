#pragma once

// Lot A2 : LUT 3D de la transformation d'affichage du viewport. Le compositeur d'ArmorPaint (compositor_pass.shader,
// patch ocio-viewport) ne peut pas exécuter le shader GPU d'OCIO : la transformation display/view est échantillonnée
// sur le CPU, avec le processeur exact d'OCIO, en une LUT 3D que le shader interpole.
//
// Entrée de la LUT : l'espace de travail (linéaire, HDR), mis en forme par la courbe ACEScct appliquée à chaque canal
// (le « shaper ») : [-0.0069, 222.86] → [0, 1], linéaire près du noir, logarithmique au-dessus. Nœuds en b, g, r.
// Stockage : texture 2D RGBA half, les n tranches b rangées en grille de cols × rows tuiles de n × n texels (une seule
// rangée de n² texels dépasserait la largeur maximale d'une texture pour n = 129). Le shader lit deux tuiles (b0, b1)
// en bilinéaire matériel et les mélange : interpolation trilinéaire.
//
// Sans dépendance à ArmorPaint ni à OCIO : testable hors de l'app (.tests/ocio_view_test.c).

#include <stdbool.h>
#include <stdint.h>

#define STUDIO_OCIO_LUT_SIZE 129 // nœuds par axe ; 65 : écarts ACES 2.0 jusqu'à ~15/255, 129 : ~9/255 (README)

typedef struct {
	int       n;             // nœuds par axe
	int       cols, rows;    // grille des tuiles
	int       width, height; // texture : cols * n × rows * n
	uint16_t *texels;        // RGBA half, width * height * 4
} studio_ocio_lut_t;

// Transforme count triplets RGB float32 contigus, sur place. Appelée depuis plusieurs threads sur des tranches
// disjointes. Rend false en cas d'échec.
typedef bool (*studio_ocio_lut_fn)(float *rgb, int count, void *user);

float studio_ocio_lut_shaper(float x);     // linéaire → [0, 1] (ACEScct, non borné)
float studio_ocio_lut_shaper_inv(float y); // [0, 1] → linéaire
void  studio_ocio_lut_layout(int n, int *cols, int *rows);

// Échantillonne fn sur les n³ nœuds (threads : 0 pour le nombre de cœurs). Rend false si fn échoue ou sans mémoire.
// Une sortie non finie devient 0, une valeur hors de la plage des half est bornée.
bool studio_ocio_lut_bake(studio_ocio_lut_t *lut, int n, studio_ocio_lut_fn fn, void *user, int threads);
void studio_ocio_lut_free(studio_ocio_lut_t *lut);

// Ce que calcule le shader (compositor_pass.shader, ocio_lut_display), en float : pour les tests.
void studio_ocio_lut_sample(const studio_ocio_lut_t *lut, const float in[3], float out[3]);

uint16_t studio_ocio_half_from_float(float f); // arrondi au plus proche pair
float    studio_ocio_half_to_float(uint16_t h);
