# ArmorPaint — vers des UDIM natifs

> **Décision 2026-09-26 : option B retenue** (un jeu de textures par tuile, « contexte de tuile », §6). Les options C et D sont écartées ; C reste une évolution possible de B si le nombre de draws devenait un problème (100+ tuiles).
> **2026-09-27 : lots D0 et D1 faits** (§3, §7). D1 garde un objet de peinture par tuile : la tuile-donnée, le maillage à plages et la migration passent au lot D2 (§7, « Lot D1 »).
> **2026-09-27 : lot D2 lancé** (go sans la recette C6), plan au §7 « Lot D2 ». Le maillage à plages et la tuile-donnée passent en D3. Sous-lots D2a à D2c faits (§7, « Lot D2a » à « Lot D2c »).
> Document de réflexion à l'origine ; le §7 sert désormais de plan (chantier D de `ARMORPAINT_LOTS.md`). Il fait l'état des UDIM après le commit upstream `99156dfc` (« paint: paint across udims », 2026-09-15), liste les limites, compare quatre architectures et propose une feuille de route.
> Base : branche `studio` au commit `fbde517a`. Estimations en **jours-homme**, avec les mêmes hypothèses que `ARMORPAINT_LOTS.md`.
> ⚠️ **§5.2 de `ARMORPAINT_ANALYSE.md` est en partie périmé.** Depuis `99156dfc`, on peut peindre à travers les tuiles et les calques partagés ne sont plus dupliqués par tuile. Ce document le remplace pour tout ce qui touche aux UDIM.

---

## 0. En bref

- **Upstream a fait un vrai pas.** On peint désormais d'un seul trait à travers les tuiles. Pour y arriver, **toutes les tuiles sont rangées dans une grille à l'intérieur de chaque texture de calque**. C'est un atlas : la mécanique existait déjà pour le raytrace multi-objets et a été réutilisée.
- **Le prix est la résolution.** Une tuile obtient `résolution projet / ceil(√N)` pixels. Avec 6 tuiles dans un projet 4K, chaque tuile fait environ **1365 px**, et l'export la ré-agrandit à 4K. Au-delà de 64 tuiles, les suivantes s'écrasent **en silence** dans la dernière case.
- **« Natif » veut dire :**
  - la tuile est une propriété des UV, pas un objet ;
  - chaque tuile a sa propre résolution ;
  - l'export se fait à la résolution native, sans ré-échantillonnage ;
  - la mémoire et l'undo sont proportionnels aux tuiles réellement peintes.
- **Contraintes du moteur :**
  - iron ne connaît **aucune texture array** (Metal ne crée que des `MTLTextureType2D`) ;
  - un shader lie **16 textures au plus** ;
  - un calque = 3 render targets pleine résolution ;
  - le `.arm` ne stocke qu'une résolution carrée par calque.
- **Architecture recommandée : option B, un jeu de textures par tuile, activé à tour de rôle par un « contexte de tuile ».**
  - Pas de nouvelle fonctionnalité GPU.
  - Le code existant tourne tuile par tuile, comme le fait déjà `layers_udim_remap`.
  - Les tuiles deviennent des plages d'index d'un même maillage : plus de split en objets.
- **Le vrai mur, c'est la VRAM.** 10 tuiles 4K × 8 calques en 16 bits font environ 32 Go. Des UDIM natifs imposent donc :
  - l'allocation paresseuse des tuiles ;
  - un undo par tuile ;
  - une éviction vers la RAM.

  Ce n'est pas une option d'après.
- **Côté fork, c'est une rupture de la règle « injections de 1 à 3 lignes ».** Les fichiers concernés sont les plus actifs upstream : 56 commits depuis juin. Recommandation :
  - la **phase 0** (correctifs sans regret) tout de suite ;
  - la **phase 1** en prototype sur une branche ;
  - un **go / no-go explicite** avant la phase 2, sur des critères de production (§9).

---

## 1. Ce qu'on entend par « UDIM natifs »

Objectifs, numérotés pour la comparaison du §5 :

| # | Objectif | Aujourd'hui |
|---|---|---|
| N1 | La tuile vient des UV (`floor(uv)`). Un même objet peut couvrir plusieurs tuiles, sans split à l'import. | ❌ Split obligatoire. L'identité de la tuile vient du nom de l'objet (`cube.1012`). |
| N2 | Peindre d'un seul trait à travers les tuiles, sans couture. | ✅ Depuis `99156dfc`, avec des défauts de filtrage aux bords de case. |
| N3 | Résolution par tuile : visage en 8K, bottes en 2K. | ❌ Une résolution globale, divisée par la grille. |
| N4 | Export à la résolution native de chaque tuile, sans ré-échantillonnage. | ❌ Chaque case est ré-agrandie. |
| N5 | Mémoire proportionnelle aux tuiles réellement peintes. | ⚠️ Constante : on paie tout de suite une texture pleine, mais chaque tuile y est sous-résolue. |
| N6 | Undo proportionnel à ce que le trait a touché. | ❌ Copie du calque entier. |
| N7 | Nombre de tuiles sans plafond pratique (au moins 100 ; Mari va bien au-delà). | ❌ 64 au plus en grille, 100 à l'import FBX, sans message. |
| N8 | Se combine avec les autres découpages : jeux de textures, split par matériau, export par objet. | ❌ Atlas et UDIM ne se combinent pas. Le split UDIM exclut le split par matériau. **D1** : split par matériau et par tuile, un jeu de textures par matériau, export par jeu et par tuile ; les jeux d'une tuile partagent sa case d'atlas jusqu'à D2. |
| N9 | UV matériau fidèles aux UV du DCC : bruits procéduraux et dégradés continus d'une tuile à l'autre. | ⚠️ Seulement quand l'ordre des cases coïncide avec la grille UDIM (§3). **✅ D1** (L8), hors vue 2D. |
| N10 | Projets existants relus sans perte. | — (contrainte de migration) |

---

## 2. Fonctionnement actuel (upstream `99156dfc`)

### 2.1 Import : un objet par tuile

- **OBJ** ([iron_obj.c:553](base/sources/iron_obj.c:553), [import_obj.c:16](paint/sources/io/import_obj.c:16)) :
  - `obj_parse(..., udim=true)` compte les tuiles (`tiles_u`, `tiles_v`) sur les UV maximales ;
  - il range les index par tuile dans `part->udims`. Chaque triangle est rangé par son **barycentre** ([iron_obj.c:232](base/sources/iron_obj.c:232) `get_tile`) ;
  - il renormalise les UV en 0-1 dans la tuile ;
  - `import_obj.c` crée ensuite un objet `nom.<1000 + v·10 + u + 1>` par tuile, chacun avec **une copie complète** de `posa`, `nora` et `texa` (seuls les index diffèrent).
- **FBX** ([io_fbx.c:215](paint/plugins/io_fbx/io_fbx.c:215) `build_udim_split`) :
  - même principe, mais les sommets sont dupliqués **par coin** ;
  - **tous les nœuds** sont fusionnés sous le nom du premier maillage ;
  - seul le premier jeu d'UV est lu ;
  - au-delà de `MAX_UDIM_TILES 100`, les triangles sont **ignorés sans message** ([io_fbx.c:274](paint/plugins/io_fbx/io_fbx.c:274)).
- **Les UV sont stockées en `i16` normalisés** (`texa`, ×32767, V inversé, [iron_obj.c:628](base/sources/iron_obj.c:628)). Sans UDIM, une UV > 1 est **repliée** (`uvx - (int)uvx`, [iron_obj.c:620](base/sources/iron_obj.c:620)). Le moteur sait dilater les UV (`scale_tex` → `tex_unpack`, [engine.c:1080](base/sources/engine.c:1080)), mais un `i16` étalé sur 10 tuiles ne laisse que 3276 pas par tuile, **soit 2,5 texels par pas pour une tuile 8K**. **Renormaliser par tuile est donc la bonne idée. C'est le split en objets qui est la mauvaise.**

### 2.2 Table des tuiles, déduite des noms

[util_mesh.c:113-195](paint/sources/util/util_mesh.c:113) :
- `util_mesh_udim_tile(name)` lit le suffixe `.1xxx` du nom d'objet ;
- `_util_mesh_udim_build_tiles()` en tire la liste triée `util_mesh_udim_tiles[64]` ;
- `util_mesh_udim_active()` vaut vrai dès qu'il y a plus d'une tuile ;
- `util_mesh_udim_slot(tile)` donne l'**index dans la liste triée**, qui sert de case de la grille ;
- `stride = ceil(√N)`, plafonné à `ATLAS_MAX_STRIDE 8`. Au-delà de 64 tuiles, `tiles_spent` renvoie tout le reste dans la case 63.

### 2.3 Atlas en grille dans chaque calque

- Les calques « UDIM » sont ceux que `util_mesh_udim_layer(l)` accepte : masque partagé (0) ou masque d'atlas, et `uv_map != 1`. Ils vivent dans un **espace atlas**, où la tuile d'index `s` occupe la case `(s % stride, s / stride)`.
- Le maillage fusionné ([util_mesh.c:327](paint/sources/util/util_mesh.c:327)) range les UV de chaque objet dans sa case (`tile_step = 32767 / stride`, ligne 358). Il sert :
  - à la carte UV et à la carte de dilatation ([util_uv.c:28](paint/sources/util/util_uv.c:28), [:125](paint/sources/util/util_uv.c:125)) ;
  - au raytrace.
