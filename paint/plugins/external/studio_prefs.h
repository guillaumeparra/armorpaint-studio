#pragma once

// Lot A5 : préférences studio, dans studio_prefs.json à côté du config.json d'ArmorPaint (dossier data/ de l'app, ou
// dossier de sauvegarde du système quand l'app est dans un dossier protégé ou en lecture seule), ou dans le fichier
// $STUDIO_PREFS. Le journal d'audit et la file des publishes (lot C5) vont dans le même dossier.
// config.json est réécrit champ par champ par config_save() : une clé inconnue y serait perdue, d'où un fichier à part.
//
// Un objet JSON plat : "version", puis une clé par réglage ("ocio.config"…). Les clés inconnues (d'une autre version
// du studio) sont gardées telles quelles. Écrit à chaque changement, par un fichier temporaire renommé. Un fichier
// illisible est journalisé, ignoré, et mis de côté (.bak) à la première écriture.
//
// En --background (bancs de test, automatisation), le fichier n'est ni lu ni écrit, sauf un $STUDIO_PREFS explicite :
// un script ne dépend pas des réglages de l'artiste et ne les change pas ; ses changements restent en mémoire.
// Thread principal.

#include <stdbool.h>

void        studio_prefs_init(void);  // external_init(), avant les modules qui lisent leurs préférences
const char *studio_prefs_path(void);  // fichier lu et écrit (ignoré en --background sans $STUDIO_PREFS)
const char *studio_prefs_error(void); // dernière erreur de lecture ou d'écriture ; "" sinon
// Dossier du fichier (malloc), pour les autres fichiers du studio (journal d'audit, publishes en attente, lot C5) ;
// NULL quand les préférences sont ignorées (--background sans $STUDIO_PREFS).
char *studio_prefs_dir(void);

bool        studio_prefs_has(const char *key);
const char *studio_prefs_string(const char *key, const char *fallback); // fallback si absente ou pas une chaîne
bool        studio_prefs_bool(const char *key, bool fallback);          // fallback si absente ou pas un booléen
// Rendent false si l'écriture du fichier échoue (studio_prefs_error) ; la valeur est gardée en mémoire.
bool studio_prefs_set_string(const char *key, const char *value); // NULL : retire la clé
bool studio_prefs_set_bool(const char *key, bool value);
