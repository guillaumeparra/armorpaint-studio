# Patches studio sur les fichiers upstream

Tout le code studio vit dans `paint/plugins/external/`. Les fichiers upstream d'ArmorPaint ne
reçoivent que des corrections ou des points d'injection de quelques lignes, listés ici. Aucun n'est proposé
en PR upstream.

Chaque patch a :
- un **id** ;
- un marqueur `STUDIO-PATCH(<id>)` dans le code upstream modifié ;
- `patches/<id>.patch` : le diff à réappliquer ;
- `patches/<id>.base` : le commit upstream contre lequel il a été validé pour la dernière fois ;
- un commit sur la branche `studio`.

**Branches** (un seul dépôt, `guillaumeparra/armorpaint-studio`) :
- `upstream` : le code d'armory3d tel quel. On n'y commite jamais ; elle avance en fast-forward sur `armory3d/main`.
- `studio` : `upstream` + ces patches + nos plugins (`paint/plugins/external/`). Branche de travail et par défaut.

`git diff upstream studio -- ':!paint/plugins/external' ':!ARMORPAINT_*.md'` montre exactement nos modifications du code upstream.
Remotes : `armory3d` (lecture seule, push désactivé) et `origin` (notre dépôt privé).

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
git fetch armory3d
git checkout upstream && git merge --ff-only armory3d/main
git checkout studio && git merge upstream     # le hook post-merge lance check_patches.sh
git push origin upstream studio
```

`merge` et non `rebase` : l'historique poussé n'est jamais réécrit.

Pour régénérer un patch après l'avoir refait : `git diff upstream studio -- <fichiers> > patches/<id>.patch`,
puis `tools/check_patches.sh --ack <id>`. Quand un autre patch touche le même fichier, ce diff mêlerait les deux :
partir d'une copie du fichier courant d'où l'on retire ce seul patch (`git apply --reverse` de l'ancien `.patch`,
avec `-C1` si un autre patch a changé son contexte), puis `git diff --no-index <copie> <fichier>`, chemins de
l'en-tête ramenés à `a/<fichier>` et `b/<fichier>`. `check_patches.sh` ne demande que ceci : chaque `.patch` se retire
seul de l'arbre courant.

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
rejouer le test sur `upstream` sans le patch.

**Non couvert.** `--export-mesh` et `--export-material` s'exécutent sur la même frame que l'import. Leur
comportement en `--background` n'a pas été vérifié.

---

## studio-exr

| | |
|---|---|
| Fichier | `paint/sources/io/export_texture.c` (`export_texture_write_texture`) |
| Commit studio | `37cfb0ec` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-26) |
| Injection | 6 lignes sous `#ifdef WITH_EXTERNAL` — l'app compile toujours sans `external/` |
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

**Test.** `.tests/run.sh` (ASan/UBSan, hors app : cas de décodage, 60 fichiers écrits relus par OpenEXR, fuzz) et, dans l'app, sur un
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
| Fichier | `base/sources/libs/sinfl.h` (`sinfl_refill`, `sinfl_decompress`, `zsinflate`) |
| Commits studio | `aca9d34d`, `4b966797` sur la branche `studio` |
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

S'y ajoute un comportement indéfini, sans conséquence observée mais signalé par UBSan dès qu'il est rendu fatal
(`4b966797`) : `zsinflate()` assemble l'adler32 de fin de flux par `eob[0] << 24` sur un `int`, qui déborde pour
un octet ≥ 0x80.

**Correctif.** Une borne pour chacun. Pour l'entrée, `struct sinfl` gagne un `bitend` et `sinfl_refill()`
alimente des octets nuls au-delà — `bitptr` dépasse légitimement `bitend` au dernier refill d'un flux valide,
seule la lecture doit rester dans les bornes. Le décodage tourne alors sur des zéros jusqu'à échouer ou remplir
la sortie, donc il s'arrête. Dans `zsinflate()`, chaque octet de l'adler32 est converti en `unsigned` avant décalage.

**Test.** `.tests/run.sh` : 22 000 mutations sur 11 fichiers, dont 3 365 confiés au décodeur DWA upstream, sans
erreur ASan. Non-régression sur données valides : EXR ZIPS et DWAA par le lecteur upstream
(`STUDIO_EXR_READER=legacy`), TIFF deflate, et la comparaison DWA contre OpenEXR du banc (écart max 0,00391,
inchangé).

**Signes que l'upstream l'a corrigé.** Un `bitend` (ou équivalent) dans `struct sinfl`, un test de `n` dans les
cas 16/17/18, ou un test de `offs` contre `out - o`. Une mise à jour de sinfl depuis vurtun/sinfl peut aussi
apporter ces bornes : rejouer le fuzz sur `upstream` sans le patch avant de le retirer.

**Non couvert.** Le décodeur DWA d'`io_exr.c` lui-même reste sans bornes ; c'est `studio_exr_dwa_legacy_safe()`
qui valide un fichier avant de le lui confier, et `io_exr_parse()` déborde toujours sa pile au-delà de 4 canaux
(cas `many.exr` du banc). N'appeler `io_exr_parse()` que derrière cette validation.

---

## sdefl-bound

| | |
|---|---|
| Fichier | `base/sources/libs/sdefl.h` (`sdefl_bound`) |
| Commit studio | `40fd15a8` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-26) |
| Trouvé | 2026-09-26, par `.tests/export.sh` (encodeur PNG du lot B2 sur du bruit, sous ASan) |

**Bug.** sdefl (encodeur deflate vendoré par iron) découpe l'entrée en blocs de 256 Ko et stocke tel quel un bloc
que Huffman n'arrive pas à réduire, en blocs stockés de 64 Ko (65 535 octets) à 5 octets d'en-tête. 256 Ko n'est pas
un multiple de 65 535 : chaque bloc stocké en coûte un de plus que ce que `sdefl_bound()` prévoit (5 blocs de
64 Ko par bloc de 256 Ko, pour 4 comptés). Sur des données incompressibles de plus de 256 Ko, l'encodeur écrit
au-delà du tampon dimensionné par `sdefl_bound()` — 25 octets sur 2 Mo de bruit, environ 1,3 Ko sur une texture 4K
RGBA. `iron_deflate_raw()` est le compresseur zlib de stb_image_write (`STBIW_ZLIB_COMPRESS`, `iron_image.c`) :
tout export PNG upstream d'une texture assez bruitée débordait le tas. `iron_deflate()` a la même borne.

**Correctif.** `sdefl_bound()` compte aussi un en-tête par bloc de 256 Ko (`sdefl_div_round_up(len,
SDEFL_BLK_MAX)`). Le code studio (`studio_image.c`) calcule sa propre borne et ne dépend pas de ce patch.

**Test.** `.tests/export.sh` : `iron_deflate_raw()` sur 8 Mo de bruit, sous ASan (débordement signalé sans le
patch, dans `sdefl_flush`).

**Signes que l'upstream l'a corrigé.** `SDEFL_BLK_MAX` (ou une taille de bloc multiple de 65 535) dans
`sdefl_bound()`, ou une mise à jour de sdefl depuis vurtun/sdefl : rejouer le test sans le patch avant de le retirer.

**Non couvert.** `sdefl_bound()` et les appelants prennent un `int` : au-delà de 2 Go d'entrée, rien n'est borné
(stb_image_write n'y arrive pas pour une texture ≤ 16K RGBA 8 bits, 1 Go).

---

## menubar-hook

| | |
|---|---|
| Fichier | `paint/sources/ui/ui_menubar.c` (`ui_menubar_add_category`, `ui_menubar_draw_category_items`, `ui_menubar_render_ui`) |
| Commit studio | `6cae0e88` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-26) |
| Injection | 22 lignes, point d'injection générique, sans dépendance à `external/` (compile sans `WITH_EXTERNAL`) |

**Motif.** La barre de menus d'ArmorPaint est codée en dur (`menubar_category_t`, une chaîne par catégorie dans
`ui_menubar_render_ui()`, un `if` par catégorie dans `ui_menubar_draw_category_items()`). Aucun plugin ne peut y
ajouter un menu. Le studio y veut le menu ShotGrid (menu standard des moteurs Toolkit : section contexte,
favoris, commandes des apps).

**Correctif.** `void ui_menubar_add_category(char *label, void (*draw_items)(void))` enregistre au plus 4
catégories, affichées après Help. Leur numéro de catégorie suit `MENUBAR_CATEGORY_HELP`, la dernière valeur de
l'enum. `ui_menubar_draw_category_items()` appelle alors `draw_items`, qui dessine ses entrées avec les helpers
`ui_menu_button` / `ui_menu_separator` / `ui_menu_label`, comme une catégorie upstream : même ouverture au
survol, même fermeture. Le plugin déclare le prototype lui-même (`external/studio_workfiles.c`), si bien qu'aucun
header upstream n'est touché.

**Test.** Lancer la démo (`.tests/demo_workfiles.sh`, voir README « Workfiles et menu ShotGrid (C3) »). Un menu
ShotGrid apparaît après Help. Survoler Help puis ShotGrid bascule d'un menu à l'autre. Le menu ShotGrid liste le
contexte, « Jump to Flow Production Tracking », « Jump to File System », puis File Open... et File Save... en
favoris. Sans plugin, ou sans appel à `ui_menubar_add_category`, la barre est identique à l'upstream.

**Signes que l'upstream l'a corrigé.** Une API de menus pour les plugins (catégorie ajoutée depuis un plugin
`.js`/minic), ou `menubar_category_t` qui gagne une valeur après `HELP` : dans ce dernier cas, **le numéro des
catégories ajoutées entre en collision** avec la nouvelle, et il faut refaire le patch. Si une API existe, y
porter `studio_workfiles_init()` et retirer le patch.

**Non couvert.** L'UI tactile (`g_config->touch_ui`) : sa barre est une rangée d'icônes, les catégories ajoutées
n'y apparaissent pas. Les raccourcis Ctrl/Cmd+Alt+O et S de `studio_workfiles.c` restent disponibles.

---

## tab-name-bounds

| | |
|---|---|
| Fichier | `base/sources/iron_ui.c` (`ui_tab`, `ui_set_hovered_tab_name`) |
| Commit studio | `ac87d69c` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-26) |
| Trouvé | 2026-09-26, plantage `ArmorPaint-2026-09-26-134607.ips` (`__chk_fail_overflow` dans `ui_set_hovered_tab_name`) |

**Bug.** Le titre de l'onglet Console est le dernier message du log (`tab_console_draw` : `"%s        "`). Un
message de 248 caractères ou plus — un chemin long, une ligne `studio: shotgrid:` — dépasse les 256 octets de
`tab_names[i]` : `ui_tab()` le copie par `strcpy`, qui déborde en silence sur l'onglet suivant (sur `tab_colors`
pour le 16e). Si la souris survole alors la fenêtre, `ui_set_hovered_tab_name()` le recopie dans
`hovered_tab_name[256]`, et le `strcpy` vérifié de macOS tue l'app.

**Correctif.** Les deux copies passent par `snprintf` borné à la taille du tampon. Un titre trop long est tronqué ;
les noms comparés par `context_in_*()` (« Layers », « Materials »…) sont courts et inchangés.

**Test.** Console dans la barre latérale, lancer la démo Workfiles (`.tests/demo_workfiles.sh`) : les lignes de
démarrage du sidecar dépassent 256 caractères. Survoler la Console pendant qu'elles s'affichent en titre : sans le
patch, plantage `__chk_fail_overflow` ; avec, titre tronqué.

**Signes que l'upstream l'a corrigé.** `strcpy` disparu de `ui_tab()` / `ui_set_hovered_tab_name()`, ou
`tab_console_draw()` qui tronque le message.

**Non couvert.** Les autres `strcpy` d'`iron_ui.c` n'ont pas été audités.

---

## export-naming

| | |
|---|---|
| Fichier | `paint/sources/io/export_texture.c` (`export_texture_run_layers`) |
| Commit studio | `0c025bd7` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-27) |
| Injection | 11 lignes sous `#ifdef WITH_EXTERNAL`, en deux blocs — l'app compile toujours sans `external/` |

`.patch` régénéré au lot D1 (2026-09-27), code inchangé : [udim-export-sets](#udim-export-sets) a changé la ligne qui
suit le premier bloc.

**Motif.** L'upstream nomme les textures exportées `<dossier choisi>/<nom du fichier>_<texture><ext>`. Le nom de
fichier est celui de la dernière boîte de fichiers (`ui_files_filename`). Un studio veut le chemin que donne son
pipeline : le template Toolkit des textures, rempli avec l'entité, l'étape, le nom et la version du fichier de
travail (lot B3).

**Correctif.** Pour chaque texture, `export_texture_run_layers()` demande le chemin à
`studio_export_texture_path()` (`external/studio_export_naming.c`) :
- **variant** : le suffixe objet ou atlas ajouté au nom par l'upstream (export par objet, calque sélectionné
  masqué sur un objet). Il est lu par différence avec `f` avant ce suffixe (`studio_base`, premier bloc).
- **channel** : le nom de texture du preset (`t->name`).
- **udim** : la tuile (`object_name`, export par tuile).
- **ext** : l'extension.

`NULL` garde le chemin upstream. L'injection ne fait que remplacer `out_path` : les pixels, les formats et
`export_texture_write_texture()` sont inchangés. Le chemin vient d'un modèle que le sidecar ShotGrid a résolu
d'avance (op `texture_pattern`) : l'export ne fait qu'une substitution de chaîne et un `mkdir -p`.

