#pragma once

// Lot A4 : interprétation des textures importées. Chaque texture importée a un espace source OCIO ; ses pixels sont
// convertis vers l'espace de travail au décodage, avant l'envoi au GPU, et rangés comme ArmorPaint les attend : en
// 8 bits, encodés gamma 2.2 (la convention des couleurs d'ArmorPaint, décodées au rendu) ; en flottant, linéaires. Un
// espace de donnée (normales, rugosité…) n'est pas converti. L'alpha ne passe pas par OCIO.
//
// Espace source, dans l'ordre :
//   1. le choix enregistré pour la texture : celui de l'utilisateur, ou la résolution d'une session précédente (un
//      projet rouvert est décodé comme il l'a été, même si la configuration a changé ses règles) ;
//   2. .k (format d'ArmorPaint) : pas de conversion ;
//   3. un nom de donnée (les listes d'ArmorPaint : normal, roughness, metallic, ao, height, opacity ; plus mask) :
//      l'espace de donnée de la configuration ; un nom de couleur de base l'emporte ;
//   4. une file_rule de la configuration autre que Default ;
//   5. le défaut du format : 8 bits → sRGB (srgb_tx), .hdr → Linear Rec.709 (sRGB), autre flottant → scene_linear.
// La règle Default n'est pas suivie : dans la configuration studio, elle désigne un espace d'affichage, qu'OCIO
// convertit par la vue inverse.
//
// Le choix se fait par texture (pas par nœud : ce sont les pixels qui sont convertis au chargement) : menu contextuel
// de l'onglet Textures (Color Space...) et menu Color (Texture Color Spaces...). Il voyage dans le .arm, dans l'entrée
// studio_color_spaces.json du paquet d'assets, qu'ArmorPaint sans studio garde telle quelle. STUDIO_OCIO_IMPORT=0
// désactive le lot : pas de conversion, l'entrée du projet n'est ni lue ni réécrite.

#include <stdbool.h>

void studio_ocio_import_init(void);      // external_init()
bool studio_ocio_import_enabled(void);   // faux avec STUDIO_OCIO_IMPORT=0
void studio_ocio_import_menu_draw(void); // entrée du menu Color (studio_ocio_view.c)

// Espace source retenu pour la texture (chemin de l'asset), "" si elle n'est pas convertie (format d'ArmorPaint, OCIO
// indisponible, lot désactivé).
const char *studio_texture_color_space(const char *file);
// Pourquoi cet espace : "user" (choisi), "name" (nom de donnée), "rule" (file_rule), "default" (défaut du format) ; ""
// sans conversion.
const char *studio_texture_color_space_source(const char *file);
// Vrai si la texture garde des couleurs linéaires (flottante, espace couleur) : le mode Auto du nœud Image Texture les
// encode pour la couleur de base (STUDIO-PATCH(image-texture-color-space)).
bool studio_texture_is_linear(char *file);
// Choisit l'espace source (nom, alias ou rôle ; NULL ou "" : automatique) et recharge la texture depuis son fichier.
// Faux si l'espace est inconnu, si le fichier manque (texture seulement dans le paquet du projet, calque d'un PSD : ses
// pixels sont déjà convertis), ou si le lot est désactivé. Sans texture de ce chemin dans le projet, le choix vaut pour
// son import. Les calques d'un PSD prennent l'espace du fichier.
bool studio_texture_set_color_space(const char *file, const char *color_space);
// Lot A5 : l'espace de travail ou la configuration ont changé. Recharge depuis leurs fichiers les textures converties
// (chacune garde son espace source) ; celles qui ne sont que dans le paquet du projet gardent leurs pixels (signalé).
// Les calques ne sont pas convertis : ce qui y est peint reste tel quel. Rend le nombre de textures rechargées.
int studio_ocio_import_reconvert(void);
// Chantier E (subzero) : recharge une texture du projet (chemin de l'asset) depuis son fichier réécrit, décodée comme à
// l'import, avec son espace source enregistré. Les matériaux et calques qui l'utilisent sont à refaire par l'appelant.
// Faux si aucun asset n'a ce chemin ou si le décodage échoue (l'ancienne texture reste).
bool studio_texture_reload(const char *file);
