#pragma once

// Lot A2 : transformation d'affichage OCIO du viewport. En modes Lit et Path Trace, le compositeur d'ArmorPaint
// (compositor_pass.shader, STUDIO-PATCH(ocio-viewport)) remplace son tonemap filmique par la transformation
// display/view/look d'OCIO, depuis l'espace de travail (studio_ocio_working_space), échantillonnée en une LUT 3D
// (studio_ocio_lut.h). La LUT .cube d'upstream s'applique ensuite, sur les valeurs d'affichage. Les autres modes
// (couleur de base, normales…) sont inchangés : ils montrent des données, pas une image.
//
// Menu Color de la barre de menus (STUDIO-PATCH(menubar-hook)) ; au démarrage : STUDIO_OCIO_VIEWPORT=0 revient au
// tonemap d'ArmorPaint, STUDIO_OCIO_DISPLAY, STUDIO_OCIO_VIEW et STUDIO_OCIO_LOOK choisissent la transformation
// (défauts de la configuration sinon). La LUT est refaite quand le choix change ou que la configuration est rechargée.
// Thread principal uniquement.

#include <stdbool.h>

void studio_ocio_view_init(void); // external_init(), après studio_ocio_init()

bool studio_ocio_view_enabled(void);         // choix de l'utilisateur
bool studio_ocio_view_active(void);          // la LUT est prête : le viewport passe par OCIO
bool studio_ocio_view_set_enabled(bool on);  // rend studio_ocio_view_active()
// NULL ou "" : display par défaut, view par défaut du display, aucun look. Refait la LUT tout de suite (~0,1 s).
// Rend false, sans rien changer, si la combinaison est inconnue de la configuration.
bool        studio_ocio_view_set(const char *display, const char *view, const char *look);
const char *studio_ocio_view_display(void); // "" sans OCIO
const char *studio_ocio_view_view(void);
const char *studio_ocio_view_look(void); // "" : aucun
const char *studio_ocio_view_status(void);
// Ce que fait le compositeur à une couleur de l'espace de travail (la LUT, interpolée comme sur le GPU) ; false si
// inactif.
bool studio_ocio_view_apply(float rgb[3]);
// Diagnostic, pour les tests : la couleur color (0xAARRGGBB, canaux k/255, stockés en half) passée par le vrai
// compositeur du viewport (mode courant, grain et vignette coupés), sur une cible 1 × 1 RGBA64 ; false en cas d'échec.
bool studio_ocio_view_probe(int color, float out[3]);