**Test.** Hors app : `.tests/sg.sh` (`naming_test.c` pour la substitution et `mkdir -p`, `sg_test.py
TestToolkit.test_texture_pattern*` pour le sidecar). Dans l'app, sur le site simulé (`sg_fixture.py`) :

```bash
export STUDIO_SG_BRIDGE=.tests/sg_fixture.py STUDIO_TEST_WORKDIR=<dir> STUDIO_TEST_TK_CORE=<tk-core>
# script : next_version de la tâche 1, project_filepath_set, export_texture_run("<dir>/dialog", 0)
ArmorPaint --background --script seed.c
# attendu : storage/demo/assets/Prop/robot/TXT/work/armorpaint/textures/v001/robot_{base,nor,occ,rough,metal}.png,
# rien dans <dir>/dialog
STUDIO_SG_NAMING=1 ArmorPaint --background <…>/robot.v001.arm --export-textures exr16 generic <dir>/out
# attendu : les 5 EXR dans textures/v001/, <dir>/out vide
ArmorPaint --background <…>/robot.v001.arm --export-textures png generic <dir>/out   # sans ShotGrid :
# attendu : robot.v001_base.png… dans <dir>/out (noms upstream)
```

**Signes que l'upstream l'a corrigé.** Un modèle de nom dans les presets d'export (`export_preset_t`), ou un hook
de nommage pour les plugins. Porter alors `studio_export_texture_path()` dessus et retirer le patch. Si la
construction de `out_path` ou l'ajout du suffixe objet change, refaire les deux blocs.

**Non couvert.** L'export « bake material » (`export_texture_run_bake_material`, autre construction de chemin).
L'export par tuile UDIM et par objet n'a pas été rejoué dans l'app : ces modes ne se règlent que dans l'UI, le
scripting n'expose pas `layers_export`. Seuls la substitution et le modèle `pattern_udim` sont testés.

## studio-export

| | |
|---|---|
| Fichier | `paint/sources/io/export_texture.c` (`export_texture_run`, `export_texture_run_layers`) |
| Commit studio | `a5083508` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-26) |
| Injection | 18 lignes sous `#ifdef WITH_EXTERNAL`, en trois blocs — l'app compile toujours sans `external/` |
| Dépend de | `export-naming` (variable `studio_base`) |

**Motif.** L'upstream écrit chaque texture d'un preset en PNG ou JPG 8 bits, ou en EXR 16/32 bits (réglage global
`base_bits`). Le preset ne décrit que quatre canaux nommés et un `color_space`. Un studio veut, par texture, le format,
la profondeur, la compression, l'espace colorimétrique et le nom de fichier, ainsi que des canaux calculés. Il veut
aussi un contrôle avant écriture, une écriture atomique et un manifeste (lots B2, B4, B5).

**Correctif.** Trois points d'injection vers `external/studio_export.c`. Ils ne font rien si le preset choisi n'est
pas un preset studio.
- **`export_texture_run`**, après le chargement du preset : `studio_export_begin()`.
  - Pour un preset studio, il rappelle `export_texture_run` en passe à blanc. L'upstream parcourt objets, atlas et
    tuiles UDIM, et le plan de fichiers est relevé.
  - Le préflight s'exécute ensuite.
  - `false` arrête l'export : problème bloquant, préflight seul, ou fin de la passe à blanc.
- **`export_texture_run_layers`**, une fois les calques aplatis dans `layers_expa/b/c` : `studio_export_layers()`.
  - En passe à blanc, il note les fichiers de la variante.
  - En passe réelle, il relit les trois textures, compose, encode et écrit en `.studio-tmp`.
  - `true` saute l'écriture upstream par canal.
- **Avant « Textures exported »** : `studio_export_end()` met les fichiers en place par renommage, puis écrit le
  manifeste. `false` (échec) supprime les temporaires ; aucun fichier existant n'est remplacé.

Les boucles de l'upstream (objets, atlas, tuiles, masque d'objet, bake) restent la seule source des variantes : le
studio ne les réimplémente pas.

**Test.** Hors app : `.tests/export.sh`, qui teste sous ASan/UBSan les encodeurs PNG et TIFF (relus par libpng et
libtiff), les expressions, les presets, le préflight et SHA-256. Dans l'app : `.tests/export_app.sh`, qui vérifie :
- les exports par script et par `--export-textures` ;
- l'égalité au pixel près d'un preset studio raw 8 bits avec `generic` upstream ;
- les empreintes du manifeste ;
- l'écrasement bloqué et le dossier en lecture seule ;
- un échec au milieu de l'écriture, qui ne remplace rien.

**Signes que l'upstream l'a corrigé.** Des presets d'export avec format et profondeur par texture, ou un hook d'export
pour les plugins. Porter alors `studio_export.c` dessus. Si `export_texture_run` ou l'aplatissement des calques change
(noms `layers_expa/b/c`, ordre des canaux, encodage des ids émission/subsurface dans `nor.a`), revoir `load_row()`.

**Non couvert.** L'export par tuile UDIM et par objet n'a pas été rejoué dans l'app : il faut un maillage à tuiles ou
des masques d'objet. Le plan et le préflight de ces modes sont testés hors app. Le mode « pack into project » garde
l'écriture upstream.

---

## obj-global-indices

| | |
|---|---|
| Fichier | `base/sources/iron_obj.c` (`obj_parse` et ses lecteurs `read_int`, `read_face`, `read_float`, `read_string`, `skip_whitespace`) |
| Commit studio | `252f9fc3` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-26) |
| Trouvé | 2026-09-26, `ArmorPaint --background tiles.obj` : EXC_BAD_ACCESS dans `obj_parse` (rc 139) |

**Bug.** Les indices d'un OBJ sont globaux au fichier : une face de l'objet 2 peut utiliser un `vn` (ou un `vt`,
un `v`) déclaré sous l'objet 1. C'est valide, et courant quand un exporteur factorise les normales. En split par
objet (le défaut, et le mode UDIM), `obj_parse()` jetait pourtant les `v`/`vt`/`vn` à chaque nouvelle part et
retranchait à ses indices le nombre d'éléments des parts précédentes (« Assumes separate vertex data per object »).
L'indice `1` de l'objet 2 devenait négatif, puis énorme en `uint32_t` : lecture hors du tampon, plantage.

Deux autres effets de la même hypothèse :
- le format de face (`v`, `v/t`, `v//n`, `v/t/n`) était deviné d'après les `vt`/`vn` lus dans la part : un objet
  sans `vn` propre lisait `f 5/5/1` comme `v/t` puis traitait `/1` comme un nouveau sommet ;
- aucun indice n'était vérifié. Un OBJ tronqué ou malformé (indice hors bornes, nul, en avant, face à moins de 3
  ou plus de 512 sommets, nom d'objet de plus de 255 caractères, fichier coupé en milieu de ligne) débordait
  `va`/`ua`/`na`, `buf`, `str` ou le tampon du fichier.

**Correctif.**
- Les `v`/`vt`/`vn` sont gardés d'une part à l'autre quel que soit le split (le split par matériau le faisait
  déjà) et libérés par la dernière part. Les indices restent globaux, sans décalage ; les indices négatifs (relatifs
  à la fin de la liste, norme OBJ) sont résolus.
- `read_face()` lit la syntaxe de chaque sommet de la face au lieu de la deviner, et `obj_index()` convertit chaque
  indice en rendant `-1` s'il est absent ou hors bornes.
- `check_face()` : une face dont une position est invalide, ou qui a moins de 3 ou plus de 512 sommets, est
  ignorée (avertissement « Skipped faces with invalid vertex indices », une fois par import). Une face dont un uv
  ou une normale est invalide garde sa géométrie : ses uvs pointent sur le `vt` 0, comme le remplissage upstream
  des faces sans uv, et si une face de la part n'a pas de normale, les normales de la part sont calculées.
- Uvs et normales sont empilés au même rythme que les positions (`push_corner`) : plus de lecture de
  `nor_indices` au-delà de sa taille quand certaines faces n'ont pas de normale.
- Toutes les lectures s'arrêtent à `bytes_length` ; `read_int()` borne `buf` et sature au lieu de déborder ;
  `read_string()` tronque le nom à 255 caractères.
- L'échelle de packing (`scale_pos`) couvre les positions déclarées dans la part, comme l'upstream, plus celles
  d'objets précédents qu'elle utilise (sans quoi le packing int16 débordait). En split par matériau, elle reste
  calculée sur toutes les positions lues, comme l'upstream.

Sur un OBJ à données séparées par objet — tout ce que l'upstream lisait sans planter — la sortie est identique,
part par part, octet pour octet (étape 1 du banc).

**Test.** `.tests/obj.sh` (hors app, ASan/UBSan, compare à `upstream:base/sources/iron_obj.c`) :
1. `valid/` : 10 fichiers (sphères en `v`, `v/t`, `v//n`, `v/t/n`, CRLF, usemtl, triangles, n-gones concaves,
   grille UDIM), splits objet, matériau et UDIM : sortie identique à l'upstream ;
2. `shared/` : faces sur les `vn` / `vt` / `v` d'un objet précédent (triangles, quads, n-gone, indices relatifs,
   positions réutilisées plus grandes que celles de la part) : sortie identique à la version à données séparées
   lue par l'upstream ;
3. `bad/` : 15 fichiers malformés, aucune erreur ;
4. fuzz (étape 6 du banc depuis obj-float-exponent) : 1 000 mutations de `valid/` et `shared/` en splits objet et
   matériau. Au 2026-09-26 : **0 plantage** contre 1 413 pour l'upstream (heap-buffer-overflow, use-after-free,
   BUS, SEGV) ; à 3 000 mutations, 0 contre 4 251.

Les étapes 4 et 5 du banc (`float`, `exp/`) sont celles de [obj-float-exponent](#obj-float-exponent).

Dans l'app :

```bash
ArmorPaint --background tiles.obj                    # le fichier du bug : rc 0 (139 avant)
ArmorPaint --background tiles.obj --script .tests/obj_names.c   # tile.1001 et tile.1002, 6 indices chacun
```

`obj_names.c` parcourt `script_get_context()->paint_object` et ses `base->children` et affiche nom et
`data->index_array->length`. Résultats au 2026-09-26 : le fichier du bug, les quads et le n-gone partagés, et
les 25 fichiers `shared/` et `bad/` du banc sortent tous en rc 0 ; les sphères à 3 objets (`v/t/n` et usemtl) et
les n-gones donnent les mêmes objets, avec les mêmes nombres d'indices, qu'avant le patch.

**Signes que l'upstream l'a corrigé.** `vind_off` / `tind_off` / `nind_off` disparaissent, ou `array_free` de
`pos_temp` n'est plus appelé pour chaque part en split objet. Rejouer alors `.tests/obj.sh` avec
`STUDIO_UPSTREAM=upstream` sur une copie d'`upstream` **sans** le patch (étape `shared/`).

**Non couvert.**
- En split par objet, les `v`/`vt`/`vn` de tout le fichier restent en mémoire jusqu'à la dernière part (l'upstream
  ne gardait que ceux de la part courante) : de l'ordre de la taille du fichier, comme en split par matériau.
- `read_float()` : l'exposant (`1e10`) débordait un `int` (UBSan `iron_obj.c:140` pendant le fuzz) : corrigé par
  [obj-float-exponent](#obj-float-exponent). Le packing int16 des coordonnées hors bornes (`float-cast-overflow`,
  saturé sur arm64) n'est pas traité : il relève de la lecture des flottants, hors du périmètre des indices.
- `import_obj_parse()` pousse la première part même sans face (fichier dont le premier objet n'a pas de face
  valide) : l'import aboutit (rc 0) avec un objet vide, comportement upstream inchangé.
- Le split UDIM (`obj_parse(…, udim)`) : repris par [udim-import](#udim-import), vérifié dans l'app par
  `.tests/udim_app.sh`.

---

## obj-float-exponent

| | |
|---|---|
| Fichier | `base/sources/iron_obj.c` (`read_float`, nouveau `read_exponent`) |
| Commit studio | `70cece4b` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-26) |
| Trouvé | 2026-09-26, fuzz de [obj-global-indices](#obj-global-indices) : UBSan `signed-integer-overflow` sur `dec *= 10` |

**Bug.** `read_float()` lisait mal les flottants en notation scientifique, que certains exporteurs écrivent pour
les coordonnées et normales minuscules (`1.0e-20`) ou pour toutes les valeurs (`%e`) :
- sur `e`/`E`, seul le premier chiffre de la mantisse était gardé (`buf[0] - 48`) : `1.5e-3` donnait 0.001,
  `-0.5e-2` donnait 0, `2.5E3` donnait 2000 ;
- l'exposant, lu par `read_int()`, prenait le `+` pour un chiffre : `1E+2` et `1.5e+00` donnaient n'importe quoi
  (inf, 1e-5) ;
- il était appliqué en multipliant un `int` par 10, `exp` fois : débordement signé dès 1e10 (UB), et une boucle de
  `exp` tours pour un exposant énorme (`1e2147483647`).

Dans l'app, un OBJ écrit en `%e` (`0.000000e+00`, `1.000000e+00`...) s'importait avec `scale_pos` à inf ou nan et
tous les sommets à 0 : maillage effondré, sans erreur.

**Correctif.**
- Sur `e`/`E`, `read_float()` lit l'exposant (`read_exponent()` : signe `+`/`-`, chiffres, saturé à 99, jusqu'à la
  fin du jeton comme le reste de `read_float()`) puis sort de la boucle : la mantisse bufferisée passe par le calcul
  décimal habituel, en entier.