- Uniformes ([uniforms.c:94](paint/sources/uniforms.c:94), [:228](paint/sources/uniforms.c:228)) :
  - `_atlas_transform` = (offset x, offset y, 1/stride) de l'objet courant, identité pour le maillage fusionné ;
  - `_atlas_stride` = stride.

### 2.4 Shaders

- `make_mesh.c` ([:142-149](paint/sources/render/make_mesh.c:142), [:319](paint/sources/render/make_mesh.c:319)) ajoute un varying `tex_coord_atlas = tex * z + xy`. Il sert à échantillonner les calques, les masques, le colorid, le picker et le wireframe.
- `make_paint.c` ([:207-211](paint/sources/render/make_paint.c:207), [:307](paint/sources/render/make_paint.c:307)) rastérise le trait directement en espace atlas (`atlas_uv`). Le picking UV renvoie aussi `atlas_uv`.
- `make_texcoord.c` ([:133](paint/sources/render/make_texcoord.c:133)) calcule l'UV matériau = `atlas_uv × stride`, soit **UV locale + (colonne, rangée) de la case**, et non + (u, v) de la tuile UDIM. Le bake, lui, lit l'atlas directement.

### 2.5 Calques « de tuile »

Un calque masqué sur un seul objet-tuile garde son propre espace 0-1, donc **toute la résolution projet pour cette seule tuile**. C'est paradoxalement le seul chemin qui donne aujourd'hui une vraie résolution par tuile.

[util_layer.c:1072-1120](paint/sources/util/util_layer.c:1072) : `layers_udim_remap` recopie un calque entre espace tuile et espace atlas, avec `draw_scaled_sub_image` et 4 render targets temporaires statiques. Cette copie sert :
- au flatten ;
- à l'export.

Elle **perd de la résolution** dans le sens tuile → atlas et **en invente** dans le sens atlas → tuile.

### 2.6 Export

[export_texture.c:226-310](paint/sources/io/export_texture.c:226) :
- **Per UDIM Tile** : une passe par objet-tuile. Les calques partagés sont **recadrés dans l'atlas puis ré-agrandis** à la taille d'export.
- **Visible** : les calques de tuile sont réduits dans leur case.
- Le découpage par atlas (jeux de textures) ne se combine pas avec le découpage UDIM.

Notre couche studio (`studio_export.c`, `studio_atlas.c`) se branche par-dessus :
- `<UDIM>` vient de `object_name` ;
- la fenêtre masque les jeux de textures quand le projet est en UDIM.

### 2.7 Schéma

```
 Maillage source (UV 0..3 × 0..2)          Calque partagé (ex. 4096², 6 tuiles → grille 3×3)
 ┌────┬────┬────┐                          ┌──────┬──────┬──────┐
 │1011│1012│1013│   import « UDIM Tile »   │ 1001 │ 1002 │ 1003 │  ← case = rang dans la liste triée,
 ├────┼────┼────┤ ───────────────────────► ├──────┼──────┼──────┤    pas la position UDIM
 │1001│1002│1003│  6 objets cube.10xx,     │ 1011 │ 1012 │ 1013 │
 └────┴────┴────┘  UV renormalisées 0..1   ├──────┼──────┼──────┤  1365 px par tuile,
                                           │ vide │ vide │ vide │  3 cases perdues (33 %)
                                           └──────┴──────┴──────┘
 Export « Per UDIM Tile » : case recadrée puis ré-agrandie à 4096 → flou, sans information nouvelle.
```

---

## 3. Limites constatées

| # | Limite | Cause (code) | Impact | Gravité |
|---|---|---|---|---|
| L1 | Résolution par tuile = `res / ceil(√N)` | atlas en grille dans une texture de calque | 6 tuiles en 4K → 1365 px ; ré-agrandies à l'export | 🔴 |
| L2 | Pas de résolution par tuile | `config_get_texture_res_*` global (109 références) ; `.arm` carré par calque ([import_arm.c:324](paint/sources/io/import_arm.c:324)) | Impossible d'avoir le visage en 8K et le corps en 2K | 🔴 |
| L3 | Au-delà de 64 tuiles, tout finit dans la case 63, sans message | `tiles_spent` ([util_mesh.c:164](paint/sources/util/util_mesh.c:164)) | Peinture silencieusement fausse | ✅ D0 (🔴) |
| L4 | Identité de la tuile tirée du nom de l'objet | `util_mesh_udim_tile(name)` | Renommer un objet dans l'onglet Meshes change sa tuile ; un nom `truc.1005` hors UDIM est pris pour une tuile. Depuis D1, un renommage garde la tuile ; reste le second cas (tuile-donnée, D2) | 🟡 D1 en partie (🟠) |
| L5 | Split obligatoire | `import_obj.c`, `build_udim_split` | Plus de split par matériau ni par objet ; N copies des sommets (OBJ) ; tout fusionné (FBX). Depuis D0 : un objet par objet (nœud FBX) et par tuile, avec ses seuls sommets ; depuis D1, aussi par matériau et par tuile. Reste le split lui-même, un objet par tuile (D2) | 🟡 D0-D1 en partie (🟠) |
| L6 | Bords de case | cases jointives, sans gouttière | Le filtrage bilinéaire et la dilatation débordent d'une case sur sa voisine : liseré aux bords des îlots collés au bord de tuile | 🟠 |
| L7 | Grille carrée | `stride = ceil(√N)` | 5 tuiles → grille 3×3 : 44 % de VRAM perdue ; 10 tuiles → 4×4 : 37 % | 🟡 |
| L8 | UV matériau ≠ UV du DCC | `atlas_uv × stride` = UV locale + (colonne, rangée) de la case | Pour 1001-1004 en grille 2×2, 1003 reçoit le décalage (0, 1) au lieu de (2, 0) : bruits UV et dégradés discontinus entre tuiles voisines | ✅ D1 (🟡) |
| L9 | Deux espaces coexistent (atlas / tuile) | `layers_udim_remap` à chaque flatten ou export | Copies GPU, 4 cibles statiques, perte de résolution dans les deux sens | 🟡 |
| L10 | Mémoire et undo non proportionnels | un calque = 3 RT pleine résolution ; undo = copie du calque entier ([util_layer.c:70](paint/sources/util/util_layer.c:70)) | Toucher une tuile sauvegarde toutes les tuiles ; dès 8K, l'undo tombe à 4 puis 1 pas | 🟡 |
| L11 | Import : tuile ≥ 10 en U | `1000 + v·10 + u + 1` jamais borné | u = 10 donne 1011, qui collisionne avec (0, 1) | ✅ D0 (🟠) |
| L12 | Import : UV négatives | `(int)` tronque vers 0 | u = -0,3 part dans 1001 ; u = -1,5 donne un index -1 : **écriture hors tableau** dans `num[]` (OBJ), confirmée sous ASan le 2026-09-26 (`heap-buffer-overflow`, 4 octets avant le tableau) | ✅ D0 (🔴) |
| L13 | Import : triangles à cheval sur deux tuiles | rangement par barycentre, puis repli `uv - (int)uv` et décalage par sommet UV partagé (le dernier écrit gagne) | Triangles déformés, sans avertissement | ✅ D0 (🟠) |
| L14 | FBX : plafond de 100 tuiles, premier jeu d'UV seulement, tous les nœuds fusionnés | `io_fbx.c:10`, `:222`, `:274` | Géométrie perdue sans message | ✅ D0 (🟠) |
| L15 | CLI `--export-textures` toujours en Visible | `args.c:161` | Pas d'export par tuile en batch (notre `studio_export_run` contourne) | 🟡 |
| L16 | `atlas_objects` indexé par position | tableau parallèle à `paint_objects` | Trier ou supprimer dans Meshes désynchronise. Depuis le 2026-09-27, tri, parent, glisser-déposer et suppression remappent `atlas_objects` et les masques de calque (patch `atlas-objects-order`) ; restent la duplication et l'ajout de forme | 🟡 en partie (🟡) |
| L17 | Cases renumérotées quand la table des tuiles change | case = rang de la tuile dans la table triée, grille `ceil(√N)` ; les calques d'atlas ne sont pas recopiés | Ajouter un maillage qui apporte une tuile, supprimer le dernier objet d'une tuile ou passer de 4 à 5 tuiles (grille 2×2 → 3×3) décale les cases : la peinture des calques partagés (et limités à un jeu) reste en place et passe à une autre tuile. Constaté le 2026-09-27 : 1001, 1002 et 1011 peintes, puis un maillage en 1003 ajouté : la 1003 montre la peinture de la 1011, l'export de la 1011 est vide. Les calques d'une tuile ne bougent pas | 🔴 → D2 |

