#pragma once

// Jeux de textures : les atlas d'ArmorPaint (g_project->atlas_objects, atlas_names, enregistrés dans le .arm). Chaque
// objet du projet appartient à un jeu ; en export Visible, chaque jeu utilisé devient un jeu de maps à part, nommé
// d'après lui (<atlas> des noms d'export), avec les calques partagés, les calques des objets du jeu et ceux limités au
// jeu. Tous les objets dans un même jeu : pas de découpage. L'onglet Atlases de la boîte d'export d'ArmorPaint n'étant
// plus accessible (patch export-window), la fenêtre d'export les édite par ces fonctions.
//
// Les objets gardent leurs UV : un atlas ne réagence rien, il choisit quels calques vont dans quel jeu. Des objets
// dont les UV se chevauchent reçoivent les mêmes pixels des calques partagés.

#include <stdbool.h>
#include <stddef.h>

void studio_atlas_init(void);

// {"names": [...], "objects": [{"name": "crate", "atlas": 0}, ...], "layers": [{"layer": "Layer 2", "atlas": 1}, ...]}
// (objets du projet dans l'ordre d'ArmorPaint ; calques limités à un jeu). Jamais définis, ou définis avant un import
// qui a changé les objets : un seul jeu "Atlas 1". malloc.
char *studio_atlas_json(void);

// {"names": [...], "objects": [indice du jeu de chaque objet, dans l'ordre de "objects"]}. Les noms : uniques, 1 à 64
// caractères imprimables. Un calque limité à un jeu y reste, même renommé ou si l'ordre des jeux change ; un jeu qui
// perdrait tous ses objets alors qu'un calque y est limité est refusé. false et err si refusé, rien n'est changé.
bool studio_atlas_set(const char *json, char *err, size_t err_size);

// Limite le calque courant au jeu atlas (indice dans les noms, utilisé par un objet, au moins deux jeux utilisés), ou
// le rend partagé (-1), comme le menu de masque du calque. false si impossible.
bool studio_atlas_limit_layer(int atlas);

// Export découpé par jeu (au moins deux jeux utilisés), comme export_texture_run.
bool studio_atlas_active(void);

// Après un import de maillage (import_mesh_finish_import, STUDIO-PATCH(udim-import)). Découpé par matériau et par tuile
// UDIM (OBJ, FBX), hors ajout à un maillage : un jeu de textures par matériau. En UDIM : pixels de chaque tuile et
// projet conseillé (studio_udim_hint), dans la barre d'état si elle est libre, dans la console seule sinon.
void studio_udim_import_done(bool append);
