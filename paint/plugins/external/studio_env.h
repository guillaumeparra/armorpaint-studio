#pragma once

// Lot E5 : environnement du studio. Un fichier studio.env (lignes CLÉ=valeur) donne les variables que le Finder et le
// Dock ne transmettent pas (OCIO, STUDIO_*, SUBZERO_*…). Il est lu au tout début d'external_init(), avant tout module
// qui lit l'environnement, et ses variables passent aux sous-processus (fenêtres Qt, sidecar ShotGrid, sbsrender).
//
// Fichiers : $STUDIO_ENV (un ou plusieurs, séparés par ':' — ';' sous Windows ; vide : aucun), sinon studio.env dans
// le dossier de l'utilisateur d'ArmorPaint (~/Library/Application Support/ArmorPaint/ sous macOS), ignoré en
// --background comme les préférences studio. Une variable déjà définie (vraie variable d'environnement, ou fichier
// précédent) est gardée : le premier qui la définit gagne.
//
// Syntaxe : lignes vides et commentaires '#' ignorés, « export » facultatif, CLÉ=valeur. Entre '…' la valeur est prise
// telle quelle ; entre "…" ou sans guillemets, $VAR et ${VAR} sont remplacés (variable indéfinie : vide), et \" \\ \$
// échappés entre "…". Sans guillemets, la valeur s'arrête à « # » précédé d'un espace. Un ~ en tête de valeur, seul
// ou suivi de '/', est le dossier de l'utilisateur.

#include <stdbool.h>

void studio_env_init(void); // external_init(), avant tout autre module

// Lit un fichier ; rend le nombre de variables prises, -1 s'il est illisible (bancs).
int studio_env_load(const char *path);
// Lit un texte au format de studio.env ; source nomme le fichier dans les origines et le log (bancs).
int studio_env_load_text(const char *text, const char *source);
// Origine de la valeur de name : le fichier qui l'a donnée, "environment" pour une vraie variable d'environnement,
// NULL si elle n'est pas définie.
const char *studio_env_origin(const char *name);
// Fichier où la fenêtre subzero écrit ses réglages (lot E11) : le premier de $STUDIO_ENV, sinon studio.env du dossier
// de l'utilisateur, qu'il existe ou non ; "" si $STUDIO_ENV est vide ou avant studio_env_init.
const char *studio_env_file(void);
