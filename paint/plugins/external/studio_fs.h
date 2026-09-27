#pragma once

// Accès fichiers des lots B4/B5 (chemins UTF-8, y compris sous Windows) : écriture atomique, espace libre, droits.
// Sans dépendance à ArmorPaint : testable hors de l'app.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

FILE *studio_fopen(const char *path, const char *mode);
// Remplace to par from (rename atomique sur le même volume).
bool studio_fs_rename(const char *from, const char *to);
bool studio_fs_remove(const char *path);
bool studio_fs_exists(const char *path);
bool studio_fs_is_dir(const char *path);
// Taille du fichier, -1 s'il n'existe pas.
int64_t studio_fs_size(const char *path);
// Octets disponibles sur le volume qui contient path (ou son plus proche ancêtre existant), -1 si inconnu.
int64_t studio_fs_free_space(const char *path);
// Plus proche dossier existant qui contient path (path lui-même s'il est un dossier) : malloc, NULL si aucun.
char *studio_fs_existing_ancestor(const char *path);
// Vrai si l'on peut créer un fichier dans le dossier dir, ou dans son plus proche ancêtre existant (les sous-dossiers
// manquants seront créés).
bool studio_fs_can_write_in(const char *dir);
// Dossier de path (malloc), "" pour un nom sans dossier.
char *studio_fs_dirname(const char *path);
// Écrit tout le fichier (sans atomicité : voir studio_export pour <final>.tmp puis rename).
bool studio_fs_write(const char *path, const void *data, size_t size);
// Contenu entier d'un fichier (malloc, terminé par un NUL non compté dans *size), NULL s'il est illisible.
char *studio_fs_read(const char *path, size_t *size);
// Ajoute data à la fin du fichier (créé s'il manque), d'une seule écriture en ajout : deux processus qui écrivent le
// même journal ligne par ligne n'en mêlent pas les lignes (lot C5).
bool studio_fs_append(const char *path, const void *data, size_t size);
// Nombre d'entrées du dossier dir dont le nom finit par suffix (noms cachés exclus), -1 si le dossier est illisible.
int studio_fs_count(const char *dir, const char *suffix);