- Le résultat est multiplié par `pow(10, exp)` en double, puis borné à ±`FLT_MAX` : pas d'inf dans le maillage, qui
  rendrait l'échelle de packing infinie. Pas de boucle, pas d'entier qui déborde. Sous `FLT_MIN`, le résultat devient
  dénormal puis 0, comme `strtof`.
- L'exposant passe par une statique (`read_float_exp`), remise à 0 dès qu'elle est appliquée : les lignes de
  `read_float()` et le haut du fichier touchés par obj-global-indices restent intacts (contexte de son `.patch`).

Sans exposant, le résultat est inchangé (étape 4 du banc : 20 000 jetons identiques bit pour bit à l'upstream ; étape
1 : sortie identique octet pour octet). La mantisse reste limitée à 9 caractères par `buf`, comme sans exposant :
`-1.234567e-05` (sortie `%e` par défaut) tient ; au-delà, les chiffres de droite sont perdus.

Trois patches touchent `iron_obj.c` : `obj-global-indices.patch` (`upstream` → `252f9fc3`), celui-ci
(`252f9fc3` → `70cece4b`), puis `udim-import.patch` (`70cece4b` → `4c1ca6cb`). Les réappliquer dans cet ordre.
Pour régénérer celui-ci : `git diff 252f9fc3 70cece4b -- base/sources/iron_obj.c` (ou les commits qui l'ont refait).

**Test.** `.tests/obj.sh` (hors app, ASan/UBSan ; `STUDIO_ROOT=<armorpaint>` si ce dépôt est lancé depuis un
worktree) :
- étape 4, `float` (`obj_float_test.c`, qui inclut `iron_obj.c` pour appeler `read_float()`) : 30 jetons à exposant
  comparés à `strtod` borné à ±`FLT_MAX`, à 1 ulp près, fin de jeton comprise : `1.5e-3`, `-0.5e-2`, `1E+2`,
  `1e-45` (dénormal), `1e999` et `-1e999` (±`FLT_MAX`), `1e-999` et `0e999` (0), `1e2147483648`, `1e`, `1e+`,
  `1.5e-3x`... Tous passent ; 3 sont à 1 ulp de `strtod` (`6.02e23`, `3.4e38`, `7.450581e-09` : mantisse
  arrondie en float avant la multiplication). L'upstream en rate 22 sur 30. Puis 20 000 jetons décimaux sans
  exposant, sortie identique à l'upstream. Jusqu'au 2026-09-27, `obj_float_test_up` incluait notre `iron_obj.c` au
  lieu de la copie upstream (`-I$BASE` passait avant elle) : cette comparaison ne comparait rien et le décompte de
  l'upstream noté ici (25 sur 30) était faux. Corrigé dans `obj.sh` avec udim-import ; les 20 000 jetons restent
  identiques ;
- étape 5, `exp/` : grille à deux objets en `%.6e`, la même en `E` majuscule, et un quad à coordonnées et normale
  minuscules (`1.0e-30`, `3.2e-12`, `1E+0`) : sortie identique, splits objet et matériau, à leur version décimale
  lue par l'upstream. Avant le patch (studio `252f9fc3`), les deux fichiers sortent différents ;
- étape 6, fuzz, avec des jetons d'exposant (`e`, `E+`, `e-45`, `e999999999999`, `e-2147483649`...) et `exp/` dans
  les graines. L'étape échoue désormais si l'arbre courant signale un UB. Au 2026-09-26, 1 000 mutations :
  **0 plantage, 0 UB** (studio `252f9fc3` sur le même fuzz : 620 `signed integer overflow` en `iron_obj.c:140`,
  upstream : 1 393 plantages) ; 3 000 mutations : 0 plantage, 0 UB.

Dans l'app (chemins absolus ; un chemin relatif faisait planter l'import avant [args-cwd-paths](#args-cwd-paths)) :

```bash
ArmorPaint <w>/exp/grid.obj --background --script .tests/obj_scale.c       # comme grid.ref.obj
ArmorPaint <w>/exp/grid.ref.obj --background --script .tests/obj_scale.c
```

`obj_scale.c` affiche, par objet, le nombre d'indices, `scale_pos` et le premier sommet packé. Résultats au
2026-09-26 : `grid.obj`, `grid_upper.obj` et `grid.ref.obj` donnent `g0(idx=72 scale=1.531250 ...)`,
`g1(idx=72 scale=4.031250 ...)`, mêmes sommets ; `tiny.obj` = `tiny.ref.obj`. Sans le patch : `scale=inf` (nan
pour l'objet fusionné) et tous les sommets à 0.

**Signes que l'upstream l'a corrigé.** Le bloc `if (c == 'E' || c == 'e')` de `read_float()` ne contient plus
`int first = buf[0] ...` ni la boucle `dec *= 10`, ou `read_float()` passe par `strtof`. Rejouer l'étape 4 de
`.tests/obj.sh` contre l'upstream sans le patch.

**Non couvert.**
- Écart d'au plus 1 ulp avec `strtof` sur certaines valeurs (mantisse arrondie en float, puis produit en double).
- Mantisse de plus de 9 caractères : tronquée comme sans exposant (comportement upstream de `read_float()`).
- `nan`, `inf` écrits en toutes lettres : non reconnus, lus comme avant (valeur quelconque, finie).
- Le packing int16 des normales (`(int)(n * 32767)`) ne borne pas une normale non normalisée : `vn 1e30 0 0` est
  lu 1e30, et la conversion float → int déborde (`float-cast-overflow`, exclu du banc, saturé sur arm64) — comme `vn 999999999`
  sans exposant.

## track-external

| | |
|---|---|
| Fichier | `.gitignore` |
| Commit studio | `df0d2954` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-26) |
| Injection | 1 ligne : `paint/plugins/external` n'est plus ignoré (remplacée par un commentaire marqueur) |

**Motif.** L'upstream ignore `paint/plugins/external/`, le dossier des plugins hors arbre. Le code studio y vit. Il
était donc dans un dépôt git séparé. Depuis le 2026-09-26, tout est dans un seul dépôt : la branche `studio` suit le
dossier, dont l'historique a été importé par `git subtree add`.

**Test.** `git check-ignore paint/plugins/external/external.c` ne renvoie rien. `git status` voit les modifications
du dossier.

**Signes que l'upstream l'a changé.** Un conflit sur `.gitignore` au merge d'`upstream`. Garder alors leurs nouvelles
lignes, et le commentaire marqueur à la place de celle d'`external`.

---

## args-cwd-paths

| | |
|---|---|
| Fichier | `paint/sources/args.c` (`args_path`, include `<unistd.h>`) |
| Commit studio | `d7e5bfb7` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-26) |
| Trouvé | 2026-09-26, `ArmorPaint --background grid.obj` (chemin relatif) : EXC_BAD_ACCESS dans `obj_parse` (rc 139) |

**Bug.** Un chemin relatif passé en ligne de commande n'est jamais résolu contre le dossier courant de l'appelant.
`args_path()` transforme `grid.obj` en `./grid.obj`, puis `iron_file_reader_open()` le résout contre le dossier de
l'app : `Contents/Resources/out/` sur macOS, le dossier du binaire (`fileslocation`) sur Linux et Windows. Le fichier
n'est pas trouvé, et l'import plante sur le blob NULL (voir [import-null-guards](#import-null-guards)). Même cause
pour `--script foo.c` (« Invalid script path ») et un projet `.arm` relatif. Seuls les dossiers d'export
(`--export-textures`, `--export-mesh`), écrits par `fopen` direct, suivaient le dossier courant.

**Correctif.** Sur Windows, Linux et macOS, `args_path()` préfixe un chemin non absolu par `getcwd()`
(`GetCurrentDirectoryW` converti en UTF-8 sur Windows) et un `/` ; un `./` de tête est retiré. Les chemins absolus
sont inchangés. Si le dossier courant est illisible (ou plus long que 1 023 caractères), et sur Android, iOS et
wasm, le comportement upstream est gardé.

Deux patches touchent `args.c` : `background-export.patch` (bas du fichier) et celui-ci (`args_path()`, en haut),
indépendants. Pour régénérer celui-ci : `git diff d7e5bfb7^ d7e5bfb7 -- paint/sources/args.c` (ou le commit qui
l'a refait).

**Test.** `.tests/import_app.sh` (section « chemins relatifs ») : `grid.obj --script names.c`,
`./grid.obj --script ./names.c` et, depuis un sous-dossier, `../grid.obj --script ../names.c` importent la grille
(`root=grid idx=6`). Avant le patch : rc 139 pour les trois. Vérifié aussi à la main : `--export-textures png
generic out` relatif écrit les 5 mêmes PNG dans `./out` qu'avant le patch.

**Signes que l'upstream l'a corrigé.** `args_path()` appelle `getcwd`, `realpath` ou un équivalent, ou
`iron_file_reader_open()` ne préfixe plus les chemins relatifs par le dossier de l'app. Rejouer la section
« chemins relatifs » de `.tests/import_app.sh` sur `upstream` sans le patch.

**Non couvert.**
- Changement de comportement sur Linux et Windows : un chemin relatif désignait jusqu'ici un fichier du dossier
  du binaire, il désigne maintenant un fichier du dossier courant (comme sur tout outil en ligne de commande).
- Lancé par `open -a ArmorPaint --args grid.obj` (macOS), le dossier courant est `/` : passer des chemins absolus.
- Testé sur macOS uniquement ; la branche Windows (`GetCurrentDirectoryW`) n'a pas été compilée.

---

## import-null-guards

| | |
|---|---|
| Fichier | `paint/sources/io/import_mesh.c` (`import_mesh_run`), `paint/sources/io/import_obj.c` (`import_obj_run`), `paint/sources/io/import_texture.c` (`import_texture_run`), `paint/plugins/io_fbx/io_fbx.c` (`io_fbx_parse`) |
| Commit studio | `33bd8667`, puis `6d7fac34` (message de l'importeur gardé) sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-27) |
| Trouvé | 2026-09-26, même commande qu'[args-cwd-paths](#args-cwd-paths) : `obj_parse` (`iron_obj.c:269`) ← `import_obj_parse` ← `import_mesh_run` ← `import_asset_run` ← `args_run_on_next_frame` |

**Bug.** `data_get_blob()` rend NULL pour un fichier absent ou illisible, et la plupart des importeurs
déréférencent le blob sans le vérifier. Au 2026-09-26, en `--background` avec un chemin absolu :
- mesh absent ou illisible (`chmod 000`) : `.obj` (`obj_parse`), `.fbx` (`import_fbx`), `.gltf` / `.glb`
  (`import_gltf_glb`) : rc 139 ;
- texture absente : `.psd`, `.tiff`, `.svg` (importeurs de `plugins.c`) : rc 139 ;
- fichier lisible mais invalide : `.gltf` / `.glb` rejeté par cgltf, `io_gltf_parse()` rend NULL et
  `import_mesh_run()` lit `mesh->name` ; `.fbx` rejeté par ufbx, `io_fbx_parse()` lit la scène NULL : rc 139.

Déjà sains : `.arm` (`import_arm_run_project` affiche « Could not open file »), `.png` / `.hdr`
(l'importeur par défaut rend NULL, `import_texture_run()` s'arrête), `.exr` (importeur studio : « EXR import:
cannot read »), `.blend` (vérifie l'OBJ exporté par Blender), l'API de script (`script_import_asset`,
`script_append_mesh` testent `iron_file_exists`).

**Correctif.**
- `import_mesh_run()` vérifie, avant de choisir l'importeur, que le fichier s'ouvre
  (`iron_file_exists(data_resolve_path(path))`, la même résolution que `data_get_blob()`), sinon affiche
  « Could not open file <path>. », comme `import_arm_run_project()`. Couvre tous les formats de mesh, plugins et
  importeurs de script compris, et tous les appelants (`import_asset_run`, boîte d'import, `project_reimport_mesh`
  sur un mesh déplacé).
- `import_mesh_run()` affiche « Could not import file <path>. » si l'importeur rend NULL pour la première part ;
  `io_fbx_parse()` rend NULL quand `ufbx_load_memory()` échoue. Depuis `6d7fac34`, seulement si l'importeur n'a pas
  déjà affiché sa propre erreur (`console_message` inchangé) : le refus d'un FBX UDIM par [fbx-split](#fbx-split)
  (« UDIM import refused: … ») reste lisible.
- `import_obj_run()` vérifie son blob (appelé aussi par l'import `.blend`).
- `import_texture_run()` fait la même vérification avant d'appeler un importeur de plugin (pas pour l'importeur
  par défaut ni pour une texture déjà en cache, ce qui garde les calques PSD, mis en cache sous un chemin fictif).

Les appels d'importeur qui encadrent ce test dans `import_texture_run()` portent [ocio-import](#ocio-import) : le
hunk de `import_texture.c` a été régénéré le 2026-09-27 avec ce contexte (état d'avant le patch reconstruit en
retirant ses 4 lignes, puis `git diff --no-index`). Les autres fichiers du `.patch` viennent des commits du patch.

**Test.** `.tests/import_app.sh` : fichiers absents (`obj fbx gltf glb psd tiff svg`) et `noread.obj`
(`chmod 000`) → « Could not open file », rc 0 ; `bad.gltf`, `bad.glb`, `bad.fbx` (contenu `garbage`) → « Could not
import file », rc 0 ; non-régression : `grid.obj`, `tri.gltf`, `tri.glb`, `quad.fbx` (FBX ASCII) donnent les
mêmes objets et nombres d'indices, `red.png` / `.svg` / `.tiff` / `.psd` (générés par `sips`) « Texture imported ».
Au 2026-09-26 : tout passe ; le même script sur le build d'avant les deux patches (`70cece4b`) : 14 échecs, tous en
rc 139, et la section de non-régression identique. Le message gardé : `.tests/udim_app.sh`, « FBX refusé (uv
négatives) : son message reste ».

**Signes que l'upstream l'a corrigé.** `import_obj_run()` ou les importeurs de `paint/plugins/plugins.c` testent
`b == NULL`, ou `data_get_blob()` ne rend plus NULL (blob vide, erreur remontée). Rejouer `.tests/import_app.sh`
sur `upstream` sans le patch.

**Non couvert.**
- Les importeurs de `paint/plugins/plugins.c` et `paint/assets/plugins/import_stl.c` ne testent toujours pas leur
  blob : protégés seulement par la vérification en amont (`import_mesh_run`, `import_texture_run`). Un nouvel
  appelant qui ne passerait pas par ces fonctions planterait encore.
- Fichier supprimé entre la vérification et la lecture : non traité.
- Parts suivantes (`has_next`) : un importeur qui rendrait NULL après la première part planterait encore.
- Un `.fbx` sans aucun mesh valide (`raw->name` NULL) n'a pas été testé.
- Polices (`import_font_run`) et sons (`import_sound_run`) ne vérifient pas le fichier ; ils ne sont joignables que
  par glisser-déposer ou le navigateur de fichiers (la ligne de commande ne les accepte pas, l'API de script teste
  `iron_file_exists`), donc avec un fichier qui existe.

---

## external-project

| | |
|---|---|
| Fichier | `paint/plugins/project.c` |
| Commit studio | `1511db63` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-26) |
| Injection | 1 ligne : `add_project(project, "external/.project");` dans le bloc `if (fs_exists("external"))` |

**Motif.** Le hook upstream compile les `.c` d'`external/`, ses shaders et ses assets, mais ne permet d'ajouter ni
bibliothèque ni define. Le lot A1 doit lier OpenColorIO en statique sur macOS (`.ocio/lib/libstudio_ocio.a`) : une
dylib chargée par dlopen est refusée par la validation des bibliothèques du hardened runtime (ArmorPaint est signé
ad hoc). L'injection fait évaluer `external/.project/project.c`, qui ajoute la bibliothèque et
`WITH_STUDIO_OCIO_STATIC` quand l'archive existe. Le dossier commence par un point : amake n'en compile pas le
contenu dans l'app.

**Test.** Après `tools/build_ocio.sh` et `../base/make --compile`, le log de démarrage d'ArmorPaint affiche
`studio: OCIO 2.5.2, config …` ; `.tests/ocio_app.sh` passe. Sans l'archive, le build réussit et le log affiche
`OCIO unavailable: ArmorPaint was built without libstudio_ocio`. Sans le patch, même message : la bibliothèque
n'est pas liée.

**Signes que l'upstream l'a changé.** Un conflit sur `paint/plugins/project.c`, ou un nouveau mécanisme upstream
pour qu'`external/` déclare ses bibliothèques (par exemple un `add_project(project, "external")` ou un
`external/project.c` évalué). Le préférer alors et déplacer `.project/project.c` à l'endroit attendu.

---

## ocio-viewport

| | |
|---|---|
| Fichiers | `paint/shaders/compositor_pass.shader`, `paint/sources/startup.c` (shader data `compositor_pass`), `paint/sources/uniforms.c` (`uniforms_ext_vec4_link`, `uniforms_ext_tex_link`) |
| Commit studio | `165d52d0` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-26) |
| Injection | shader : 1 constante `float4 ocio_lut_data`, 1 texture `ocio_lut_tex`, la fonction `ocio_shaper` et un bloc de lecture de LUT 3D (~35 lignes) ; `startup.c` : la constante et l'unité de texture déclarées (7 constantes, 3 textures) ; `uniforms.c` : des valeurs neutres pour les deux liens (7 lignes) |

**Motif.** Le lot A2 affiche le viewport à travers la transformation display/view/look d'OCIO. Le compositeur
d'ArmorPaint applique en dur un tonemap filmique (Hejl) qui encode aussi le gamma, ou une LUT `.cube` (8 bits,
entrée bornée à [0, 1] après un `pow(0.4545)`). Aucun des deux ne peut porter une vue ACES : il faut une LUT 3D
en half, avec une mise en forme logarithmique de l'entrée HDR, lue par le shader.

**Correctif.** Le shader lit `ocio_lut_data` (nœuds, colonnes et rangées de la grille de tuiles). Si `x > 0` et
que le viewport fait un tonemap (`tonemap_strength > 0` : modes Lit et Path Trace), la couleur passe par la courbe
ACEScct puis par la LUT (interpolation trilinéaire : bilinéaire matériel dans deux tuiles, puis mélange), à la place
du tonemap filmique. La LUT `.cube` s'applique ensuite sur les valeurs d'affichage, sans le `pow(0.4545)` : tonemap
et `.cube` ne sont plus exclusifs quand OCIO est actif. Le format de la LUT est décrit dans
`external/studio_ocio_lut.h`, et `studio_ocio_lut_sample()` en est la réplique CPU. `uniforms.c` rend `(0, 0, 0, 0)`
et la texture `empty_white` : sans plugin, le constant buffer ne garde pas les restes d'un autre draw et le
compositeur se comporte comme upstream. Le plugin (`external/studio_ocio_view.c`) chaîne `uniforms_vec4_links` et
`uniforms_tex_links` pour fournir la vraie LUT.

**Test.** `.tests/ocio_view_app.sh` : des couleurs passent par le vrai compositeur (`studio_ocio_view_probe`,
cible 1 × 1) et sont comparées à PyOpenColorIO (≤ 0,14/255 sur les vues SDR ACES 2.0, Un-tone-mapped, avec look,
Display P3). Désactivé (`studio_ocio_view_set_enabled(0)`, qui rend les mêmes valeurs neutres que `uniforms.c`),
la sortie est le tonemap filmique de Hejl. En mode couleur de base, l'entrée ressort telle quelle. Sans le patch, le
plugin se construit mais le compositeur reste filmique : les comparaisons à PyOpenColorIO échouent.

**Signes que l'upstream l'a changé.** Un conflit sur `compositor_pass.shader` ou sur la shader data
`compositor_pass` de `startup.c` (constantes ajoutées : l'ordre des constantes fixe leurs offsets, garder
`ocio_lut_data` en dernier) ; un nouveau tonemap (AgX, ACES) ou une gestion couleur upstream (OCIO, LUT 3D half) :
comparer, et retirer le patch si l'upstream offre une LUT 3D HDR qu'un plugin peut remplir. Le shader généré se
vérifie dans `paint/build/out/data/compositor_pass.vert.metal`.

**Non couvert.** Les sphères d'environnement (`render_envsphere.c`, `render_pathsphere.c`, désactivées par défaut)
gardent leur propre tonemap filmique. Les aperçus de matériaux qui passent par le compositeur en mode Lit suivent
la vue OCIO, comme le viewport.

---

## export-window

| | |
|---|---|
| Fichier | `paint/sources/ui/box_export.c` (`box_export_show_textures`) |
| Commit studio | `8f5e0900` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-26) |
| Injection | 4 lignes sous `#ifdef WITH_EXTERNAL` : l'appel upstream passe dans le `#else` — l'app compile toujours sans `external/` |

**Motif.** Le lot B6 remplace la boîte d'export de textures d'ArmorPaint par la fenêtre Qt du studio
(`external/studio_export_window.c`, `assets/studio_export_window.py`) : presets studio seulement, espace OCIO,
préflight en direct. Chaque entrée d'export doit l'ouvrir : File > Export > Textures (`ui_menubar.c`), l'icône
d'export de la barre d'outils, Export du menu d'un calque (`tab_layers.c`, mode Selected), les raccourcis
(`util_shortcut.c`). Tous passent par `box_export_show_textures()`.

**Correctif.** Avec `WITH_EXTERNAL`, `box_export_show_textures()` appelle `studio_export_window_show()` au lieu
d'ouvrir la boîte. Le mode d'export posé par l'appelant (`layers_export`) est repris par la fenêtre. Un export abouti
devient l'export courant (dossier, preset, mode, marge) : `Ctrl+E` (`box_export_show_textures` n'est pas appelé)
le refait par `export_texture_run` sans rouvrir de fenêtre.

