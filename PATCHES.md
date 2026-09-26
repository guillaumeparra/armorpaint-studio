# Patches studio sur les fichiers upstream

Tout le code studio vit dans `paint/plugins/external/` (ce dépôt). Les fichiers upstream d'ArmorPaint ne
reçoivent que des corrections ou des points d'injection de quelques lignes, listés ici. Aucun n'est proposé
en PR upstream.

Chaque patch a :
- un **id** ;
- un marqueur `STUDIO-PATCH(<id>)` dans le code upstream modifié ;
- `patches/<id>.patch` : le diff à réappliquer ;
- `patches/<id>.base` : le commit upstream contre lequel il a été validé pour la dernière fois ;
- un commit sur la branche `studio` du dépôt armorpaint.

## Après chaque pull de l'upstream

`tools/check_patches.sh` tourne automatiquement après un `git pull` (merge ou rebase) une fois les hooks
installés par `tools/install_hooks.sh`. On peut aussi le lancer à la main. Pour chaque patch, il indique :

| Sortie | Action |
|---|---|
| ✅ présent | rien |
| ⚠️ PERDU | `git apply patches/<id>.patch`, rebuild, rejouer le test du patch |
| ❌ CONFLIT | réanalyser : relire le code upstream actuel, refaire le patch, régénérer le `.patch` |
| 🔎 l'upstream a modifié les fichiers | vérifier si le bug a été corrigé en amont (rejouer le test **sans** le patch) : si oui, supprimer le patch ; sinon `tools/check_patches.sh --ack <id>` |

Mise à jour type, depuis la racine d'armorpaint :

```bash
git checkout main && git pull --ff-only
git checkout studio && git rebase main        # le hook post-rewrite lance check_patches.sh
```

Pour régénérer un patch après l'avoir refait : `git diff main studio -- <fichiers> > patches/<id>.patch`,
puis `tools/check_patches.sh --ack <id>`.

---

## background-export

| | |
|---|---|
| Fichier | `paint/sources/args.c` (`args_run_export_queue`, `args_run_on_next_frame`) |
| Commit studio | `a396bd95` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-26) |
| Constaté depuis | `2efd0ff3` (2026-09-15), toujours présent en `85f6cf1c` |

**Bug.** `ArmorPaint --background <mesh> --export-textures <type> <preset> <dir>` quitte sans écrire aucun
fichier. `args_run_on_next_frame()` programme l'export pour la frame suivante
(`sys_notify_on_next_frame(&args_run_export_queue)`) puis, en `--background`, appelle `iron_stop()` sur la
frame courante : l'app s'arrête avant l'export. Sans `--background`, les mêmes arguments produisent les
fichiers.

**Correctif.** En `--background`, `args_run_on_next_frame()` n'appelle plus `iron_stop()` quand un export de
textures est programmé. C'est `args_run_export_queue()` qui l'appelle après `export_texture_run()`, sauf si
`--api` ou `--script` gèrent déjà l'arrêt. `export_texture_run()` est synchrone, donc tout est écrit avant
l'arrêt.

**Test.** Avec un mesh quelconque :

```bash
ArmorPaint --background cube.obj --export-textures png generic /tmp/out   # attendu : 5 PNG, exit 0
ArmorPaint --background cube.obj --export-textures exr16 generic /tmp/out # attendu : 5 EXR
ArmorPaint --background cube.obj --script export.c                        # non-régression
ArmorPaint --background cube.obj                                          # doit quitter seul
```

Résultats au 2026-09-26 : 5 fichiers en environ 0,5 s pour PNG et EXR, octet pour octet identiques à
l'export en mode fenêtré, et aucune régression sur `--script`, `--api` et `--background` seul.

**Signes que l'upstream l'a corrigé.** `iron_stop()` n'est plus appelé dans la branche `args_background` de
`args_run_on_next_frame()`, ou l'export de textures redevient synchrone dans cette fonction. Dans ce cas,
rejouer le test sur `main` sans le patch.

**Non couvert.** `--export-mesh` et `--export-material` s'exécutent sur la même frame que l'import. Leur
comportement en `--background` n'a pas été vérifié.

---

## studio-exr

| | |
|---|---|
| Fichier | `paint/sources/io/export_texture.c` (`export_texture_write_texture`) |
| Commit studio | `37cfb0ec` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-26) |
| Injection | 5 lignes sous `#ifdef WITH_EXTERNAL` — l'app compile toujours sans `external/` |
| Import | **aucune injection** : `studio_exr_init()` remplace l'entrée `"exr"` de `import_texture_importers` |

**Motif.** Le writer EXR upstream (`io/export_exr.c`, dérivé de miniexr) écrit un header octet par octet, sans
compression, sans alpha, et duplique le canal des maps mono-canal en RRR (voir `ARMORPAINT_ANALYSE.md` §5.4).
Le lecteur (`plugins/io_exr/io_exr.c`) ne gère que None et ZIPS, ignore le décalage `dataWindow`, et remplit un
tableau `channels[4]` sans borne.

