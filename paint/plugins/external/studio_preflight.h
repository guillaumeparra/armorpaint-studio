#pragma once

// Lot B5 : vérifications avant écriture d'un export studio. Le plan (fichiers exacts que l'export va écrire, relevés
// par une passe à blanc d'export_texture_run) et le contexte (résolution, calques, tuiles UDIM) sont fournis par la colle
// (studio_export.c) ; ce module ne regarde que le disque (existence, droits, espace libre). Sans dépendance à
// ArmorPaint : testable hors de l'app.
//
// Deux niveaux : bloquant (l'export n'a pas lieu) et avertissement (l'export a lieu, le rapport et le manifeste le
// mentionnent).

#include "studio_preset.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
	char                          *path;    // chemin final
	const studio_preset_texture_t *texture; // texture du preset
	char                          *variant; // objet ou atlas, "" sinon
	char                          *udim;    // tuile ("1001"), "" sinon
	int                            width;
	int                            height;
	uint64_t                       raw_size; // échantillons avant compression (estimation haute)
	bool                           exists;   // renseigné par studio_preflight_run
} studio_plan_file_t;

typedef struct {
	const studio_preset_t *preset;
	studio_plan_file_t    *files;
	int                    file_count;
	int                    width; // résolution de la texture d'export
	int                    height;
	int                    source_bits;      // profondeur des calques aplatis (8, 16, 32)
	const char            *mode;             // "visible", "selected", "per object", "per UDIM tile"...
	bool                   udim_mesh;        // un objet du projet porte des tuiles UDIM
	bool                   per_tile;         // l'export est fait par tuile
	const char *const     *tiles;            // tuiles trouvées sur les objets ("1001"...)
	int                    tile_count;
	int                    layer_count;      // calques exportés
	int                    hidden_layers;    // calques masqués, donc ignorés
	const char            *naming_error;     // échec du nommage (template ShotGrid...), bloquant
	const char            *manifest_path;    // NULL sans manifeste
	const char            *destination;      // dossier choisi par l'utilisateur
} studio_plan_t;

typedef struct {
	bool  blocking;
	char *code; // identifiant stable ("not_writable"...)
	char *message;
} studio_issue_t;

typedef struct {
	studio_issue_t *issues;
	int             count;
	int             blocking;
	uint64_t        total_size; // somme des raw_size
} studio_report_t;

void studio_preflight_run(studio_plan_t *plan, studio_report_t *report);
// Ajoute un problème (utilisé aussi par la colle pour ce qu'elle seule voit).
void studio_report_add(studio_report_t *report, bool blocking, const char *code, const char *format, ...);
void studio_report_free(studio_report_t *report);

// Rapport lisible (malloc) : résumé, problèmes, liste des fichiers avec leur taille estimée.
char *studio_report_text(const studio_plan_t *plan, const studio_report_t *report);
// Rapport JSON (malloc) : {"ok", "preset", "width", "height", "total_size", "files": [...], "issues": [...]}.
char *studio_report_json(const studio_plan_t *plan, const studio_report_t *report);

// "1.5 GB", "320 MB", "12 KB".
void studio_format_size(uint64_t bytes, char *out, size_t out_size);
