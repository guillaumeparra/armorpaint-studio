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
| Validé contre | upstream `85f6cf1c` (2026-09-26) |
| Injection | 11 lignes sous `#ifdef WITH_EXTERNAL`, en deux blocs — l'app compile toujours sans `external/` |

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
- Le split UDIM (`obj_parse(…, udim)`) n'a été vérifié qu'hors app, le mode ne se règle que dans l'UI.

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

Deux patches touchent `iron_obj.c` : `obj-global-indices.patch` (`upstream` → `252f9fc3`) puis
`obj-float-exponent.patch` (`252f9fc3` → `70cece4b`). Les réappliquer dans cet ordre. Pour régénérer celui-ci :
`git diff <commit studio d'obj-global-indices> studio -- base/sources/iron_obj.c`.

**Test.** `.tests/obj.sh` (hors app, ASan/UBSan ; `STUDIO_ROOT=<armorpaint>` si ce dépôt est lancé depuis un
worktree) :
- étape 4, `float` (`obj_float_test.c`, qui inclut `iron_obj.c` pour appeler `read_float()`) : 30 jetons à exposant
  comparés à `strtod` borné à ±`FLT_MAX`, à 1 ulp près, fin de jeton comprise : `1.5e-3`, `-0.5e-2`, `1E+2`,
  `1e-45` (dénormal), `1e999` et `-1e999` (±`FLT_MAX`), `1e-999` et `0e999` (0), `1e2147483648`, `1e`, `1e+`,
  `1.5e-3x`... Tous passent ; 3 sont à 1 ulp de `strtod` (`6.02e23`, `3.4e38`, `7.450581e-09` : mantisse
  arrondie en float avant la multiplication). L'upstream en rate 25 sur 30. Puis 20 000 jetons décimaux sans
  exposant, sortie identique à l'upstream ;
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
| Commit studio | `33bd8667` sur la branche `studio` |
| Validé contre | upstream `85f6cf1c` (2026-09-26) |
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
  `io_fbx_parse()` rend NULL quand `ufbx_load_memory()` échoue.
- `import_obj_run()` vérifie son blob (appelé aussi par l'import `.blend`).
- `import_texture_run()` fait la même vérification avant d'appeler un importeur de plugin (pas pour l'importeur
  par défaut ni pour une texture déjà en cache, ce qui garde les calques PSD, mis en cache sous un chemin fictif).

**Test.** `.tests/import_app.sh` : fichiers absents (`obj fbx gltf glb psd tiff svg`) et `noread.obj`
(`chmod 000`) → « Could not open file », rc 0 ; `bad.gltf`, `bad.glb`, `bad.fbx` (contenu `garbage`) → « Could not
import file », rc 0 ; non-régression : `grid.obj`, `tri.gltf`, `tri.glb`, `quad.fbx` (FBX ASCII) donnent les
mêmes objets et nombres d'indices, `red.png` / `.svg` / `.tiff` / `.psd` (générés par `sips`) « Texture imported ».
Au 2026-09-26 : tout passe ; le même script sur le build d'avant les deux patches (`70cece4b`) : 14 échecs, tous en
rc 139, et la section de non-régression identique.

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
