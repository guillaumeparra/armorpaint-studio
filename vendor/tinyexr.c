// tinyexr v3 (C11 pur, BSD-3), compilé en une seule unité de compilation.
// Les sources vivent dans .tinyexr/ : amake ignore les dossiers qui commencent par un point, elles ne sont donc
// compilées qu'ici, avec la configuration ci-dessous (amake ne permet pas de passer des -D depuis external/).
// Mise à jour : tools/update_tinyexr.sh. Version : .tinyexr/VERSION.

// ZSTD est une extension tinyexr hors standard OpenEXR : on ne vendore pas zstd.
#define EXR_NO_ZSTD

// Encodage/décodage parallèle par bloc (x5 sur ZIP en 4K avec 10 cœurs). macOS : GCD. Linux : pthreads.
// Windows : threads C11 (MSVC 2022 17.8+) si l'en-tête existe, sinon séquentiel.
#if defined(__APPLE__)
#define EXR_USE_THREADS
#elif defined(__linux__)
#define EXR_USE_THREADS
#define EXR_THREADS_PTHREAD
#elif defined(_WIN32) && defined(__has_include)
#if __has_include(<threads.h>)
#define EXR_USE_THREADS
#endif
#endif

#include ".tinyexr/unity.inc"