**Phase 0 faite le 2026-09-27** (macOS) : L3 et L11 à L14 corrigées, L5 en partie, par les patches `udim-import`,
`fbx-split` et `udim-tile-limit` (`paint/plugins/external/PATCHES.md`). L'import donne aussi le conseil de résolution
du tableau ci-dessous. L8 passe en phase 1 et L6 en phase 2 (§7).

**Lot D1 fait le 2026-09-27** (macOS) : L8 corrigée, L4 et L5 en partie, N8 à l'export (jeux × tuiles), par les
patches `udim-core`, `udim-material-uv`, `udim-export-sets`, `udim-tile-name` et la partie D1 d'`udim-import` et
`fbx-split`. L17 trouvée en route, laissée à la phase 2 (§7, « Lot D1 »).

**Tableau utile pour la production.** Avec le système actuel, la résolution projet nécessaire se déduit du nombre de tuiles :

| Tuiles | Grille | Pour 2K / tuile | Pour 4K / tuile | VRAM d'un calque 16 bits (3 RT) pour 4K / tuile |
|---|---|---|---|---|
| 1 | 1×1 | 2K | 4K | 0,4 Go |
| 2-4 | 2×2 | 4K | 8K | 1,6 Go (undo réduit à 4 pas) |
| 5-9 | 3×3 | 6K (custom) | 12K (custom) | 3,6 Go (undo à 1 pas) |
| 10-16 | 4×4 | 8K | 16K | 6,4 Go (undo à 1 pas) |

→ **Jusqu'à 4 tuiles en 4K, le système actuel tient**, en projet 8K. Au-delà, il ne tient plus. **C'est le premier critère de décision : combien de tuiles, et à quelle résolution, sur nos assets réels ?**

---

## 4. Contraintes qui orientent les choix

1. **Pas de texture array dans iron.**
   - `gpu_texture_t` ne connaît que `width` et `height`.
   - Le backend Metal ne crée que des `MTLTextureType2D` ([metal_gpu.m:102](base/sources/backends/metal_gpu.m:102), [:163](base/sources/backends/metal_gpu.m:163), [:564](base/sources/backends/metal_gpu.m:564)).
   - Il faudrait l'ajouter dans les trois backends (Metal, D3D12, Vulkan), puis dans `kong`, le langage de shader, et dans tout le pipeline de rendu vers une tranche (render-to-slice).
2. **16 textures liées au plus** par shader (`GPU_MAX_TEXTURES`, [iron_gpu.h:21](base/sources/iron_gpu.h:21)). Un calque en consomme 3 (couleur, normale, pack). Lier toutes les tuiles d'un calque dans un seul draw est donc impossible dès 6 tuiles. **Le rendu doit se faire tuile par tuile**, ou passer par une texture array.
3. **Un calque = 3 render targets** pleine résolution (`texpaint`, `texpaint_nor`, `texpaint_pack`), référencés partout :

   | Symbole | Références | Fichiers |
   |---|---|---|
   | `->texpaint` | 135 | 20 |
   | `->texpaint_nor` | 64 | 11 |
   | `->texpaint_pack` | 66 | 12 |
   | `config_get_texture_res_*` | 109 | 16 |

   Changer le **type** de ces champs, c'est toucher la moitié du code de peinture.
4. **Précédent utile** : `layers_udim_remap` fabrique déjà un `slot_layer_t` temporaire dont les trois pointeurs désignent d'autres textures, puis le passe au code existant. **Remplacer les pointeurs de textures d'un calque, sans rien changer d'autre, est donc un geste qui fonctionne déjà dans le code.**
5. **Format `.arm`** : `layer_data_t` = une résolution (`res`, carrée) et un blob LZ4 par texture ([import_arm.c:324](paint/sources/io/import_arm.c:324), [:615](paint/sources/io/import_arm.c:615)).
6. **UV en `i16`** (§2.1) : il faut garder des UV locales 0-1 par tuile, et porter l'offset de tuile ailleurs (uniforme par draw).
7. **Règle du fork** (`ARMORPAINT_LOTS.md`, principe directeur) : le code studio vit dans `external/`, et les fichiers upstream ne reçoivent que des injections de 1 à 3 lignes. Des UDIM natifs **ne rentrent pas dans cette règle**, quelle que soit l'option. Il faut une exception assumée et inscrite dans `PATCHES.md`.
8. **Activité upstream** : sur ~600 commits depuis juin 2026, 56 touchent `slot_layer.c`, `util_layer.c`, `render_path_paint.c`, `make_mesh.c`, `make_paint.c` ou `export_texture.c`. Les UDIM ont été retouchés les 1er et 15 septembre. Chaque pull aura des conflits sur ces fichiers.

---

## 5. Options

### Option A — Améliorer l'atlas en grille (garder l'architecture upstream)

**Principe.** Garder « toutes les tuiles dans une texture de calque », mais corriger ce qui se corrige localement :
- **Gouttière entre cases** (8 à 16 px) et **clamp de l'échantillonnage** à la case (L6).
- **Grille rectangulaire** (`cols × rows` au plus juste, textures non carrées) (L7) : 5 tuiles tiennent en 3×2 au lieu de 3×3.
- **UV matériau = UV UDIM réelles** (L8) : un uniforme `_udim_offset` (u, v de la tuile) remplace `atlas_uv × stride`.
- **Erreur explicite au-delà de 64 tuiles** (L3), au lieu de l'écrasement silencieux.
- **Résolution projet suggérée** d'après le nombre de tuiles (tableau du §3), par exemple dans la fenêtre d'import.
- Corrections d'import L11-L14 (communes à toutes les options, voir phase 0).

