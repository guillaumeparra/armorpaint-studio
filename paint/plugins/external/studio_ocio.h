#pragma once

// Lot A1 : gestion couleur OCIO. OpenColorIO vit dans libstudio_ocio (shim C++, ABI : studio_ocio_lib.h), liée en
// statique sur macOS et chargée par dlopen ailleurs ; ce module choisit la configuration, garde les processeurs en cache
// et sert le reste d'ArmorPaint (export A3, viewport A2, import A4). Sans la bibliothèque ou sans configuration valide, OCIO est
// indisponible et studio_ocio_status() dit pourquoi : ArmorPaint reste utilisable, rien n'échoue en silence.
//
// Configuration, dans l'ordre : chemin explicite (studio_ocio_reload, scripts), $OCIO, la préférence (lot A5), sinon la
// configuration studio intégrée à OCIO (ocio://studio-config-latest, ACES). $OCIO passe avant la préférence : c'est
// le pipeline qui la choisit (lanceur du show). Un $OCIO ou une préférence invalides sont une erreur, pas un repli :
// le repli cacherait au studio que sa configuration n'est pas celle qu'il croit.
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
void        studio_ocio_set_config_preference(const char *path); // "" : aucune ; prise au prochain rechargement
const char *studio_ocio_config_preference(void);
// "explicit", "$OCIO", "preferences" ou "built-in default" : d'où vient la dernière configuration, chargée ou refusée.
const char *studio_ocio_config_origin(void);
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
// retrouvé dans la configuration par studio_ocio_lib_identify_builtin(STUDIO_OCIO_WORKING_BUILTIN) : l'automatique.
//
// Le projet le porte (lot A5). Mode du projet, dans l'ordre de recherche :
//   STUDIO_OCIO_WS_NAMED     : le nom enregistré, sans repli ;
//   STUDIO_OCIO_WS_AUTOMATIC : l'automatique ; si la configuration ne le trouve pas (pas de rôles d'échange),
//                              $STUDIO_OCIO_WORKING_SPACE, puis le nom enregistré. Un projet sans choix enregistré
//                              (fait avant le lot A5, ou par ArmorPaint sans studio) est dans ce mode : ses couleurs
//                              sont celles d'ArmorPaint ;
//   STUDIO_OCIO_WS_DEFAULT   : nouveau projet : $STUDIO_OCIO_WORKING_SPACE, puis la préférence, puis l'automatique.
// Un nom (projet, variable, préférence) inconnu de la configuration est une erreur, sans repli. Noms, alias ou rôles ;
// cherché à nouveau à chaque rechargement. La génération ne change que si l'espace trouvé change.
#define STUDIO_OCIO_WORKING_BUILTIN "lin_rec709_srgb"
#define STUDIO_OCIO_WS_DEFAULT      0
#define STUDIO_OCIO_WS_AUTOMATIC    1
#define STUDIO_OCIO_WS_NAMED        2
const char *studio_ocio_working_space(void);                            // nom canonique ; NULL si la configuration n'en a pas
const char *studio_ocio_working_space_error(void);                      // pourquoi il est NULL ; "" sinon
const char *studio_ocio_working_space_origin(void);                     // "project", "project, automatic", "$STUDIO_OCIO_WORKING_SPACE",
                                                                        // "preferences", "automatic" ; "" sans espace de travail
bool        studio_ocio_working_space_auto(void);                       // trouvé par l'automatique
void        studio_ocio_set_working_space_preference(const char *name); // "" : automatique
const char *studio_ocio_working_space_preference(void);
void        studio_ocio_set_project_working_space(int mode, const char *name);
int         studio_ocio_project_working_space_mode(void);
const char *studio_ocio_project_working_space_name(void);

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
