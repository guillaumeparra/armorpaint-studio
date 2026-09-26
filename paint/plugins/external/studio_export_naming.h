#pragma once

// Lot B3 : les textures exportées sont nommées par le template Toolkit des textures (op texture_pattern du sidecar,
// réglage STUDIO_SG_TEXTURE_TEMPLATES), rempli avec les champs du fichier de travail ouvert : nom, version… Par exemple
// assets/Prop/robot/TXT/work/armorpaint/textures/v003/robot_base.1001.png pour robot.v003.arm. Le dossier choisi dans
// la boîte d'export est alors ignoré.
//
// Le modèle est demandé au sidecar quand le projet ouvert change (sauvegarde, ouverture), puis gardé : l'export ne fait
// que remplacer les marqueurs, sans appel à ShotGrid. Hors pipeline (projet jamais sauvé, fichier hors des templates
// du projet, pas de template de textures), ou en cas d'erreur (signalée dans la console), les noms restent ceux
// d'ArmorPaint.
//
// STUDIO_SG_NAMING : "1" active le nommage, "0" le coupe. Par défaut il est actif dès que ShotGrid sert dans la session
// (sidecar lancé, ou ArmorPaint lancé par ShotGrid Desktop).

void studio_export_naming_init(void);

// Point d'injection dans export_texture_run_layers() (paint/sources/io/export_texture.c, STUDIO-PATCH(export-naming)).
// variant : l'objet ou l'atlas exporté à part ("" sinon), placé devant le canal ("head_base") ; channel : le nom de la
// texture du preset ("base", "nor"…) ; udim : la tuile (".1001", "" hors export par tuile) ; ext : l'extension d'ArmorPaint
// (".png", ou ".1001.png" par tuile). Retourne le chemin (chaîne d'ArmorPaint, dossiers créés), NULL pour garder le
// nom d'ArmorPaint.
char *studio_export_texture_path(char *variant, char *channel, char *udim, char *ext);

// Chemin d'une texture de preset studio (lot B4) d'après le template ShotGrid, sans créer de dossier ni écrire dans la
// console : studio_export.c vérifie tout avant d'écrire (lot B5). udim sans point ("1001", "" hors export par tuile),
// ext sans point ("exr"). 1 : *path (malloc) ; 0 : pas de nommage ShotGrid pour ce projet, le nom vient du preset ;
// -1 : *error (malloc), le template ne convient pas (tuile exigée ou absente), l'export est bloqué.
int studio_export_naming_path(const char *variant, const char *channel, const char *udim, const char *ext, char **path,
                              char **error);
// Template Toolkit qui nomme les textures du projet ouvert, NULL hors pipeline.
const char *studio_export_naming_template(void);
