#pragma once

// Lot B1 : lecture et écriture EXR par tinyexr v3 (vendor/tinyexr.c), à la place du writer maison
// (paint/sources/io/export_exr.c) et de l'importeur du plugin io_exr.

#include "iron_array.h"
#include <stdbool.h>

// Lit la configuration (variables d'environnement STUDIO_EXR_*) et remplace l'importeur "exr".
// Appelé par external_init(), donc après plugins_init() qui a enregistré l'importeur upstream.
void studio_exr_init(void);

// Point d'injection dans export_texture_write_texture() (paint/sources/io/export_texture.c,
// STUDIO-PATCH(studio-exr)). pixels : RGBA entrelacé, half (bits = 16) ou float (bits = 32).
// type 1 : RGB (alpha ignoré). type 2 : le canal `off` seul, écrit en EXR mono-canal "Y". type 3 : RGBA.
// Retourne false si le writer upstream doit prendre la main (STUDIO_EXR_WRITER=legacy, bits non gérés).
// Retourne true sinon, y compris en cas d'échec : l'erreur est alors signalée dans la console.
bool studio_exr_write_texture(char *path, buffer_t *pixels, int width, int height, int bits, int type, int off);

// Attributs "string" ajoutés à chaque EXR écrit (asset, task, version… fournis par les lots B3/C, colorspace par
// le lot A3). value == NULL retire l'attribut. L'attribut "software" est toujours écrit.
void studio_exr_set_metadata(const char *name, const char *value);
void studio_exr_clear_metadata(void);

// Lots B4/B5 (studio_export.c) : encode en mémoire avec les réglages de l'app. compression : exr_compression, ou -1
// pour celle de STUDIO_EXR_COMPRESSION (zip par défaut). Attributs "software" et studio_exr_set_metadata. Mêmes
// conventions de pixels et de type que studio_exr_write_texture. *out (malloc) en cas de succès, err sinon.
bool studio_exr_encode_texture(const void *pixels, size_t pixels_size, int width, int height, int bits, int type, int off,
                               int compression, void **out, size_t *size, char *err, size_t err_size);
// "ArmorPaint <version> (<sha>), studio <version>, tinyexr" : aussi l'étiquette Software des PNG et TIFF studio.
const char *studio_exr_software_string(void);
