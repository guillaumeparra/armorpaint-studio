#pragma once

// Lot A1 : ABI C de libstudio_ocio, le shim C++ au-dessus d'OpenColorIO v2 (sources dans .ocio/, construit par
// tools/build_ocio.sh). OCIO n'a pas d'API C, et amake ne compile pas de C++ depuis external/ : la bibliothèque est
// construite à part, puis liée en statique (macOS, .project/project.c) ou chargée par dlopen depuis data/ (Linux).
// Elle embarque OCIO et ses dépendances et n'exporte que les symboles ci-dessous.
//
// Contrat commun à toutes les fonctions :
// - aucune exception ne traverse l'ABI : une erreur rend NULL, 0 ou -1, et studio_ocio_lib_error() la décrit
//   (message propre au thread appelant, valide jusqu'au prochain appel sur ce thread) ;
// - un argument NULL ou hors bornes est une erreur, jamais un plantage ;
// - les chaînes rendues appartiennent à la configuration et restent valides jusqu'à studio_ocio_lib_config_free().
//
// Threads : une configuration et un processeur sont en lecture seule une fois créés. Plusieurs threads peuvent
// appeler studio_ocio_lib_apply() en même temps sur le même processeur (sur des pixels disjoints).

// Incrémenté à chaque changement incompatible : studio_ocio.c refuse une bibliothèque d'une autre version.
#define STUDIO_OCIO_LIB_ABI 2 // 2 : studio_ocio_lib_identify_builtin

#ifdef __cplusplus
extern "C" {
#endif

typedef struct studio_ocio_lib_config studio_ocio_lib_config_t;
typedef struct studio_ocio_lib_proc   studio_ocio_lib_proc_t;

// Listes de la configuration, pour studio_ocio_lib_count() et studio_ocio_lib_name().
enum {
	STUDIO_OCIO_LIB_COLORSPACES = 0, // espaces actifs (les inactifs sont exclus, comme dans les applications OCIO)
	STUDIO_OCIO_LIB_DISPLAYS    = 1, // displays actifs
	STUDIO_OCIO_LIB_LOOKS       = 2,
	STUDIO_OCIO_LIB_ROLES       = 3, // nom du rôle ; son espace : studio_ocio_lib_resolve(nom du rôle)
};

int         studio_ocio_lib_abi(void);
const char *studio_ocio_lib_ocio_version(void); // version d'OpenColorIO embarquée, ex. "2.5.2"
const char *studio_ocio_lib_error(void);        // dernière erreur du thread appelant ("" si aucune)

// Charge une configuration : chemin d'un fichier .ocio ou .ocioz, ou URI d'une configuration intégrée à OCIO
// ("ocio://studio-config-latest", "ocio://cg-config-latest", "ocio://default"). La configuration est validée
// (Config::validate) : une configuration incohérente est refusée ici plutôt qu'à la première transformation.
studio_ocio_lib_config_t *studio_ocio_lib_config_load(const char *path_or_uri);
void                      studio_ocio_lib_config_free(studio_ocio_lib_config_t *cfg);
const char               *studio_ocio_lib_config_name(studio_ocio_lib_config_t *cfg);
const char               *studio_ocio_lib_config_description(studio_ocio_lib_config_t *cfg);

int         studio_ocio_lib_count(studio_ocio_lib_config_t *cfg, int list);
const char *studio_ocio_lib_name(studio_ocio_lib_config_t *cfg, int list, int i);

// Nom canonique de l'espace désigné par un nom, un alias ou un rôle ; NULL s'il n'existe pas.
const char *studio_ocio_lib_resolve(studio_ocio_lib_config_t *cfg, const char *name);
// Espace de cfg équivalent à l'espace builtin de la configuration intégrée d'OCIO (ocio://default), par exemple
// "lin_rec709_srgb" : d'abord par nom ou alias, puis par comparaison des transformations
// (Config::IdentifyBuiltinColorSpace, qui demande les rôles d'interchange aces_interchange ou
// cie_xyz_d65_interchange). NULL si aucun ne correspond.
const char *studio_ocio_lib_identify_builtin(studio_ocio_lib_config_t *cfg, const char *builtin);
// 1 si l'espace est de la donnée (isData : normales, rugosité…, jamais transformées), 0 sinon, -1 s'il n'existe pas.
int studio_ocio_lib_is_data(studio_ocio_lib_config_t *cfg, const char *colorspace);
// Encodage déclaré de l'espace ("scene-linear", "sdr-video", "log", "data"…), "" s'il n'en déclare pas, NULL s'il
// n'existe pas.
const char *studio_ocio_lib_encoding(studio_ocio_lib_config_t *cfg, const char *colorspace);
// Famille déclarée de l'espace ("ACES", "Utility"…, séparateur "/"), "" si aucune, NULL s'il n'existe pas.
const char *studio_ocio_lib_family(studio_ocio_lib_config_t *cfg, const char *colorspace);
// Espace d'un fichier selon les file_rules de la configuration (règle par défaut si aucune ne s'applique).
const char *studio_ocio_lib_colorspace_from_path(studio_ocio_lib_config_t *cfg, const char *path);

const char *studio_ocio_lib_default_display(studio_ocio_lib_config_t *cfg);
const char *studio_ocio_lib_default_view(studio_ocio_lib_config_t *cfg, const char *display);
int         studio_ocio_lib_view_count(studio_ocio_lib_config_t *cfg, const char *display);
const char *studio_ocio_lib_view_name(studio_ocio_lib_config_t *cfg, const char *display, int i);

// Processeur CPU d'un espace vers un autre (noms, alias ou rôles).
studio_ocio_lib_proc_t *studio_ocio_lib_proc_colorspace(studio_ocio_lib_config_t *cfg, const char *src,
                                                        const char *dst);
// Processeur CPU d'affichage : espace source → display/view, avec un look optionnel (NULL ou "" : aucun) ;
// inverse != 0 pour le sens retour.
studio_ocio_lib_proc_t *studio_ocio_lib_proc_display(studio_ocio_lib_config_t *cfg, const char *src,
                                                     const char *display, const char *view, const char *looks,
                                                     int inverse);
void studio_ocio_lib_proc_free(studio_ocio_lib_proc_t *proc);
int  studio_ocio_lib_proc_is_noop(studio_ocio_lib_proc_t *proc); // 1 si le processeur ne change rien, 0 sinon
// Empreinte du processeur (identique pour deux processeurs équivalents), pour les caches et les manifestes.
const char *studio_ocio_lib_proc_cache_id(studio_ocio_lib_proc_t *proc);

// Transforme sur place des pixels float32 entrelacés, 3 (RGB) ou 4 canaux (RGBA ; l'alpha n'est pas touché).
// stride : octets d'une ligne à la suivante, 0 pour des lignes contiguës. Rend 1, ou 0 en cas d'erreur.
int studio_ocio_lib_apply(studio_ocio_lib_proc_t *proc, float *pixels, long width, long height, int channels,
                          long stride);

#ifdef __cplusplus
}
#endif
