#pragma once

// Chantier E (subzero) : matériaux procéduraux .sbs et .sbsar, rendus par sbsrender (Adobe Substance 3D Designer,
// chemin dans Preferences > subzero, préférence subzero.sbsrender ; vide : Designer cherché dans /Applications ou
// /opt/Adobe). Les paramètres sont réglés dans une fenêtre Qt (assets/subzero_window.py, sur studio_window.h), qui rend
// les textures dans <dossier du .arm>/<projet>_subzero/<Nom>/ avec la recette subzero.json, puis crée ou met à jour ici
// un matériau ordinaire : un nœud Image Texture par fichier, relié à la sortie PBR. Le matériau vit sans subzero ni
// sbsrender ; la recette permet de le rééditer (menu subzero > Edit Selected Material).
//
// Ouverture : menu subzero > Open, import d'un .sbs ou .sbsar (Import Asset, glisser-déposer), ou script.
//
// Protocole de la fenêtre (en plus de celui de studio_window.h) :
//   requêtes    state       -> {"sbsrender", "project": {"file"}, "resolution": [x, y], "bits": 8|16|32,
//                               "open": {"file"} | {"dir"} | null (à ouvrir au démarrage),
//                               "ocio": {"enabled", "status", "config", "name", "origin", "version", "working_space",
//                                        "display", "view", "look", "viewport"} (lot E11 : la configuration qui lit
//                                        les textures, la transformation d'affichage du viewport),
//                               "env": {"file" (studio.env où écrire les réglages, "" sans), "origins": {variable:
//                                       fichier ou "environment"} des variables OCIO, STUDIO_OCIO_* et SUBZERO_OCIO_*}}
//               create      {"dir", "name", "links": [{"file", "output": 0 couleur | 1 alpha, "socket",
//                           "color_space" (facultatif : nom, alias ou rôle de la configuration, sinon espace intégré
//                           comme "srgb_tx"), "color_space_was" (facultatif : celui du rendu précédent)}]}
//                           -> {"material", "reloaded", "created", "removed", "kept"} ; textures importées, matériau
//                           créé et choisi (annulable)
//               update      {"dir", "name", "links"} -> la même réponse ; textures rechargées, nœuds ajoutés ou
//                           retirés, liens de l'artiste gardés, calques de remplissage refaits à la frame suivante ;
//                           sans matériau pour ce dossier (supprimé), il est recréé
//               Après l'une ou l'autre, les textures du dossier absentes de "links" (autre format, sortie retirée)
//               sont retirées du projet ("removed" : leur nombre), sauf celles qu'un autre nœud utilise encore
//               ("kept" : leurs fichiers, que la fenêtre ne doit pas supprimer). Espace d'une texture : une neuve
//               prend "color_space" ; une déjà là garde le sien, sauf s'il vaut encore "color_space_was" (l'artiste
//               ne l'a pas changé) : elle passe alors dans "color_space".
//               show_prefs  {"tab": "color"} facultatif -> {} ; ouvre Preferences > subzero (ou Color)
//   évènements  open {"file"} | {"dir"}, raise, prefs {"sbsrender"}, project {"state": {...}},
//               ocio {"state": {...}} (configuration, affichage du viewport ou import OCIO changés)
// Les chemins de "links" doivent être dans "dir". Thread principal.

#include <stdbool.h>

void subzero_init(void); // external_init()

// Ouvre la fenêtre sur un .sbs, un .sbsar, ou le dossier d'un matériau envoyé (sa recette) ; NULL : fenêtre vide.
// Fenêtre déjà ouverte : elle passe au premier plan et ouvre ce qui est donné.
void subzero_open(const char *path);
bool subzero_is_open(void);

// Dossier subzero (avec sa recette) du matériau, "" pour un matériau ordinaire. Chaîne temporaire (string_tmp).
const char *subzero_material_dir(void *slot_material);