| | |
|---|---|
| Couvre | N2, N7 (jusqu'à 64), N9 ; améliore N5 |
| Ne couvre pas | N1, N3, N4, N6 : la résolution reste partagée |
| Coût | 8 à 12 j |
| Fork | Faible à moyen : patches localisés dans `util_mesh.c`, `make_texcoord.c`, `uniforms.c`, les importeurs |
| Risque | Faible ; upstream peut faire la même chose et rendre nos patches obsolètes, ce qui est tant mieux |

### Option B — Un jeu de textures par tuile, avec un « contexte de tuile » ⭐

**Principe.**
- Chaque calque porte **un jeu de 3 render targets par tuile**, chacun à la résolution de sa tuile.
- Une fonction `layers_set_tile(t)` fait pointer `texpaint`, `texpaint_nor` et `texpaint_pack` **de tous les calques** vers le jeu de la tuile `t`, comme `layers_udim_remap` le fait déjà pour un calque.
- Tout le code existant (peinture, flatten, masques, groupes, dilatation, export) **reste écrit pour « une texture par calque »**. Il est simplement exécuté **dans une boucle sur les tuiles concernées**.
- Le maillage n'est plus splitté :
  - il garde un seul vertex buffer, avec des UV locales 0-1 ;
  - son index buffer est **trié par tuile**, avec une plage `[début, fin)` par tuile (ce que `part->udims` calcule déjà) ;
  - un draw par plage, avec l'uniforme `_udim_offset`.

| | |
|---|---|
| Couvre | N1 à N9 ; N10 par migration |
| Coût | 45 à 70 j, phases 1 à 4 (§7) |
| Fork | Élevé mais **concentré** : la boucle sur les tuiles vit dans une poignée de points d'entrée (rendu viewport, passe de peinture, flatten, export, save/load). Le reste du code ne voit qu'un calque normal |
| Risque | Moyen. Les points délicats sont les caches indexés par calque (aperçus, `texpaint_blend*`, render targets nommés) et la sélection des tuiles touchées par un trait |
| GPU | Aucune nouvelle fonctionnalité. Plus de draws : N plages au lieu d'un objet, soit quelques dizaines, négligeable |

### Option C — Texture array + table des tuiles (façon Blender)

**Principe.**
- Chaque canal d'un calque devient une **texture 2D array**. Les tuiles sont rangées dans des tranches, éventuellement plusieurs petites tuiles par tranche.
- Une petite texture de correspondance donne, pour chaque tuile, (tranche, offset, échelle).
- Le shader calcule `tile = floor(uv)`, lit la table, puis échantillonne. Un seul draw par objet.

C'est ainsi que Blender gère ses images « tiled ».

| | |
|---|---|
| Couvre | N1 à N9 |
| Coût | 70 à 110 j, dont 25 à 40 j de moteur : texture arrays, render-to-slice et génération mip dans les 3 backends, plus le support dans `kong` |
| Fork | Très élevé : `base/` (moteur), générateurs de shaders, tout le code qui fait `draw_begin(texpaint)` |
| Risque | Élevé : validation sur trois API graphiques, tests Windows et Linux que nous ne faisons pas aujourd'hui |
| Intérêt | Meilleures performances avec beaucoup de tuiles (100+). Surtout une **évolution possible de B** : l'API « contexte de tuile » peut plus tard être réalisée par des tranches d'array |

### Option D — Texture virtuelle / streaming (façon Mari)

**Principe.** Découper toutes les textures en pages (128² par exemple), garder en VRAM un cache des pages visibles, et gérer une table de pages plus un retour de faisabilité (feedback buffer). La résolution devient pratiquement illimitée.

| | |
|---|---|
| Couvre | Tout, avec en plus une VRAM bornée |
| Coût | 6 mois et plus ; c'est l'« option C » du §13.6 de l'analyse (outil dédié) |
| Verdict | Hors de portée d'un fork. À ne considérer que si ArmorPaint devient l'outil principal de texturing héros |

### Comparaison

| | N1 sans split | N2 couture | N3 res/tuile | N4 export natif | N5 mémoire | N6 undo | N7 >64 | N8 combinable | N9 UV matériau | Coût (j) | Fork |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Actuel | ❌ | ✅ | ❌ | ❌ | ⚠️ | ❌ | ❌ | ❌ | ⚠️ | — | — |
| A | ❌ | ✅ | ❌ | ❌ | ⚠️+ | ❌ | ❌ | ❌ | ✅ | 8-12 | faible |
| **B** | ✅ | ✅ | ✅ | ✅ | ✅ (phase 4) | ✅ (phase 4) | ✅ | ✅ | ✅ | 45-70 | élevé, concentré |
| C | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | 70-110 | très élevé |
| D | ✅ | ✅ | ✅ | ✅ | ✅✅ | ✅ | ✅ | ✅ | ✅ | 120+ | hors fork |

---

## 6. Option B en détail

### 6.1 Modèle de données

```c
// Projet : la table des tuiles devient une donnée, plus une déduction des noms d'objets
typedef struct udim_tile {
	i32  id;         // 1001, 1002, ...
	i32  u, v;       // position UDIM (id - 1001 = u + 10·v)
	i32  res_x;      // résolution propre ; 0 = résolution projet
	i32  res_y;
	i32  atlas;      // jeu de textures (studio_atlas), -1 = celui de l'objet
} udim_tile_t;
// g_project->udim_tiles : tableau trié par id (vide = projet non UDIM, comportement actuel inchangé)

// Calque : un jeu de textures par tuile, alloué à la demande
typedef struct layer_tile {
	gpu_texture_t *texpaint;       // NULL = tuile jamais peinte (transparente / valeur de fond)
	gpu_texture_t *texpaint_nor;
	gpu_texture_t *texpaint_pack;
	bool           dirty;          // modifiée depuis le dernier save ou snapshot d'undo
} layer_tile_t;
// slot_layer_t gagne : layer_tile_t *tiles (indexé comme g_project->udim_tiles)
// texpaint / texpaint_nor / texpaint_pack restent : ils désignent la tuile du contexte courant

// Maillage : plages d'index par tuile au lieu d'objets séparés
typedef struct mesh_tile_range {
	i32  tile;         // index dans g_project->udim_tiles
	u32  start, count; // dans l'index buffer trié par tuile
	aabb_t bounds;     // boîte monde des triangles de la tuile (sélection par le pinceau)
} mesh_tile_range_t;
```

- **Mode non UDIM** = une seule tuile 1001 à la résolution projet. Le code reste le même dans les deux modes, et c'est ce qui garantit N10.
- **Masques de calque** : même structure (`layer_tile_t` réduit à un canal).

### 6.2 Import sans split

- L'OBJ garde le calcul actuel des tuiles (`part->udims`). Mais au lieu de créer N objets, il **concatène les N listes d'index** dans l'ordre des tuiles et enregistre les plages. Les UV restent renormalisées 0-1 : bonne précision en `i16`.
- Split par objet ou par matériau **et** UDIM deviennent orthogonaux : un objet `body` peut couvrir 1001 à 1004 (N8). Le lot D1 a fait le split par matériau et par tuile (objets `<matériau>.<tuile>`, un jeu de textures par matériau) ; l'objet unique à plages reste pour la phase 2.
- Corrections L11 à L14 : **faites en phase 0** (2026-09-27, patches `udim-import` et `fbx-split`), reprises telles quelles :
  - tuile calculée par `floorf`, avec une tolérance aux bords (`base/sources/iron_udim.c`), plus par `(int)` ;
  - une uv hors des tuiles 1001-1990 (négative, u ≥ 10, nan) fait refuser le fichier, avec un message ;
  - triangle à cheval : **coupé le long des bords de tuile** (positions, normales, uv, couleurs interpolées) plutôt que rangé par barycentre ; le nombre de triangles coupés est journalisé ;
  - FBX : un objet par nœud et par tuile, plafonds levés (tableaux dynamiques). Reste à faire : choisir le jeu d'UV qui porte les tuiles (c'est toujours le premier).
- Un vieux maillage déjà splitté (`cube.1001`…) reste lisible : au chargement, on le **refusionne** (§6.9).

### 6.3 Rendu viewport

- `render_path` dessine chaque objet **plage par plage**. Avant chaque plage : `layers_set_tile(t)`, puis l'uniforme `_udim_offset = (u, v)` pour les UV matériau. Le lot D1 a posé `_udim_offset`, par objet de tuile (`util_udim_draw_paint_meshes`) : il passera à la plage.
- Le shader de maillage n'échantillonne plus en `tex_coord_atlas` mais en `tex_coord` (UV locale). `make_mesh.c` **se simplifie** : le varying atlas disparaît.
- Le composite des calques pour l'affichage (le « flatten » vers `texpaint` du viewport) se fait par tuile. Un cache par tuile évite de recomposer les tuiles non modifiées. C'est le point de performance à surveiller.

### 6.4 Peinture à travers les tuiles

La passe de peinture rastérise le maillage **en espace UV** dans la cible du calque, et le pinceau est évalué **en 3D** (position monde reconstruite). La continuité à travers une couture UV est donc automatique, dès que chaque tuile touchée reçoit sa passe :

```
pour chaque plage de l'objet peint :
    si bounds(plage) ∩ sphère_pinceau (ou frustum du décal / de la projection) :
        layers_set_tile(plage.tile)
        passe de peinture sur [start, count), cible = tuile, UV locales
        layer.tiles[plage.tile].dirty = true
```