**Test.** `.tests/export_window_app.sh` : un script appelle `studio_export_window_show()`, la fenêtre tourne sans
intervention (Qt offscreen) et exporte. Pour vérifier le menu lui-même : lancer l'app, File > Export > Textures ouvre
la fenêtre Qt. Sans le patch, la boîte upstream s'ouvre.

**Signes que l'upstream l'a changé.** `box_export_show_textures` renommé, découpé, ou qui ne sert plus à toutes les
entrées d'export (une nouvelle entrée qui appelle directement `ui_box_show_custom(&box_export_show_textures_box…)`) :
la rediriger aussi. Une nouvelle option dans l'onglet Export de la boîte (autre que format, résolution, preset,
layers, pack, padding) : voir s'il faut l'ajouter à la fenêtre.

**Non couvert.** Les onglets Presets (éditeur de presets upstream) et Atlases, et « Pack into project », ne sont plus
accessibles. La boîte d'export de bake (`box_export_show_bake_material`) et l'export de maillage ne sont pas touchés.

---

## udim-import

| | |
|---|---|
| Fichiers | `base/sources/iron_udim.c` et `.h` (nouveaux), `base/sources/iron_obj.c` (`obj_parse`, nouveaux `obj_udim_*`) et `.h`, `paint/sources/io/import_obj.c` (`import_obj_parse`, nouveau `_import_obj_udim`), `paint/sources/io/import_mesh.c` (`import_mesh_run`, `import_mesh_finish_import`, `_import_mesh_unique_name`) ; lot D1 : `paint/sources/enums.h` (`SPLIT_TYPE_MATERIAL_UDIM`), `paint/sources/ui/box_import_mesh.c` (`project_import_mesh_box_draw`) |
| Commit studio | `4c1ca6cb` (lot D0) et `16ac7cb8` (lot D1) sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-27) |
| Trouvé | 2026-09-26, analyse des UDIM (`ARMORPAINT_UDIM.md` §3, L11 à L13) ; L12 confirmé sous ASan : `heap-buffer-overflow` sur `num[tile] += 3` |

**Bug.** En `Split By: UDIM Tile`, `obj_parse()` rangeait chaque triangle dans la tuile du barycentre de ses uv,
converti par `(int)` :
- uv négatives : `(int)` tronque vers 0. u = -0,3 part dans 1001 ; u = -1,5 donne l'index -1, et `num[tile] += 3`
  écrit 4 octets avant le tableau (L12) ;
- u ≥ 10 : la tuile `1000 + v·10 + u + 1` n'est jamais bornée, u = 10 donne 1011, qui est la tuile (0, 1) (L11) ;
- triangle à cheval sur plusieurs tuiles : rangé entier dans une seule, puis ramené en `uv - tuile` sommet par
  sommet. Un `vt` partagé par des triangles de tuiles différentes prend le décalage du dernier écrit (L13). Triangles
  étirés, sans avertissement ;
- uv énorme (`vt 1e9 0`) : la boucle `while (u > tiles_u) tiles_u++` ne finit pas ;
- tout le fichier formait un seul objet : chaque tuile recevait une copie de tous les sommets, et les noms d'objets
  étaient perdus.

Deux défauts voisins, trouvés par les bancs :
- un premier objet sans `o` prenait le nom laissé par le fichier précédent (`str` statique jamais remis à zéro) ;
- ajouter le même fichier renommait `body.1001` en un autre nom de tuile (`body.1003` dans le banc) :
  `_import_mesh_unique_name()` incrémente le dernier nombre du nom.

**Correctif.**
- `iron_udim.c`, sans dépendance : la tuile (u, v) couvre [u, u + 1] × [v, v + 1], son id est 1001 + u + 10 v ; les
  importeurs acceptent u de 0 à 9 et v de 0 à 98 (tuiles 1001-1990). `udim_split_triangle()` découpe un triangle
  le long des bords de tuile (Sutherland-Hodgman dans chaque case couverte, puis éventail) et interpole positions,
  normales (renormalisées), uv, second jeu d'uv et couleurs. Une uv à moins de `UDIM_EPS` (1e-4) d'un bord reste dans
  la tuile de son triangle ; un triangle sans aire en uv garde sa géométrie, dans la tuile de son milieu.
- `obj_parse(…, udim)` découpe chaque triangle (`obj_udim_cut_triangles`) : les coins coupés sont ajoutés aux
  tableaux temporaires le temps du packing, puis retirés (les indices des parts suivantes restent ceux du fichier).
  Les uv hors des tuiles sont comptées (`obj_udim_invalid`, première uv dans `obj_udim_invalid_uv`), leurs triangles
  écartés. Le split par objet (`o`) vaut aussi en UDIM.
- `import_obj_parse()` en UDIM (`_import_obj_udim`) : un objet par objet du fichier et par tuile
  (`<objet>.<tuile>`), avec ses seuls sommets. Une uv hors des tuiles, ou plus de tuiles que l'atlas des calques
  n'en tient (64), fait refuser le fichier (« UDIM import refused: … ») : rien n'est importé et le projet garde son
  maillage (`import_mesh_run` s'arrête si aucun objet n'a été ajouté). Les objets sans uv sont importés sans tuile,
  **après** les autres : un premier objet sans uv fait proposer un dépliage de tout le maillage, qui écraserait les
  uv UDIM.
