# ArmorPaint — analyse technique de référence

> Document de travail. Sert de base à toute intervention future sur ce dépôt.
> **Commit analysé** : `2efd0ff3` (branche `main`, 2026-09-14)
> ⚠️ **2026-09-26** : `main` mis à jour sur l'upstream `85f6cf1c` (+60 commits, 310 fichiers). Le build est passé de `project.js` à `project.c` (amake en C) et plusieurs fichiers ont bougé (`slots/`, `util/`, `uv_unwrap`). Les références fichier:ligne ci-dessous datent de `2efd0ff3` : les revérifier avec graft. Notre travail est sur la branche locale `studio`, et le code studio dans `paint/plugins/external/` (dépôt séparé, voir §16).
> **Analyse faite le** : 2026-09-14
> Toute ligne citée (`fichier.c:123`) vaut pour ce commit — revérifier avant d'agir si le dépôt a bougé.

---

## 1. Identité du projet

| | |
|---|---|
| Nature | Logiciel de texture painting 3D PBR (concurrent de Substance Painter, pas de Mari) |
| Langage | **C pur** (C11) — **depuis mars 2026 seulement.** Voir §1.1. |
| Licence code | zlib/libpng (`license.md`) — permissive, usage commercial et fork autorisés. Détail en §11. |
| Modèle éco | Source ouverte, **binaires distribués payants**. Compiler soi-même est légal et prévu. |
| Auteur | Lubos Lenco (armory3d). **5862/6243 commits = 1 personne.** |
| Activité | Très soutenue : 1097 commits en 2026, dernier commit le jour de l'analyse |
| Taille | ~93 kLOC hors libs vendorées et code généré. 348 MB avec l'historique git. |
| Contributeurs | 39 au total, mais 7 seulement actifs en 2026 dont 1 significatif → **bus factor = 1** |

### 1.1 ⚠️ Chronologie des réécritures — facteur de risque dominant

Le projet a été **intégralement réécrit deux fois en deux ans** :

| Période | Langage | Durée |
|---|---|---|
| déc. 2017 → janv. 2024 | **Haxe** (`.hx`) | ~6 ans |
| janv. 2024 → mars 2026 | **TypeScript** (`.ts`) | ~2 ans |
| **mars 2026 → aujourd'hui** | **C** | **~6 mois** |

Commits pivots : `ee67b50c` (2024-01-17, « Use ts »), `313378a9` (2026-03-06, « paint: move ts to c », **396 fichiers**).

Conséquences pour toute décision d'investissement :
- La base C n'a que **6 mois de maturité**. Les bugs trouvés (débordement de pile EXR, résolutions non carrées non persistées) sont cohérents avec une base jeune, pas encore éprouvée en production.
- Le mainteneur a démontré qu'il **réécrit tout quand ça l'arrange**, sans concertation. Une troisième réécriture n'est pas exclue ; elle invaliderait tout fork d'un coup.
- Corollaire pratique : la surface d'API à laquelle on s'accroche n'a aucune garantie de stabilité à 2-3 ans.

### Décomposition du code

| Zone | LOC | Rôle |
|---|---|---|
| `paint/sources/` (hors `startup.c`) | 63 800 | Application ArmorPaint |
| `base/sources/iron_*` + `engine.*` | 25 000 | Moteur « iron » (rendu, UI, math, IO, physique) |
| `base/sources/backends/` | 19 000 | Abstraction OS + GPU (D3D12 / Vulkan / Metal / WebGPU) |
| `base/sources/kong/` | 17 000 | « kongruent » : compilateur de shaders maison (`.kong` → HLSL/MSL/SPIR-V/WGSL) |
| `paint/plugins/` | 50 500 | Importeurs compilés (dont ufbx et cgltf vendorés) |
| `paint/sources/startup.c` | 451 KB | **Données générées** (meshes du compass/gizmo hardcodés). Ne pas éditer à la main. |

---

## 2. Compilation

### Toolchain

Pas de CMake, pas de Makefile classique. Build maison :

```
base/make (bash) → base/tools/bin/<platform>/amake (binaire QuickJS précompilé, commité)
                 → base/tools/make.js (109 KB de JS)
                 → project.js (base/ + paint/) décrit sources, libs, defines
                 → génère un projet natif (VS solution / Xcode project / Makefile)
```

Outils embarqués dans `amake` :
- `aimage.c` — convertit png/jpg/hdr en format `.k` (header + pixels LZ4)
- `ashader.c` — wrapper sur `kong`, compile les `.kong` vers l'API graphique cible
- `quickjs` — moteur JS qui exécute `make.js`

`base/tools/tcc/` = Tiny C Compiler embarqué (chemin de build alternatif).
`base/tools/iris/` = moteur d'inférence LLM local (Qwen3, GGUF, safetensors, Metal) — pour les nœuds « Neural ».

### Options de `make.js` (`base/tools/make.js:3248`)

`--target` (windows/linux/macos/ios/android/wasm), `--graphics`, `--compile`, `--run`, `--debug`,
`--ccompiler`, `--cppcompiler`, `--arch`, `--visualstudio`, `--js <file>`, `--ashader`, `--embed`.

### Commandes

```bash
cd paint
../base/make --compile          # génère ET compile
../base/make --run              # + lance (Linux)
../base/make --target wasm --compile --embed
```

### Statut sur CETTE machine (macOS arm64, 2026-09-14)

- Génération du projet Xcode : **OK**
- Compilation des 37 shaders `.kong` → Metal : **OK**
- Compilation finale : **ÉCHEC** — `xcode-select: error: tool 'xcodebuild' requires Xcode, but active developer directory '/Library/Developer/CommandLineTools' is a command line tools instance`

Seuls les Command Line Tools sont installés, pas Xcode.app. Sur macOS le build passe obligatoirement par `xcodebuild` (Metal + link).

**Fix** : installer Xcode.app puis `sudo xcode-select -s /Applications/Xcode.app`.

**Contrôle de santé du code effectué malgré tout** — syntax-check clang complet réussi :

```bash
cd paint && clang -fsyntax-only -std=c11 \
  -I../base/sources -I../base/sources/libs -Ibuild \
  -DIRON_METAL -DIRON_BGRA -DWITH_KONG -DWITH_PLUGINS -DWITH_PHYSICS -DWITH_EVAL \
  -DWITH_BC7 -DIDLE_SLEEP -DWITH_NFD -DWITH_COMPRESS -DWITH_IMAGE_WRITE \
  -DWITH_VIDEO_WRITE -DNDEBUG -DEMBED_H_PATH='"'"$PWD"'/build/embed.h"' \
  sources/main.c
```

Zéro erreur. Avec `-Wall -Wextra` : **9 warnings seulement** sur 64 kLOC (dont 2 réels : `argv` potentiellement non initialisé dans `nodes_neural/edit_image_node.c:66` et `nodes_neural/text_to_image_node.c:21`). C'est remarquablement propre pour du C de cette taille.

### Dépendances système

- **Windows** : VS + clang tools. D3D12, dxgi, dxguid, Winhttp, Dwmapi, dsound, dinput8.
- **Linux** : clang, `libvulkan-dev libgtk-3-dev libssl-dev libxi-dev libxrandr-dev libxcursor-dev libasound2-dev`
- **macOS** : Xcode complet
- **Android** : Android Studio / NDK — **iOS** : Xcode

### CI

3 workflows GitHub (`linux_vulkan`, `macos_metal`, `windows_direct3d12`) qui font **uniquement `--compile`**. Aucun test exécuté, aucun artefact publié.

---

## 3. Architecture

### Unity build

`paint/sources/main.c` fait `#include` de **tous** les `.c` de l'application (~130 fichiers). Un seul translation unit.

Conséquences pratiques :
- Ajouter un fichier `.c` → il faut l'ajouter à `main.c` **et** parfois au `project.js`
- Pas de compilation incrémentale par fichier (tout se recompile)
- `static` n'isole rien : tout est dans le même TU, attention aux collisions de noms
- Les prototypes vivent dans `paint/sources/functions.h` (63 KB), les types dans `types.h` (32 KB), les enums dans `enums.h`, les globales dans `globals.h`

### Conventions de code

