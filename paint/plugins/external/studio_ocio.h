#pragma once

// Lot A1 : gestion couleur OCIO. OpenColorIO vit dans libstudio_ocio (shim C++, ABI : studio_ocio_lib.h), liée en
// statique sur macOS et chargée par dlopen ailleurs ; ce module choisit la configuration, garde les processeurs en cache
// et sert le reste d'ArmorPaint (export A3, viewport A2, import A4). Sans la bibliothèque ou sans configuration valide, OCIO est
// indisponible et studio_ocio_status() dit pourquoi : ArmorPaint reste utilisable, rien n'échoue en silence.
//
// Configuration, dans l'ordre : chemin explicite (préférences, lot A5), sinon $OCIO, sinon la configuration studio
// intégrée à OCIO (ocio://studio-config-latest, ACES). Un $OCIO invalide est une erreur, pas un repli : le repli
// cacherait au studio que sa configuration n'est pas celle qu'il croit.
//
// Threads : studio_ocio_init() et studio_ocio_shutdown() depuis le thread principal, hors de tout export. Tout le
// reste peut être appelé de n'importe quel thread ; studio_ocio_transform() est sûr en parallèle (bandes disjointes).

#include <stdbool.h>

// Bibliothèque : liée en statique sur macOS (.project/project.c) ; ailleurs, chargée par dlopen depuis $STUDIO_OCIO_LIB
// ou data/libstudio_ocio.so. Sur macOS, $STUDIO_OCIO_LIB ne sert qu'aux bancs de test (build sans liaison statique).
#define STUDIO_OCIO_DEFAULT_CONFIG "ocio://studio-config-latest"

void studio_ocio_init(void); // external_init() : charge la bibliothèque et la configuration par défaut, journalise
// Recharge la configuration : config_path NULL ou "" pour l'ordre par défaut. Rend studio_ocio_ready().
bool        studio_ocio_reload(const char *config_path);
void        studio_ocio_shutdown(void);
// Incrémenté à chaque rechargement : les noms rendus avant (displays, views…) ne sont plus valides, les caches des
// appelants (LUT du viewport) sont à refaire.
int         studio_ocio_generation(void);
bool        studio_ocio_ready(void);
const char *studio_ocio_status(void);        // état lisible : versions et configuration, ou la raison de l'échec
const char *studio_ocio_config_source(void); // fichier ou URI chargé ; "" si aucun
const char *studio_ocio_config_name(void);
const char *studio_ocio_version(void); // version d'OpenColorIO ; "" sans bibliothèque

int         studio_ocio_colorspace_count(void);
const char *studio_ocio_colorspace_name(int i);
int         studio_ocio_display_count(void);
const char *studio_ocio_display_name(int i);
int         studio_ocio_look_count(void);
const char *studio_ocio_look_name(int i);
int         studio_ocio_role_count(void);
const char *studio_ocio_role_name(int i);
const char *studio_ocio_display_default(void);
const char *studio_ocio_view_default(const char *display);
int         studio_ocio_view_count(const char *display);
const char *studio_ocio_view_name(const char *display, int i);
const char *studio_ocio_view_colorspace(const char *display, const char *view); // espace d'affichage ; NULL si inconnue
// 1 si l'espace d'affichage de la view est encodé hdr-video (PQ, HLG…) ou edr-video (Display P3 HDR d'Apple, au-delà
// de 1), 0 sinon, -1 si inconnue. Le viewport
// d'ArmorPaint sort en 8 bits SDR : une view HDR n'y a pas de sens.
int studio_ocio_view_is_hdr(const char *display, const char *view);

// Espace de travail : celui des couleurs d'ArmorPaint une fois décodées. Le sélecteur de couleur, les calques et le
// viewport upstream travaillent en primaires Rec.709 encodées gamma 2.2 : décodées, c'est « Linear Rec.709 (sRGB) »,
// retrouvé dans la configuration par studio_ocio_lib_identify_builtin(STUDIO_OCIO_WORKING_BUILTIN). Réglable par
// $STUDIO_OCIO_WORKING_SPACE (nom, alias ou rôle), relu à chaque rechargement ; le projet le portera au lot A5.
#define STUDIO_OCIO_WORKING_BUILTIN "lin_rec709_srgb"
const char *studio_ocio_working_space(void);       // nom canonique ; NULL si la configuration n'en a pas
const char *studio_ocio_working_space_error(void); // pourquoi il est NULL ; "" sinon

const char *studio_ocio_resolve(const char *name);          // nom canonique d'un espace, alias ou rôle ; NULL sinon
int         studio_ocio_is_data(const char *colorspace);     // 1 donnée, 0 couleur, -1 inconnu
const char *studio_ocio_encoding(const char *colorspace);    // "scene-linear", "sdr-video"… ; NULL si inconnu
const char *studio_ocio_family(const char *colorspace);      // NULL si inconnu
const char *studio_ocio_colorspace_from_path(const char *p); // file_rules de la configuration
const char *studio_ocio_file_rule(const char *p);            // nom de la file_rule qui s'applique ("Default" : aucune autre)
// Espace de la configuration équivalent à un espace intégré d'OCIO ("srgb_tx", "lin_rec709_srgb"…) ; NULL sinon.
const char *studio_ocio_identify_builtin(const char *builtin);

// Transforme sur place des pixels float32 entrelacés (3 ou 4 canaux, alpha intact), lignes contiguës. Les processeurs
// sont gardés en cache par couple (src, dst). Rend false si OCIO est indisponible ou un nom inconnu (message dans le
// journal une fois par couple). Deux espaces qui se résolvent au même nom : rien à faire, rend true.
bool studio_ocio_transform(float *pixels, int width, int height, int channels, const char *src, const char *dst);
// Empreinte de la transformation src → dst (manifeste d'export) ; NULL si indisponible.
const char *studio_ocio_transform_id(const char *src, const char *dst);

// Lot A2 : transformation d'affichage, de l'espace src vers display/view, avec un look (NULL ou "" : aucun). Même
// contrat que studio_ocio_transform ; le cache est commun.
bool        studio_ocio_display_transform(float *pixels, int width, int height, int channels, const char *src,
                                          const char *display, const char *view, const char *look);
const char *studio_ocio_display_transform_id(const char *src, const char *display, const char *view, const char *look);