- `obj_parse()` vide le nom au début de chaque fichier ; `_import_mesh_unique_name()` garde le suffixe de tuile et
  met le `.00X` devant (`body.001.1001`).
- `import_mesh_finish_import()` appelle `studio_udim_import_done(import_mesh_append)` (`external/studio_atlas.c`) :
  pixels par tuile dans la grille de l'atlas, et résolution de projet pour 2048 et 4096 px par tuile
  (`studio_udim_hint`) ; depuis le lot D1, d'abord les jeux de textures par matériau (ci-dessous).

**Lot D1 : par matériau et par tuile ensemble.** L'upstream n'a qu'un découpage à la fois : par objet, par
matériau **ou** par tuile UDIM (L5, N8 d'`ARMORPAINT_UDIM.md`).
- `SPLIT_TYPE_MATERIAL_UDIM` (3) s'ajoute à `split_type_t`. La boîte d'import garde le choix Object / Material et
  gagne une case « UDIM Tiles » à côté : Object + UDIM est l'ancien « UDIM Tile », Material + UDIM le nouveau mode.
  `plugins_split_by` suit, pour l'importeur FBX ([fbx-split](#fbx-split)).
- `_import_obj_udim(…, by_material)` lit les parts par `usemtl` au lieu de `o` : un objet `<matériau>.<tuile>` par
  matériau et par tuile, rassemblé de toutes les parts du matériau (un OBJ coupe un matériau à chaque objet et à chaque
  `v` après des faces ; Maya répète `usemtl` dans chaque objet). Les parts sont packées chacune à son échelle : leurs
  positions sont recalées sur la plus grande (`scale_pos`).
- Après un tel import (pas un ajout), `studio_udim_import_done()` crée un jeu de textures par matériau, nommé comme
  lui, avec ses tuiles (et le matériau sans uv, sans tuile). Refusé par `studio_atlas_set` (nom de plus de 64
  caractères, plus de 256 matériaux) : les jeux restent ceux d'avant, la console dit pourquoi.

L'include et les globales d'`iron_obj.c` sont placés hors des lignes d'obj-global-indices et d'obj-float-exponent
(contexte de leurs `.patch`), et le découpage juste après la dernière ligne de contexte d'obj-global-indices. Chaîne
d'`iron_obj.c` : voir [obj-float-exponent](#obj-float-exponent). Le patch couvre deux commits, et `import_obj.c` et
`import_mesh.c` portent aussi [import-null-guards](#import-null-guards) : pour le régénérer, retirer ce patch d'une
copie de chaque fichier (voir l'introduction).

**Test.** `.tests/obj.sh` (hors app, ASan/UBSan), section `udim/` :
- `udim_clean` : grille UDIM sans triangle à cheval, sortie identique octet pour octet à l'upstream ;
- 14 cas d'`obj_udim.py`, relus par `obj_test` : objets et tuiles attendus, triangles écartés et coupés, uv locales
  dans [0, 1], pas de déformation (chaque sommet produit, remis en uv globale, retombe sur un triangle d'origine à
  la position 3D que ce triangle donne à cette uv), surfaces 3D et uv conservées. L'upstream échoue sur les 8 cas de
  bug : `neg` (plantage), `u10` (tuiles 1010 et 1011), `straddle`, `straddle_v`, `straddle4`, `long` (une seule
  tuile au lieu de 2 à 4), `huge` (boucle sans fin), `shared_vt` (sommet déformé) ;
- fuzz : les mutations sont aussi lues en UDIM par l'arbre courant. Au 2026-09-27, 1 000 mutations : **0 plantage**
  sur 3 000 lectures, contre 1 393 pour l'upstream sur 2 000.

Dans l'app, `.tests/udim_app.sh` (OBJ) : `body.1001`, `body.1002`, `body.1011` et `cap` sans tuile ; un triangle
coupé, signalé ; `cap` sans uv en tête du fichier : pas de dépliage proposé ; fichiers refusés (uv négatives,
70 tuiles) : message, maillage gardé ; le même fichier ajouté : `body.001.1001` ; conseil de résolution. Au
2026-09-27 : tout passe. Pendant le développement, ce banc a trouvé un use-after-free dans la boucle en deux passes
de `_import_obj_udim` (part libérée sans vider sa case), corrigé avant le commit.

Lot D1, `.tests/udim_app.sh` section 7 (OBJ et FBX) : `skin.1001`, `skin.1002`, `skin.1011` et `cloth.1002`, un
jeu de textures par matériau, un triangle coupé ; positions recalées et surfaces uv conservées par matériau, relues
par un export OBJ du maillage (`script_export_mesh`) ; témoin : le même fichier par objet et par tuile, sans jeu.
`.tests/sets_app.sh` section 6 : `udim_cube.obj` par matériau et par tuile, jeux body (1001, 1002, 1003, 1011) et
cap (1012, 1013). Au 2026-09-27 : tout passe. Mutation : sans le recalage d'échelle, le contrôle des positions échoue
(positions × 2,2).

**Signes que l'upstream l'a corrigé.** `get_tile()` disparaît ou arrondit par `floorf`, `num[tile]` est borné, ou
`reading_object` n'est plus conditionné par `!udim`. Rejouer la section `udim/` d'`.tests/obj.sh` contre l'upstream
sans le patch : les cas de bug doivent passer.

**Non couvert.**
- Par matériau et par tuile, un matériau reste un objet par tuile (`skin.1001`, `skin.1002`), comme en D0 : le
  regroupement en un objet à plages d'index est reporté au lot D2 (`ARMORPAINT_UDIM.md` §7).
- Un ajout (append) ne crée pas de jeu de textures : `atlas_objects` ne couvre plus les objets ajoutés, et les jeux
  sont ignorés (tout dans « Atlas 1 ») tant qu'on ne les redéfinit pas, comme pour tout ajout upstream.
- L'identité de la tuile vient toujours du nom de l'objet (L4) : le renommage dans l'onglet Meshes la garde depuis le
  lot D1 ([udim-tile-name](#udim-tile-name)), un nom `truc.1005` hors UDIM reste une tuile.
- Les uv vues par les matériaux : corrigées au lot D1 (L8, [udim-material-uv](#udim-material-uv)).
- glTF / GLB : pas de split UDIM, comme l'upstream. FBX : voir [fbx-split](#fbx-split).
- Un triangle coupé devient plusieurs triangles : le nombre de sommets exporté par `--export-mesh` change.

---

## fbx-split

| | |
|---|---|
| Fichier | `paint/plugins/io_fbx/io_fbx.c` (`io_fbx_parse`, splits par matériau et par tuile) |
| Commit studio | `de0ba0c5` (lot D0) et `16ac7cb8` (lot D1) sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-27) |
| Trouvé | 2026-09-26, analyse des UDIM (`ARMORPAINT_UDIM.md` §3, L14), puis banc `.tests/fbx.sh` |

**Bug.** Dans l'importeur FBX :
- split par matériau plafonné à 64 matériaux (`MAT_SPLIT_MAX`), split UDIM à 100 tuiles (`MAX_UDIM_TILES`) : les
  faces au-delà étaient perdues sans message ;
- en UDIM, tous les nœuds d'une tuile étaient fusionnés en un seul mesh, la tuile d'un triangle était celle du
  barycentre de ses uv convertie par `(int)` (mêmes défauts que l'OBJ : u = -0,5 dans 1001, u ≥ 10 dans la rangée
  suivante, triangle à cheval étiré) ;
- un nœud dont seules certaines faces ont des uv faisait lire le tableau d'uv au-delà de sa fin ; le second jeu
  d'uv et les couleurs n'étaient pas repris par les splits ;
- un fichier sans aucun triangle faisait lire le tableau des meshes au-delà de sa fin.

**Correctif.**
- Tableaux des splits alloués à la demande, sans plafond.
- UDIM : un mesh par nœud et par tuile (`<nœud>.<tuile>`), triangles à cheval coupés par `iron_udim.c` (voir
  [udim-import](#udim-import)). Une uv hors des tuiles 1001-1990, ou plus de 64 tuiles, fait refuser le fichier avec
  un message (`console_error`) ; `io_fbx_parse()` rend alors NULL, et `import_mesh_run()` garde ce message
  ([import-null-guards](#import-null-guards)). Les nœuds sans uv gardent leur mesh, sans tuile, après les autres
  (pas de dépliage proposé).
- Uv pour toutes les faces ou aucune (`mixed_uvs`), second jeu d'uv et couleurs repris comme dans le split par objet.
- Aucun triangle : NULL au lieu d'une lecture hors du tableau.

Lot D1 : par matériau et par tuile (`plugins_split_by` 3, voir [udim-import](#udim-import)), un mesh par matériau et
par tuile, `<matériau>.<tuile>`, rassemblé d'un nœud à l'autre (`udim_material_tiles`, table des matériaux gardée
pour tout le fichier) ; une face sans matériau donne ses tuiles sous le nom de son nœud, un nœud sans uv garde son mesh.

Sans UDIM ni bug connu, la sortie est identique à l'upstream, octet pour octet. Pour régénérer : retirer ce patch d'une
copie d'`io_fbx.c` (voir l'introduction) ; le hunk `ufbx_load_memory` d'`io_fbx_parse` appartient à
import-null-guards.

**Test.** `.tests/fbx.sh` (hors app, ASan/UBSan ; `fbx_test.c` lit le FBX comme `import_mesh_run`) :
- `same/` : 8 fichiers (nœuds avec et sans uv, uv mêlées, second jeu d'uv et couleurs, uv hors [0, 1], jusqu'à
  64 matériaux), splits par objet et par matériau : sortie identique à l'upstream (15 comparaisons) ;
- `udim/` : 21 cas de `fbx_udim.py` (ceux de l'OBJ sauf `shared_vt`, plus `tiles110`, uv mêlées, nœud sans uv en
  tête, second jeu d'uv et couleurs, fichier vide, 70 matériaux) ; l'upstream échoue sur 16 d'entre eux (plantages,
  tuiles fausses, nœuds fusionnés, 100 tuiles sur 110, 64 meshes sur 70) ;
- fuzz : au 2026-09-27, 300 mutations lues en split 0, 1 et 2 : **0 plantage** sur 900 lectures, contre 508 pour
  l'upstream ;
- lot D1 : 3 cas par matériau et par tuile (`mat_udim` : deux nœuds, deux matériaux, un triangle à cheval ;
  `mat_udim_mixed` : nœud sans matériau, nœud sans uv en tête ; `mat_udim_neg` : refus), mêmes vérifications que
  l'UDIM, par matériau ; le fuzz lit aussi le split 3. Mutation (table des matériaux remise à zéro à chaque nœud) :
  `mat_udim` échoue (meshes `skin.1001` et `cloth.1002` en double).

Dans l'app, `.tests/udim_app.sh` (FBX) : `body.1001`, `body.1002`, `body.1011`, `cap` sans tuile, un triangle coupé ;
`cap` sans uv en tête : pas de dépliage proposé ; uv négatives : refusé, son message reste, maillage gardé.

**Signes que l'upstream l'a corrigé.** `MAT_SPLIT_MAX` ou `MAX_UDIM_TILES` disparaissent, ou le split UDIM ne
fusionne plus les nœuds. Rejouer `.tests/fbx.sh` contre l'upstream sans le patch.

**Non couvert.**
- Mêmes limites que l'OBJ ([udim-import](#udim-import)) : un objet par matériau (ou par nœud) et par tuile.
- Seul le premier jeu d'uv décide de la tuile ; le second est interpolé sans être découpé à ses propres bords.

---

## udim-tile-limit

| | |
|---|---|
| Fichiers | `paint/sources/util/util_mesh.c` (`_util_mesh_udim_build_tiles`, `util_mesh_udim_slot`, `util_mesh_atlas_slot`, `util_mesh_build_merged_data`), `paint/sources/uniforms.c` (`_atlas_transform`), `paint/sources/util/util_layer.c` (`layers_udim_tile_slot`, `layers_flatten`), `paint/sources/io/export_texture.c` (`export_texture_run_layers`) |
| Commit studio | `a34a70d5` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-27) |
| Trouvé | 2026-09-26, analyse des UDIM (`ARMORPAINT_UDIM.md` §3, L3) |

**Bug.** Les textures de calque tiennent au plus 64 tuiles (grille 8 × 8). Au-delà, l'upstream gardait les 64
premières tuiles rencontrées et donnait aux autres la case 63, sans message : leur peinture recouvrait la 64e tuile,
et l'export par tuile la réécrivait sous leurs noms. Un objet sans tuile, dans un maillage UDIM, recevait la case 0 :
peindre dessus peignait la 1001.

**Correctif.**
- Les 64 plus petites tuiles ont une case, les autres aucune (`util_mesh_udim_slot` rend -1), avec un message
  (« UDIM: 70 tiles, the layers hold 64: the tiles from 1065 on are not painted nor exported. ») ; dans un maillage
  à plusieurs tuiles, les objets sans tuile sont signalés aussi. Un message par changement de maillage, pas à chaque
  reconstruction.
- Un objet sans case a toutes ses uv sur un texel hors de l'atlas (`util_mesh_build_merged_data`, et
  `_atlas_transform` pour le rendu par objet) : rien n'est peint, aucune tuile touchée.
- `layers_udim_tile_slot()` rend -2 pour un calque de tuile sans case : `layers_flatten()` le saute.
- Export par tuile : pas de fichier pour une tuile sans case, avec un message ; muet pendant la passe à blanc du
  préflight studio (`studio_export_planning()`, qui signale déjà ces tuiles et bloque l'export).

Pour régénérer : `git diff a34a70d5^ a34a70d5`.

**Test.** `.tests/udim_app.sh`, section « 70 tuiles » (import par objet de `p.1001` … `p.1070`) : message ; les 64
premières dans l'atlas, chacune dans sa case, `p.1065` à `p.1070` hors de l'atlas ; peindre `p.1065` ne touche aucune
tuile (l'upstream peignait la case de la 1064) ; témoin : peindre `p.1063` peint la 1063, elle seule ; export par
tuile : 1001 à 1064, 5 maps chacune, 1065 à 1070 écartées avec un message ; preset studio : bloqué par le préflight,
rien d'écrit. Au 2026-09-27 : tout passe.

**Signes que l'upstream l'a corrigé.** `util_mesh_udim_tiles_spent` disparaît, ou `util_mesh_udim_slot()` ne rend
plus `ATLAS_MAX_SLOTS - 1`. Rejouer la section « 70 tuiles » d'`.tests/udim_app.sh` sur l'upstream sans le patch.

**Non couvert.**
- Les tuiles au-delà de 64 ne sont pas peignables : la limite elle-même disparaît avec la phase D2 (un jeu de
  textures par tuile).
- Un calque de tuile sans case est gardé dans le projet mais n'apparaît dans aucun export.

---

## arm-relative-paths

| | |
|---|---|
| Fichiers | `paint/sources/io/export_arm.c` (`export_arm_run_project`), `base/sources/iron_path.c` (`path_to_relative`) |
| Commit studio | `209bae06` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-27) |
| Trouvé | 2026-09-27, banc du publish (`.tests/publish_app.sh`, lot C4) : la copie publiée désignait sa texture par `../../work/armorpaint/../../…` |

**Bug.** Deux défauts dans les chemins des fichiers d'un `.arm` enregistré (maillage source, textures importées,
envmap) :
- `export_arm_run_project()` écrit dans le fichier les chemins du maillage source et de l'envmap relatifs au `.arm`
  en les mettant à la place de ceux du projet ouvert (`g_project->mesh_assets`, `g_project->envmap`), qui ne
  redeviennent jamais absolus. Après un premier enregistrement, un Save As dans un autre dossier écrit des chemins
  relatifs au premier dossier (`../src/robot.obj` depuis `b/deep/`, au lieu de `../../src/robot.obj`). Reimport Mesh
  (`project_reimport_mesh`, qui teste `iron_file_exists` sur ce chemin relatif) ouvre alors le navigateur de fichiers
  au lieu de réimporter. Les textures ne sont pas touchées : leurs chemins sont recalculés à chaque enregistrement
  depuis `g_project->_->assets`, qui reste absolu.
- un projet ouvert garde des chemins qui passent par son dossier : `import_arm_run_project()` les recompose en
  `<dossier du .arm>/<chemin relatif>` sans les normaliser (`/w/a/../src/wood.png`), et `path_to_relative()` compare
  les éléments un à un. Enregistré dans `c/`, le projet désigne `../a/../src/wood.png`, qui ne se résout plus si `a/`
  manque : dossier renommé, ou dossier de travail absent chez qui ouvre une copie publiée. Résultat : « Could not
  locate texture », textures roses.

**Correctif.**
- `export_arm_run_project()` garde les chemins absolus du maillage et de l'envmap avant d'y mettre les chemins
  relatifs, et les remet une fois le fichier écrit.
- `path_to_relative()` normalise les deux chemins (`path_normalize`, qui retire les `dossier/..`) avant de les
  comparer. Seul `export_arm.c` l'appelle.

Trois patches touchent `export_arm_run_project()` : [project-copy](#project-copy) (`relative_to`, plus haut),
[project-entries](#project-entries) (juste avant `util_encode_project()`) et celui-ci, indépendants. Pour régénérer celui-ci : `git diff 209bae06^ 209bae06 -- base/sources/iron_path.c
paint/sources/io/export_arm.c` (ou le commit qui l'a refait).

**Test.** `.tests/arm_paths_app.sh` : ArmorPaint en `--background`, chemins lus dans les `.arm` par
`.tests/arm_paths.py`. Un projet avec maillage, texture et envmap pris dans `src/` est enregistré dans `a/` puis dans
`b/deep/` pendant la même session : chaque fichier a des chemins relatifs à son dossier vers `src/`, et l'envmap
reste absolue en mémoire. `a/p.arm` rouvert et enregistré dans `c/` : chemins vers `src/` sans passer par `a/`. Avec
`a/` renommé, `c/r.arm` rouvert retrouve texture et envmap. Au 2026-09-27, tout passe ; sans le patch, 4 contrôles
sur 5 échouent (`../src/robot.obj` depuis `b/deep/`, envmap relative en mémoire, `../a/../src/wood.png`, « Could not
locate texture »). La copie publiée en dépend aussi : `.tests/publish_app.sh`, étape 3.

**Signes que l'upstream l'a corrigé.** `export_arm_run_project()` ne remplace plus `g_project->mesh_assets` et
`g_project->envmap` (ou les remet), `import_arm_run_project()` normalise les chemins qu'il recompose, ou
`path_to_relative()` normalise lui-même. Rejouer `.tests/arm_paths_app.sh` sur l'upstream sans le patch.

**Non couvert.**
- Reimport Mesh : déduit du code de `project_reimport_mesh`, pas joué par le banc (pas accessible par script).
- En mémoire, les chemins du projet ouvert gardent leurs `dossier/..` (upstream) ; seuls les chemins écrits sont
  normalisés.
- `path_normalize()` ne retire que les `dossier/..` : ni les `./`, ni les liens symboliques.
- Fichier sur un autre disque que le projet (Windows) : écrit en absolu, comme l'upstream. Testé sur macOS
  uniquement.

---

## project-copy

| | |
|---|---|
| Fichier | `paint/sources/io/export_arm.c` (`export_arm_run_project`) |
| Commit studio | `1256a538` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-27) |
| Injection | 6 lignes sous `#ifdef WITH_EXTERNAL` : pendant une copie, `relative_to` est le chemin écrit — l'app compile toujours sans `external/` |

**Motif.** Le publish du lot C4 enregistre dans ShotGrid une copie du projet, placée dans le dossier du template de
publish (`tk-armorpaint/hooks/tk-multi-publish2/basic/publish_session.py`). Un `.arm` désigne ses fichiers (textures
importées, maillage source, polices, sons, envmap) par des chemins relatifs à son dossier : copié tel quel ailleurs,
il ne les trouve plus (« Could not locate texture »). ArmorPaint écrit donc la copie lui-même (requête host
`publish_copy`, `external/studio_publish.c`), mais `export_arm_run_project(path)` calcule ces chemins depuis le projet
ouvert (`g_project->_->filepath`), pas depuis `path`.

**Correctif.** Avec `WITH_EXTERNAL`, `relative_to` devient `path` pendant l'écriture d'une copie
(`studio_project_copy()`, vrai le temps de l'appel dans `publish_copy`) : les chemins sont relatifs à la copie. Le
projet ouvert ne change pas : `export_arm_run_project()` s'arrête avant les fichiers récents quand `path` n'est pas
le projet ouvert.

Pour régénérer : `git diff 1256a538^ 1256a538 -- paint/sources/io/export_arm.c` (un seul hunk ; ceux
d'[arm-relative-paths](#arm-relative-paths) et de [project-entries](#project-entries), plus bas dans la même
fonction, sont dans leurs propres `.patch`).

**Test.** `.tests/publish_app.sh`, étape 3. Le site simulé range `work/armorpaint/` et `publish/armorpaint/` à la
même profondeur, comme tk-config-default2 : un fichier rangé ailleurs (`src/`) a le même chemin relatif depuis les
deux dossiers, et ne distingue pas le patch. Le banc importe donc aussi une texture rangée dans le dossier de travail
(`decal.png`). Sur la copie publiée, `arm_paths.py` vérifie que chaque chemin est relatif, normalisé et mène au
fichier attendu : `../../work/armorpaint/decal.png` pour celle-ci. Rouverte, la copie trouve toutes ses textures ;
avec `work/` renommé, texture et envmap sont trouvées dans `src/`, et seule `decal.png` manque ; témoin : une simple
copie du `.arm` perd sa texture. Au 2026-09-27, tout passe (19 contrôles). Sans le patch, la copie désigne
`decal.png` dans `publish/`, et deux contrôles de l'étape 3 échouent (chemins, réouverture).

**Signes que l'upstream l'a changé.** `relative_to` n'est plus calculé depuis `g_project->_->filepath`, ou
`export_arm_run_project()` prend le dossier de référence en paramètre : le patch devient inutile. Un conflit sur ces
lignes : refaire l'injection juste avant les appels `export_arm_*_to_files(relative_to, …)`.

**Non couvert.** La copie est écrite depuis l'état en mémoire, juste après l'enregistrement du projet par le hook : un
changement entre les deux (impossible pendant le publish, qui tient ArmorPaint) ne serait pas dans le fichier de
travail.

---

## encode-image-overflow

| | |
|---|---|
| Fichier | `base/sources/iron_image.c` (`_encode_image`, `_encode_image_func`) |
| Commit studio | `6f789b6f` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-27) |
| Trouvé | 2026-09-27, banc du lot A4 (`.tests/ocio_import_app.sh`, étape 3) : un `.arm` enregistré avec Pack Assets plantait à la réouverture (« MTLTextureDescriptor has width of zero ») |
| Constaté depuis | `f4f90952` (2024-11-10, arrivée d'armorcore), toujours présent en `85f6cf1c` |

**Bug.** `_encode_image()`, derrière `iron_encode_png()` et `iron_encode_jpg()`, donne à stb_image_write un tampon
de `w * h * 4` octets, et son rappel `_encode_image_func()` y ajoute chaque morceau du fichier sans borne. Le fichier
encodé dépasse souvent la taille des pixels : petite image (signature et chunks PNG, environ 70 octets pour les 4
octets d'un pixel), image bruitée (un octet de filtre par rangée, deflate qui ne réduit rien), en-têtes JPEG. Le
rappel écrit alors au-delà du bloc du tas. Appelants : Pack Assets (`export_arm_pack_assets`, qui redessine chaque
texture du projet en RGBA 8 bits et l'encode en PNG, en JPEG pour un `.jpg`), la capture du viewport rangée dans
l'onglet Textures (`viewport_save_texture`) et `util_texture_capture_output`. Sur le banc, les PNG du paquet sortaient
corrompus : stb les refusait à la réouverture et une texture de largeur nulle partait vers Metal (plantage).
Ailleurs, la corruption du tas peut rester muette.

**Correctif.** Le rappel connaît la capacité du tampon (`_encode_capacity`, en `size_t` : une texture 16K bruitée
dépasse 1 Go une fois encodée) et la double par `realloc` quand un morceau ne tient pas. Le tampon rendu reste
`_encode_data` : les appelants le libèrent comme avant.

Pour régénérer : `git diff 6f789b6f^ 6f789b6f -- base/sources/iron_image.c`.

**Test.** `.tests/export.sh` : `image_test` encode du bruit en 1 × 1, 8 × 4, 16 × 16 et 64 × 64, relit chaque PNG
avec libpng (pixels identiques) et vérifie les marqueurs de début et de fin du JPEG, sous ASan : sans le patch, ASan
signale le débordement dans `_encode_image_func`. `.tests/ocio_import_app.sh`, étape 3 : le `.arm` enregistré avec
Pack Assets se rouvre ailleurs, sans ses fichiers. Au 2026-09-27, tout passe.

**Signes que l'upstream l'a corrigé.** `_encode_image_func()` borne ou agrandit son tampon, ou `_encode_image()`
le dimensionne autrement que `w * h * 4` (`stbi_write_png_to_mem`, par exemple) : rejouer `image_test` sans le
patch.

**Non couvert.** Un `realloc` qui échoue n'est pas traité (le `malloc` upstream ne l'est pas non plus).
`_encode_size` reste un `int` : au-delà de 2 Go de fichier, rien n'est borné (hors de portée d'une texture ≤ 16K
RGBA 8 bits).

---

## ocio-import

| | |
|---|---|
| Fichiers | `base/sources/iron.h` (`iron_texture_decoded`, `gpu_create_texture_from_bytes`, `gpu_create_texture_from_encoded_bytes`), `paint/sources/io/import_texture.c` (`import_texture_run`), `paint/sources/ui/tab_textures.c` (`tab_textures_draw`, `tab_textures_draw_context_menu`) |
| Commit studio | `2e532915` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-27) |
| Injection | `iron.h` : un pointeur de fonction nul par défaut et deux appels (10 lignes, sans dépendance à `external/`) ; le reste en 6 blocs sous `#ifdef WITH_EXTERNAL` (25 lignes) — l'app compile toujours sans `external/` |

**Motif.** Le lot A4 interprète chaque texture importée dans son espace couleur source (sRGB, ACEScg, donnée…) et
convertit ses pixels vers l'espace de travail OCIO. ArmorPaint décode les images dans
`gpu_create_texture_from_encoded_bytes()` (PNG, JPEG, HDR, K… par iron) ou les reçoit décodées d'un importeur de
plugin par `gpu_create_texture_from_bytes()` (EXR studio, PSD, TIFF), puis les envoie aussitôt au GPU : aucun plugin
ne voit les pixels avant l'envoi, ni ne sait à quelle texture importée ils appartiennent.

**Correctif.**
- `iron.h` : `iron_texture_decoded`, pointeur de fonction nul par défaut, reçoit les pixels, la taille, le format et
  la source (chemin ou extension passés à `gpu_create_texture_from_encoded_bytes()`, NULL pour des octets bruts)
  juste avant `gpu_texture_init_from_bytes()`. Sans plugin, rien ne change.
- `import_texture.c` : `import_texture_run()` passe l'importeur (celui d'iron ou d'un plugin) à
  `studio_texture_import(path, fn)`, qui l'appelle dans une portée : les pixels décodés pendant l'appel sont ceux de
  la texture `path`. Tous les imports de texture passent par là : import d'asset, de dossier, projet rouvert,
  matériaux et brosses d'un `.arm`, Reimport, calques d'un PSD, captures.
- L'espace de chaque texture est écrit dans l'entrée `studio_color_spaces.json` du paquet d'assets du `.arm` par
  [project-entries](#project-entries) : le hunk de `export_arm.c` d'origine (lot A4) a été remplacé par ce point
  d'injection commun au lot A5.
- `tab_textures.c` : l'infobulle d'une texture donne son espace et sa source ; « Color Space... » dans le menu
  contextuel ouvre le choix de l'espace.

`import_texture_run()` porte aussi [import-null-guards](#import-null-guards). Pour régénérer celui-ci :
`git diff 2e532915^ 2e532915 -- base/sources/iron.h paint/sources/io/import_texture.c paint/sources/ui/tab_textures.c`
(ou le commit qui l'a refait).

**Test.** `.tests/ocio_import_app.sh` : ArmorPaint en `--background`, pixels relus par `studio_texture_dump` et
comparés à PyOpenColorIO par `ocio_import_check.py` : PNG, TIFF, PSD à plat et à calques en 8 bits, EXR float et
half (file_rule), `.hdr`, donnée reconnue à son nom ; puis choix par texture, vignette du navigateur de fichiers,
`.arm` rouvert, paquet d'assets sans les fichiers, `STUDIO_OCIO_IMPORT=0`. Au 2026-09-27, les 31 contrôles passent.
Le plugin référence `iron_texture_decoded` : sans le patch, l'app ne se lie pas.

**Signes que l'upstream l'a changé.** Un conflit dans `gpu_create_texture_from_bytes()` ou
`gpu_create_texture_from_encoded_bytes()` (chargement des textures remanié : garder l'appel juste avant l'envoi au
GPU, après le décodage) ; une autre signature d'importeur dans `import_texture_run()` ; une gestion couleur upstream
des textures importées (espace par texture, OCIO) : comparer au lot A4 et retirer ce qui fait double emploi.

**Non couvert.**
- Une texture déjà dans le cache d'ArmorPaint n'est pas décodée à nouveau : une capture du viewport, par exemple,
  garde ses pixels. Une texture décodée hors import (vignette du navigateur de fichiers) n'est pas convertie ; un
  import qui la trouve en cache la décode à nouveau.
- Les textures des matériaux ou brosses importés d'un autre `.arm` sont résolues comme un import : les choix
  enregistrés dans cet autre `.arm` ne sont pas lus.

---

## image-texture-color-space

| | |
|---|---|
| Fichier | `paint/sources/nodes_material/image_texture_node.c` (`parser_material_texture_store`) |
| Commit studio | `92962b40` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-27) |
| Trouvé | 2026-09-27, lot A4 (espaces des textures importées face à l'option du nœud), confirmé par le banc |
| Constaté depuis | `7c993830` (2023-02-08, `MaterialParser.hx`), toujours présent en `85f6cf1c` |
| Injection | 2 exposants échangés ; le mode Auto en 2 blocs sous `#ifdef WITH_EXTERNAL` (8 lignes, dépend du lot A4) |

**Bug.** Le nœud Image Texture a une option d'espace couleur (Auto, Linear, sRGB, DirectX Normal Map). Dans
`parser_material_texture_store()`, chaque branche fait l'inverse de son commentaire : Linear sur la couleur de base
(« Linear to sRGB ») applique `pow(x, 2.2)`, et sRGB sur une autre entrée (« sRGB to linear ») `pow(x, 1/2.2)`. Or
ArmorPaint garde la couleur de base encodée gamma 2.2 (décodée au rendu) et les autres canaux linéaires : un gris
linéaire 0,5 en Linear sur la couleur de base donnait 55/255 au lieu de 186/255 (décodé deux fois), une rugosité
sRGB 0,5 donnait 186/255 au lieu de 55/255.

**Correctif.** Les exposants sont échangés : Linear sur la couleur de base encode (`pow(x, 1/2.2)`), sRGB ailleurs
décode (`pow(x, 2.2)`). Avec `WITH_EXTERNAL`, Auto sur une texture qui garde des couleurs linéaires (flottante,
d'un espace couleur, convertie par le lot A4 : `studio_texture_is_linear()`) vaut Linear : une texture EXR de
couleur de base n'a plus besoin de l'option.

Changement de comportement : un projet qui règle Linear ou sRGB sur un nœud Image Texture rend désormais la
conversion annoncée ; un réglage choisi pour compenser l'inversion est à revoir.

Pour régénérer : `git diff 92962b40^ 92962b40 -- paint/sources/nodes_material/image_texture_node.c`.

**Test.** `.tests/ocio_import_app.sh`, section 5 : un gris linéaire 0,5 (`.hdr`) en couleur de base (Linear, puis
Auto) et en rugosité (sRGB), calque de remplissage, export des valeurs d'ArmorPaint telles quelles (EXR 32 bits,
espace raw) mesuré sur les texels couverts par les UV : 186/255 en couleur de base, 55 ou 56/255 en rugosité. Au
2026-09-27, tout passe ; sans le patch, les trois contrôles échouent (55, 186 et 128/255).

**Signes que l'upstream l'a corrigé.** Les exposants des deux branches échangés en amont, ou l'option d'espace
remaniée : rejouer la section 5 sans le patch.

**Non couvert.** Seule la conversion gamma 2.2 d'ArmorPaint est corrigée : l'option ne passe pas par OCIO (pour une
texture importée, c'est l'espace choisi par texture, lot A4, qui convertit). Sans le lot A4 (`STUDIO_OCIO_IMPORT=0`
ou build sans `external/`), Auto ne convertit jamais, comme l'upstream.

---

## preferences-tab

| | |
|---|---|
| Fichier | `paint/sources/ui/box_preferences.c` (`box_preferences_add_tab`, `box_preferences_show_box`) |
| Commit studio | `f3c88ab3` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-27) |
| Injection | 22 lignes, point d'injection générique, sans dépendance à `external/` (compile sans `WITH_EXTERNAL`) |

**Motif.** La boîte des préférences d'ArmorPaint a ses onglets codés en dur (un `ui_tab` par onglet dans
`box_preferences_show_box()`, `preferences_tab_t` dans `enums.h`). Aucun plugin ne peut y ajouter un onglet. Le lot
A5 y veut l'onglet Color : configuration OCIO, espace de travail des nouveaux projets.

**Correctif.** `i32 box_preferences_add_tab(char *label, void (*draw)(void))` enregistre au plus 4 onglets,
affichés après Plugins. Elle rend l'index de l'onglet, à mettre dans `box_preferences_tab` avant
`box_preferences_show()` pour l'ouvrir (il suit `PREFERENCES_TAB_PLUGINS`, le dernier de l'enum), ou -1 quand c'est
plein. `draw` dessine le contenu avec les widgets d'iron, comme un onglet upstream. Le plugin déclare le prototype
lui-même (`external/studio_ocio_prefs.c`) : aucun header upstream n'est touché.

**Test.** À l'écran : Color > Color Preferences... (ou `studio_ocio_prefs_show()` en script) ouvre les préférences
sur l'onglet Color, après Plugins ; vérifié le 2026-09-27 par une capture de la fenêtre. Sans appel à
`box_preferences_add_tab`, la boîte est identique à l'upstream. Les réglages de l'onglet sont testés par leurs
fonctions, sans l'interface : `.tests/ocio_prefs_app.sh`.

**Signes que l'upstream l'a corrigé.** Une API d'onglets de préférences pour les plugins, ou `preferences_tab_t`
qui gagne une valeur après `PLUGINS` (ou un onglet conditionnel avant Plugins) : dans ce dernier cas, **l'index
des onglets ajoutés entre en collision** avec le nouveau, et il faut refaire le patch. Si une API existe, y porter
`studio_ocio_prefs_init()` et retirer le patch.

**Non couvert.** Rien de plus que l'upstream : un onglet ajouté a la taille de la boîte et défile comme les autres.

---

## project-entries

| | |
|---|---|
| Fichier | `paint/sources/io/export_arm.c` (`export_arm_run_project`) |
| Commit studio | `b7fba27a` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-27) |
| Injection | un bloc de 4 lignes sous `#ifdef WITH_EXTERNAL`, juste avant `util_encode_project()` |

**Motif.** Des modules du studio gardent des données d'un projet dans son `.arm` : l'espace couleur de chaque
texture (lot A4), l'espace de travail du projet (lot A5), le contexte ShotGrid ensuite (lot C5). Le format `.arm`
n'a pas de champ libre ; le paquet d'assets (`packed_assets`) accepte n'importe quel fichier, et ArmorPaint sans
studio le relit et le réécrit tel quel. Le lot A4 y écrivait son entrée par un hunk à lui ; un hunk par module
aurait multiplié les injections dans la même fonction.

**Correctif.** Un seul appel, `studio_project_save_entries(texture_files)` (`external/studio_project.c`), juste
avant `util_encode_project()` : il détecte d'abord un projet ouvert ou remplacé depuis la dernière frame, puis
appelle les modules inscrits par `studio_project_on_save()`, qui écrivent leur entrée par
`studio_project_set_entry(name, text)`. Remplace le hunk d'[ocio-import](#ocio-import) : `studio_color_spaces.json`
passe aussi par là.

Trois patches touchent `export_arm_run_project()` : [project-copy](#project-copy),
[arm-relative-paths](#arm-relative-paths) et celui-ci (un hunk juste avant `util_encode_project()`), indépendants.
`git diff upstream studio -- paint/sources/io/export_arm.c` les mêle, et le commit `b7fba27a` montre le
remplacement du hunk d'ocio-import. Pour régénérer celui-ci : retirer le bloc `STUDIO-PATCH(project-entries)` (et
la ligne vide qui le suit) d'une copie du fichier, placer la copie en `a/paint/sources/io/export_arm.c` et le
fichier en `b/paint/sources/io/export_arm.c`, puis `git diff --no-index --no-prefix a/paint/sources/io/export_arm.c
b/paint/sources/io/export_arm.c`.

**Test.** `.tests/ocio_import_app.sh` (entrée `studio_color_spaces.json` écrite, relue, gardée avec
`STUDIO_OCIO_IMPORT=0`) et `.tests/ocio_prefs_app.sh` (entrée `studio_ocio.json` écrite à l'enregistrement, absente
sans OCIO, relue à l'ouverture, dans un `.arm` à paquet d'assets). Au 2026-09-27, tout passe. Sans le patch,
aucune entrée n'est écrite et ces contrôles échouent.

**Signes que l'upstream l'a changé.** Un conflit dans `export_arm_run_project()` (enregistrement remanié : garder
l'appel après le remplissage de `packed_assets` et avant l'encodage), ou un format `.arm` avec des métadonnées
libres : y porter les entrées et retirer le patch.

**Non couvert.** L'export d'un matériau ou d'une brosse seuls (`export_arm_run_material`, `_brush`) n'écrit pas
d'entrée : ils ne portent pas l'état du projet.

---

## udim-core

| | |
|---|---|
| Fichiers | `paint/sources/util/util_udim.c` et `.h` (nouveaux), `paint/sources/global.h` (include), `paint/sources/main.c` (unity build) |
| Commit studio | `d2bd0efe` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-27) |
| Injection | deux lignes d'include ; le reste dans un fichier que l'upstream n'a pas |

**Motif.** Les lots D1 à D4 (UDIM natifs, `ARMORPAINT_UDIM.md` §7) ajoutent du code de cœur : il doit compiler dans
le unity build avec le reste de `util/`, donc pas dans `external/`. `ARMORPAINT_UDIM.md` §8 : la logique va dans un
fichier à nous, `util_udim.c`, et les fichiers upstream ne gardent que les points d'appel. Ce patch n'ajoute que le
fichier et ses deux includes ; les points d'appel ont chacun leur patch ([udim-material-uv](#udim-material-uv) au
lot D1).

**Contenu (lot D1).**
- `util_udim_tile_uv(object)` : (tuile u, -tuile v) d'un objet de tuile, pour les uv matériau d'un calque de tuile ;
  `util_udim_offset(object)` : le même décalage, relatif à la case de l'objet dans l'atlas, pour les calques
  partagés. Zéro pour le maillage fusionné, le plan 2D, un objet sans tuile, et (offset) une tuile sans case.
- `util_udim_draw_paint_meshes()` : dessine le contexte « paint ». Les calques UDIM partagés (et limités à un jeu)
  sont peints objet par objet, chacun avec son décalage, au lieu du maillage fusionné. Le bake et les outils qui
  pointent (Picker, ColorID, Material, Cursor) gardent le maillage fusionné, dont leurs shaders lisent les calques en
  espace atlas ; un filtre de calque (un seul objet affiché) passe par le dessin upstream.

Le commentaire d'en-tête d'`util_udim.h` pose la règle du lot D1 : un maillage UDIM garde un objet de peinture par
tuile, nommé `<objet>.<tuile>` comme dans le format upstream, pour que le `.arm` reste lisible par ArmorPaint et par
les builds studio précédents. Pour régénérer : `git diff upstream studio -- paint/sources/util/util_udim.c
paint/sources/util/util_udim.h paint/sources/global.h paint/sources/main.c` (aucun autre patch dans ces fichiers).

**Test.** Ceux des patches qui l'appellent ([udim-material-uv](#udim-material-uv)). Sans UDIM,
`util_udim_draw_paint_meshes()` appelle `render_path_draw_meshes("paint")` comme l'upstream. Au 2026-09-27, les bancs
d'app sans UDIM passent inchangés : `import_app.sh`, `export_app.sh`, `publish_app.sh`, `arm_paths_app.sh`,
`ocio_*_app.sh`, `sg_context_app.sh`, et `sets_app.sh` hors du cube UDIM.

**Signes que l'upstream l'a changé.** Un fichier `util_udim.c` upstream, ou un conflit sur les includes de
`main.c` (liste du unity build remaniée) : renommer le nôtre ou déplacer la ligne.

**Non couvert.** Rien au-delà des patches qui l'appellent.

---

## udim-material-uv

| | |
|---|---|
| Fichiers | `paint/sources/render/make_texcoord.c` (`make_texcoord_run`), `paint/sources/render/render_path_paint.c` (`render_path_paint_commands_paint`, `_particle`), `paint/sources/uniforms.c` (`uniforms_ext_vec2_link`), `paint/sources/util/util_mesh.c` (nouveau `util_mesh_merged_scope`) |
| Commit studio | `d2bd0efe` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-27) |
| Trouvé | 2026-09-26, analyse des UDIM (`ARMORPAINT_UDIM.md` §3, L8) ; reporté du lot D0 au lot D1 |

**Bug.** Sur un calque UDIM partagé, les matériaux lisent `atlas_uv × stride` : l'uv locale de la tuile plus la
colonne et la rangée de sa **case d'atlas**, pas de sa tuile. Les cases suivent l'ordre des tuiles dans une grille
carrée : pour 1001-1004 en grille 2 × 2, la 1003 reçoit le décalage (0, 1) au lieu de (2, 0). Un bruit procédural ou
un dégradé sur les uv se répète d'une tuile à l'autre, discontinu entre tuiles voisines, contrairement au DCC. Un
calque limité à une tuile lisait ses propres uv 0-1 : le même matériau n'y tombait pas au même endroit que sur le
calque partagé.

**Correctif.**
- Calques partagés : `(atlas_uv × stride + _udim_offset)`, l'uv UDIM réelle de la tuile (avec le retournement de v
  des uv stockées). L'uniforme est propre à chaque objet : le contexte « paint » dessine les objets de tuile un par
  un au lieu du maillage fusionné (`util_udim_draw_paint_meshes`, [udim-core](#udim-core) ; `util_mesh_merged_scope`
  expose les objets fusionnés).
- Calques d'une tuile : `(tex + _udim_tile)`, les mêmes uv que sur le calque partagé.
- Le bake garde l'espace atlas (il lit les textures de calque), comme les pipettes et les cartes uv, triangles et
  dilatation, qui gardent le maillage fusionné.

Pour régénérer : `git diff d2bd0efe^ d2bd0efe -- <les quatre fichiers>` ; `uniforms.c` porte aussi ocio-viewport et
udim-tile-limit, `util_mesh.c` udim-tile-limit.

**Test.** `.tests/udim_app.sh` section 6 : bruit procédural (UV Map → Noise Texture) rempli sur cinq tuiles pleines
(1001 à 1004, 1011), export par tuile, écart moyen (0-255) entre les bords :
- 1003 | 1004 (cases d'atlas non voisines) : 1,20, contre 0,99 entre deux colonnes à la même distance dans une tuile,
  et 64,00 pour le témoin (bord de 1001) ;
- 1001 / 1011 (en haut) : 1,10, contre 1,08, témoin 45,18 ;
- calque limité à 1004 contre le calque partagé : 0,14, témoin (1003) 53,44.

Au 2026-09-27 : tout passe. Mutation (décalages à zéro, le comportement upstream) : 79,83 à droite, 47,04 en haut,
51,21 pour le calque de tuile ; les trois contrôles échouent.

**Signes que l'upstream l'a corrigé.** `make_texcoord_run()` n'écrit plus `atlas_uv * constants.atlas_stride` seul
pour les calques UDIM, ou le rendu passe par tuile. Rejouer la section 6 sans le patch.

**Non couvert.**
- Vue 2D : le plan 2D n'est pas un objet de tuile, `util_udim_offset()` y rend zéro : les matériaux y gardent l'espace
  de la case, comme l'upstream (pas de banc en vue 2D).
- Matériaux sur le second jeu d'uv (`uv_map == 1`) : pas un calque UDIM pour l'upstream, inchangé.

---

## udim-export-sets

| | |
|---|---|
| Fichier | `paint/sources/io/export_texture.c` (`export_texture_run_layers`, `export_texture_run` ; nouveaux `export_texture_mask_set_objects`, `export_texture_layer_in_set`, `export_texture_run_udim_sets`) |
| Commit studio | `e7235382` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-27) |
| Trouvé | 2026-09-27, lot D1 (export jeux de textures × tuiles UDIM), banc `.tests/udim_app.sh` section 8 |

**Bug.** Un calque limité à un jeu de textures a un masque au-delà des objets (`object_mask > paint_objects->length`,
voir `project_get_atlas_objects`). L'export upstream le lisait comme un masque d'objet :
- par tuile UDIM, par objet et en Selected, `paint_objects->buffer[mask - 1]` est lu au-delà de la fin du tableau.
  Au 2026-09-27, l'export par tuile d'un calque limité à un jeu fait planter ArmorPaint (segfault, rc 139) ;
- en Selected avec des jeux, la sélection était ignorée : chaque jeu sortait tous ses calques (`atlas_export` ne
  regardait pas le mode).
Et l'export par tuile ignorait les jeux : une série de maps par tuile, tous jeux mêlés (N8).

**Correctif.**
- `export_texture_mask_set_objects(mask)` : les objets du jeu d'un masque de jeu ; NULL pour un masque d'objet, sans
  masque, un jeu plus utilisé, ou des jeux qui ne correspondent plus aux objets (le calque peint alors tous les
  objets, comme dans le viewport).
- Masques d'objet seuls là où l'upstream lit `paint_objects` (trois endroits). En Selected, un calque limité à un
  jeu est nommé d'après le jeu ; par objet, il va avec les objets du jeu, et donne un fichier à chacun.
- Selected avec des jeux : le calque sélectionné seul.
- Par tuile, avec des jeux qui correspondent aux objets (`export_texture_run_udim_sets`) : pour chaque jeu, les maps
  de chacune de ses tuiles, `<fichier>_<jeu>_<canal>.<tuile>`. Un jeu reçoit les calques que lui donne l'export
  Visible : partagés, limités à un de ses objets (une tuile reçoit ceux de ses objets), limités au jeu.

Côté plugin : le preset studio remplit `<atlas>` avec le jeu en Per UDIM Tile aussi (`studio_export.c`), et la
fenêtre d'export montre la section des jeux dans ce mode, avec sa note et le résumé « 9 maps × 6 texture set tiles
(2 sets, 6 UDIM tiles) » (`studio_export_window.py`).

`export_texture.c` porte aussi export-naming, studio-export, studio-exr et udim-tile-limit : `git diff upstream
studio` les mêle. Pour régénérer celui-ci : `git diff e7235382^ e7235382 -- paint/sources/io/export_texture.c` (tout
le lot D1 de ce fichier), ou le retirer d'une copie (voir l'introduction).

**Test.** `.tests/udim_app.sh` section 8, sur `mats.obj` par matériau et par tuile (skin : 1001, 1002, 1011 ; cloth :
1002) :
- par tuile : les maps de skin sur ses trois tuiles, de cloth sur la 1002 seule (4 × 5 fichiers) ; un calque limité au
  jeu skin (rouge) dans les fichiers de skin seuls, un calque limité à cloth (bleu) dans `cloth.1002` seul, même là où
  leurs uv se chevauchent dans la 1002 ; un calque partagé dans les quatre, la même case pour les deux jeux de la
  1002 ; un calque limité à `skin.1001` dans `skin.1001` seul, pas de fichier pour cloth, sans calque ;
- preset studio, `<atlas>` et `<UDIM>` : `skin_base_color.1001` … `cloth_base_color.1002`, 9 maps × 4 ;
- par objet : un fichier par objet des jeux des calques, chacun avec le calque de son jeu ;
- Selected, calque limité au jeu cloth : nommé cloth, 5 maps.

`.tests/sets_app.sh` section 6 : cube UDIM par matériau et par tuile, fenêtre d'export en Per UDIM Tile (jeux body et
cap, note du mode, résumé) et export de 54 EXR, body sur ses 4 tuiles, cap sur 1012 et 1013. Au 2026-09-27 : tout
passe. Mutations, sur la section 8 seule :
- A, chaque jeu reçoit tous les calques, sur toutes les tuiles : 5 contrôles sur 7 échouent (`cloth.1001`,
  `cloth.1011` en trop, calques mêlés) ;
- B, `export_texture_mask_set_objects()` toujours NULL : par objet et Selected échouent (fichiers `untitled_*`) ;
- C, lectures upstream des masques remises : ArmorPaint plante (rc 139) dès l'export par tuile d'un calque limité à
  un jeu ; 5 contrôles échouent.

**Signes que l'upstream l'a corrigé.** Les lectures `paint_objects->buffer[slot_layer_get_object_mask(…) - 1]`
d'`export_texture.c` bornent le masque, ou l'export par tuile connaît les jeux (`atlas_names` dans la boucle des
tuiles). Rejouer la section 8 sans le patch (mutation C).

**Non couvert.**
- L'atlas des calques garde une case par tuile, partagée par les jeux de la tuile : là où leurs uv se chevauchent, un
  calque partagé donne les mêmes texels aux deux jeux (les calques limités à un jeu restent séparés). Stockage par
  tuile : lot D2.
- Jeux ignorés quand ils ne correspondent plus aux objets (`atlas_objects` plus court après un ajout) : export par
  tuile sans jeux, comme l'upstream.
- En Selected, le preset studio met le nom du jeu dans `<object>` (la variante du fichier).

---

## udim-tile-name

| | |
|---|---|
| Fichier | `paint/sources/ui/tab_meshes.c` (`tab_meshes_draw_mesh_slot` : édition du nom ; nouveaux `tab_meshes_rename_object`, `tab_meshes_tile_suffix`, `tab_meshes_tile_base`) |
| Commit studio | `8cb80cb8` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-27) |
| Trouvé | 2026-09-27, lot D1 (`ARMORPAINT_UDIM.md` §3, L4) |

**Bug.** La tuile UDIM d'un objet est le suffixe `.1001` de son nom (`util_mesh_udim_tile`). L'onglet Meshes renommait
l'objet au nom tapé, tel quel : `body.1001` renommé `torso` perdait sa tuile, `Box` renommé `Box.1005` en gagnait
une. La table des tuiles change alors au prochain calcul (fusion du maillage, réouverture du projet) : la tuile
renommée disparaît de l'export par tuile et les cases des autres tuiles peuvent se décaler (L17).

**Correctif.** Le champ d'édition ne montre que le nom devant le suffixe ; le suffixe s'affiche à droite du champ et
reste : `skin.1011` renommé `torso` donne `torso.1011`. Un suffixe tapé est retiré, avec un message s'il désigne une
autre tuile (« robe.1005: the UDIM tile of an object comes from its import, a rename keeps it ») ; un nom vide laisse
le nom d'une tuile ; un objet sans tuile n'en gagne pas (`cap.1005` tapé donne `cap`). `tab_meshes_rename_object()`
porte la logique (avec le renommage upstream des stages et des données partagées) ; le plugin l'expose aux scripts
(`studio_object_rename`). La duplication et l'ajout passent par `_import_mesh_unique_name()`, qui garde le suffixe
depuis le lot D0 ([udim-import](#udim-import)). Pour régénérer : `git diff upstream studio --
paint/sources/ui/tab_meshes.c` (aucun autre patch dans ce fichier).

**Test.** `.tests/udim_app.sh` section 9 : `skin.1011` → `torso.1011` ; `robe.1005` tapé pour `cloth.1002` →
`robe.1002`, avec le message ; nom vide : `skin.1001` gardé ; `skin2.1002` tapé pour `skin.1002` → `skin2.1002` ;
`cap.1005` tapé pour `cap` → `cap`, avec le message ; projet sauvé puis rouvert : mêmes noms, jeux et tuiles ; export
par tuile : la 1011 toujours là, le calque limité à `torso.1011` dans `skin.1011` seul. Au 2026-09-27 : tout passe.
Mutation (suffixe jamais reconnu, le comportement upstream) : les 4 contrôles échouent, l'export après réouverture ne
sort plus que `skin.1002`.

Dans l'interface, le 2026-09-27 (événements souris et clavier injectés par un script temporaire, capture de la seule
fenêtre d'ArmorPaint) : double-clic sur `skin.1011`, le champ montre `skin`, `.1011` à sa droite ; `torso` tapé
donne `torso.1011` ; champ vidé : le nom reste.

**Signes que l'upstream l'a corrigé.** L'édition du nom (`tab_meshes_mesh_name_edit`) ne renomme plus au nom tapé tel
quel, ou la tuile devient une donnée de l'objet. Rejouer la section 9 sans le patch.

**Non couvert.**
- La tuile vient toujours du nom : un objet importé sous le nom `truc.1005` hors UDIM reste une tuile (L4), jusqu'à
  la tuile-donnée du lot D2.
- L'onglet Meshes ne regroupe pas les tuiles d'un objet sous un nœud (`ARMORPAINT_UDIM.md` §6.10) : lot D2.
