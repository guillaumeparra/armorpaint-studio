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

Modules prévus, un par lot (voir `ARMORPAINT_LOTS.md` à la racine d'armorpaint) : `studio_exr` (B1),
`studio_export` (B2-B5), `studio_ocio` (A), `studio_shotgrid` (C), `studio_ui` (B6/C3). Les tiers vendorés
iront dans `vendor/`, les assets dans `assets/` (copiés dans `data/`).

Build : `../base/make --compile` depuis `paint/`. Le log de démarrage affiche `studio: external <version>
loaded`.