- Style C procédural, préfixage par module : `slot_layer_*`, `export_texture_*`, `render_path_*`, `ui_*`
- Pas de POO, pas de vtables (sauf pointeurs de fonction pour les hooks d'import)
- État global assumé : `g_project`, `g_context`, `g_config`, `g_ui`, `g_theme`, `base_*`
- Types courts : `i32`, `f32`, `u8`, `bool`
- `.clang-format` présent à la racine — **l'utiliser systématiquement**
- Allocation : `malloc`/`calloc`/`free` manuels + macro `ALLOC_INIT`. Arrays maison (`i32_array_t`, `any_array_t`, `string_array_t`) dans `iron_array.c`.

### Carte des répertoires

```
base/                       moteur générique réutilisable
  sources/
    iron_*.c/h              moteur : gpu, draw, ui, math, json, file, obj, physics, armpack…
    engine.c/h              scene graph, objets, matériaux
    backends/               par OS/API : direct3d12_gpu.c, vulkan_gpu.c, metal_gpu.m,
                            webgpu_gpu.c, windows/linux/macos/ios/android_system
    kong/                   compilateur de shaders .kong
    libs/                   stb_*, bc7enc, minic (interpréteur C), lz4x, nfd, minimp4/h264
  shaders/*.kong            shaders du moteur
  tools/                    amake, make.js, tcc, iris (LLM), io_export_arm.py (addon Blender)
  docs/                     6 fichiers markdown (dev, pas utilisateur)

paint/                      application
  sources/
    main.c                  unity build + _kickstart()
    base.c                  boucle principale, init
    args.c                  CLI
    config.c context.c      préférences / état courant
    project.c               nouveau/ouvrir/sauver
    slot_layer.c            ⚠ cœur du modèle de layers
    history.c               undo (44 KB)
    io/                     import_* / export_* (dont export_exr.c)
    render/                 render paths + make_* (génération de shaders de peinture)
    nodes_material/         60 nœuds façon Blender/Cycles
    nodes_brush/            15 nœuds de brush
    nodes_neural/           10 nœuds IA
    ui/                     33 fichiers d'interface (immediate mode maison)
    util/                   22 utilitaires
    minic_api.c/_list.h     ⚠ API de scripting exposée
  plugins/                  importeurs compilés : io_exr, io_fbx, io_gltf, io_psd,
                            io_svg, io_tiff, uv_unwrap
  assets/
    plugins/*.c             scripts minic chargés à chaud
    export_presets/*.json   presets d'export
    keymap_presets/ locale/ themes/
  shaders/*.kong            shaders de l'app
```

---

## 4. Fonctionnalités (relevées dans le code)

### Peinture
Brush, Eraser, Fill, Decal, Text, Clone, Blur/Smudge, Particle, ColorID, Picker, Material, Cursor, Select, Bake (`main.c:263`).
Fill par Object / Face / Angle / UV Island (`enums.h:123`).
Projection UV : UV map, Triplanar, Project (`enums.h:129`).
Symétrie, ruler mode, pression stylet, masques par couche, stencils.
Workflows : PBR / Base / **Sculpt** (`enums.h:86`).
Sculpt : Draw et Grab (`enums.h:133`).

### Layers
- Layers, groupes, masques, filtres — hiérarchie à 2 niveaux (parent + grand-parent)
- **18 modes de fusion** (Mix → Value, `enums.h:136`)
- Masque par objet (`object_mask`), opacité, visibilité
- 255 layers max desktop / 18 mobile (`util/util_layer.c:5`)
- Résolutions : 2048 / 4096 / 8192 / 16384 / custom (slider 1→16384, non carré autorisé)
- Profondeur : 8 / 16 / 32 bits

### Baking (`enums.h:19`)
Curvature, Normal (tangent), Normal Object, Height, Derivative, Position, Texcoord, MaterialID, ObjectID, Vertex Color, **Occlusion**, **Lightmap**, **Bent Normal**, **Thickness**.
AO / Bent / Thickness / Lightmap en **ray-tracing hardware**.

### Rendu viewport
- 3 modes : Deferred, Forward, **Path Trace** (`enums.h:92`)
- 16 modes d'affichage (Lit, Base Color, Normal, Occlusion, Roughness, Metallic, Opacity, Height, Emission, Subsurface, Texcoord, Object Normal, Material ID, Object ID, Mask, Path Trace)
- Path tracer « Brute » maison, 4 qualités (Fast / Quality / Multi Fast / Multi Quality)
- SSAO, TAA, bloom, supersampling (0.25× → 4×), compositeur (vignette, grain, contraste, gamma, tonemap filmic, LUT `.cube`)
- HDRI env maps, sphères d'environnement, compass, gizmo

### Nœuds
- **60 nœuds matériaux** calqués sur Blender/Cycles (Noise, Voronoi, Gabor, Wave, Magic, Brick, Checker, Gradient, ColorRamp, Curves, Mapping, Bump, Normal Map, Blur, Warp, Quantize, Tilesheet, Text Texture, Camera Texture, Bake Texture, Curvature Bake, Script…)
- **15 nœuds brush**
- **10 nœuds « Neural »** (desktop uniquement) : Text→Image, Image→PBR, Image→3D Mesh, Upscale, Edit Image, Text→Text
- Groupes de nœuds, prévisualisation live

### Timeline
Keyframes de layers et de meshes, tweening (`types.h:664`), système de « stages ». Export vidéo MP4/H264 (`minimp4`/`minih264e`).

### IO

| Direction | Formats |
|---|---|
| Mesh in | obj, **blend** (natif), fbx, gltf, glb (plugins) |
| Mesh out | obj, arm, glb |
| Texture in | jpg, png, tga, bmp, gif, hdr, k, psd, **exr**, tiff, svg |
| Texture out | **png, jpg, exr** (aucun tiff/tga en sortie) |
| Projet | `.arm` — msgpack avec typed arrays (`iron_armpack.c`), pixels bruts LZ4 |
| Autres | polices, sons (wav/ogg), thèmes, keymaps, LUT `.cube` |

Addon Blender fourni : `base/tools/io_export_arm.py` (Blender 5.0).

### Divers
- 14 locales (fr incluse), thèmes JSON, keymaps remappables
- UI immediate-mode maison (`iron_ui.c`, 97 KB) — pas de Qt/Dear ImGui
- Mode « Player » (export WASM d'un viewer)
- Console avec agents IA : Qwen local, **Claude Code**, Codex, Grok — génèrent des scripts minic qui pilotent l'app (`nodes_neural/text_to_text_node.c:290`)
- Physique (bullet-like maison, `iron_physics.c` 43 KB)
- **Aucune télémétrie** détectée

---

## 5. Réponses aux questions pipeline VFX

### 5.1 « Est-ce que ça tourne partout ? » — ⚠️ Partiellement

**Plateformes** : Windows (D3D12), Linux (Vulkan), macOS (Metal), iOS, Android, WASM (WebGPU).

**Mais** :
- **Aucun fallback OpenGL.** Linux exige un driver Vulkan fonctionnel. Pas de rendu sur station sans GPU moderne, en VM sans passthrough, ni via X11 forwarding / VNC logiciel.
- **Le mode `--background` n'est pas headless.** `sys_start(config_get_options())` crée la fenêtre à `main.c:389`, alors que `args_parse()` n'est appelé qu'à `base.c:520`, donc *après*. `--background` ne fait que quitter après la tâche — il faut quand même un display et un contexte GPU. **Un render node sans GPU/display ne peut pas exécuter ArmorPaint en CLI.**
- Path tracing et bake RT exigent du ray-tracing **hardware** : D3D12 Tier 1.1, `VK_KHR_ray_query`, ou Metal `supportsRaytracing`. Indisponible sur WASM.
- macOS/iOS/Vulkan compilent avec `IRON_BGRA` → chemins de code différents de Windows pour l'ordre des canaux. Surface de bug spécifique par plateforme.
- Mobile : 8 bits seulement, 18 layers max, pas de nœuds Neural.

### 5.2 UDIM — ⚠️ Existe, mais implémentation « pauvre »

> **Mise à jour 2026-09-26** : partiellement périmé depuis le commit upstream `99156dfc` (« paint across udims », 2026-09-15) — la peinture à travers les tuiles marche et les calques partagés ne sont plus dupliqués par tuile (atlas en grille, résolution `res / ceil(√N)` par tuile). État actuel, limites et pistes vers des UDIM natifs : **[ARMORPAINT_UDIM.md](ARMORPAINT_UDIM.md)**.
>
> **Mise à jour 2026-09-27** (lot D0, dans notre fork) : les « limites dures » ci-dessous sont corrigées à l'import, sauf l'export par nom d'objet et le mode Visible en CLI. UV négatives, u ≥ 10 et plus de 64 tuiles : le fichier est refusé avec un message. Les triangles à cheval sont coupés le long des bords de tuile. En FBX, plus de plafond et un objet par nœud et par tuile. Au-delà des 64 tuiles de l'atlas, les tuiles en trop sont signalées au lieu d'écraser la 64e. Patches `udim-import`, `fbx-split`, `udim-tile-limit` de `paint/plugins/external/PATCHES.md`.

**Ce qui marche** :
- Import avec `Split by: UDIM Tile` pour OBJ (`io/import_obj.c:14`) et FBX (`plugins/io_fbx/io_fbx.c:215`)
- Nommage conforme : `<mesh>.<1001+>`, export en `<projet>.1001.exr` — compatible Mari/Maya/Arnold
- Export `Per Udim Tile` (`io/export_texture.c:560`)

**Comment c'est fait** : chaque tile devient **un objet paint séparé** avec ses UV renormalisés en 0-1. Ce n'est pas un vrai système UDIM — c'est un split de mesh. Conséquences :
- Pas de peinture continue à travers la frontière entre deux tiles (couture garantie)
- **Résolution unique pour tous les tiles** (`config_get_texture_res_x/y` est global). Impossible d'avoir le visage en 8K et le corps en 2K.
- Chaque tile a besoin de ses propres layers (masqués par objet) → la VRAM est multipliée par le nombre de tiles

**Limites dures** :
- `MAX_UDIM_TILES 100` en FBX (`io_fbx.c:10`). Au-delà, les tiles sont **silencieusement ignorés** (`continue` à la ligne 274).
- `tile_id = 1000 + tile_v * 10 + tile_u + 1` — `tile_u` n'est pas clampé à 10. Un UV avec u ≥ 10 produit un ID qui collisionne avec la rangée v+1.
- **UV négatifs cassés** : `(int)((uv0.x + uv1.x + uv2.x) / 3.0)` tronque vers zéro, donc u = -0.5 → tile 0 au lieu de -1. Idem dans `iron_obj.c:190`.
- L'assignation de tile se fait sur le **centroïde du triangle** — un triangle à cheval sur deux tiles part entièrement dans l'un des deux.
- En FBX, `build_udim_split` boucle sur **tous les nodes de la scène** et fusionne tout dans le même jeu de tiles. Un FBX multi-objets avec des ranges UDIM distincts est mal découpé.
- L'export UDIM déduit le tile via `substring(name, len-5, len)` (`export_texture.c:567`) — les 5 derniers caractères du nom d'objet. Fragile hors du chemin d'import UDIM.
- `--export-textures` en CLI force `EXPORT_MODE_VISIBLE` (`args.c:161`) → **le mode per-UDIM-tile est inaccessible en ligne de commande**.

### 5.3 4K minimum en sortie — ✅ oui, mais la VRAM est le mur

Résolutions disponibles : 2048, 4096, 8192, 16384 + custom.

**Le modèle mémoire est le problème.** `slot_layer_alloc_textures` (`slot_layer.c:6`) alloue **3 render targets pleine résolution par layer** : `texpaint` (base+opacity), `texpaint_nor` (normal), `texpaint_pack` (occ/rough/metal/height). Tout reste en VRAM, non compressé, **sans aucun streaming, tiling ou virtual texturing** (recherche exhaustive : aucune occurrence de sparse / virtual texture / streaming / page table dans le moteur).

Coût VRAM par layer :

| Résolution | 8 bits | 16 bits | 32 bits |
|---|---|---|---|
| 2K | 50 MB | 101 MB | 201 MB |
| **4K** | **201 MB** | **402 MB** | **805 MB** |
| 8K | 805 MB | 1.61 GB | 3.22 GB |
| 16K | 3.2 GB | 6.4 GB | 12.9 GB |

À quoi s'ajoutent l'undo (qui alloue des layers complets), les render targets de travail, le gbuffer, les masques.

Le code l'admet explicitement — `util/util_layer.c:71` :
```c
if (config_get_texture_res_x() >= 8192 || config_get_texture_res_y() >= 8192) { // Save memory for >=8k
    g_config->undo_steps = config_get_texture_res_x() == 8192 ? 4 : 1;
```
→ **à 8K l'undo tombe à 4 pas, à 16K à 1 seul pas.**

Par défaut `undo_steps = 6` (`config.c:212`). En 4K/16-bit, l'undo seul consomme ~2.4 GB.

**Verdict pratique** : 4K sur un objet simple avec peu de layers passe sur une carte 16-24 GB. 4K × 10 UDIM tiles × 15 layers, non. 8K est marginal. 16K est théorique.

### 5.4 « Tout sortir en EXR » — ❌ Non. C'est le point le plus faible.

L'écriture EXR est un fichier maison de 320 lignes (`io/export_exr.c`) dérivé de `miniexr` (Aras Pranckevičius, 2013). Le header est écrit **octet par octet en dur**.

| Attendu en VFX | État réel |
|---|---|
| Compression (ZIP/PIZ/DWAA) | ❌ **Aucune.** `no compression` en dur, ligne 148. |
| Canal alpha | ❌ **Jamais écrit.** Seuls B, G, R sortent (`export_exr_write_bgr`). Les presets qui mettent un 4ᵉ canal (`opac` chez xplane, `smooth` chez unity) **perdent silencieusement cette donnée en EXR** — alors qu'elle passe en PNG. |
| EXR mono-canal | ❌ Roughness/metal/AO sortent en **RRR** (canal dupliqué 3×) → 3× la taille pour rien. |
| Métadonnées | ❌ Aucune. Pas de `chromaticities`, pas d'attribut de colorspace, pas de `owner`/`comments`. |
| Tiled / mipmap | ❌ Scanline uniquement. |
| Multipart / deep | ❌ |
| Taille de chlist | Codée en dur à 55 octets (ligne 63) — ajouter un canal impose de réécrire le header |
| Performance | `u8_array_push` par octet. Un EXR 8K half = ~400 millions d'appels. |

Taille d'un EXR 4K non compressé : **100 MB** en half RGB, **201 MB** en float. En 8K : 402 MB / 805 MB. Sur un asset à 20 UDIM × 5 maps, on parle de **10 GB par version**. Ingérable en pipeline.

**En lecture** (`plugins/io_exr/io_exr.c`), c'est à peine mieux :
- Compressions supportées : **None et ZIPS uniquement**. PIZ, DWAA, DWAB, RLE, ZIP (blocs de 16 lignes) → refus avec `"Error: This exr compression type is not yet implemented"`. **DWAA est le format le plus courant en VFX.**
- Pas de gestion du décalage `dataWindow` (xMin/yMin lus mais jamais appliqués)
- Pas de tiled, pas de multipart, pas de canaux nommés hors R/G/B/A
- ⚠️ **Débordement de pile** : `channel_t channels[4]` (ligne 26) est rempli par une boucle sans borne (ligne 60-82). Un EXR avec 5 canaux ou plus **écrase la pile**. C'est un bug de sécurité exploitable par un fichier malveillant.

**Note** : 16 et 32 bits **forcent** l'EXR (`export_texture.c:215`). Il n'existe aucun chemin PNG-16 ou TIFF-16.

### 5.5 OCIO — ❌ Totalement absent

Recherche exhaustive : zéro occurrence de OCIO, OpenColorIO, ACES, ACEScg, chromaticities, ou d'un quelconque système de gestion de couleur.

Ce qui existe :
- Un enum `color_space_t` à 4 valeurs (`enums.h:452`) : AUTO, LINEAR, SRGB, DIRECTX_NORMAL_MAP — utilisé pour l'**import**
- À l'export, **2 choix** seulement : `linear` ou `srgb` (`main.c:250`)
- Le « srgb » est un **gamma 2.2 pur** (`export_texture.c:3` : `export_texture_gamma = 1.0/2.2`, puis `math_pow(value, gamma)`), pas la courbe sRGB par morceaux. Écart mesurable dans les noirs.
- Le viewport applique un tonemap **Filmic Hable/Uncharted2 en dur** (`shaders/compositor_pass.kong:41`), non configurable

**Le LUT `.cube`** (`io/import_lut.c`) est un pis-aller de color grading viewport, pas du color management :
- Stocké en **RGBA 8 bits** et **clampé 0-1** (ligne 78-81) → un LUT ACES avec des valeurs hors [0,1] est écrasé, et 8 bits de précision donnent du banding
- Le shader applique `pow(color, 0.4545)` **avant** le lookup (`compositor_pass.kong:69`) et clampe l'entrée à 0-1 → incompatible avec un LUT log→display type ACES CCT ; les highlights HDR sont écrasés avant d'atteindre le LUT
- Pas de shaper LUT, pas d'interpolation tétraédrique
- **Mutuellement exclusif avec le tonemap** (`if`/`else`)
- Appliqué au **viewport seulement**, jamais aux textures exportées

**Conséquence** : impossible de garantir qu'une texture peinte dans ArmorPaint se retrouve au bon endroit dans un pipeline ACEScg. Le painting se fait dans un espace non défini, affiché avec un tonemap arbitraire.

### 5.6 ShotGrid / workfiles — ❌ Aucune intégration, mais des accroches existent

Rien de natif : pas de ShotGrid, Kitsu, ftrack, Prism, Deadline, USD, ni aucun système de publication.

**Les accroches réelles** :

1. **CLI** (`args.c:81`) :
```
armorpaint [options] [fichier]
  --background                        quitte après la tâche (mais ouvre quand même une fenêtre)
  --export-textures <type> <preset> <path>    type: png|jpg|exr16|exr32
  --export-mesh <path>
  --export-material <path>
  --reload-mesh                       réimporte le mesh au démarrage
  --script <path>                     exécute un script minic sur le projet ouvert
  --api                               imprime la référence complète de l'API de scripting
  --player
```

2. **Scripting minic** — un interpréteur C embarqué (`base/sources/libs/minic.c`) avec ~600 fonctions exposées (`paint/sources/minic_api_list.h`). Fonctions utiles au pipeline :
```c
script_project_new()                    script_project_open(path)
project_save(save_and_quit)             project_filepath_get() / _set(s)
project_basepath_get()                  script_get_project()
script_import_asset(path, hdr_as_envmap)
export_texture_run(path, bake_material) script_export_mesh(path)
script_export_material(path)            project_reskin_mesh(frame)
iron_file_save_bytes(path, bytes, len)  file_download_to(url, dst, done, size)
viewport_save_texture_to_file(tex, path)
```

3. **Format `.arm` lisible hors app** : msgpack avec typed arrays (`iron_armpack.c`), donc **parsable depuis Python**. Un outil de pipeline peut inspecter/patcher un projet sans lancer ArmorPaint. Un addon Blender Python d'exemple existe (`base/tools/io_export_arm.py`).

4. **Plugins scripts à chaud** : `paint/assets/plugins/*.c`, interprétés par minic au lancement, avec hooks `on_ui` / `on_update` / `on_delete` (`plugin.c`). Exemples fournis : `autosave.c`, `converter.c`, `import_stl.c`, `make_tilesheet.c`.

5. **Plugins compilés** : `paint/plugins/`, avec un hook `WITH_EXTERNAL` → `external_init()` (`plugins.c:154`) et `paint/plugins/external` déjà dans `.gitignore` — le point d'extension officiel pour du code natif propriétaire.

**Ce qui manque pour brancher ShotGrid** :
- ❌ **Pas d'exécution de process externe dans l'API de scripting.** `iron_exec_async` existe au niveau moteur (`iron_system.h:105`, fork/execvp sur Linux, NSTask sur macOS) mais **n'est pas exposé à minic**. Un script ne peut pas appeler `oiiotool`, `shotgun`, `rez`, ou un script Python.
- ❌ **Pas de HTTP utilisable.** `iron_net_request` supporte GET/POST HTTPS (`iron_net.h`), mais seul `file_download_to` (GET vers fichier) est exposé. Pas de POST, pas de headers custom, pas de réponse en mémoire → **l'API REST ShotGrid est inatteignable depuis un script**.
- ❌ Pas de hooks de cycle de vie exposés (pre-save, post-export)
- ❌ Pas de gestion de versions/tokens

**Les deux points 1 et 2 sont des ajouts de quelques lignes chacun** dans `minic_api_list.h` + `minic_api.c`. C'est le chemin de contribution le plus rentable.

---

## 6. Limites dures — tableau récapitulatif

| # | Limite | Emplacement | Gravité VFX |
|---|---|---|---|
| 1 | EXR sans compression, sans alpha, sans métadonnées, sans mono-canal | `io/export_exr.c` | 🔴 Bloquant |
| 2 | Import EXR : ni DWAA, ni PIZ, ni RLE, ni ZIP | `plugins/io_exr/io_exr.c:99` | 🔴 Bloquant |
| 3 | Aucun OCIO, aucun ACES | absent partout | 🔴 Bloquant |
| 4 | Pas de streaming/tiling : toute la VRAM d'un coup, 3 RT pleine res par layer | `slot_layer.c:6` | 🔴 Bloquant à l'échelle |
| 5 | Undo forcé à 4 (8K) ou 1 (16K) pas | `util/util_layer.c:71` | 🔴 |
| 6 | UDIM = split de mesh, résolution unique globale, coutures entre tiles | `import_obj.c`, `io_fbx.c` | 🔴 |
| 7 | `MAX_UDIM_TILES = 100`, tiles au-delà silencieusement ignorés | `io_fbx.c:10,274` | 🟠 |
| 8 | UV négatifs mal gérés (troncature vers zéro) | `iron_obj.c:190`, `io_fbx.c:257` | 🟠 |
| 9 | `--background` n'est pas headless : fenêtre + GPU requis | `main.c:389` vs `base.c:520` | 🔴 pour la ferme |
| 10 | Pas de fallback OpenGL (Vulkan/D3D12/Metal obligatoires) | `base/project.js` | 🟠 |
| 11 | Path tracer et bake RT : **64 objets max** | `metal_gpu.h:28` (`GPU_RAYTRACE_MAX_OBJECTS`) | 🟠 (fatal si >64 tiles UDIM) |
| 12 | Pas d'Alembic, pas d'USD | `iron_path.c:28` | 🟠 |
| 13 | Positions/normales/UV quantifiées en **int16** | `iron_obj.c:490-623` | 🟠 |
| 14 | Résolutions non carrées non persistées : `layer_data_t.res` est un seul champ, relu en `base_res_x = base_res_y = l0->res` | `types.h:626`, `import_arm.c:592` | 🟠 **bug** |
| 15 | Débordement de pile : `channel_t channels[4]` rempli sans borne | `io_exr.c:26,60` | 🟠 **bug sécurité** |
| 16 | `--export-textures` force le mode « Visible » (pas de per-UDIM en CLI) | `args.c:161` | 🟠 |
| 17 | Pas de `exec` ni de HTTP POST dans l'API de scripting | `minic_api_list.h` | 🟠 |
| 18 | Projets `.arm` = pixels bruts LZ4 → fichiers de plusieurs GB | `io/export_arm.c:238` | 🟠 |
| 19 | Padding UV (dilate) plafonné à 16 px | `box_preferences.c:372` | 🟡 |
| 20 | 2 UV sets maximum (uv0, uv1) | `tab_layers.c:783` | 🟡 |
| 21 | Canaux d'export figés (23 noms en dur), pas d'AOV arbitraire | `main.c:244` | 🟡 |
| 22 | Aucun test de régression | `base/tests/` = 3 démos | 🟠 organisationnel |
| 23 | Bus factor = 1 | historique git | 🔴 organisationnel |
| 24 | Nœuds Neural : téléchargent et **exécutent** des binaires depuis HuggingFace, **sans vérification de hash ni de signature** | `nodes_neural/neural_node.c:181` | 🔴 sécurité IT |

---

## 7. Avis sur la qualité du code

### Points forts

- **Très propre pour du C de cette taille.** 9 warnings avec `-Wall -Wextra` sur 93 kLOC. Compile sans erreur du premier coup.
- **Cohérence remarquable.** Un seul auteur, un seul style, des conventions de nommage respectées partout. On trouve n'importe quoi en 30 secondes avec un grep.
- **Zéro dépendance externe lourde.** Pas de Qt, pas de boost, pas de CMake, pas de npm. Tout est vendoré en single-header. Le dépôt est autonome — c'est un atout majeur pour la reproductibilité d'un build en studio sur 10 ans.
- **Lisible.** Le code fait ce qu'il dit. Pas d'abstraction spéculative, pas de patterns gratuits.
- **Compilation rapide** grâce à l'unity build.
- `.clang-format` présent et respecté.

### Points faibles

- **Aucun test.** `base/tests/` contient 3 démos du moteur (triangle, cube, fall). Les seuls tests unitaires sont ceux de l'interpréteur minic. La CI ne fait que compiler. **Toute modification est un pari.**
- **Gestion mémoire artisanale.** Des `free()` commentés en production (`iron_obj.c:643-647`). Pas de sanitizers dans la CI, pas d'ASAN/valgrind. Sur un logiciel qui manipule des gigaoctets de pixels, c'est inquiétant.
- **Globales partout.** `g_project`, `g_context`, `g_config`, plus des dizaines de globales par module. Aucune isolation testable. Le multithread est de fait exclu.
- **Unity build.** Pratique pour un solo dev, hostile à la collaboration : tout est dans un TU, pas de compilation incrémentale, `static` ne protège de rien.
- **Fichiers monstres.** `iron_ui.c` 97 KB, `iron_ui_nodes.c` 68 KB, `history.c` 44 KB, `functions.h` 63 KB (un seul header de prototypes pour toute l'app).
- **Chemins d'erreur pauvres.** Beaucoup de `return NULL` silencieux, des `continue` qui avalent des données (les tiles UDIM au-delà de 100), peu de messages à l'utilisateur.
- **Code IO fragile.** Les parseurs EXR/TIFF/PSD sont écrits à la main, sans fuzzing, avec au moins un débordement de pile confirmé.
- **Binaires commités** : `amake` (1-4 MB par plateforme), `dxc.exe`, `dxcompiler.dll`, `glslangValidator`, `tcc/bin`. Auditables en théorie (les sources d'amake sont là), problématiques pour une politique IT stricte.

### Note globale

**7/10 en artisanat, 4/10 en ingénierie industrielle.**

C'est du très bon code de développeur solo : cohérent, lisible, efficace, sans dette d'abstraction. Ce n'est pas du code d'équipe : pas de tests, pas de filets, pas de modularité, une seule personne qui connaît l'ensemble.

---

## 8. Capacité de contribution

### Facilité : élevée

- Le code est facile à lire et à naviguer
- Ajouter un nœud, un importeur, un preset, une locale est mécanique et documenté par l'exemple
- Le build tourne en une commande
- Boucle d'itération rapide

### Frictions

- **Aucun test** → chaque PR se valide à l'œil. Il faut compiler et tester manuellement à chaque fois.
- Aucun `CONTRIBUTING.md`, aucune convention de PR documentée
- Sur ~30 merges en 2025-2026, la quasi-totalité du travail reste celui du mainteneur. Les PR externes sont rares. **Le taux d'acceptation d'une contribution structurante est incertain** — mieux vaut ouvrir une issue avant d'investir.
- Les changements profonds (streaming de textures, OCIO, refonte EXR) touchent au cœur d'un design tenu par une personne → risque de refus élevé.

### Recettes concrètes

**Ajouter un fichier source app** → le créer dans `paint/sources/<zone>/`, ajouter le `#include` dans `paint/sources/main.c` (ordre alphabétique dans son bloc), déclarer les prototypes dans `functions.h`, les types dans `types.h`.

**Ajouter un nœud matériau** → copier `paint/sources/nodes_material/<proche>.c`, l'inclure dans `main.c`, l'enregistrer dans `nodes_material.c`, ajouter la traduction dans `paint/assets/locale/*.json`.

**Ajouter un importeur compilé** → créer `paint/plugins/io_xxx/io_xxx.c`, l'enregistrer dans `plugins_init()` (`paint/plugins/plugins.c:146`), ajouter l'extension à `_path_texture_formats` / `_path_mesh_formats`.

**Ajouter un plugin script (pas de recompilation)** → écrire un `.c` minic dans `paint/assets/plugins/`, l'activer dans Preferences → Plugins. Voir `hello_world.c`, `autosave.c`.

**Étendre l'API de scripting** → ajouter une ligne `X<n>(nom, "signature", ret, args...)` dans `paint/sources/minic_api_list.h` + l'implémentation dans `minic_api.c` / `minic_impl.c`.

**Ajouter un preset d'export** → un JSON dans `paint/assets/export_presets/`. Format : `{"textures":[{"name":"...","channels":["base_r","base_g","base_b","1.0"],"color_space":"linear"}]}`. Canaux valides listés dans `main.c:244`.

**Toujours** : lancer `clang-format` avec le `.clang-format` de la racine avant de committer.

### Interventions par rapport effort/valeur

| Intervention | Effort | Valeur pipeline | Faisable en fork ? |
|---|---|---|---|
| Exposer `iron_exec_async` à minic | **~1 h** | 🔥 Débloque tout : appel d'`oiiotool`, de Python, de la CLI ShotGrid | oui, trivial |
| Exposer un vrai HTTP (POST + headers + réponse mémoire) à minic | ~1 j | 🔥 API REST ShotGrid directe | oui |
| Réécrire l'export EXR sur **tinyexr** (header-only, BSD) | ~2-3 j | 🔥 Compression, alpha, mono-canal, métadonnées | oui, isolé dans `export_exr.c` |
| Remplacer l'import EXR par tinyexr | ~1 j | 🔥 DWAA/PIZ/tiled + corrige le débordement de pile | oui |
| Corriger le débordement `channels[4]` | **~15 min** | Sécurité | oui, PR upstream évidente |
| Corriger les UV négatifs UDIM | ~1 h | Correction | oui, PR upstream |
| Lever `MAX_UDIM_TILES` (alloc dynamique) | ~2 h | Assets lourds | oui, PR upstream |
| Ajouter `--export-textures` per-UDIM en CLI | ~2 h | Automatisation | oui, PR upstream |
| Corriger la résolution non carrée dans `.arm` | ~2 h | Correction | oui, PR upstream |
| Presets d'export VFX (Arnold/Karma/VRay/RenderMan) | ~1 h | Confort | trivial, fichiers JSON |
| Vrai mode headless (offscreen, sans fenêtre) | ~1-2 sem | 🔥 Ferme de rendu | fork, refonte de l'init |
| Intégration OCIO (v2, C++ → wrapper C) | ~3-4 sem | 🔥 Bloquant levé | fork ; casse la doctrine « zéro dépendance » |
| Résolution par tile UDIM | ~2-4 sem | 🔥 | fork profond (`config_get_texture_res_*` est global) |
| Streaming / tiling de textures | **~2-3 mois** | 🔥🔥 Le vrai verrou | réécriture du cœur, hors de portée raisonnable |

---

## 9. Verdict pour un pipeline VFX

### En l'état : non viable comme outil de texturing hero

Les cinq bloquants :
1. **Pas de color management.** Aucune garantie de cohérence colorimétrique avec un pipeline ACES.
2. **EXR inutilisable en production.** Pas de compression, pas d'alpha, pas de DWAA en lecture.
3. **Pas de streaming de textures.** La VRAM plafonne l'usage bien avant les besoins d'un asset hero.
4. **UDIM cosmétique.** Split de mesh, résolution unique, coutures, 100 tiles max.
5. **Pas de headless réel.** Impossible d'automatiser sur une ferme sans GPU/display.

C'est un outil conçu pour le **jeu vidéo et la 3D temps réel** — les presets d'export (Unity, Unreal, Unigine, Minecraft, XPlane) le disent explicitement. Il n'y a aucun preset VFX, et ce n'est pas un oubli : c'est la cible produit.

### Là où il a une vraie place

- **Assets secondaires / props / set dressing** en 2K-4K, peu de layers, sortie PNG 8 bits
- **Prototypage et lookdev rapide** — le path tracer viewport est bon et instantané
- **Poste de travail léger / mobile / itinérant** — 200 MB d'app, tourne sur un laptop
- **Base de fork** pour un studio prêt à investir : licence zlib, code lisible, zéro dépendance, extension par plugin natif prévue
- **Baker** : la suite de bakes (curvature, bent normal, thickness, AO RT) est complète et rapide

### Si l'on veut vraiment l'intégrer

Feuille de route minimale, par ordre de rentabilité :

1. **Semaine 1** — Exposer `iron_exec_async` + HTTP POST à minic. Écrire un plugin script de publication qui appelle un script Python maison (lui-même parlant à ShotGrid). *Ça résout la question workfiles sans toucher au cœur.*
2. **Semaine 2-3** — Remplacer `export_exr.c` et `io_exr.c` par tinyexr. Corriger le débordement de pile au passage. *Ça résout l'EXR.*
3. **Semaine 4** — Presets d'export VFX + `--export-textures` per-UDIM + correction des UV négatifs et de `MAX_UDIM_TILES`.
4. **Mois 2** — Décider sur OCIO : soit intégrer OCIO v2 (rompt la doctrine zéro-dépendance, mais c'est le seul vrai chemin), soit implémenter un pipeline ACEScg minimal en dur (matrices + transfer functions), suffisant si le studio n'a qu'une config OCIO.
5. **Au-delà** — Streaming de textures et résolution par tile. À ce stade, on ne contribue plus : on maintient un fork lourd. **Comparer honnêtement avec le coût de licences Mari.**

### Le vrai arbitrage

Le mur n'est pas technique, il est **structurel** : un mainteneur unique, aucun test, un design mémoire qui ne connaît pas le streaming. Contribuer upstream sur les petits correctifs est facile et bienvenu. Faire converger ArmorPaint vers un outil VFX demanderait un fork permanent et un ETP dédié.

**Recommandation** : l'utiliser tel quel pour ce qu'il fait bien (props, 4K, 8 bits, bake), investir 2-4 semaines sur les accroches pipeline (exec + HTTP + tinyexr + presets), et ne pas en faire l'outil de texturing hero d'un show.

---

## 10. Index de navigation rapide

| Sujet | Fichier |
|---|---|
| Point d'entrée, unity build | `paint/sources/main.c` |
| Boucle principale, init | `paint/sources/base.c` |
| CLI | `paint/sources/args.c` |
| Modèle de layers, allocation VRAM | `paint/sources/slot_layer.c:6` |
| Limites layers, resize, undo auto | `paint/sources/util/util_layer.c:5,71` |
| **Export textures / logique color space** | `paint/sources/io/export_texture.c` |
| **Écriture EXR** | `paint/sources/io/export_exr.c` |
| **Lecture EXR** | `paint/plugins/io_exr/io_exr.c` |
| Parseur OBJ + split UDIM | `base/sources/iron_obj.c:183,219,547` |
| Import OBJ / dispatch UDIM | `paint/sources/io/import_obj.c` |
| Import FBX + split UDIM | `paint/plugins/io_fbx/io_fbx.c:215` |
| Format projet `.arm` | `base/sources/iron_armpack.c`, `paint/sources/io/export_arm.c`, `import_arm.c` |
| LUT `.cube` | `paint/sources/io/import_lut.c`, `paint/shaders/compositor_pass.kong:67` |
| **API de scripting (liste)** | `paint/sources/minic_api_list.h` |
| API de scripting (impl) | `paint/sources/minic_api.c`, `minic_impl.c` |
| Interpréteur C | `base/sources/libs/minic.c` |
| Plugins compilés (enregistrement) | `paint/plugins/plugins.c:146` |
| Plugins scripts (cycle de vie) | `paint/sources/plugin.c` |
| Enums (résolutions, bits, modes, blend) | `paint/sources/enums.h` |
| Config / préférences | `paint/sources/config.c` |
| UI export | `paint/sources/ui/box_export.c` |
| Presets export | `paint/assets/export_presets/*.json` |
| Canaux exportables (liste en dur) | `paint/sources/main.c:244` |
| Render paths | `paint/sources/render/render_path_*.c` |
| Path tracer | `paint/sources/render/render_path_raytrace.c`, `base/shaders/raytrace/src/` |
| Limite 64 objets RT | `base/sources/backends/metal_gpu.h:28` |
| Exec de process (non exposé à minic) | `base/sources/iron_system.h:105`, `backends/linux_system.c:1551`, `macos_system.m:1161` |
| HTTP (non exposé à minic) | `base/sources/iron_net.h` |
| Nœuds IA / téléchargement de binaires | `paint/sources/nodes_neural/neural_node.c:181` |
| Config du build | `base/project.js`, `paint/project.js` |
| Build tool | `base/tools/make.js:3248` (options) |
| Formats supportés | `base/sources/iron_path.c:28,37` |
| Addon Blender | `base/tools/io_export_arm.py` |
| Docs dev | `base/docs/` |

---

## 11. Licence — ce qui est permis

> Résumé factuel, pas un avis juridique. Faire valider par le service juridique avant toute redistribution externe.

### 11.1 Le code d'ArmorPaint : zlib/libpng

Trois obligations, c'est tout :
1. Ne pas se prétendre auteur de l'original
2. **Marquer clairement les versions modifiées comme telles**
3. Conserver la notice de licence dans les distributions de sources

Ce qui est **explicitement autorisé** :
- ✅ Forker, modifier, étendre sans limite
- ✅ Usage commercial, y compris sur des productions payantes
- ✅ **Garder les modifications privées** — aucune clause copyleft, aucune obligation de reverser
- ✅ Distribution interne studio, et même distribution binaire externe
- ✅ Relicencier les *ajouts* comme on veut (le code d'origine reste zlib)
- ✅ Aucune attribution obligatoire dans l'UI (« appréciée mais pas requise »)

C'est l'une des licences les plus permissives qui existent — plus souple que MIT sur l'attribution binaire.

### 11.2 Dépendances vendorées — toutes permissives

| Composant | Licence | Lié au binaire |
|---|---|---|
| iron / kongruent / Kore / nfd / nanosvg | zlib | oui |
| ufbx, cgltf, jsmn, bc7enc, sdefl/sinfl, quickjs, iris, llama.cpp | MIT | oui (quickjs/llama = outils) |
| stb_*, lz4x, minimp4, minih264e, untar | Public domain / Unlicense | oui |
| lz4-wasm | BSD-2 | WASM uniquement |
| Roboto, Material icons | Apache 2.0 | assets |
| **Nœuds matériaux (dérivés de Blender/Cycles)** | **Apache 2.0** | oui |
| **tcc** | **LGPL 2.1** | ❌ **non — compilateur alternatif de build uniquement** |

**Aucune contamination copyleft.** Deux points de vigilance seulement :

- **TCC est LGPL** mais n'est qu'un compilateur optionnel (`--ccompiler tcc`, `base/tools/make.js:1849`). Compiler avec un outil LGPL n'impose rien sur le binaire produit — même statut que GCC. Aucune obligation.
- **Apache 2.0 sur les nœuds Cycles** (`paint/assets/licenses/license_cycles_nodes.md`) : impose de conserver les notices et **d'indiquer les fichiers modifiés**. Contient aussi une clause de licence de brevet. Compatible avec du propriétaire, mais à respecter si tu redistribues.
- **minih264e** : le code est public domain, mais **H.264 lui-même est un format breveté**. Si tu ne comptes pas exporter de vidéo, désactive-le : `flags.with_video_write = false` dans `paint/project.js:11`. Ça supprime le sujet.

### 11.3 Verdict licence

**Oui, sans réserve.** Tu peux forker, ajouter EXR/OCIO/UDIM/ShotGrid, garder le tout privé, et l'utiliser en production commerciale. Les seules obligations sont de garder les notices et de marquer ton build comme modifié — une ligne dans l'About et un fichier `LICENSES.md`.

Si tu redistribues un binaire hors du studio, fais un inventaire de licences propre (ça se génère une fois) et désactive `with_video_write` si tu n'en as pas besoin.

---

## 12. Stratégie de fork — mesures empiriques

La question n'est pas « est-ce permis » (oui) mais « vais-je pouvoir suivre l'upstream ». Réponse mesurée sur ce dépôt.

### 12.1 Volatilité de l'upstream

Sur 12 mois : **30 commits touchant plus de 50 fichiers**, dont plusieurs à 150-400 fichiers.

Mais l'essentiel est du **bruit de formatage** — le mainteneur réaligne périodiquement tout le code avec `.clang-format` :

| Commit | Lignes changées | Hors whitespace | Nature |
|---|---|---|---|
| `e20e5d6d` « cleanup » | 11 639 | 227 (**1%**) | réalignement pur |
| `5405d47f` « Format » | 29 857 | 4 165 (13%) | réalignement |
| `114efb24` « cleanup » | 1 273 | — | **réorganisation de l'arborescence** (`sources/*.c` → `sources/io/`) |
| `313378a9` « move ts to c » | 91 611 | 99% | migration de langage (événement unique) |

⚠️ **Les messages de commit ne sont pas fiables** : « cleanup » recouvre aussi bien du whitespace que des déplacements de fichiers ou du refactoring réel. On ne peut pas filtrer par message.

### 12.2 Dérive par fichier — 6 mois depuis la migration C

| Fichier | Dérive hors-whitespace | Verdict |
|---|---|---|
| `io/export_exr.c` | **8%** | 🟢 très stable — sûr à remplacer |
| `plugins/io_exr/io_exr.c` | 4 commits / 12 mois | 🟢 très stable |
| `base/sources/iron_obj.c` | 7 commits / 12 mois | 🟢 stable |
| `io/import_obj.c` | 37% | 🟡 moyen |
| `io/export_texture.c` | **86%** | 🔴 très volatil |
| `slot_layer.c` | 48 commits / 12 mois | 🔴 très volatil |
| `main.c`, `config.c`, `minic_api.c` | 38-60 commits / 12 mois | 🔴 très volatil |

### 12.3 Test de rebase réel

Fork simulé à partir du 2026-06-15 (3 mois de retard), avec trois patchs représentatifs :
- **A** — hook OCIO + redirection vers un exportateur maison, **dans le corps** de `export_texture_write_texture()` (le fichier à 86% de dérive)
- **B** — remplacement **intégral** de `export_exr.c`
- **C** — correction des UV négatifs **dans le corps** de `get_tile()` (`iron_obj.c`)

Rebasé sur `HEAD` après 3 mois d'upstream incluant une réorganisation d'arborescence et plusieurs reformatages massifs :

| Patch | Résultat |
|---|---|
| A — dans `export_texture.c` (86% de dérive) | ✅ **fusionné sans conflit** |
| C — dans `iron_obj.c` | ✅ **fusionné sans conflit** |
| B — réécriture complète de `export_exr.c` | ⚠️ **1 conflit**, trivial (`git checkout --ours`) |

**Git suit les renommages de fichiers automatiquement** — le déplacement `paint/sources/` → `paint/sources/io/` n'a causé aucun problème.

**Conclusion empirique : la maintenance d'un fork est beaucoup plus facile que la volatilité brute ne le laissait craindre.** Le bruit de formatage ne génère pas de conflit sur des patchs localisés. Compter **une demi-journée de rebase par trimestre**.

Astuces qui suppriment l'essentiel du bruit résiduel :
```bash
git rebase --rebase-merges -X ignore-all-space upstream/main   # ignore le réalignement
git config merge.renamelimit 999999                            # suit les gros renommages
git config rerere.enabled true                                 # mémorise les résolutions récurrentes
```
Et **toujours formater ses patchs avec le `.clang-format` du dépôt** avant de committer.

### 12.4 ⭐ Le point d'extension officiel : `paint/plugins/external/`

Découverte décisive. Depuis le **22 août 2026** (`d44aa2e6`, il y a 3 semaines), `paint/plugins/project.js:12` contient :

```js
if (fs_exists(os_cwd() + "/../paint/plugins/external")) {
    project.add_cfiles("external/**");
    project.add_shaders("external/*.kong");
    project.add_assets("external/assets/*", {destination : "data/{name}"});
    project.add_define("WITH_EXTERNAL");
}
```

Et `paint/plugins/plugins.c:179` appelle `external_init()` au démarrage si `WITH_EXTERNAL` est défini.

`paint/plugins/external` est dans le `.gitignore` **upstream**.

**Traduction** : tu déposes du code C natif (et des shaders `.kong`, et des assets) dans `paint/plugins/external/`, il est compilé dans le binaire, il s'initialise au lancement — et il **n'apparaît jamais dans `git status`**. Zéro conflit de merge possible sur ce code, pour toujours.

Le couplage se fait par **forward declarations** de symboles C (voir `paint/plugins/io_exr/io_exr.c:8-10`), donc uniquement via l'ABI C : tant que les signatures des fonctions du cœur que tu appelles ne changent pas, ton code tient.

C'est exactement le mécanisme pour ce que tu veux faire. Le mainteneur l'a ajouté récemment — probablement en réponse à des demandes de ce type.

### 12.5 Répartition recommandée de tes ajouts

| Ce que tu veux | Où le mettre | Conflits |
|---|---|---|
| **Exporter maison** (écriture au bon endroit du pipeline) | `plugins/external/` + enregistrement dans `external_init()` | **zéro** |
| **EXR tinyexr (écriture)** | `plugins/external/studio_exr.c` + **1 ligne** redirigée dans `export_texture.c:71` | 1 ligne |
| **EXR tinyexr (lecture)** | `plugins/external/` — enregistre `"exr"` dans `import_texture_importers`, écrase l'existant | **zéro** |
| **OCIO** | lib + code dans `plugins/external/`, **1 appel** injecté dans `export_texture_write_texture()` | 1 ligne |
| **ShotGrid / workfiles** | plugin script minic dans `assets/plugins/` **ou** `plugins/external/` (UI + logique) | **zéro** |
| **Exposer `exec` / HTTP au scripting** | patch de `minic_api_list.h` + `minic_api.c` | 🟠 fichiers chauds — **à proposer en PR upstream** |
| **Corrections UDIM** (UV négatifs, `MAX_UDIM_TILES`, per-tile en CLI) | `iron_obj.c`, `io_fbx.c`, `args.c` | 🟢 fichiers calmes — **PR upstream** |
| **Résolution par tile UDIM** | `config_get_texture_res_*` est global → touche `slot_layer.c` + `util_layer.c` | 🔴 très volatils, fork lourd |

**Règle d'or** : tout ce qui peut vivre dans `plugins/external/` y va. Ne patcher le cœur que pour des **points d'injection d'une ou deux lignes**. Chaque patch du cœur doit être un `#ifdef WITH_EXTERNAL` appelant une fonction de ton code — jamais de logique métier dans les fichiers upstream.

Avec cette discipline, ta surface de conflit se réduit à **une poignée de lignes**, et le test de rebase ci-dessus montre qu'elles passent la plupart du temps sans intervention.

### 12.6 Ce qui reste hors de portée

- **Streaming / tiling de textures** — réécriture du cœur (`slot_layer.c`, `util_layer.c`, tout le render path). 2-3 mois, fork permanent et divergent. À ce stade, comparer honnêtement au coût de licences Mari.
- **Résolution par tile UDIM** — `config_get_texture_res_x/y()` est global et appelé depuis partout. 2-4 semaines, touche les fichiers les plus chauds.
- **Vrai headless** — l'ordre `sys_start()` puis `args_parse()` est structurel (`main.c:389` vs `base.c:520`). Faisable (1-2 semaines) mais touche l'init, zone chaude.

### 12.7 Bonus : un importeur USD a existé

`353aa92e` (2025-12-16) a **retiré** `paint/plugins/io_usd/`, basé sur **tinyusdz**. Probablement parce que c'était du C++ dans un projet C pur.

Le code est récupérable :
```bash
git show 353aa92e^:paint/plugins/io_usd/io_usd.cc
git checkout 353aa92e^ -- paint/plugins/io_usd/
```
Point de départ utilisable pour un import USD dans `plugins/external/` (où le C++ ne gênera personne).

### 12.8 Verdict sur la stratégie de fork

**Techniquement, oui — et plus facilement que prévu.** Trois raisons :
1. Le hook `plugins/external/` permet de mettre **l'essentiel** de tes ajouts hors de l'arbre versionné
2. Le test de rebase montre que les patchs localisés survivent aux réorganisations et reformatages
3. Les fichiers EXR et UDIM, ceux qui t'intéressent le plus, sont parmi les **plus stables** du dépôt

**Le risque réel n'est pas le merge, c'est le projet lui-même** (§1.1) : deux réécritures complètes en deux ans, une base C de 6 mois, un bus factor de 1. Ce n'est pas un risque qu'on mitige par de la technique.

**Recommandation** : dimensionner l'investissement comme un **pari à 18-24 mois**, pas comme une fondation à 10 ans. Concrètement :
- Garder tout ton code dans `plugins/external/` + un dépôt séparé, versionné indépendamment
- Ne jamais mettre de logique métier dans les fichiers upstream
- Pousser en PR upstream tout ce qui est correction de bug (UDIM, débordement EXR, résolution non carrée) : c'est accepté, ça allège ton fork, et c'est du bon voisinage
- Traiter ton exporter maison comme **le** livrable, et ArmorPaint comme un frontend interchangeable — de sorte qu'un pivot de l'upstream ne coûte que le réamorçage, pas la logique pipeline

Le dépôt accepte les PR externes : `1adadfd4` a mergé la PR #2117 d'un contributeur tiers ajoutant `viewport_save_texture_to_file` à l'API de scripting. Étendre `minic_api_list.h` par PR est donc un chemin réaliste — et évite d'avoir à patcher ce fichier chaud dans ton fork.

---

## 13. Hypothèse : tout réécrire en Rust

> Évaluation demandée le 2026-09-14. Conclusion : **non recommandé sous cette forme** — mais la question mène à une meilleure option (§13.6).

### 13.1 Ce qui disparaîtrait (remplacé par des crates)

| Zone | LOC actuelles | Équivalent Rust |
|---|---|---|
| backends GPU + OS | 18 984 | `wgpu` + `winit` |
| `kong` (compilateur de shaders maison) | 16 933 | `naga` / WGSL — **disparaît entièrement** |
| UI immediate-mode (`iron_ui*`) | 6 420 | `egui` |
| libs vendorées (stb, bc7enc, lz4, ufbx, cgltf, nanosvg…) | 45 427 | `image`, `exr`, `lz4_flex`, `ufbx-rs`, `gltf`, `usvg`, `texpresso` |
| math / json / array / map / file | 7 182 | `std` + `glam`, `serde` |
| physique | 1 600 | `rapier3d` |
| audio | 718 | `cpal` / `rodio` |
| **Total supprimé** | **~97 000** | |

C'est le gain immédiat : **~97 kLOC d'infrastructure qui deviennent des lignes de `Cargo.toml`.**

### 13.2 Ce qui resterait à réimplémenter

| Zone | LOC |
|---|---|
| UI applicative (33 fichiers) | 16 149 |
| 60 nœuds matériaux | 8 127 |
| cœur (layers, history, project, context, parsers) | 7 796 |
| render paths + `make_*` (génération de shaders de peinture) | 7 638 |
| utilitaires paint (brush, clone, uv, stencil, raycast…) | 7 240 |
| import / export | 5 291 |
| scripting | 5 379 → largement remplacé par `rhai` ou `mlua` |
| plugins (hors libs vendorées) | 3 442 |
| nœuds brush | 1 828 |
| nœuds IA | 1 707 → **à jeter** |
| shaders `.kong` → WGSL | 2 916 |
| **Total réel à écrire** | **≈ 57 000 – 64 000** |

### 13.3 Estimation d'effort

Portage avec référence fonctionnelle complète (l'algorithme est sous les yeux) : **150-250 LOC/jour** pour un dev Rust senior à l'aise en graphismes.

- 57 000 LOC ÷ 200 = **285 j/h** de codage pur
- shaders kong → WGSL : **+20-30 j/h**
- facteur classique pour compréhension du domaine, debug GPU multi-backend, tests (qu'il faut écrire — l'original n'en a aucun) : **× 1,5 à 2**

→ **450 à 600 jours-homme, soit 22-30 mois-homme.**

Avec 3 devs Rust seniors : **12 à 15 mois calendaires**, montée en compétence sur le domaine comprise.

**À comparer au fork chirurgical de §12.5 : 1 à 2 mois-homme pour 80 % de la valeur pipeline. Ratio de 15 à 25×.**

### 13.4 Le piège : réécrire ArmorPaint ne donne pas ce qu'on veut

Les limites bloquantes identifiées en §9 ne sont **pas des limites du langage C**. Ce sont des choix d'architecture :

| Limite | Cause réelle | Le Rust la corrige-t-il ? |
|---|---|---|
| Pas de streaming de textures | 3 render targets pleine res par layer, tout en VRAM | ❌ non — il faut changer le design |
| UDIM = split de mesh | choix de modèle de données | ❌ non |
| Résolution globale, pas par tile | `config_get_texture_res_*` est un singleton | ❌ non |
| Pas d'OCIO | fonctionnalité absente | ❌ non — c'est du travail à faire quel que soit le langage |
| EXR pauvre | exportateur maison | ❌ non (mais la crate `exr` le règle en C aussi bien qu'en Rust) |
| Pas de headless | ordre d'init | ❌ non |

**Porter ligne à ligne en Rust produit un ArmorPaint en Rust — avec exactement les mêmes limites.** Pour lever ces limites il faut changer l'architecture, ce qui n'est plus un portage mais une conception neuve.

Et inversement : ArmorPaint embarque une masse de fonctionnalités dont un pipeline VFX n'a aucun usage — nœuds IA, physique, timeline d'animation, export MP4/H264, mode player WASM, builds Android/iOS, mode sculpt. **Le recouvrement fonctionnel entre « ArmorPaint » et « ce dont un studio VFX a besoin » est faible.** On paierait 25 mois-homme pour reproduire majoritairement ce qu'on ne veut pas.

### 13.5 Ce que Rust apporterait vraiment — et ce qu'il n'apporterait pas

**Gains réels :**
- 🟢 **Sécurité mémoire.** Pertinent ici : parseurs de fichiers non fuzzés (débordement de pile confirmé dans `io_exr.c:26`), manipulation de plusieurs Go de pixels, `free()` commentés en production (`iron_obj.c:643`).
- 🟢 **Parallélisme sûr.** C'est le gain le plus sous-estimé. L'app est **strictement mono-thread** (zéro création de thread dans `paint/sources/`, 323 variables globales). Compositing de layers, dilate, bake, export multi-UDIM sont massivement parallélisables — `rayon` rendrait ça trivial et sûr. En C avec 323 globales, c'est hors de portée.
- 🟢 **`wgpu` apporte un fallback OpenGL/GLES**, ce qui lèverait la limite « pas de fallback » de §9.
- 🟢 **La crate `exr`** (pure Rust) est excellente : ZIP, PIZ, RLE, B44, multipart, deep. Elle réglerait le point §5.4 d'un coup.
- 🟢 Cargo au lieu d'un build maison à binaires commités ; outillage, tests, CI standards.
- 🟢 Recrutable et maintenable **en équipe** — l'inverse du design actuel.

**Ce que Rust n'apporte pas ici — et c'est décisif :**
- 🔴 **OCIO, OpenUSD, OpenImageIO, Ptex, OpenSubdiv sont tous du C++.** En Rust il faut du FFI vers du C++ (les bindings `opencolorio-rs` sont peu maintenus ; rien de mature pour USD ni Alembic). Or **ce sont exactement les briques d'un pipeline VFX.**
- 🔴 Perte définitive de tout suivi de l'upstream (assumé dans la question, mais ça supprime 1000+ commits/an de travail gratuit).
- 🔴 Compilation nettement plus lente que l'unity build C actuel.
- 🔴 Perte du « zéro dépendance » — atout réel pour reproduire un build à 10 ans, ce qui compte en VFX (rebuild d'un show archivé).

**Observation qui en découle** : si l'objectif est un outil VFX, le langage naturel n'est ni C ni Rust, c'est **C++** — parce qu'OIIO, OCIO, USD, Ptex et OpenSubdiv y sont natifs. C'est d'ailleurs ce qu'utilisent Mari, Substance, Katana et Nuke.

### 13.6 Verdict et meilleure option

**Réécrire ArmorPaint en Rust : non.** 25 mois-homme pour obtenir le même outil avec les mêmes limites structurelles, en perdant l'upstream, et en devant quand même faire du FFI C++ pour OCIO et USD.

**Mais la question est juste — elle révèle simplement que le vrai arbitrage est ailleurs.** Trois options cohérentes, par ordre de coût :

| Option | Coût | Ce qu'on obtient |
|---|---|---|
| **A. Fork chirurgical** (§12) | 1-2 mois-homme | 80 % de la valeur pipeline sur un outil qui plafonne à ~4K / peu de layers. Suivi upstream conservé. |
| **B. A + refonte ciblée** du modèle de textures dans le fork | +3-6 mois-homme | Lève le plafond VRAM et la résolution par tile. Fork lourd, upstream difficile à suivre. |
| **C. Outil maison** (Rust **ou** C++), conçu pour le VFX | 18-30 mois-homme | Exactement ce qu'on veut : UDIM tuilé natif, OCIO, EXR, streaming, pipeline. ArmorPaint sert de **référence de conception**, pas de base de code. |

**Si l'enveloppe est de 20+ mois-homme, C est le bon choix — pas la réécriture.** À ce budget, on ne porte pas ArmorPaint : on écrit l'outil dont on a besoin, en s'inspirant librement de ses bonnes idées (le render path de peinture, la génération de shaders `make_*`, le modèle de nœuds). La licence zlib autorise explicitement de s'en inspirer, d'en copier des portions et de garder le résultat privé — pas besoin de clean-room.

Et dans ce scénario, **Rust devient un choix défendable** — pas parce qu'on porte, mais parce qu'on conçoit du neuf : `wgpu` + `egui` + `rayon` + `exr` couvrent l'essentiel, et le FFI vers OCIO/USD reste un coût borné et localisé.

**Recommandation** : commencer par A (1-2 mois, valeur immédiate, réversible). A donne aussi la vraie mesure du besoin : si après six mois d'usage réel le plafond VRAM et l'UDIM sont les seuls points bloquants, on saura précisément quoi construire — et on aura un cahier des charges validé par la production au lieu d'une spéculation.

Ne jamais engager C avant d'avoir vécu A.

---

## 14. Parallélisation — mesures, et correction du §13.5

> Benchmarks écrits et exécutés le 2026-09-14 sur Mac M-series (10 cœurs, SSD NVMe ~2,8 GB/s).
> ⚠️ Ce sont des **micro-benchmarks isolés** reproduisant les boucles d'ArmorPaint. L'app n'a **jamais pu être compilée ni exécutée** (pas d'Xcode, cf. §2) — aucun profil runtime réel n'a été obtenu.

### 14.1 Mesures

**Encodage EXR** — 4096×4096, RGBA half → RGB half, une map :

| Méthode | Temps | Gain |
|---|---|---|
| ArmorPaint (`u8_array_push` octet par octet) | **36 ms** | — |
| Écriture directe pré-allouée, 1 thread | 7 ms | ×5,1 |
| Écriture directe, 10 threads | 4 ms | ×8,4 |

**Sauvegarde projet `.arm`** — LZ4 (`lz4x.h` du projet), 4K/16-bit, 15 layers × 3 textures = 5,6 GB :

| | Temps |
|---|---|
| Mono-thread (actuel) | **5,7 s** |
| 10 threads | **1,4 s** (×4,2) |

**I/O disque** — export UDIM 20 tiles × 5 maps :

| | Volume | Temps (SSD local 2,8 GB/s) | Temps (NAS 200 MB/s) |
|---|---|---|---|
| EXR non compressé (actuel) | 9,4 GB | 3,4 s | **~47 s** |
| EXR + ZIP (~2,5×) | 3,8 GB | 1,4 s | **~19 s** |

### 14.2 Ce que ça change — correction

**J'ai écrit en §13.5 que le parallélisme était « le gain le plus sous-estimé ». Les mesures disent le contraire.**

- L'encodage EXR par `u8_array_push`, que je pensais catastrophique, coûte **36 ms** en 4K. Le compilateur inline bien et le `realloc` amorti fonctionne. Paralléliser fait gagner 32 ms par map — **3 secondes sur un export UDIM complet**. Négligeable.
- La sauvegarde projet gagne 4,3 s. Réel mais modeste.
- **Le gain d'I/O vient de la compression, pas du parallélisme** — et il est décisif sur stockage réseau, qui est la norme en studio : 47 s → 19 s par export.

**Raison de fond** : tout ce qui est lourd dans ArmorPaint est **déjà sur GPU** — peinture, compositing de layers, dilate, bake (RT hardware), path tracing. Le CPU ne fait que de l'orchestration, du readback et de l'encodage. Paralléliser les boucles CPU existantes optimise ce qui n'est pas le goulot.

**Le vrai goulot, non mesurable sans binaire**, est ailleurs :
1. Le **readback GPU→CPU** (`gpu_get_texture_pixels`, `export_texture.c:367`) — synchronisation + transfert PCIe de centaines de Mo
2. Le **flatten des layers** sur GPU, refait pour chaque tile UDIM
3. Et surtout : **est-ce que ça tient en VRAM** (§5.3) — le vrai mur

### 14.3 Le parallélisme qui paierait vraiment

Pas « paralléliser les boucles », mais **deux choses structurelles** :

**a) Le parallélisme de processus — gratuit, et déjà bloqué par autre chose.**
Traiter 50 assets, c'est lancer 50 instances sur une ferme. Aucun thread nécessaire, gain linéaire immédiat. **Sauf que `--background` n'est pas headless** (§5.1) : il faut un display et un GPU par instance. → **Le vrai levier de débit pipeline n'est pas le multithread, c'est le headless.** 1-2 semaines de travail, pas 25 mois.

**b) Le streaming de tuiles asynchrone — le seul parallélisme qui lève le mur.**
Charger/décompresser/évincer des tuiles de texture en tâche de fond pendant que l'artiste peint. C'est ce qui permet le 8K/16K multi-UDIM, et c'est exactement le design de Mari. Ce n'est pas une optimisation, c'est **une architecture** — donc l'option C de §13.6, pas un patch.

### 14.4 Conséquence sur l'arbitrage A / B / C

**Le facteur « parallélisation » ne déplace pas la frontière entre les options.** Il la rend plus nette :

| Option | Effet du parallélisme |
|---|---|
| **A** — fork chirurgical | Gains quasi nuls à paralléliser le CPU. **En revanche** : compression EXR (I/O ÷2,5, décisif sur NAS) + mode headless (débit ×N sur ferme) donnent **tout le gain de débit réel, sans aucun multithread.** |
| **B** — refonte du modèle de textures | C'est là que le parallélisme commence à compter (streaming), mais dans les 323 globales d'un code mono-thread C, c'est très risqué. |
| **C** — outil neuf | Le streaming concurrent est un choix de conception dès le départ. |

**Et c'est ici que Rust cesse d'être un goût pour devenir un argument technique.** Un cache de tuiles concurrent — workers de décompression, éviction LRU, accès depuis le thread de rendu et le thread I/O — est précisément la structure où le C devient dangereux (data races silencieuses, use-after-free) et où l'ownership de Rust élimine des classes entières de bugs à la compilation. `rayon` pour le calcul, `tokio` pour l'I/O, `Arc`/`RwLock` pour le cache.

Mais cet argument **ne vaut que pour l'option C**. Pour A et B, le langage n'a aucune importance : le gain est dans la compression et le headless, deux choses qui se font en C en quelques semaines.

### 14.5 Verdict révisé

**La parallélisation ne justifie pas à elle seule de passer à Rust.** Elle ne rentabilise pas un portage (§13) : les mesures montrent des gains de quelques secondes sur des opérations qui n'en prennent déjà que quelques-unes.

Ordre de priorité réel pour le débit pipeline, du plus rentable au moins :

1. **Compression EXR** — I/O ÷2,5, stockage ÷2,5. Quelques jours (crate `exr` ou tinyexr). 🔥
2. **Mode headless** — permet N instances sur ferme, débit ×N. 1-2 semaines. 🔥
3. Parallélisation de la sauvegarde `.arm` — 4 s par save. Quelques jours. 🟡
4. Parallélisation des boucles d'export — ~3 s par export UDIM. 🟢 négligeable
5. Streaming de tuiles — lève le mur VRAM, mais c'est l'option C.

**Les deux premiers points, qui portent l'essentiel du gain, se font dans le fork en C sans écrire une ligne de Rust ni un seul thread.**

---

## 15. Validation empirique sur binaire réel (2026-09-15)

> **Le build a enfin abouti.** Cette section corrige et complète les sections précédentes, qui étaient toutes basées sur de l'analyse statique.

### 15.1 Build : ce qu'il fallait faire

Trois blocages successifs sur macOS, dans cet ordre :

1. **Pas d'Xcode.app** → installer Xcode (les Command Line Tools ne suffisent pas : `xcodebuild` + compilateur `metal` requis)
2. **Licence non acceptée** → `sudo xcodebuild -license accept` *(nécessite le mot de passe utilisateur)*
3. **Composants Xcode non initialisés** → `xcodebuild -runFirstLaunch` *(fonctionne **sans** sudo)*

Ensuite : **`../base/make --compile` → BUILD SUCCEEDED en 10 secondes.**

| | |
|---|---|
| Binaire | `paint/build/build/Release/ArmorPaint.app` |
| Exécutable | 2,86 Mo, Mach-O arm64 |
| Bundle complet | 25 Mo |
| Temps de build (complet, from scratch) | **~10 s** |

Le temps de build de 10 s confirme l'atout de l'unity build — à comparer aux minutes qu'imposerait un projet Rust/C++ équivalent.

### 15.2 ⚠️ Correction : le nombre de warnings

**§2 annonçait « 9 warnings seulement ». C'est faux à l'échelle du projet.** Mon syntax-check ne compilait que `paint/sources/main.c` (l'unity build de l'application), pas le moteur `base/sources/` ni les plugins.

Chiffres réels du build complet : **3371 warnings**, soit **1665 uniques** (fichier:ligne dédupliqués).

Répartition :

| Type | Nombre | Gravité |
|---|---|---|
| `-Wstrict-prototypes` (`void f()` au lieu de `void f(void)`) | **3058 (91 %)** | 🟢 bruit stylistique pur |
| `-Wcomma` | 82 | 🟡 |
| **`-Wincompatible-pointer-types`** | **53** | 🔴 **réel** |
| `-Wunused-variable` | 21 | 🟢 |
| `-Wdeprecated-declarations` | 19 | 🟡 |
| **`-Wsometimes-uninitialized`** | **17** | 🔴 **réel** |
| `-Wunused-function` | 14 | 🟢 |
| **`-Wconditional-uninitialized`** | **5** | 🔴 **réel** |
| **`-Wformat`** | **4** | 🔴 **réel** (crash possible) |

Le gonflement vient de l'unity build : un header inclus partout voit ses warnings recomptés à chaque inclusion (852 pour `functions.h`, qui ne contient qu'une centaine de déclarations uniques).

**Verdict révisé** : ~91 % du bruit est cosmétique et se corrigerait d'un coup avec `(void)` dans les prototypes. Restent **~79 warnings réellement suspects**, concentrés dans `kong_spirv.c` (16), `startup.c` (13, généré), `iron_ui_nodes.c` (10), `plugins.c` (8), `iron_map.c` (7).

Le jugement de §7 (« remarquablement propre ») **tient toujours sur le code applicatif** (`paint/sources/`, qui était bien à 9 warnings), mais **pas sur le moteur et le compilateur de shaders**. À nuancer.

### 15.3 🔴 BUG MAJEUR : `--background --export-textures` ne produit rien

**Découvert au test, confirmé par lecture du code.**

```bash
# Ne produit AUCUN fichier — quitte en 0,17 s
ArmorPaint --background cube.obj --export-textures png generic /out

# Les MÊMES arguments sans --background → 5 PNG corrects
ArmorPaint cube.obj --export-textures png generic /out
```

**Cause** — [args.c:190](paint/sources/args.c:190) vs [args.c:214](paint/sources/args.c:214) :

```c
if (args_export_textures) {
    ...
    sys_notify_on_next_frame(&args_run_export_queue, NULL);   // ligne 190 : export à la frame SUIVANTE
}
...
if (args_api)          { ... }
else if (args_script)  { ... }
else if (args_background) { iron_stop(); }                     // ligne 215 : arrêt à la frame COURANTE
```

`iron_stop()` s'exécute avant que la frame suivante n'arrive. L'export programmé n'a jamais lieu. Idem pour `--export-mesh` et `--export-material` (qui, eux, s'exécutent en synchrone — à vérifier au cas par cas).

**Conséquence** : **le mode batch documenté de l'outil est inopérant.** C'est le mode que tout pipeline utiliserait en premier.

**Correctif** (quelques lignes, candidat idéal à une PR upstream) : faire appeler `iron_stop()` par `args_run_export_queue` lorsque `args_background` est vrai, au lieu de l'appeler immédiatement.

### 15.4 ✅ Le contournement fonctionne — et il est excellent

`--script` suit un chemin de code différent : `args_run_script()` appelle lui-même `iron_stop()` **après** exécution ([args.c:126](paint/sources/args.c:126)).

```c
// export.c
void main() {
    export_texture_run("/chemin/de/sortie", 0);
}
```

```bash
ArmorPaint --background cube.obj --script export.c
```

**Résultat mesuré** : `Textures exported` + **5 PNG produits**, en **0,42 s** — boot moteur, import OBJ, rendu des layers, readback GPU et écriture des 5 maps compris.

**C'est décisif pour les chantiers pipeline** : le batch headless est **utilisable dès aujourd'hui**, sans patcher une seule ligne, via `--script`. Et l'API de scripting expose tout le nécessaire (`--api` produit **1241 lignes** de référence, dont `export_texture_run`, `script_project_open`, `project_save`, `script_export_mesh`).

⚠️ Non vérifié : **si une fenêtre est réellement créée** en `--background`. Le code de §5.1 dit que `sys_start()` précède `args_parse()`, donc probablement oui. À tester sur une machine sans display (SSH sans X, nœud de ferme) avant de dimensionner l'industrialisation.

### 15.5 Autres constats du binaire

- `--api` fonctionne et produit **1241 lignes** de référence C de l'API de scripting → à générer une fois et versionner dans le dépôt studio, c'est la documentation de référence pour les lots.
- `.arm` est traité comme **projet**, jamais comme mesh ([iron_path.c:290](base/sources/iron_path.c:290)) — passer un `.arm` de `paint/assets/meshes/` en argument échoue silencieusement. Piège à connaître.
- `path_is_folder()` teste seulement l'absence de point dans le dernier segment ([iron_path.c:399](base/sources/iron_path.c:399)) — un dossier contenant un point est rejeté comme destination d'export. Fragile pour des chemins studio type `/prod/show.v2/...`.
- L'export produit `untitled_base.png`, `untitled_nor.png`, `untitled_occ.png`, `untitled_rough.png`, `untitled_metal.png` — **le préfixe `untitled` est le nom de fichier du projet**, ce qui confirme le besoin du moteur de nomenclature (lot B3).

### 15.6 Impact sur le plan de lots

| Lot | Révision |
|---|---|
| **B7 — batch & headless** | **Revu à la baisse.** Le batch via `--script` fonctionne déjà. Reste : corriger le bug §15.3 (~1 j, upstreamable) + valider le headless sans display. Estimation **8 j → 4 j**. |
| **C1 — pont d'exécution** | Confirmé faisable : l'API de scripting est riche et opérationnelle. |
| **B3 — nomenclature** | Confirmé indispensable (`untitled_base.png`). |
| Correctifs upstream | ~~Ajouter le bug §15.3 à la liste des PR à proposer~~ → **corrigé localement le 2026-09-26** (patch `background-export`, §16). Décision : aucune PR upstream. |

---

## 16. Mise en œuvre — état au 2026-09-26

**Décision de Guillaume : aucune PR upstream.** Les correctifs sont faits chez nous et inventoriés pour être réanalysés ou réappliqués à chaque pull. Cela remplace les recommandations « PR upstream » des §8, §12.5, §12.8 et du plan de lots.

**Organisation (depuis le 2026-09-26, un seul dépôt, privé : `github.com/guillaumeparra/armorpaint-studio`) :**
- Branche **`upstream`** : le code d'armory3d tel quel. On n'y commite jamais ; elle avance en fast-forward sur `armory3d/main`.
- Branche **`studio`** (par défaut) : `upstream` + nos patches sur les fichiers upstream (un commit par patch) + nos plugins dans `paint/plugins/external/`. Ce dossier, ignoré par l'upstream, est suivi ici (patch `track-external`) ; son ancien dépôt séparé y a été importé avec son historique (`git subtree add`).
- Ce document et `ARMORPAINT_LOTS.md` sont suivis à la racine de `studio` (ils ne font pas partie de l'upstream).
- Remotes : `armory3d` (lecture seule, push désactivé) et `origin` (le dépôt privé). Mise à jour : `git fetch armory3d`, fast-forward d'`upstream`, `git merge upstream` dans `studio`, push des deux branches.
- `git diff upstream studio -- ':!paint/plugins/external' ':!ARMORPAINT_*.md'` = exactement nos modifications du code upstream.
- `external/PATCHES.md` est le registre des patches upstream : pour chacun, le bug, le correctif, le test et les signes que l'upstream l'a corrigé. `external/patches/<id>.patch` + `<id>.base` (dernier commit upstream validé).
- `external/tools/check_patches.sh` signale, pour chaque patch, s'il est présent, perdu (réapplicable), en conflit, ou si l'upstream a modifié les fichiers patchés depuis la dernière validation. Les hooks git `post-merge` et `post-rewrite` d'armorpaint le lancent après chaque pull ou rebase (`tools/install_hooks.sh`).

**Patches actifs :**

| id | Fichier | Objet |
|---|---|---|
| `background-export` | `paint/sources/args.c` | Bug §15.3 corrigé : `--background --export-textures` produit bien les fichiers (PNG/EXR, octet pour octet identiques au mode fenêtré, ~0,5 s) |
| `sdefl-bound` | `base/sources/libs/sdefl.h` | Bug trouvé au lot B2 : `sdefl_bound` ne comptait pas l'en-tête de bloc stocké ajouté à chaque bloc de 256 Kio. Débordement de tas sur des données incompressibles, atteint aussi par l'export PNG upstream |
| `studio-export` | `paint/sources/io/export_texture.c` | Lots B2/B4/B5 : 3 points d'injection (début, calques aplatis, fin) vers `external/studio_export.c`. Presets studio, préflight, écriture atomique, manifeste |

Liste complète et à jour : `external/PATCHES.md` (`tools/check_patches.sh` la vérifie).

**Constat sur `85f6cf1c`** : le hook `external/` est toujours là, désormais dans `paint/plugins/project.c` (`fs_exists("external")` → `add_cfiles("external/**")`, shaders `external/*.shader`, assets `external/assets/*`, `WITH_EXTERNAL`). amake ignore les dossiers commençant par un point, donc le `.git` d'`external/` ne gêne pas.