- Un trait touche en pratique 1 ou 2 tuiles, donc un surcoût de 1 à 2 passes.
- **Picking** (colorid, pipette UV, remplissage par face/UV island) : le gbuffer de pick doit écrire l'**id de tuile** en plus de l'UV locale (un canal libre, ou `_udim_offset` ajouté à l'UV avant écriture). Aujourd'hui `tex_coord_pick = atlas_uv`.
- **Filtres en texels** (flou, smudge, clone, taille des brosses « en pixels ») : la densité de texels change d'une tuile à l'autre dès que N3 est en place. La plupart des outils travaillent en espace monde ou écran : **à inventorier**.
- **Remplissage (fill layers)** : évalué par tuile, avec les UV réelles via `_udim_offset`, donc continu.

### 6.5 Dilatation et padding

- Carte UV et carte de dilatation **par tuile**, à la résolution de la tuile (aujourd'hui une seule carte sur le maillage fusionné).
- Plus de débordement d'une tuile sur sa voisine (L6 disparaît) : chaque tuile est une texture indépendante, avec un bord en `clamp`.
- Le rayon de dilatation doit être exprimé **en proportion** de la résolution de la tuile, pas en pixels fixes. Sinon, un rayon de 16 px en 2K et en 8K ne couvre pas la même chose.

### 6.6 Composition, masques, groupes, bake

- `layers_flatten`, les masques, les groupes et les modes de fusion tournent tuile par tuile. Le code reste celui d'aujourd'hui, appelé dans la boucle.
- **Bake** (AO, courbure, normales depuis high-poly) : une passe par tuile, cible à la résolution de la tuile.
- **Raytrace** : il utilise aujourd'hui l'atlas du maillage fusionné (`config_is_raytrace_multi`, 64 objets au plus). Deux possibilités :
  - le garder comme **proxy basse résolution** pour l'éclairage ;
  - bake par tuile.

  C'est le point le moins clair. Il faut **garder l'atlas raytrace indépendant de l'atlas UDIM**, alors qu'ils partagent aujourd'hui la même mécanique de cases.

### 6.7 Export

- **Per UDIM Tile** : une passe par tuile, **sans recadrage ni ré-agrandissement**, à la résolution de la tuile (N4). `<UDIM>` vient de `udim_tiles[t].id` et non plus d'un nom d'objet.
- **UDIM × jeux de textures** : un jeu de textures devient un ensemble de tuiles. L'export écrit `<atlas>` × `<UDIM>` (N8). Notre `studio_export` sait déjà nommer les deux ; il suffit de lui passer la tuile au lieu de `object_name`. Fait au lot D1 dans l'atlas actuel (`udim-export-sets`) : `<fichier>_<jeu>_<canal>.<tuile>`, `<atlas>` et `<UDIM>` dans le preset studio.
- **Résolution par map** (studio) : elle se combine avec la résolution par tuile par une échelle relative (« map ½ de la tuile »), ou alors on la retire en mode UDIM. **Décision de produit à prendre.**
- CLI `--export-textures` : un argument de mode (L15). Au passage, ça rend notre `studio_export_run` moins nécessaire.

### 6.8 Mémoire et undo (phase 4, indispensable en pratique)

**Estimation.** Personnage de 10 tuiles 4K, 8 calques 16 bits : 10 × 8 × 3 × 128 Mo ≈ **31 Go**. Même à 5 tuiles 4K et 6 calques : 12 Go. Leviers, par ordre de rendement :

1. **Allocation paresseuse par tuile.** Un calque de peinture n'alloue une tuile qu'au premier coup de pinceau. Un calque « yeux » peint sur une seule tuile coûte 1/10. `texpaint == NULL` est lu comme transparent par le composite.
2. **Canaux paresseux.** `texpaint_nor` et `texpaint_pack` ne sont alloués que si le calque peint la normale, la rugosité ou le métal (`paint_nor`, `paint_rough`…). Un calque de couleur seule divise sa mémoire par 3.
3. **Undo par tuile sale.** Le snapshot d'undo ne copie que les tuiles `dirty` du trait (1 ou 2), au lieu du calque entier. Pour 10 tuiles, c'est une division par 5 à 10, et l'undo n'a plus à tomber à 1 pas.
4. **Éviction vers la RAM.** Les tuiles non affichées depuis N secondes (caméra, isolation de tuile) sont compressées en LZ4 en RAM, avec le code du `.arm`, puis rechargées à la demande. C'est un « streaming du pauvre », à peu de frais puisque le codec existe.
5. **8 bits par défaut** pour les calques de couleur. Un réglage par calque existe déjà (`slot_layer_resize_and_set_bits`).
6. **Fill layers non cuits** à étudier : ils sont aujourd'hui rendus dans leur texture, alors qu'ils pourraient être évalués au composite. C'est un gain important, mais il touche au modèle de calque. Hors périmètre.

Le moniteur VRAM du plan OCIO/export (si on le fait) doit afficher le détail **par tuile**.

### 6.9 Format `.arm` et migration

- **Nouveau bloc `udim_tiles`** au niveau projet : id, résolution, jeu de textures.
- Chaque `layer_data_t` gagne un tableau `tiles[] = { tile_index, res, blob_lz4 × 3 }`. Les tuiles jamais peintes sont absentes, donc le fichier est plus petit.
- **Version de format incrémentée.** Un ancien ArmorPaint ne lira pas ces fichiers : c'est à documenter.
- **Migration d'un projet `99156dfc`** (atlas en grille) :
  1. détecter les objets `nom.1xxx` ;
  2. les refusionner en un objet à plages ;
  3. découper chaque calque partagé case par case en tuiles, à `res/stride`, ré-agrandi à la résolution de tuile choisie. **La perte d'origine reste**, ce qui doit être signalé ;
  4. affecter à leur tuile les calques de tuile, qui sont déjà pleine résolution.
- **Migration d'un projet non UDIM** : une tuile 1001, rien d'autre ne change.
- **Sidecar studio** (`.export_settings.json`) : les jeux de textures y sont déjà. Ajouter la résolution par tuile **dans le `.arm`**, pas dans le sidecar, puisque c'est une donnée de peinture et non d'export.

### 6.10 Interface

- Onglet **Meshes** : un nœud par objet, avec ses tuiles en enfants. Chaque tuile affiche sa résolution (menu), sa VRAM et une action « isoler » (n'afficher et ne peindre que cette tuile). Pas fait au lot D1 : avec la phase 2.
- Import : l'option « Split By: UDIM Tile » disparaît au profit d'une case **« UDIM » cochée automatiquement** dès que des UV sortent de 0-1. Le split par objet ou matériau reste disponible à côté. Lot D1 : la case « UDIM Tiles » est là, à côté d'Object / Material ; elle ne se coche pas encore d'elle-même.
- Fenêtre d'export studio : la ligne « N UDIM tiles » devient un tableau des tuiles (id, résolution, jeu). La section Jeux de textures ne se masque plus en mode UDIM. Lot D1 : la section des jeux s'affiche en Per UDIM Tile, avec le résumé « N texture set tiles » ; le tableau des tuiles reste à faire.

---

## 7. Feuille de route

| Phase | Contenu | Livrable testable | Estimation |
|---|---|---|---|
| **0 — Correctifs sans regret** ✅ 2026-09-27 | L11 à L14 dans l'importeur OBJ et FBX (refus avec message des uv hors 1001-1990, triangles à cheval coupés, plafond FBX dynamique, un objet par objet et par tuile). Erreur explicite au-delà de 64 tuiles (L3). Résolution projet conseillée à l'import (tableau du §3). **Reportés :** UV matériau réelles (L8) en phase 1 : un uniforme `_udim_offset` par objet ne s'applique pas au maillage fusionné, peint en un seul draw, alors que la phase 1 dessine par plage ; gouttière et clamp entre cases (L6) en phase 2, qui supprime l'atlas qu'ils corrigeraient. | `.tests/obj.sh` (udim/), `.tests/fbx.sh`, `.tests/udim_app.sh` : UV négatives, u ≥ 10, triangles à cheval, 70 tuiles (atlas, peinture, export), conseil de résolution | **8-12 j** |
| **1 — Tuile = donnée, maillage à plages** 🟡 2026-09-27, en partie | **Fait (lot D1)** : UV matériau = UV UDIM réelles (L8, reporté de la phase 0), par un décalage par objet de tuile ; split par matériau et par tuile ; jeux de textures × tuiles à l'export ; renommage qui garde la tuile. **Passé en phase 2** : `g_project->udim_tiles` sauvé dans le `.arm`, import sans split avec plages d'index, rendu viewport par plage, migration des objets `nom.1xxx` (voir « Lot D1 » ci-dessous). Stockage encore en atlas. | `.tests/udim_app.sh` sections 6 à 9, `.tests/sets_app.sh` section 6, `.tests/fbx.sh` : bruit procédural continu entre 1003 et 1004 et entre 1001 et 1011, `skin` et `cloth` par tuile, export jeux × tuiles | **10-15 j** (la part reportée est à réestimer avec la phase 2) |
| **2 — Jeu de textures par tuile** | Reçu de la phase 1 : `udim_tiles` dans le `.arm`, maillage à plages (un draw par plage dans les trois backends), migration des objets `nom.1xxx`. `layer_tile_t`, `layers_set_tile`, boucles dans la peinture, le flatten, les masques, la dilatation, le bake et l'export. Nouveau bloc `.arm` et migration depuis l'atlas. Suppression de l'espace atlas UDIM (`tex_coord_atlas`, `layers_udim_remap`), ce qui corrige L17. **Résolution encore uniforme.** | Export Per UDIM Tile bit à bit identique à un rendu d'une seule tuile en projet 1 tuile ; peinture continue à travers 1001/1002 | **20-30 j** |
| **3 — Résolution par tuile** | `res_x`/`res_y` par tuile, UI Meshes, dilatation proportionnelle, inventaire et correction des outils en texels, export natif par tuile | Visage 8K + corps 2K, export aux deux résolutions, sans ré-échantillonnage | **6-10 j** |
| **4 — Mémoire** | Allocation paresseuse par tuile et par canal, undo par tuile sale, éviction LZ4, moniteur VRAM par tuile | 10 tuiles 4K × 8 calques qui tiennent sur une machine de 24 Go | **12-20 j** |
| **Total B** | | | **~56-87 j** (phase 0 comprise) |

- Les phases 0 et 1 sont **utiles même si on s'arrête là**.
- La phase 2 est **le point de non-retour pour le fork** : format `.arm` divergent et gros patches dans les fichiers chauds.
- La phase 3 n'a pas de sens sans la phase 4 sur des assets réels.

### Lot D1 (2026-09-27) : fait, reporté

**Décision : un objet de peinture par tuile, comme l'upstream.** La phase 1 prévoyait de remplacer les objets
`nom.1xxx` par des plages d'index d'un seul maillage. Le lot D1 les garde, pour trois raisons :
- iron ne sait dessiner qu'un index buffer entier (`gpu_draw_internal`, sous Metal `indexCount:current_ib->count` et
  `indexBufferOffset:0`) : dessiner par plage demande un draw à plage dans les trois backends (Metal, D3D12, Vulkan) ;
- chaque objet de tuile a déjà son draw : un uniforme `_udim_offset` par objet suffit pour L8, sans plages ;
- le `.arm` ne change pas. Il reste lisible par ArmorPaint et par les builds studio précédents : aucun format ne
  diverge avant le go / no-go de la phase 2.

La tuile-donnée (`udim_tiles` dans le `.arm`), l'import sans split à plages d'index, le rendu par plage et la migration
des objets `nom.1xxx` passent donc à la phase 2, qui change de toute façon le format et rend par tuile.

**Fait** (patches `udim-core`, `udim-material-uv`, `udim-export-sets`, `udim-tile-name`, et la partie D1
d'`udim-import` et `fbx-split` ; détail et mutations dans `paint/plugins/external/PATCHES.md`) :
- UV matériau = UV UDIM réelles (L8) : les calques partagés (et limités à un jeu) sont peints objet de tuile par objet
  de tuile avec `_udim_offset`, les calques d'une tuile lisent `_udim_tile`. Un bruit procédural est continu de 1003 à
  1004 et de 1001 à 1011, et le même sur le calque d'une tuile que sur le calque partagé. Le bake et les outils qui
  pointent (Picker, ColorID, Material, Cursor) gardent le maillage fusionné ;
- import OBJ et FBX « Material » + « UDIM Tiles » : un objet par matériau et par tuile (`skin.1001`), rassemblé d'un
  objet du fichier à l'autre, et un jeu de textures par matériau ;
- export Per UDIM Tile par jeu : `<fichier>_<jeu>_<canal>.<tuile>`, chaque jeu sur ses seules tuiles ; le preset
  studio remplit `<atlas>` et `<UDIM>` ; la fenêtre d'export montre les jeux dans ce mode. Deux bugs upstream des
  masques de jeu corrigés en route : l'export par tuile d'un calque limité à un jeu plantait (lecture hors du tableau
  des objets), et Selected avec des jeux sortait tous les calques ;
- renommer un objet dans l'onglet Meshes garde sa tuile (L4 en partie) ;
- scripts `studio_layer_new` et `studio_object_rename`, pour les bancs.

Tests : `.tests/udim_app.sh` sections 6 à 9, `.tests/sets_app.sh` section 6, `.tests/fbx.sh` (matériau × UDIM) ;
sans le correctif (mutations), leurs contrôles échouent.

**Limites connues après D1**
- L17 (§3) : les cases se décalent quand la table des tuiles change. Pas de garde en D1 ; le stockage par tuile de la
  phase 2 la corrige. En attendant : importer toutes les tuiles avant de peindre (README d'`external/`).
- Les jeux d'une tuile partagent sa case d'atlas : là où leurs uv se chevauchent, un calque partagé donne les mêmes
  texels aux deux jeux. Les calques limités à un jeu restent séparés.
- Après un import en ajout, les jeux ne correspondent plus aux objets : la fenêtre les ignore (« Atlas 1 ») et l'export
  par tuile sort sans jeux, jusqu'à ce qu'on les redéfinisse. Un ajout ne crée pas de jeux par matériau.
- En Selected, le preset studio met le nom du jeu dans `<object>`.
- Vue 2D : le plan 2D n'est pas un objet de tuile ; les matériaux y gardent l'espace de la case.
- La tuile vient toujours du nom : un objet `truc.1005` hors UDIM reste une tuile (L4).
- Onglet Meshes : les tuiles d'un objet ne sont pas regroupées sous lui (§6.10).

### Lot D2 (2026-09-27) : plan

Go donné le 2026-09-27, sans attendre la recette C6. Branche `d2`, fusionnée dans `studio` quand tous les bancs passent.

**Périmètre.** Un jeu de textures par tuile pour chaque calque, résolution uniforme (celle du projet). Le `.arm` change
et la migration des projets atlas se fait à l'ouverture. **Reportés en D3**, avec l'onglet Meshes et la résolution par
tuile :
- le maillage à plages d'index. iron ne dessine qu'un index buffer entier, et il faudrait un draw à plage dans les
  trois backends. Le contexte de tuile n'en dépend pas : chaque objet de tuile a déjà son draw ;
- la tuile-donnée (`udim_tiles` dans le `.arm`). La table reste déduite des noms d'objets. Les données d'un calque
  sont rangées par numéro de tuile, pas par case : L17 disparaît quand même.

**Contexte de tuile par échange de contenu.** Le pointeur `gpu_texture_t *` d'une texture de calque est recopié
partout : `_image` de la render target, emplacements d'undo, calques live, caches. Le rendre variable par tuile
toucherait tous ces endroits. On garde donc le pointeur comme une **vue** stable, et c'est son contenu qui change :
- `util_udim.c` tient, pour chaque vue, un magasin `tiles[t]` de contenus `gpu_texture_t` (par valeur), indexé par
  numéro de tuile ;
- `util_udim_set_tile(t)` range le contenu courant dans son emplacement et met celui de `t` à sa place, dans toutes
  les vues, après avoir vidé les barrières en attente sur les cibles de rendu ;
- le code existant (peinture, flatten, fusion, dilatation, échanges d'undo) travaille sans changement sur la tuile
  courante ;
- `gpu_texture_destroy` copie déjà le contenu de la structure : le contenu fait office d'identité de la ressource ;
- le code qui remplace le pointeur d'une vue (`slot_layer_invert_mask`, `slot_layer_resize_and_set_bits`) passe au
  remplacement de contenu.

**Calques à tuiles** : calques et masques quand l'UDIM est actif et `uv_map != 1`, sauf ceux limités à un objet hors
tuile.
- Quand l'UDIM devient actif, la texture existante va à la première tuile, et les autres sont allouées puis vidées ; les
  calques de remplissage sont re-remplis.
- Une tuile qui apparaît dans la table est allouée, une qui disparaît est libérée.
- Toutes les tuiles d'un calque sont allouées d'emblée (l'allocation paresseuse est pour D4). Même mémoire que l'atlas
  à résolution de tuile égale, mais un projet 4K à 5 tuiles pèse 5 fois plus qu'avant : la résolution du projet
  devient celle de chaque tuile. Les emplacements d'undo et `texpaint_blend0/1` n'allouent que les tuiles touchées.

**Viewport.** Le shader de maillage échantillonne en `tex_coord` (UV locale), et `tex_coord_atlas` disparaît. Les
liens des calques à tuiles (`_texpaint<i>`, `_nor`, `_pack`, `_texpaint_vert`) sont résolus par objet, d'après sa
tuile : la vue si c'est la tuile courante, l'emplacement du magasin sinon. L'aperçu live du pinceau ne s'affiche pas sur
les calques à tuiles en D2.

**Peinture.** `render_path_paint_commands_paint` boucle sur les tuiles touchées, pour la seule branche de peinture (pas
les pipettes). Pour chaque tuile : `set_tile`, instantané d'undo et de blend si besoin, corps inchangé, puis dessin des
seuls objets de la tuile. UV matériau = UV locale + `_udim_tile`.
- Tri des tuiles pour les outils en forme de pinceau :
  - mode monde : les 8 coins de la boîte de la tuile projetés à l'écran, rectangle comparé au segment du trait élargi
    du rayon écran majoré ;
  - caméra orthographique : rayon écran exact ;
  - mode aligné caméra : borne prise sur la profondeur minimale de la scène ;
  - xray, décal, texte, particules, remplissage, bake : toutes les tuiles.
- La symétrie reprend tous les objets de la tuile.
- Le clone d'une tuile à l'autre n'est pas pris en charge (documenté).

**Undo.** Un pas de peinture prend un instantané de la tuile au premier passage et n'échange que les tuiles qu'il
tient. Les pas génériques échangent les vues entières, donc toutes les tuiles. Un emplacement réutilisé pour un pas de
peinture libère les tuiles qu'il ne tient plus.

**Cartes et pipettes.**
- Dilatation, carte UV, carte des triangles et îlots UV sont calculés par tuile, depuis les objets de la tuile, chacun
  avec sa validité.
- Le gbuffer de pick écrit le numéro de tuile. La pipette de calque ou de masque le lit, et refait sa passe sur la bonne
  tuile.

**Maillage fusionné.** Il garde ses UV d'atlas, seulement comme proxy du raytrace et du bake raytrace : un flatten par
tuile, rangé dans les cases du proxy. Les pipettes dessinent encore le maillage fusionné.

**Calques et export.**
- Le flatten, la fusion de calques, les masques et l'export tournent tuile par tuile.
- L'export Per UDIM Tile se fait à la résolution native, sans recadrage ni ré-agrandissement.
- L'export Visible d'un projet UDIM sort le composite en atlas.

**`.arm`.**
- Chaque calque gagne un tableau `tiles` (`{tile, texpaint, texpaint_nor, texpaint_pack}`), ajouté après `path_text`.
- Le blob d'origine reçoit le composite en atlas, à la résolution du projet. Un ArmorPaint officiel ouvre donc le
  fichier, avec la perte de l'atlas.
- À l'ouverture d'un fichier de l'ère atlas, chaque case est recadrée puis agrandie à la résolution du projet, et les
  calques d'une tuile vont à leur tuile.

**Sous-lots** : chacun a ses tests, son commit et ses entrées dans `PATCHES.md`.

| Sous-lot | Contenu |
|---|---|
| D2a | Magasin, contexte de tuile, viewport ; création, suppression, vidage, duplication et redimensionnement des calques ; undo générique |
| D2b | Boucle de peinture, tri des tuiles, undo, blend et cartes par tuile, pipettes, remplissage, bake |
| D2c | Flatten et opérations de calques (fusion, masques, groupes) |
| D2d | Export |
| D2e | `.arm` et migration |
| D2f | Proxy raytrace, nettoyage (`layers_udim_remap` et l'espace atlas UDIM), docs, patches, bancs |

**Tests.**
- Export par tuile bit à bit identique à celui d'un projet d'une seule tuile.
- Peinture continue à travers la couture 1001/1002.
- Undo et redo d'un trait sur deux tuiles, avec les tuiles tenues par chaque pas.
- Aller-retour save/load.
- Migration de la fixture `.tests/fixtures/udim_atlas` (projet 256 px, 5 tuiles, écrit par le build D1), comparée à
  son export d'origine.
- Blob d'origine = composite en atlas.
- L17 : la peinture suit son numéro de tuile quand la table change.
- Sections 4 et 10 d'`udim_app.sh` (propres à l'atlas) réécrites.

### Lot D2a (2026-09-27) : fait

Patch `udim-tiles` (points d'appel) et `util_udim.c` (patch `udim-core`). Banc `.tests/udim_tiles_app.sh`, 19 contrôles.
- **Magasins.** Chaque texture de calque à tuiles (base, nor, pack ; la texture d'un masque) est une vue dont le
  contenu est celui de la tuile du contexte ; les autres tuiles attendent dans le magasin de la vue.
  `gpu_delete_texture` prévient `util_udim.c` (pointeur `gpu_texture_deleting` dans `iron.h`) : une vue supprimée
  libère ses tuiles. Une tuile absente de la vue (objet sans tuile, tuile non tenue) montre un 1 × 1 transparent
  partagé.
- **Règle et table.** `util_udim_sync()` avant chaque rendu : calques et masques à tuiles quand la table a au moins
  deux tuiles (`uv_map != 1`, pas un calque limité à un objet sans tuile) ; un calque qui passe aux tuiles garde sa
  texture sur la tuile du contexte, les autres tuiles reçoivent son fond (texel (0, 0)) ; une tuile qui apparaît dans
  la table reçoit aussi le fond de chaque calque, une qui disparaît est libérée ; UDIM coupé : une texture par calque,
  celle de la tuile qui reste. Les tuiles sont rangées par numéro : un maillage ajouté dans une nouvelle tuile ne
  déplace plus la peinture (L17).
- **Vue 3D.** Le gbuffer dessine chaque objet avec sa tuile (`util_udim_draw_meshes`) ; les calques sont lus en
  `tex_coord` (plus d'espace atlas dans `make_mesh`).
- **Opérations.** Clear, Invert Mask et Duplicate bouclent sur les tuiles ; le redimensionnement (résolution du
  projet) redimensionne chaque tuile en place, emplacements d'undo compris ; Delete passe par l'undo.
- **Undo générique.** `history_copy_to_undo` sauve toutes les tuiles d'un calque dans l'emplacement (un contenu par
  tuile) ; l'échange d'undo (`slot_layer_swap`) échange les tuiles que l'emplacement tient. Un emplacement qui avait
  sauvé tout le calque reçoit, à l'échange, les tuiles apparues depuis dans la table : un Redo qui refait l'opération
  sur toutes les tuiles (Clear) puis un Undo rendent la tuile ajoutée telle qu'elle était.
- **Pas encore (D2b à D2f).** La peinture d'un projet UDIM est cassée sur la branche `d2` : elle écrit encore en
  espace atlas dans la tuile du contexte. Pas de calque live (aperçu du pinceau, outil Material) sur un calque à
  tuiles. Flatten, fusion, export et `.arm` travaillent sur la tuile du contexte seule.

### Lot D2b (2026-09-27) : fait

Patch `udim-tiles` (points d'appel D2b) et `util_udim.c` (patch `udim-core`). Un bug upstream a été corrigé en route
(patch `bake-derivative`). Banc `.tests/udim_tiles_app.sh`, sections 7 à 10 : 49 contrôles en tout.
- **Peinture.** `render_path_paint_commands_paint` peint un calque à tuiles tuile par tuile. `util_udim_paint_tiles`
  retient les tuiles touchées par le trait :
  - pinceau, gomme, clone et flou : boîtes des objets projetées à l'écran, comparées au segment du trait élargi du
    rayon ; en caméra ortho, le rayon exact ; aligné sur la caméra, la profondeur de l'objet le plus proche ;
  - xray, décalque, texte, particules, remplissage et bake : toutes les tuiles.

  Pour chaque tuile retenue, il met la tuile et rappelle le corps upstream, qui ne dessine que les objets de cette
  tuile, dans leurs uv locales (matériaux : uv + `_udim_tile`). Le trait est continu à la couture 1001 | 1002. Au-delà
  des 64 tuiles tenues (`UDIM_MAX_TILES`), le pinceau ne peint rien.
- **Symétrie.** Le miroir de l'objet de peinture passe par la même boucle. Les tuiles d'en face sont retenues d'après
  les boîtes en miroir et peintes dans le même pas d'undo. Scripts : `studio_paint_option("sym_x", 1)`. La symétrie
  est peinte par la frame (`render_path_paint_draw`), pas par `script_paint_world`.
- **Undo.** Copie sur écriture : `gpu_begin` prévient `util_udim.c` (pointeur `gpu_target_beginning` dans
  `iron_gpu.h`). Un pas sauve la tuile du contexte, puis chaque tuile à son premier dessin. Il ne tient que les tuiles
  qu'il a changées, et un trait dans le vide ne tient rien. Undo et redo n'échangent que ces tuiles.
- **Blend et dilatation.** `texpaint_blend0/1` reçoivent un contenu par tuile peinte, libéré en fin de trait. La
  dilatation passe par les tuiles peintes, chacune avec sa carte.
- **Cartes et pipettes.** Les cartes UV, de triangles et de dilatation sont calculées par tuile. Le gbuffer écrit la
  tuile de chaque objet (`gbuffer2.r`, tuile - 1000). Les pipettes de calque et de masque travaillent sur la tuile
  sous le pinceau. Les remplissages de face et d'îlot UV ne peignent que cette tuile, et l'undo ne tient qu'elle.
- **Vue 2D.** Un menu choisit la tuile montrée et peinte. Le point de la pipette n'est dessiné que sur la tuile piquée.
- **Bakes.** Nœud Bake Texture et bakes de calque se font tuile par tuile, dans les uv de la tuile : Texcoord,
  Position, Object ID, etc. L'aperçu Curvature d'un calque à tuiles a un aperçu par tuile. Pour le high poly :
  - le high est l'objet choisi et les autres tuiles de son maillage : même nom au suffixe de tuile près (`high.1001`,
    `high.1002`). Un objet `high.001.1001`, venu d'un OBJ coupé en parties, n'en fait pas partie ;
  - une tuile n'est bakée que si elle porte un low, c'est-à-dire un objet du calque autre que le high. Les autres
    tuiles restent vides, et un high choisi égal au low donne du noir ;
  - l'upstream dessinait tout le maillage fusionné dans les deux passes, le high par-dessus le low ; désormais chaque
    passe ne dessine, sur chaque tuile, que le high (première passe) ou le low (suivantes). Les bakes Normal (Object)
    et Position simples dessinent toujours tous les objets.

  Normal, Height et Derivative sont vérifiés à la valeur près : high plat ou incliné, par tuile.
- **Bug upstream corrigé (`bake-derivative`).** Le bake Derivative sautait sa passe de hauteur et prenait la dérivée
  des positions : 0,588 au lieu de 0,5 pour un high confondu avec le low.
- **Scripts pour les bancs** (`studio_atlas.c`) :
  - `studio_paint_option` : `camera_align`, `ortho`, `xray`, `sym_x/y/z`, `fill_type` ;
  - `studio_udim_pick` ;
  - `studio_clone_source` ;
  - `studio_bake`, `studio_bake_state` : le bouton Bake d'un nœud ; bake lancé depuis `update()`, puisque le nœud vide
    sa texture à la frame suivante ;
  - `studio_udim_pixel`, canaux 3 à 8 : cartes UV, de triangles et de dilatation, pipette, aperçus de nœud,
    emplacements d'undo.
- **Limites** (voulues ou upstream) :
  - pas d'aperçu live du pinceau sur un calque à tuiles, ni de calque live pour l'outil Material (tout D2) ;
  - pas de clone d'une tuile à l'autre : une source sur une autre tuile ne peint rien ;
  - ColorID, flou et smudge ne lisent que la tuile peinte ;
  - xray tient toutes les tuiles à chaque pas ;
  - upstream : un maillage importé en ajout n'a pas la mise à l'échelle du premier (`viewport_scale_to_bounds`). Un
    high poly ajouté à part donne donc une hauteur fausse ; il faut mettre low et high dans le même fichier ;
  - upstream : `cotangent_frame` donne NaN sur des normales constantes. Le low d'un bake de normales doit avoir des
    normales qui varient (lissées).

  Non testés : sculpt, particules, décalques et texte sur un calque à tuiles (même chemin que le pinceau).

### Lot D2c (2026-09-27) : fait

Patch `udim-tiles` (points d'appel D2c) et `util_udim.c` (patch `udim-core`). Banc `.tests/udim_tiles_app.sh`,
section 11 : 59 contrôles en tout.
- **Deux opérations feuilles.** Toutes les opérations de calque passent par `layers_merge_layer` (fusion d'un calque
  dans un autre) ou `layers_apply_mask` (masque appliqué). Sur un calque à tuiles, chacune se rappelle pour chaque
  tuile (`util_udim_merge_layer`, `util_udim_apply_mask`). Merge Down, Merge Group, Apply Mask et les masques de
  groupe sont donc faits tuile par tuile ; l'undo passe par la copie sur écriture de D2b (vérifié pour Merge Down et
  Apply Mask).
- **Flatten.** `layers_flatten` compose la tuile du contexte, où chaque calque à tuiles est lu tel quel ; il n'est
  plus recopié dans une case d'atlas. Ses appelants bouclent sur les tuiles dont ils ont besoin (export en D2d).
  Scripts : `studio_udim_pixel`, canaux 9 à 11 (base, nor, pack du flatten d'une tuile).
- **Boucles imbriquées.** Une boucle sur les tuiles lancée dans une autre (Merge Group → fusion de chaque enfant) ne
  refait pas le tour : `util_udim_loop_begin` rend 1 dès qu'un niveau parcourt les tuiles.
- **Surcharges de matériau des maillages** (onglet Meshes, un matériau glissé sur un maillage). Le flatten d'un
  projet UDIM les dessine par-dessus les calques. Elles le sont désormais sur la tuile de leur objet, dans ses uv,
  plus dans sa case d'atlas.
- **ColorID → To Mask.** Le masque est écrit sur toutes les tuiles, la carte d'id lue dans les uv de chaque tuile,
  comme le fait le pinceau ColorID.
- **Scripts pour les bancs** (`studio_atlas.c`) : `studio_layer_op` `merge_down`, `merge_group`, `apply_mask` ;
  `studio_layer_set_parent` ; `studio_mesh_override` ; `studio_colorid_mask`.
- **Limites** :
  - une fusion d'un calque à tuiles dans un calque qui n'en a pas (limité à un objet sans tuile) ne lit que la tuile
    du contexte ;
  - upstream, non corrigé : des masques empilés sur un calque sont fusionnés en MIX dans le flatten (donc l'export) :
    le dernier créé, le plus haut, remplace les autres, alors que la vue 3D les multiplie. Le banc le vérifie, pour
    voir si l'upstream le change ;
  - upstream, non corrigé : l'Undo d'un Merge Group ne rend pas le calque de base (même sans UDIM).

---

## 8. Coût de maintenance du fork

- **Surface.** La phase 2 touche environ 12 fichiers upstream, dont les 6 plus actifs (§4.8). Il faut compter **une demi-journée à deux jours de résolution de conflits par pull** sur la durée de vie du fork, davantage si upstream continue à retravailler les UDIM, ce qui est probable après `99156dfc`.
- **Format.** Des `.arm` écrits par notre build ne s'ouvrent plus dans un ArmorPaint officiel. Il faut prévoir un **export « compatible upstream »**, qui écrit l'atlas en grille avec perte, si des fichiers doivent circuler hors studio.
- **Règle `PATCHES.md`.** Un `STUDIO-PATCH(udim-*)` par point d'entrée : `udim-tiles`, `udim-ranges`, `udim-layer-tiles`, `udim-arm`, `udim-export`. Au lot D1 : `udim-core` (le fichier `util_udim.c`), `udim-material-uv`, `udim-export-sets`, `udim-tile-name`. Chacun a son `.patch` et son `.base`, et on accepte que `check_patches.sh` en signale souvent. Le registre existant supporte ce fonctionnement, mais le volume change d'ordre de grandeur (aujourd'hui : injections de quelques lignes).
- **Pour limiter la surface :**
  - mettre la logique (table des tuiles, allocation, éviction, migration) dans un **nouveau fichier** `paint/sources/util/util_udim.c`, qu'upstream ne touche pas ;
  - ne garder dans les fichiers chauds que les **boucles** `for tile … layers_set_tile(tile)` et les points d'appel ;
  - ne pas mettre ce fichier dans `external/` : c'est du cœur, il doit compiler dans le unity build avec le reste de `util/`.
- **Pas de PR upstream** (décision du 2026-09-26). On ne peut donc pas espérer « reverser » l'option B pour réduire le coût. En revanche, **suivre ce qu'upstream fait des UDIM** avant chaque phase est obligatoire : s'il part vers une résolution par tuile, on s'aligne sur son modèle au lieu de diverger.

---

## 9. Recommandation

> Décision prise le 2026-09-26 : **option B**. Les points 2 et 3 ci-dessous ne décident plus du choix d'architecture, ils servent à dimensionner les phases 3-4 (résolutions et mémoire réelles).

1. **Maintenant : phase 0**, 8 à 12 j. Elle corrige des bugs réels (L3 et L12 peuvent corrompre la peinture ou la mémoire), rend le système actuel honnête et ne ferme aucune porte. Les patches sont petits et localisés.
2. **Mesurer le besoin réel** avant d'aller plus loin. Sur 3 à 5 assets représentatifs du studio : nombre de tuiles, résolution par tuile attendue, nombre de calques. Si la réponse est « ≤ 4 tuiles en 4K », **le système actuel en projet 8K suffit** (tableau du §3). Dans ce cas, s'arrêter à la phase 0 plus la phase 1 (split libéré, jeux × UDIM).
3. **Si le besoin dépasse** (personnages de 10 tuiles et plus, résolution mixte), faire la phase 1 **sur une branche de prototype**, puis décider la phase 2 en go / no-go sur ces critères :
   - le besoin est confirmé ;
   - l'état d'upstream sur les UDIM à ce moment-là ;
   - l'acceptation explicite de l'exception à la règle des injections de 1 à 3 lignes ;
   - le format `.arm` divergent est acceptable.
4. **Ne pas faire l'option C maintenant.** L'API « contexte de tuile » de B permet de passer plus tard aux texture arrays sans toucher au code de peinture, si le nombre de draws devient un problème (100 tuiles et plus).
5. **Écarter l'option D**, sauf changement de statut d'ArmorPaint dans le pipeline.

---

## 10. Questions ouvertes et vérifications à faire

- [x] **L12** : écriture hors tableau confirmée le 2026-09-26. Deux triangles, le second en u ∈ [-1,8 ; -1,2], import `Split by: UDIM Tile` : ASan signale un `heap-buffer-overflow` sur `num[tile] += 3` (`iron_obj.c:569`), 4 octets avant le tableau. Corrigé par le lot D0 le 2026-09-27 (patch `udim-import`, cas `neg` d'`.tests/obj.sh`).
- [ ] **L17** : un garde en attendant la phase 2 ? Avertir quand un ajout ou une suppression change la table des tuiles d'un projet déjà peint, ou recopier les cases des calques d'atlas.
- [ ] Inventorier les outils qui travaillent **en texels** (flou, smudge, clone, taille de brosse en pixels, rayon de dilatation) pour la phase 3.
- [ ] Composite par tuile : mesurer le coût de `layers_flatten` × N tuiles à chaque trait. Faut-il un cache par tuile, ou recomposer seulement la tuile sale ?
- [ ] Raytrace et bake : garder l'atlas du maillage fusionné comme proxy, ou passer au bake par tuile ?
- [ ] `kong` : vérifier s'il sait exprimer des texture arrays, pour garder ouverte la porte de l'option C.
- [ ] Choix de produit : **résolution par map × résolution par tuile** (§6.7) et comportement des calques de tuile existants après migration.
- [ ] Mettre à jour `ARMORPAINT_ANALYSE.md` §5.2 et §6 (tableau des limites) avec les faits de `99156dfc`, ou les renvoyer ici.

---

## Annexe — Références de code (upstream `99156dfc`)

| Sujet | Emplacement |
|---|---|
| Parse OBJ, tuiles, barycentre, renormalisation | [iron_obj.c:232](base/sources/iron_obj.c:232), [:553-612](base/sources/iron_obj.c:553), [:620-629](base/sources/iron_obj.c:620) |
| Objets par tuile (OBJ) | [import_obj.c:16-57](paint/sources/io/import_obj.c:16) |
| Split FBX | [io_fbx.c:10](paint/plugins/io_fbx/io_fbx.c:10), [:215-310](paint/plugins/io_fbx/io_fbx.c:215) |
| Table des tuiles, grille, calques UDIM | [util_mesh.c:113-195](paint/sources/util/util_mesh.c:113) |
| Maillage fusionné en cases | [util_mesh.c:327-370](paint/sources/util/util_mesh.c:327) |
| Uniformes atlas | [uniforms.c:94](paint/sources/uniforms.c:94), [:228](paint/sources/uniforms.c:228) |
| Shaders | [make_mesh.c:142](paint/sources/render/make_mesh.c:142), [make_paint.c:207](paint/sources/render/make_paint.c:207), [make_texcoord.c:133](paint/sources/render/make_texcoord.c:133) |
| Carte UV / dilatation | [util_uv.c:28](paint/sources/util/util_uv.c:28), [:125](paint/sources/util/util_uv.c:125) |
| Remap atlas ↔ tuile | [util_layer.c:1072-1120](paint/sources/util/util_layer.c:1072) |
| Undo réduit dès 8K | [util_layer.c:70](paint/sources/util/util_layer.c:70) |
| Export par tuile | [export_texture.c:226-310](paint/sources/io/export_texture.c:226) |
| `.arm` : résolution carrée | [import_arm.c:324](paint/sources/io/import_arm.c:324), [:615](paint/sources/io/import_arm.c:615) |
| Limites GPU | [iron_gpu.h:21](base/sources/iron_gpu.h:21), [metal_gpu.m:102](base/sources/backends/metal_gpu.m:102) |
| Maillages de test | `paint/plugins/external/.tests/udim_gen.py`, `.tests/sets_app.sh` |
