# ArmorPaint — code studio

Ce dossier est compilé dans ArmorPaint par le hook upstream `paint/plugins/external/`
(`paint/plugins/project.c`, define `WITH_EXTERNAL`). `external_init()` est appelé au démarrage par
`plugins_init()` (`paint/plugins/plugins.c`). Le dossier est ignoré par le `.gitignore` upstream : c'est un
dépôt git séparé.

- `external.c` : point d'entrée. Chaque module s'y enregistre.
- `studio.h` : déclarations communes et forward declarations des symboles du cœur. Chaque `.c` d'ici est
  une unité de compilation séparée de l'app.
- `PATCHES.md` : registre des modifications apportées aux fichiers upstream. **À relire à chaque pull.**
- `patches/` : les diffs correspondants.
- `tools/check_patches.sh` : vérifie les patches après un pull.
- `tools/install_hooks.sh` : installe les hooks git qui lancent ce contrôle.
- `vendor/` : tiers vendorés. Les sources vivent dans un sous-dossier commençant par un point
  (`vendor/.tinyexr/`), qu'amake ne compile pas : un seul `.c` par bibliothèque les inclut, avec notre
  configuration. `vendor/patches/` contient nos correctifs sur ces tiers, réappliqués par le script de
  mise à jour (voir `vendor/patches/README.md`).
- `assets/` : fichiers de données (copiés dans `data/`), licences des tiers.
- `.tests/` : bancs de test hors app, sous ASan/UBSan. Le dossier commence par un point : amake l'ignore.

Modules, un par lot (voir `ARMORPAINT_LOTS.md` à la racine d'armorpaint) :

| Module | Lot | État |
|---|---|---|
| `studio_exr` | B1 | ✅ lecture et écriture EXR par tinyexr |
| `studio_export` | B2-B5 | à faire |
| `studio_ocio` | A | à faire |
| `studio_shotgrid` | C | à faire |
| `studio_ui` | B6/C3 | à faire |

Build : `../base/make --compile` depuis `paint/`. Le log de démarrage affiche `studio: external <version>
loaded`, puis une ligne par module.

## Variables d'environnement

Réglages de secours, lus au démarrage. Ils existent pour diagnostiquer et pour revenir au comportement
upstream sans rebuild ; les réglages destinés aux artistes passeront par l'UI et les presets (lots B4/B6).

| Variable | Effet |
|---|---|
| `STUDIO_EXR_COMPRESSION` | `none`, `rle`, `zips`, `zip` (défaut), `piz`, `pxr24`, `b44`, `b44a`, `htj2k256`, `htj2k32` |
| `STUDIO_EXR_WRITER=legacy` | rend l'écriture EXR au writer upstream |
| `STUDIO_EXR_READER=legacy` | rend la lecture EXR à l'importeur upstream |
| `STUDIO_EXR_THREADS` | nombre de threads de codec (défaut : cœurs matériels) |

## Tests

```sh
.tests/run.sh [dossier de travail]     # FUZZ=<n> pour le nombre de mutations
```

Requiert les outils OpenEXR (`brew install openexr`) comme référence indépendante.
