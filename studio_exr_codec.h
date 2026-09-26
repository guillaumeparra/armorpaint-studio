#pragma once

// Codec EXR du lot B1, sans dépendance à ArmorPaint (tinyexr + libc) : testable hors de l'app (.tests/).
// Seule exception : studio_exr_dwa_legacy_safe() valide les flux deflate avec zsinflate() (base/sources/libs/sinfl.h),
// le décodeur même qu'utilise io_exr. La colle avec ArmorPaint (console, importeurs, fichiers) est dans studio_exr.c.

#include "vendor/.tinyexr/exr.h"
#include <stdbool.h>
#include <stddef.h>

// Dimension maximale d'une texture importée (limite GPU d'ArmorPaint).
#define STUDIO_EXR_MAX_DIM 16384

typedef struct studio_exr_encode {
	const void *pixels; // RGBA entrelacé, half (bits = 16) ou float (bits = 32), width * height pixels
	size_t      pixels_size;
	int         width;
	int         height;
	int         bits;
	// Conventions d'export_texture_write_texture() : 1 = RGB (alpha ignoré), 2 = le canal `off` seul (EXR
	// mono-canal "Y"), 3 = RGBA.
	int                type;
	int                off;
	exr_compression    compression;
	int                attr_count; // attributs "string" ajoutés à l'en-tête
	const char *const *attr_names;
	const char *const *attr_values;
} studio_exr_encode_t;

// Encode en mémoire. *out est alloué par malloc (à libérer par free) en cas de succès.
exr_result studio_exr_encode(const studio_exr_encode_t *in, void **out, size_t *out_size);

typedef struct studio_exr_image {
	void *pixels; // RGBA entrelacé (malloc), half si is_half, float sinon
	int   width;
	int   height;
	bool  is_half;
	int   compression; // exr_compression de la part lue, renseignée dès que l'en-tête est lu (sinon -1)
	char  note[160];   // ce qui a été lu quand ce n'est pas simplement R, G, B, A (calque, Y, fenêtre…), sinon ""
} studio_exr_image_t;

// Décode la première part non-deep (scanline ou tuilée, niveau 0) en RGBA :
// - canaux R, G, B, A ; à défaut le premier calque qui en a ("diffuse.R"…) ; Y en niveaux de gris, Y/RY/BY
//   reconstruit en couleur ; à défaut le premier canal en niveaux de gris. R, G ou B seul est lu en niveaux de gris,
//   sinon R, G, B absents = 0. Alpha absent = 1 ;
// - l'image couvre la fenêtre d'affichage (displayWindow), 0 hors dataWindow ;
// - half si tous les canaux retenus sont half, float sinon.
// DWAA/DWAB : EXR_ERROR_UNSUPPORTED, avec out->compression renseigné (voir studio_exr_dwa_legacy_safe).
exr_result studio_exr_decode(const void *data, size_t size, studio_exr_image_t *out, char *err, size_t err_size);
void       studio_exr_image_free(studio_exr_image_t *img);

// Octets à zéro que l'appelant ajoute après une copie du fichier avant de la confier à io_exr_parse : le décodeur
// Huffman upstream peut lire un octet au-delà d'un flux mal formé, la validation ci-dessous garantit le reste.
#define STUDIO_EXR_DWA_PAD 16

// Vérifie qu'un EXR DWAA/DWAB peut être confié sans risque au décodeur upstream (io_exr_parse), qui ne borne ni le
// nombre de canaux (débordement de pile au-delà de 4), ni les offsets, ni les tailles internes des chunks DWA, et
// boucle sans fin sur un flux deflate invalide (iron_inflate). Rejoue tout ce que io_exr_parse et dwa_decode lisent :
// en-tête, table d'offsets, en-tête de chaque chunk, table Huffman, flux deflate (avec zsinflate, le même décodeur).
// Hors WITH_COMPRESS (décodeur DWA upstream absent) : toujours false.
bool studio_exr_dwa_legacy_safe(const void *data, size_t size, char *why, size_t why_size);

// Nom court d'une compression ("zip", "dwaa"…) et l'inverse (insensible à la casse, -1 si inconnu).
const char *studio_exr_compression_name(int compression);
int         studio_exr_compression_from_name(const char *name);
