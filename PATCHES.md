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