**Correctif.** L'écriture passe par `studio_exr_write_texture()` (`external/studio_exr.c`, tinyexr) : compression
(zip par défaut), alpha, EXR mono-canal `Y` pour rough/metal/AO, attribut `software`, et écriture atomique
(`.tmp` puis `rename()`). La fonction rend `false` — et l'upstream reprend la main — si `STUDIO_EXR_WRITER=legacy`
ou si la profondeur n'est pas 16/32 bits. La lecture passe par tinyexr, avec repli sur le décodeur upstream pour
DWAA/DWAB (que tinyexr ne lit pas) uniquement après validation du fichier, `STUDIO_EXR_MAX_DIM` et bornes comprises.

**Test.** `.tests/run.sh` (ASan/UBSan, hors app : 41 cas de décodage, encodage, fuzz) et, dans l'app, sur un
projet 2048² :

```bash
ArmorPaint --background noise.arm --export-textures exr16 generic <dir>   # tinyexr
STUDIO_EXR_WRITER=legacy ArmorPaint --background noise.arm --export-textures exr16 generic <dir>  # upstream
```

Résultats au 2026-09-26, preset `generic`, 5 maps 2048² half :

| | upstream | studio |
|---|---|---|
| Total | 120 Mo | **4,7 Mo** |
| `noise_base` (RGB) | 25 198 905 o | 1 687 687 o |
| `noise_metal` (mono-canal) | 25 198 905 o | 13 277 o |

Tous les canaux sont identiques au writer upstream, valeur par valeur (`exr_test cmp`), les maps mono-canal
sortant en `Y` là où l'upstream écrivait R=G=B. Avec le preset `xplane`, `opac` et `smooth` sont désormais
présents en canal `A` — l'upstream les perdait silencieusement.

**Signes que l'upstream l'a corrigé.** `export_exr_run()` gagne une compression et un canal alpha, ou l'écriture
EXR passe à une bibliothèque tierce. Dans ce cas, comparer les deux writers avant de retirer le patch : la
nomenclature, les métadonnées et le mono-canal restent des besoins studio.

**Non couvert.** L'export EXR de `--export-mesh` / `--export-material` (aucun), les EXR tuilés et multipart en
écriture (scanline uniquement), et l'écriture DWAA/DWAB (tinyexr ne l'implémente pas ; `STUDIO_EXR_COMPRESSION`
les refuse).

---

## sinfl-bounds

| | |
|---|---|
| Fichier | `base/sources/libs/sinfl.h` (`sinfl_refill`, `sinfl_decompress`) |
| Commit studio | `aca9d34d` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-26) |
| Trouvé | 2026-09-26, par `.tests/run.sh` (fuzz DWAA/DWAB sous ASan) |

**Bug.** sinfl (décodeur deflate vendoré par iron, dérivé de vurtun/sinfl) ne borne ni son entrée ni sa sortie
sur les chemins Huffman. `iron_inflate()` lui donne pourtant des données de fichiers non maîtrisées : import EXR
(DWA et ZIPS, `plugins/io_exr/io_exr.c`) et import TIFF (`plugins/io_tiff/io_tiff.c`). Trois bornes manquaient :

1. une référence arrière pouvait pointer **avant le début** du tampon de sortie (`src = out - offs`) : seule la
   fin était contrôlée ;
2. un code de répétition de longueurs (16, 17, 18) écrivait jusqu'à 138 entrées alors que `n` n'était testé
   qu'en tête de boucle : débordement de `lens[288 + 32]` sur la pile. Une répétition à `n == 0` lisait aussi
   `lens[-1]` ;
3. `sinfl_refill()` lisait 8 octets à partir de `bitptr` sans jamais regarder la fin de l'entrée, et continuait
   au-delà : seul le chemin des blocs non compressés vérifiait `e`.

**Correctif.** Une borne pour chacun. Pour l'entrée, `struct sinfl` gagne un `bitend` et `sinfl_refill()`
alimente des octets nuls au-delà — `bitptr` dépasse légitimement `bitend` au dernier refill d'un flux valide,
seule la lecture doit rester dans les bornes. Le décodage tourne alors sur des zéros jusqu'à échouer ou remplir
la sortie, donc il s'arrête.

**Test.** `.tests/run.sh` : 22 000 mutations sur 11 fichiers, dont 3 365 confiés au décodeur DWA upstream, sans
erreur ASan. Non-régression sur données valides : EXR ZIPS et DWAA par le lecteur upstream
(`STUDIO_EXR_READER=legacy`), TIFF deflate, et la comparaison DWA contre OpenEXR du banc (écart max 0,00391,
inchangé).

**Signes que l'upstream l'a corrigé.** Un `bitend` (ou équivalent) dans `struct sinfl`, un test de `n` dans les
cas 16/17/18, ou un test de `offs` contre `out - o`. Une mise à jour de sinfl depuis vurtun/sinfl peut aussi
apporter ces bornes : rejouer le fuzz sur `main` sans le patch avant de le retirer.

**Non couvert.** Le décodeur DWA d'`io_exr.c` lui-même reste sans bornes ; c'est `studio_exr_dwa_legacy_safe()`
qui valide un fichier avant de le lui confier, et `io_exr_parse()` déborde toujours sa pile au-delà de 4 canaux
(cas `many.exr` du banc). N'appeler `io_exr_parse()` que derrière cette validation.
