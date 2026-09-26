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
apporter ces bornes : rejouer le fuzz sur `main` sans le patch avant de le retirer.

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

**Test.** `.tests/obj.sh` (hors app, ASan/UBSan, compare à `main:base/sources/iron_obj.c`) :
1. `valid/` : 10 fichiers (sphères en `v`, `v/t`, `v//n`, `v/t/n`, CRLF, usemtl, triangles, n-gones concaves,
   grille UDIM), splits objet, matériau et UDIM : sortie identique à l'upstream ;
2. `shared/` : faces sur les `vn` / `vt` / `v` d'un objet précédent (triangles, quads, n-gone, indices relatifs,
   positions réutilisées plus grandes que celles de la part) : sortie identique à la version à données séparées
   lue par l'upstream ;
3. `bad/` : 15 fichiers malformés, aucune erreur ;
4. fuzz : 1 000 mutations de `valid/` et `shared/` en splits objet et matériau. Au 2026-09-26 : **0 plantage**
   contre 1 413 pour l'upstream (heap-buffer-overflow, use-after-free, BUS, SEGV) ; à 3 000 mutations, 0 contre
   4 251.

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
`STUDIO_UPSTREAM=main` sur une copie de `main` **sans** le patch (étape `shared/`).

**Non couvert.**
- En split par objet, les `v`/`vt`/`vn` de tout le fichier restent en mémoire jusqu'à la dernière part (l'upstream
  ne gardait que ceux de la part courante) : de l'ordre de la taille du fichier, comme en split par matériau.
- `read_float()` : l'exposant (`1e10`) est calculé dans un `int` par multiplications successives, qui débordent
  au-delà de 9 et bouclent `exp` fois — un exposant énorme prend du temps sans planter. UBSan le signale pendant le
  fuzz (`iron_obj.c:140`), seul UB restant. Le packing int16 des coordonnées hors bornes (`float-cast-overflow`,
  saturé sur arm64) n'est pas non plus traité. Ces deux points relèvent de la lecture des flottants, hors du
  périmètre des indices.
- `import_obj_parse()` pousse la première part même sans face (fichier dont le premier objet n'a pas de face
  valide) : l'import aboutit (rc 0) avec un objet vide, comportement upstream inchangé.
- Le split UDIM (`obj_parse(…, udim)`) n'a été vérifié qu'hors app, le mode ne se règle que dans l'UI.
