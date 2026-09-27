# ArmorPaint — plan de lots

> Découpage opérationnel des trois chantiers : **OCIO**, **exporter studio**, **ShotGrid workfiles**.
> Base : commit `2efd0ff3`. **Décision 2026-09-26 : aucune PR upstream.** Tout ce que ce plan marque « PR upstream » (C1, correctifs) se fait localement, sur la branche `studio` ou dans `external/`, et s'inscrit dans `external/PATCHES.md`. Voir `ARMORPAINT_ANALYSE.md` pour l'analyse technique de référence.
> Estimations en **jours-homme** pour un dev C à l'aise en graphismes, hors montée en compétence sur le codebase (compter +5 j au démarrage).

---

## Principe directeur commun

Règle non négociable, issue de §12.5 de l'analyse :

> **Tout le code studio vit dans `paint/plugins/external/`** — un dossier déjà présent dans le `.gitignore` upstream, compilé automatiquement et initialisé via `external_init()` ([paint/plugins/plugins.c:179](paint/plugins/plugins.c:179)).
> **Les fichiers upstream ne reçoivent que des points d'injection d'une à trois lignes**, jamais de logique métier.

Concrètement :
- `paint/plugins/external/` est un **dépôt git séparé** (submodule ou symlink vers un repo studio)
- Chaque patch du cœur est un appel à une fonction `studio_*` déclarée en forward declaration
- Un fichier `external/PATCHES.md` liste les injections, pour les rejouer si un rebase les perd

Squelette de départ (lot 0, **2 j**) — ✅ **fait le 2026-09-26** (`external.c`, `studio.h`, `PATCHES.md`, `tools/`). Les modules `studio_*` seront créés avec leur lot :

```
paint/plugins/external/
├── external.c              // external_init() : point d'entrée unique
├── studio_ocio.c/.h
├── studio_export.c/.h
├── studio_shotgrid.c/.h
├── studio_ui.c/.h          // panneaux et boîtes de dialogue
├── vendor/                 // tinyexr, OCIO wrapper…
├── assets/                 // icônes, configs par défaut
└── PATCHES.md              // inventaire des injections dans l'upstream
```

---

# Chantier A — OCIO

**Objectif** : cohérence colorimétrique de bout en bout — import, viewport, export — pilotée par une config OCIO studio.

**État actuel** : néant. 2 color spaces (`linear`/`srgb`), le « srgb » étant un gamma 2.2 pur ([export_texture.c:3](paint/sources/io/export_texture.c:3)) et non la courbe sRGB par morceaux. Tonemap Filmic figé dans le shader. LUT `.cube` 8 bits clampé 0-1, viewport seulement.

### A0 — Décision d'architecture · **3 j** — ✅ **fait le 2026-09-26** : voie A, OCIO 2.5.2 lié en statique (macOS)

Trancher entre deux voies. À faire en premier, tout le reste en dépend.

| | **A. OCIO v2 réel (FFI C++)** | **B. Pipeline ACES figé en dur** |
|---|---|---|
| Couverture | Toute config studio, tout colorspace | Un seul workflow, codé en matrices |
| Coût | 8-12 j d'intégration | 2-3 j |
| Dépendance | OpenColorIO + OpenEXR + Imath (C++) | aucune |
| Risque build | Élevé (C++ dans un projet C, 5 plateformes) | Nul |
| Verdict | **Recommandé si le studio a déjà une config OCIO** | Acceptable si un seul show / un seul workflow |

Le reste du chantier suppose **la voie A**. Avec la voie B, retirer A1 et diviser A2-A4 par deux.

**Livrable** : note de décision + POC de link OCIO sur une plateforme.

**Décision (2026-09-26).**
- **Voie A, le vrai OpenColorIO**, version épinglée `v2.5.2` (VFX Reference Platform CY2026). Il est construit
  depuis ses sources, dépendances comprises, par `external/tools/build_ocio.sh`. Pas de binaires versionnés : les
  produits sont ignorés par git et reconstruits par le script (2 à 3 minutes la première fois).
- **Configuration.** `$OCIO` s'il est défini ; sinon la configuration studio intégrée à OCIO
  (`ocio://studio-config-latest`, ACES 2.0). Un `$OCIO` invalide est une erreur, sans repli.
- **macOS : liaison statique, pas de dlopen.** ArmorPaint est signé ad hoc avec le hardened runtime, et sa
  validation des bibliothèques refuse toute dylib qui n'est pas de la même équipe. Le POC par dlopen a échoué
  pour cette raison.
  - `build_ocio.sh` fusionne le shim, OCIO et ses dépendances en un objet (`ld -r`) où seuls les 24 symboles
    `studio_ocio_lib_*` restent visibles. L'archive est liée par `external/.project/project.c`.
  - Ce projet est évalué par amake grâce à une injection upstream d'une ligne : patch `external-project`. Le hook
    `external` ne permet pas d'ajouter une bibliothèque.
  - Coût : environ 7 Mo sur le binaire.
- **Linux : dlopen** de `data/libstudio_ocio.so`. Écrit, non testé.
- **Windows : à faire.** Le shim et le CMake sont portables ; restent le script et la liaison.
- **POC** : dans l'app, ACEScg → `sRGB - Texture` donne les mêmes valeurs que PyOpenColorIO.

### A1 — Wrapper C autour d'OCIO v2 · **8 j**

OCIO est du C++ ; le projet est du C. Il faut une couche `extern "C"`.

- `vendor/ocio_c/ocio_c.cpp` — API minimale :
  ```c
  bool  studio_ocio_init(const char *config_path);   // $OCIO ou chemin explicite
  int   studio_ocio_colorspace_count(void);
  const char* studio_ocio_colorspace_name(int i);
  const char* studio_ocio_display_default(void);
  const char* studio_ocio_view_default(const char *display);
  // CPU : transforme un buffer en place
  bool  studio_ocio_transform_cpu(void *px, int w, int h, int channels, int bits,
                                  const char *src_cs, const char *dst_cs);
  // GPU : récupère le shader + la LUT 3D générés par OCIO
  bool  studio_ocio_gpu_shader(const char *src_cs, const char *display,
                               const char *view, char **glsl_out,
                               void **lut3d_out, int *lut3d_edge);
  ```
- Build : ajouter le C++ dans `paint/plugins/project.js` via le bloc `external` (déjà prévu, cf. [paint/plugins/project.js:12](paint/plugins/project.js:12))
- ⚠️ **Le vrai risque est ici** : linker OCIO+Imath sur Windows/Linux/macOS. Prévoir des builds statiques pré-compilés par plateforme, versionnés dans le dépôt studio.

**Livrable** : lib liée, `studio_ocio_init()` charge la config studio et liste les colorspaces.

✅ **Fait le 2026-09-26** (macOS ; Linux écrit, non testé ; partie GPU reportée en A2).
- **Shim et API.** Le shim C++ `external/.ocio/studio_ocio_lib.cpp` expose une ABI C versionnée
  (`studio_ocio_lib.h`), sans exception qui la traverse. Le module `external/studio_ocio.c/.h` fournit :
  configuration et statut, listes (espaces, displays, views, looks, rôles), alias, `is_data`, encodage, file_rules,
  et une transformation CPU RGB/RGBA sur place.
- **Transformation.** Processeurs en cache par couple, appel sûr depuis plusieurs threads. L'alpha n'est jamais
  transmis à OCIO : certaines vues ACES 2.0 le modifient.
- **API de scripting** minic.
- **Tests.**
  - `.tests/ocio.sh`, sous ASan/UBSan : liaison statique et dlopen. Résultat **bit à bit** égal à PyOpenColorIO
    2.5.2 sur la configuration du banc (LUT, look, file_rules), la studio et la CG : 12 576 valeurs. Couvre aussi
    les threads, les entrées invalides, les rechargements et les bibliothèques absentes ou incompatibles.
  - `.tests/ocio_app.sh` : dans l'app, avec le défaut intégré, un `$OCIO` de test et un `$OCIO` invalide.
- **Différent du plan.** Le build passe par `.project/project.c` et non par le bloc `external` de `project.js`.
  `studio_ocio_transform` ne prend que du float32 : les exports 8/16 bits convertissent avant l'encodage (A3).

### A2 — Viewport : display transform · **5 j** — ✅ **fait le 2026-09-26** (macOS)

- Point d'injection : le compositor. OCIO génère du GLSL ; le projet utilise `kong` → MSL/HLSL/SPIR-V. **Deux options** :
  - **a)** Utiliser la LUT 3D générée par OCIO (`getGpuLut3D`) et l'injecter dans le chemin LUT existant, en remplaçant le stockage 8 bits par du **RGBA half ou float** et en supprimant le clamp 0-1 ([import_lut.c:78](paint/sources/io/import_lut.c:78), [compositor_pass.kong:67](paint/shaders/compositor_pass.kong:67)). **Recommandé** — réutilise l'existant, ~3 j.
  - **b)** Traduire le GLSL OCIO vers kong. Coûteux et fragile.
- Retirer le `pow(color, 0.4545)` appliqué avant le lookup ([compositor_pass.kong:69](paint/shaders/compositor_pass.kong:69)) — il est incompatible avec une LUT OCIO.
- Rendre tonemap et LUT **non exclusifs** (aujourd'hui `if`/`else`).
- UI : sélecteurs Display / View / Look dans la barre de statut.

**Injection upstream** : ~3 lignes dans `compositor_pass.kong` + 2 dans `uniforms.c`.

**Livrable** : le viewport affiche via la chaîne OCIO du studio ; changement de view à chaud.

✅ **Fait le 2026-09-26** (macOS).
- **Rendu.** En modes Lit et Path Trace, le compositeur remplace le tonemap filmique par la transformation
  display/view/look d'OCIO, depuis l'espace de travail. Elle est calculée sur le CPU par le processeur exact d'OCIO
  en une LUT 3D 129³ RGBA half, avec une mise en forme ACEScct de l'entrée HDR (`external/studio_ocio_lut.c`), puis
  interpolée par le shader. Environ 0,1 s par calcul. La LUT `.cube` s'applique après, sur les valeurs d'affichage.
- **Patch upstream `ocio-viewport`** : `compositor_pass.shader`, `startup.c` (une constante, une texture) et
  `uniforms.c` (valeurs neutres : sans plugin, le compositeur est celui d'upstream). Le plugin
  (`external/studio_ocio_view.c`) chaîne les liens d'uniformes.
- **UI** : menu Color (case OCIO Viewport, sous-menus Display, View, Look ; vues HDR marquées). Variables
  `STUDIO_OCIO_VIEWPORT`, `STUDIO_OCIO_DISPLAY`, `_VIEW`, `_LOOK` ; API minic. Nouvelle fonction de l'ABI (v3) :
  l'espace d'affichage d'une vue, qui repère les vues HDR.
- **Précision** contre OCIO exact : gris ≤ 0,3/255 ; 99,9 % des couleurs ≤ 2,3/255 en sRGB ACES 2.0 (≤ 4/255 sur
  les autres displays SDR) ; maximum 8,9/255 en sRGB (17,5/255 en P3-D65) sur des couleurs très saturées, que la
  compression de gamut ACES 2.0 traite selon la teinte. Tableau complet dans le README.
- **Tests.** `.tests/ocio.sh` lance `ocio_view_test.c` (half, courbe, tuiles, interpolation exacte d'une LUT
  linéaire, précision de chaque view sur 400 000 couleurs, configurations studio et CG). `.tests/ocio_view_app.sh`,
  dans l'app : des couleurs passent par le vrai compositeur et sont comparées à PyOpenColorIO (≤ 0,14/255), puis le
  choix, les erreurs, le retour au filmique, le mode couleur de base, le rechargement de configuration et les
  variables d'environnement.
- **Différent du plan.**
  - Pas de LUT générée par OCIO : `getGpuLut3D` est l'API d'OCIO v1, et le shader GPU d'OCIO v2 pour ACES 2.0
    est analytique (320 lignes, deux tables 1D), sans LUT 3D. La LUT est échantillonnée depuis le processeur CPU,
    pour toute vue et tout look.
  - La LUT `.cube` d'upstream n'est pas modifiée : la LUT OCIO a sa propre texture half et sa propre lecture.
  - Menu Color dans la barre de menus plutôt que la barre de statut (le point d'injection `menubar-hook` existait).
  - Le choix n'est pas mémorisé : préférences et projet au lot A5. Les sphères d'environnement gardent leur tonemap
    filmique. Traduire le shader GPU d'OCIO en kong (sans erreur de LUT) reste une piste pour A6, si un show en a besoin.

### A3 — Export : transform par texture · **6 j** — ✅ **fait le 2026-09-26** (macOS)

- Remplacer le champ `color_space` (2 valeurs) par un nom de colorspace OCIO dans le preset d'export ([box_export.c:330](paint/sources/ui/box_export.c:330), [main.c:250](paint/sources/main.c:250)).
- Injecter la transformation **avant** l'encodage, dans `export_texture_write_texture()` ([export_texture.c:6](paint/sources/io/export_texture.c:6)) :
  ```c
  studio_ocio_transform_cpu(pixels->buffer, res_x, res_y, 4, bits, working_cs, t->color_space);
  ```
- Définir le **working space** du projet (probablement ACEScg ou sRGB-linear) et le stocker dans le `.arm`.
- Rétrocompatibilité : mapper l'ancien `"linear"`→`scene_linear`, `"srgb"`→`srgb_texture`.
- Corriger au passage le gamma 2.2 → vraie courbe sRGB si la voie B est retenue.

**Injection upstream** : 1 ligne dans `export_texture.c` + le champ de preset.

**Livrable** : un preset peut demander `ACEScg`, `sRGB - Texture`, `Utility - Raw` etc. par texture.

✅ **Fait le 2026-09-26** (macOS).
- **Preset.** Dans un preset studio (B4), `color_space` accepte tout nom d'espace OCIO (nom, alias ou rôle), validé
  à l'export contre la configuration chargée. Hérité de `defaults`, comme les autres clés.
- **Conversion.** Dans la composition studio (B5), ligne par ligne et dans ses threads : couleurs décodées,
  expressions, RGB converti de l'espace de travail vers la cible, puis quantification. L'alpha ne passe pas par
  OCIO, une texture à 1 canal est convertie comme un gris, un espace de donnée écrit les valeurs brutes.
- **Espace de travail** : l'équivalent de `lin_rec709_srgb` dans la configuration (`Linear Rec.709 (sRGB)` en ACES),
  trouvé par `IdentifyBuiltinColorSpace`, sinon `$STUDIO_OCIO_WORKING_SPACE`. Nouvelle fonction de l'ABI (v2).
- **Préflight** : 4 codes bloquants (OCIO indisponible, espace inconnu, pas d'espace de travail, pas de conversion)
  et 4 avertissements (couleur en donnée, donnée en couleur, EXR non linéaire, linéaire en 8 bits).
- **Manifeste** : espace canonique et identifiant de conversion par fichier ; version, configuration et espace de
  travail au niveau du manifeste.
- **Tests.** `.tests/ocio_export_app.sh`, dans l'app : 13 textures d'une texture procédurale (1,7 million de
  couleurs) en EXR 32/half, TIFF 16 et PNG 8, relues et comparées à PyOpenColorIO : **identiques au bit près**. Plus
  le préflight, le manifeste et les cas bloquants. `.tests/ocio.sh` couvre l'espace de travail hors app.
- **Différent du plan.**
  - Aucune injection upstream : la conversion se fait dans la composition studio, pas dans
    `export_texture_write_texture()`. Les presets upstream restent en `linear`/`srgb`.
  - Pas de mappage des mots-clés : `raw`, `linear` et `srgb` gardent leur sens (les conversions d'ArmorPaint, sans
    OCIO), et les presets livrés les gardent pour marcher sans OCIO. `Linear Rec.709 (sRGB)` donne au bit près le
    même fichier que `linear`.
  - L'espace de travail vient de la configuration ou d'une variable d'environnement ; le stocker dans le `.arm` va
    au lot A5.

### A4 — Import : interprétation des textures entrantes · **4 j** — ✅ **fait le 2026-09-27** (macOS)

- À l'import d'une texture ([import_texture.c](paint/sources/io/import_texture.c)), proposer un colorspace source (avec heuristique : `_basecolor`→sRGB, `_nor`/`_rough`/`_metal`→Raw).
- Transformer vers le working space à l'import.
- Étendre l'enum `color_space_t` ([enums.h:452](paint/sources/enums.h:452)) ou le remplacer par une chaîne OCIO.
- Node `Image Texture` : exposer le colorspace dans les boutons du nœud.

**Livrable** : une texture importée est interprétée correctement.

✅ **Fait le 2026-09-27** (macOS).
- **Conversion au décodage.** Chaque texture importée a un espace source OCIO ; ses pixels sont convertis vers
  l'espace de travail juste avant l'envoi au GPU, sur tous les cœurs : en 8 bits réencodés gamma 2.2 (la convention
  d'ArmorPaint), en flottant gardés linéaires ; un espace de donnée n'est pas converti, l'alpha jamais. Patch
  upstream `ocio-import` : un pointeur de fonction dans `iron.h`, appelé au décodage ; l'import de texture enveloppé
  (`import_texture.c`) ; l'entrée du `.arm` écrite à l'enregistrement (`export_arm.c`) ; infobulle et menu
  contextuel (`tab_textures.c`).
- **Espace source**, dans l'ordre : choix enregistré (de l'artiste, ou de l'import précédent : un projet rouvert se
  décode comme avant), `.k` sans conversion, nom de donnée (étiquettes d'ArmorPaint et `mask`) → `Raw`, `file_rule`
  autre que `Default` (`.exr` → ACES2065-1 dans les configurations studio et CG), défaut du format (8 bits → sRGB,
  `.hdr` → Linear Rec.709, autre flottant → `scene_linear`).
- **UI** : « Color Space... » dans le menu contextuel de l'onglet Textures, « Texture Color Spaces... » dans le menu
  Color (toutes les textures) : « Automatic » ou un espace de la configuration, texture rechargée depuis son
  fichier ; espace et source dans l'infobulle. `STUDIO_OCIO_IMPORT=0` désactive le lot. API minic.
- **Projet** : les espaces voyagent dans le `.arm`, entrée `studio_color_spaces.json` du paquet d'assets (qu'ArmorPaint
  sans studio garde), avec la configuration et l'espace de travail ; un changement de configuration est signalé à la
  réouverture.
- **Tests.** `.tests/ocio_import_app.sh`, dans l'app : pixels relus du GPU et comparés à PyOpenColorIO
  (`ocio_import_check.py`) pour PNG, TIFF, PSD à plat et à calques, EXR float et half (valeurs extrêmes, NaN,
  infinis), `.hdr`, donnée ; choix par texture, vignette du navigateur, `.arm` rouvert, paquet d'assets sans les
  fichiers, désactivation, nœud Image Texture : 31 contrôles, tous passent.
- ✅ **Bugs trouvés en route, corrigés** :
  - nœud Image Texture (upstream, depuis 2023) : ses options Linear et sRGB convertissaient à l'envers (patch
    `image-texture-color-space`) ; Auto vaut désormais Linear sur une texture flottante convertie. **Change le rendu
    des projets qui réglaient Linear ou sRGB.**
  - encodage PNG/JPEG (upstream, depuis 2024) : débordement de tas dans `_encode_image` (tampon de `w * h * 4`
    octets), qui corrompait les textures du paquet d'assets (Pack Assets) et faisait planter leur réouverture (patch
    `encode-image-overflow`).
  - tinyexr (vendoré) : un bloc deflate à 19 codes de distance était décodé faux (des EXR ZIP d'OpenEXR 3.5 ne
    s'importaient pas) ; l'Adler-32 n'était pas vérifié (correctifs `deflate-hdist-19`, `zlib-adler32`).
- **Différent du plan.**
  - Pas d'espace OCIO dans les boutons du nœud Image Texture, ni d'enum `color_space_t` étendu : le choix se fait par
    texture, puisque ce sont les pixels qui sont convertis au chargement. L'option du nœud reste la conversion gamma
    2.2 d'ArmorPaint (corrigée).
  - Pas de question à l'import : l'heuristique décide seule (nom, file_rule, format), l'artiste corrige ensuite.
  - La règle `Default` des file_rules n'est pas suivie (elle désigne `sRGB - Display`, qu'OCIO convertirait par la
    vue inverse) ; le rôle `texture_paint` n'est pas lu (défaut 8 bits : l'équivalent de `srgb_tx`).
  - Le numéro de tuile UDIM est retiré du nom avant la reconnaissance des données.
  - Limites : couleurs bornées à [0, 1] en 8 bits après conversion ; textures du paquet d'assets déjà converties
    (leur espace ne change qu'avec le fichier) et redessinées en 8 bits par ArmorPaint, flottantes comprises
    (upstream) ; calques d'un PSD dans l'espace du fichier ; choix d'un autre `.arm` (matériaux, brosses importés)
    non lus.

### A5 — Config, persistance, UI · **3 j** — ✅ **fait le 2026-09-27** (macOS)

- Onglet OCIO dans les préférences : chemin de config (défaut `$OCIO`), working space, display/view par défaut.
- Persister dans `config.json` et dans le `.arm` (le projet transporte son working space).
- Message d'erreur clair si la config est absente ou invalide — **ne pas échouer silencieusement**.

✅ **Fait le 2026-09-27** (macOS).
- **Préférences studio** : `studio_prefs.json` à côté du `config.json` d'ArmorPaint (ou `$STUDIO_PREFS`), objet JSON
  plat, clés inconnues gardées, écriture atomique, fichier illisible mis de côté en `.bak`. Configuration OCIO,
  espace de travail des nouveaux projets, choix du viewport (display, view, look, OCIO ou filmique) ; les variables
  d'environnement l'emportent (`$OCIO`, `STUDIO_OCIO_*`).
- **UI** : onglet Color des préférences (patch upstream `preferences-tab`) : configuration (champ, Browse, Built-in)
  et statut, espace des nouveaux projets, espace du projet courant. Menu Color : sous-menu Working Space (projet
  courant) et « Color Preferences... ». Au démarrage, une boîte signale OCIO indisponible ou un espace de travail
  introuvable, et où le corriger.
- **Projet** : l'espace de travail voyage dans l'entrée `studio_ocio.json` du `.arm`, avec le nom et la source de la
  configuration. Un nouveau projet suit la préférence jusqu'à son enregistrement, qui fige l'espace trouvé ; un projet
  rouvert reprend le sien ; sans entrée (avant A5, ArmorPaint sans studio) : l'automatique. Autre configuration :
  signalée ; espace inconnu de la configuration : erreur, sans repli. Point d'injection commun des entrées du
  `.arm` : patch `project-entries` (remplace le hunk de `export_arm.c` d'`ocio-import`), module `studio_project`
  (détection du changement de projet, entrées du paquet d'assets), prêt pour le contexte ShotGrid de C5.
- **Changement en cours de session** (configuration ou espace de travail) : les textures converties sont rechargées
  depuis leurs fichiers et converties à nouveau ; celles qui ne sont que dans le paquet d'assets gardent leurs pixels
  (signalé) ; viewport et exports suivent.
- **Tests.** `.tests/ocio_prefs_app.sh`, dans l'app : fichier des préférences (`--background`, écriture, relecture,
  clé inconnue, `$OCIO`, préférence invalide, fichier illisible), espace de travail du projet (nouveau, enregistré,
  rouvert, sans entrée, autre configuration, erreurs), textures reconverties (pixels comparés à PyOpenColorIO),
  viewport : 40 contrôles, tous passent. Onglet, menu et boîte vérifiés à l'écran (captures de la fenêtre).
- **Différent du plan.**
  - Fichier `studio_prefs.json` à part plutôt que `config.json` : `config_save()` d'ArmorPaint réécrit `config.json`
    champ par champ depuis sa structure et perdrait les clés du studio.
  - En `--background`, les préférences ne sont ni lues ni écrites sans `$STUDIO_PREFS` : scripts et bancs ne
    touchent pas aux réglages de l'artiste.
  - L'espace de travail du projet se change dans le menu Color (Working Space) ; les préférences donnent celui des
    nouveaux projets.
  - Pas de display/view par défaut à régler : le dernier choix du menu Color est gardé.
  - Les calques ne sont pas convertis quand l'espace de travail change (ArmorPaint n'a pas de conversion de
    calque) : ce qui est peint garde ses valeurs, lues dans le nouvel espace. Le sélecteur de couleur et les couleurs
    des nœuds ne passent pas par OCIO : leurs valeurs sont celles de l'espace de travail.

### A6 — Validation · **4 j**

- Jeu de mires : rampe linéaire, ColorChecker, valeurs > 1 (spéculaires HDR).
- Comparaison numérique contre `ociobakelut` / `oiiotool --colorconvert` sur les mêmes entrées.
- Round-trip : import sRGB → peinture → export ACEScg → relecture Nuke.

| Chantier A — total | **33 j** (voie A) · **~18 j** (voie B) |
|---|---|

---

# Chantier B — Exporter studio

**Objectif** : produire exactement les fichiers attendus par le pipeline — bons formats, bonne nomenclature, bon endroit — de façon reproductible et vérifiable.

**État actuel** : presets JSON à 3 champs (`name`, `channels`, `color_space`), 23 canaux figés ([main.c:244](paint/sources/main.c:244)), formats PNG/JPG/EXR seulement, nommage `<projet>_<texture>.<ext>` non paramétrable, EXR sans compression ni alpha.

### B1 — Remplacement du moteur EXR · **6 j** — ✅ **fait le 2026-09-26**

Prérequis de tout le reste. Voir §5.4 de l'analyse pour les défauts.

- ✅ **tinyexr v3** (C11 pur, BSD-3) dans `external/vendor/.tinyexr/`, compilé en une unité par `vendor/tinyexr.c`, régénéré par `tools/update_tinyexr.sh`.
- ✅ `studio_exr_write_texture()` : compression (none/rle/zips/zip/piz/pxr24/b44/b44a/htj2k, zip par défaut), **canal alpha**, EXR **mono-canal `Y`** pour rough/metal/AO, attribut `software` + API `studio_exr_set_metadata()` pour les lots B3/C, et **écriture atomique** (`.tmp` puis `rename()`, prévu en B5).
- ✅ Import remplacé depuis `external_init()`, **sans patcher upstream**. Débloque PIZ, B44, pxr24, HTJ2K, les calques, le multipart, le tuilé et le décalage `dataWindow`. Le débordement de pile d'[io_exr.c:26](paint/plugins/io_exr/io_exr.c:26) n'est plus atteignable par l'import.
- ⚠️ **DWAA/DWAB en lecture seulement, par repli** : tinyexr ne les décode pas. Le fichier est validé (`studio_exr_dwa_legacy_safe()`) avant d'être confié au décodeur upstream, qui ne borne rien. Pas d'écriture DWA.

**Injection upstream** : 6 lignes sous `#ifdef WITH_EXTERNAL` dans `export_texture.c`. L'import : **zéro**.

**Gain mesuré** : projet 2048², preset `generic`, 5 maps half — **120 Mo → 4,7 Mo**, à pixels identiques. Une map mono-canal passe de 25,2 Mo à 13 Ko. (L'estimation initiale de ÷2,5 sur NAS était prudente ; elle portait sur le temps d'I/O, pas sur la taille.)

**Effet de bord** : le banc de test (`external/.tests/`, ASan/UBSan fatals, 22 000 mutations) a mis au jour **cinq défauts** sur des fichiers malformés, tous corrigés — quatre dans le décodeur deflate d'iron (`PATCHES.md#sinfl-bounds`, qui protège aussi l'import TIFF) et un dans tinyexr (`vendor/patches/jph-d64-null.patch`).

**Revue du 2026-09-26** : le banc vérifie aussi l'écriture contre une référence indépendante — 60 fichiers écrits par le module (10 compressions × half/float × RGB/Y/RGBA) relus par OpenEXR, identiques au bit près à la relecture tinyexr, et aux valeurs source pour les compressions sans perte. L'import refuse une `dataWindow` démesurée par rapport à l'image (plus de 4 fois sa surface, plancher 4096²) : elle faisait décoder jusqu'à 4 Gio de pixels jetés.

### B2 — Formats additionnels · **4 j** — ✅ **fait le 2026-09-26** (sans Ptex ; macOS)

- ✅ `external/studio_image.c` : **PNG 8/16 bits**. Filtres adaptatifs (heuristique libpng), deflate parallèle par
  bandes, chunk `Software`.
- ✅ **TIFF 8/16/32 bits** (32 = float) : none, **LZW** (compatible libtiff) ou **Deflate**, par bandes de ~256 Kio
  compressées en parallèle. Prédicteur horizontal (entiers) ou flottant (32 bits). Relus au bit près par libpng et
  libtiff sur 498 images, `tiffinfo` sans avertissement.
- ✅ **Profondeur découplée du format** par texture (voir B4). L'EXR passe par l'encodeur B1 en mémoire, le JPG par stb.
- ✅ **Bug upstream trouvé** en testant, corrigé chez nous (patch `sdefl-bound`) : `sdefl_bound` sous-estimait la
  sortie des données incompressibles de plus de 256 Kio. Il en résultait un débordement de tas, atteint aussi par
  l'export PNG upstream via `iron_deflate_raw`.

- **TIFF 16 bits** (attendu par certains départements) — écriture, LZW/Deflate
- **PNG 16 bits** (aujourd'hui impossible : >8 bits force l'EXR, [export_texture.c:215](paint/sources/io/export_texture.c:215))
- Optionnel : **Ptex**, si le pipeline l'utilise (sinon ne pas le faire)
- Découpler **profondeur** et **format** : aujourd'hui 16/32 bits ⇒ EXR obligatoire

### B3 — Moteur de nomenclature · **5 j** — ✅ **fait le 2026-09-26** (option A : templates Toolkit ; macOS, validé sur site simulé)

Plan initial : un moteur de templates maison (JSON dans `external/assets/naming_templates/`, variables `show`,
`asset`, `variant`, `channel`, `udim`…, formatage, prévisualisation live).

**Retenu : les templates Toolkit du studio**, résolus par le sidecar. La syntaxe, les clés et le padding sont ceux
que la production maintient déjà dans `templates.yml`. On n'ajoute ni langage ni fichier de config.

- ✅ **Op `texture_pattern` du sidecar.** Elle résout, depuis le fichier de travail ouvert, le template
  `armorpaint_{asset,shot}_textures` (`STUDIO_SG_TEXTURE_TEMPLATES`, clés `STUDIO_SG_TEXTURE_KEYS`). Les champs
  viennent du fichier (nom, version…) et de son contexte. Le modèle rendu garde des marqueurs `<channel>`, `<UDIM>`
  et `<ext>`, en deux variantes : avec et sans tuile. Le projet Toolkit se déduit du chemin (racine de stockage puis
  `tank_name`).
- ✅ **Côté ArmorPaint** (`studio_naming.c` pur et testé, colle `studio_export_naming.c`) :
  - modèle demandé dès que le projet ouvert change ;
  - à l'export, substitution de chaîne et `mkdir -p` sans shell ;
  - noms upstream hors pipeline ou en cas d'erreur (signalée dans la console) ;
  - pas d'attente si une fenêtre ShotGrid est ouverte ;
  - `STUDIO_SG_NAMING=1` pour la ferme.
- ✅ **Injection upstream `export-naming`** (11 lignes dans `export_texture_run_layers`). Le suffixe objet ou atlas
  se place devant le canal.
- Textures versionnées avec le `.arm` : `robot.v003.arm` → `textures/v003/robot_base.png`. Le dossier de la boîte
  d'export est ignoré quand un template s'applique ; B6 donnera une UI dédiée.
- Écarté : prévisualisation live (B6), variables `colorspace`/`res`/`bits`/`date`/`user` (ce seraient des clés
  Toolkit à passer au sidecar : à faire si un template en a besoin), export « bake material ».
- Non rejoué dans l'app : export par tuile UDIM et par objet, qui ne se règlent que dans l'UI. Les tests couvrent le
  modèle et la substitution.

### B4 — Presets studio étendus · **4 j** — ✅ **fait le 2026-09-26** (colorspace raw/linear/srgb ; espaces OCIO depuis A3)

- ✅ **Format de preset studio** (`external/studio_preset.c`, lecture stricte). Par texture : format, profondeur,
  compression, `color_space`, suffixe et modèle de nom (`<file>[_<variant>]_<channel>[.<UDIM>].<ext>`, sous-dossiers
  permis). Au niveau du preset : résolutions du show, politique d'écrasement, manifeste. Le nommage ShotGrid (B3)
  garde la priorité.
- ✅ **Canaux par expression** (`external/studio_expr.c`) : `+ - * /`, `abs sqrt min max pow step clamp mix`, sur
  les 21 canaux upstream. Constantes repliées à la compilation, évaluation vectorisée par ligne, fuzzée.
- ✅ **Presets livrés** : basic (tous les canaux), Arnold, Karma, Cycles, Unreal Engine, avec les noms de paramètres de chaque shader. RenderMan, V-Ray et Redshift retirés le 2026-09-26 (même contenu qu'Arnold aux noms près : un studio les ajoute par `STUDIO_EXPORT_PRESETS`).
- ✅ Intégrés à la liste d'export existante (boîte, `--export-textures`) par un preset upstream généré. Un preset
  upstream homonyme n'est jamais écrasé. `STUDIO_EXPORT_PRESETS` pour les presets du studio ou du show.
- ✅ Injection upstream `studio-export` : 18 lignes, 3 blocs dans `export_texture.c`. Les boucles upstream (objets,
  atlas, tuiles, bake) restent la seule source des variantes.
- Reste : les canaux d'un nœud de bake (ils passent par le calque aplati).

- Étendre le format de preset : format et profondeur **par texture** (pas globaux), colorspace OCIO, compression, template de nommage, suffixe de canal
- Canaux personnalisés au-delà des 23 en dur — permettre une expression (`occ*rough`, constante, canal d'un nœud de bake)
- Presets livrés : **Arnold**, **Karma**, **RenderMan**, **V-Ray**, **Redshift** — aujourd'hui il n'y a que des presets jeu vidéo

### B5 — Preflight / validation · **5 j** — ✅ **fait le 2026-09-26** (macOS)

- ✅ **Plan exact** par une passe à blanc de l'export upstream, puis **préflight** (`external/studio_preflight.c`).
  Il vérifie :
  - les tuiles UDIM manquantes (trous dans la suite) ;
  - aucun calque visible (bloquant), calques masqués, texture uniforme (« calque vide ? ») ;
  - la résolution hors show ;
  - la cohérence couleur (données en JPG, EXR sRGB, linéaire en 8 bits, profondeur supérieure à la source) ;
  - l'espace disque, les droits d'écriture, les fichiers existants (`warn` / `block`), les doublons de chemin ;
  - les limites PNG et TIFF.
- ✅ **Rapport** texte (boîte de dialogue, console) et JSON (scripts) : fichiers, tailles estimées, bloquant et
  avertissements séparés.
- ✅ **Écriture atomique** : tout en `.studio-tmp`, renommé seulement quand tout est écrit. Un échec ne remplace rien
  (testé dans l'app).
- ✅ **Manifeste** `<file>.export.json` : SHA-256 et taille par fichier, preset (et son empreinte), versions
  d'ArmorPaint et du studio, utilisateur, machine, date, projet, résolution, mode, template ShotGrid, avertissements.
- ✅ API minic : `studio_export_run`, `studio_export_preflight`, `studio_export_last_report`.
- Non rejoué dans l'app : export par tuile UDIM et par objet (plan et préflight testés hors app).

Ce qui distingue un exporter studio d'un bouton « Export ».

- Vérifications **avant** écriture : tiles UDIM manquants, layers vides, résolution non conforme au standard du show, colorspace incohérent, espace disque, droits d'écriture, version déjà existante
- Rapport : liste des fichiers à produire, tailles estimées, avertissements bloquants vs informatifs
- **Écriture atomique** : fichiers temporaires puis `rename()` — jamais de sortie partielle sur le réseau si l'export échoue en cours
- Sidecar `.json` par export : contenu, checksums, contexte, version d'ArmorPaint

### B6 — UI dédiée · **6 j**

- Nouvelle boîte de dialogue « Studio Export », pilotée depuis `external`, plutôt que d'étendre `box_export.c` (fichier chaud, 14 commits/an)
- Sélection preset / template / version / destination, prévisualisation, rapport de preflight
- Un bouton dans le menubar → injection de **2 lignes** dans `ui_menubar.c`
- ✅ **Fait le 2026-09-26** (macOS), en Qt (PySide6, environnement Python fourni par rez) plutôt qu'en boîte native : fenêtre `external/studio_export_window.c` + `assets/studio_export_window.py`. Au lieu d'un bouton de plus, toutes les entrées d'export de textures (menu File, icône, menu de calque, raccourcis) ouvrent la fenêtre : patch `export-window` (4 lignes dans `box_export_show_textures`). Preset, espace OCIO, calques, destination, préflight en direct ; Ctrl+E refait le dernier export. Template/version : pris du fichier de travail (B3), pas encore choisis dans la fenêtre.
- ✅ **Maps et sidecar du projet** (2026-09-26) : profondeur et résolution par map dans la fenêtre (avec « pour toutes »), maps désactivables, sans toucher au preset ; clé `resolution` des presets (mise à l'échelle à l'export, la résolution du projet ne change pas ; résolutions du show vérifiées par texture). Les presets studio sont **seulement lus** (plus aucun fichier écrit dans `data/`, le dossier de l'app n'étant pas inscriptible au studio). Presets du projet et choix de la fenêtre dans un sidecar à côté du `.arm` (`<nom sans version>.export_settings.json`), écrit par la fenêtre seule.
- ✅ **Nom des fichiers et jeux de textures** (2026-09-26) : modèle de nom modifiable dans la fenêtre (menu de variables : `<name>`, `<version>`, `<atlas>`, `<object>`, `<UDIM>`, `<resolution>`, `<bits>`, `<preset>`…, variables inconnues refusées), gardé par preset dans le sidecar et dans « Save as preset » ; le template ShotGrid (B3) reste prioritaire. Les atlas d'ArmorPaint (onglet Atlases devenu inaccessible) s'éditent dans la section « Texture sets » (`external/studio_atlas.c`, enregistrés avec le `.arm`) : un jeu de maps par jeu en export Visible. Maillages de test `.tests/udim_gen.py` (cube à 6 tuiles UDIM, props), banc `.tests/sets_app.sh`. Reste : l'upstream ne remappe pas `atlas_objects` quand l'onglet Meshes trie, réapparente ou supprime des objets.

### B7 — Mode batch & headless · **4 j** *(révisé après test sur binaire — voir §15.6 de l'analyse)*

- ✅ **Le batch fonctionne déjà** via `--background --script` (mesuré : 5 maps exportées en 0,42 s). Pas besoin de l'implémenter.
- 🔴 **Corriger le bug `--background --export-textures`** : `iron_stop()` ([args.c:215](paint/sources/args.c:215)) s'exécute avant l'export programmé ([args.c:190](paint/sources/args.c:190)) → aucun fichier produit. ~1 j. ✅ **Fait le 2026-09-26** : corrigé localement (patch `background-export`, branche `studio`), sans PR upstream.
- Étendre la CLI : preset studio, template, mode UDIM (aujourd'hui `--export-textures` force `EXPORT_MODE_VISIBLE`, [args.c:161](paint/sources/args.c:161))
- **Vrai mode headless** — le gros morceau : `sys_start()` crée la fenêtre à [main.c:389](paint/sources/main.c:389) *avant* `args_parse()` ([base.c:520](paint/sources/base.c:520)). Il faut une fenêtre offscreen et une boucle sans présentation.
- ⚠️ Nécessite quand même un GPU. Sur ferme : nœuds équipés, ou `llvmpipe`/`lavapipe` sur Linux (lent mais fonctionnel).
- Code de retour et log machine-lisible

**Gain** : débit ×N par parallélisme de processus — le levier de débit le plus rentable (§14.3).

| Chantier B — total | **34 j** *(B7 révisé de 8 à 4 j)* |
|---|---|

---

# Chantier C — ShotGrid workfiles

**Objectif** : ouvrir et sauvegarder les projets ArmorPaint via ShotGrid, avec contexte et versioning ; publier les textures.

**État actuel** : rien. Et deux **verrous techniques** à lever d'abord (§5.6) :
- `iron_exec_async` existe ([iron_system.h:105](base/sources/iron_system.h:105)) mais **n'est pas exposé** au scripting
- Le HTTP est limité à un GET-vers-fichier — **pas de POST, pas de headers** → l'API REST ShotGrid est inatteignable

### C1 — Pont d'exécution · **3 j** — ✅ **fait le 2026-09-26** (macOS ; Linux écrit, non testé)

Lever le premier verrou.

- ✅ **Pas `iron_exec_async`**, contrairement au plan : un seul processus à la fois avec un état global (`iron_exec_async_done`) déjà partagé par les nœuds neuraux, ni stdin, ni capture, ni code de sortie, ni timeout, et sous Windows les `"` des arguments deviennent des `'` (du JSON en argument est cassé). Le plan le supposait réutilisable ; il ne l'est pas pour un sidecar.
- ✅ `external/studio_proc.c` : `posix_spawnp` **sans shell** (argv tel quel, pas d'injection), groupe de processus propre (un timeout tue aussi les petits-enfants), stdin/stdout/stderr en pipes lus en parallèle, sortie plafonnée, timeout, annulation, SIGTERM puis SIGKILL, protocole ligne à ligne pour le sidecar C2 (0,02 ms l'aller-retour).
- ✅ `external/studio_exec.c` : jobs asynchrones sur thread, et **API minic** (`studio_exec_run/start/state/exit_code/stdout/…`) enregistrée par `minic_register()` depuis `external_init()`. **Zéro ligne upstream**, ni `minic_api_list.h` ni `minic_api.c`. Contrepartie : ces fonctions n'apparaissent pas dans `--api`, elles sont documentées dans `external/README.md`.
- ✅ Les jobs encore ouverts sont tués à la fermeture d'ArmorPaint (`atexit`), descendants compris.
- ⚠️ **Windows non implémenté** : les fonctions échouent avec un message explicite. À écrire (`CreateProcessW`, job object, pipes overlapped) quand une machine Windows sera disponible pour le tester — **bloquant pour C2 si des postes Windows sont concernés**.

**Tests** : `external/.tests/proc.sh`, 44 contrôles (35 en C1, 9 ajoutés en C2 pour le processus qui dure) sous ASan/UBSan fatals (arguments piégés — espaces, guillemets, `$`, backticks, UTF-8, JSON —, 8 Mio aller-retour, timeout avec petit-enfant, SIGKILL si SIGTERM ignoré, plafond de sortie, 500 échanges ligne à ligne, ni fuite de descripteur ni zombie sur 200 lancements). Mutation testing sur les parades : chaque parade retirée fait échouer le banc, sauf l'ignorance de SIGPIPE dont la course n'a pas pu être provoquée (400 essais) — gardée, documentée comme non couverte. Dans l'app : script minic en `--background --script` (sortie, stderr, code, job asynchrone, timeout, programme introuvable, stdin, aucun orphelin à la fermeture).

### C2 — Sidecar Python · **8 j** — ✅ **fait le 2026-09-26** (macOS ; validé sur un site simulé, recette réelle en C6)

Ne **pas** implémenter l'API ShotGrid en C. Un processus Python séparé, piloté en JSON sur stdin/stdout.

- ✅ **Toolkit (tk-core/sgtk)**, l'intégration du studio : `external/assets/studio_sg_bridge.py` (protocole) + `studio_sg_session.py` (opérations). tk-core est celui de ShotGrid Desktop (bundle cache) ou `STUDIO_SGTK_PATH` ; il embarque `shotgun_api3`, rien à installer. Auth = **la session de ShotGrid Desktop** (ou un script via `STUDIO_SG_SCRIPT_*`). Amorçage par `ToolkitManager` (plugin `basic.armorpaint`, moteur `tk-shell` — `tk-armorpaint` depuis C3 —, sans moteur en repli) : la configuration de pipeline du projet, ses templates, son schéma de dossiers, ses hooks.
- ✅ Opérations : `auth`, `context`, `list_projects`, `list_entities`/`list_assets`, `list_tasks`, `list_workfiles`, `next_version` (crée les dossiers de la tâche), `register_publish` (`sgtk.util.register_publish`), `upload_thumbnail` ; plus un hook studio (`STUDIO_SG_HOOK`) pour en ajouter sans toucher au sidecar.
- ✅ Protocole v1 : une ligne JSON par message, `id` en tête, codes d'erreur stables (`auth_required`, `sg_unreachable`, `template_missing`…). stdout réservé au protocole (un `print()` de hook ou un sous-processus part sur stderr), stdin réservé (**jamais d'invite de mot de passe** : une session expirée devient `auth_required`).
- ✅ **Un projet Toolkit par processus** (tk-core ne sait pas en changer à chaud, et l'amorçage bascule sur le core de la configuration) : changer de projet redémarre le sidecar, et le superviseur **rejoue la requête**, sans que l'appelant le voie.
- ✅ Superviseur C (`external/studio_sg.c`, testable hors app) : lancé au premier appel puis gardé, appels sérialisés, délai par appel (300 s par défaut : un premier amorçage peut télécharger la configuration), annulation, relance après crash, stderr relayé ligne par ligne dans le log d'ArmorPaint (progression de l'amorçage), arrêt propre à la sortie. Jobs asynchrones pour l'UI (C3). `studio_json.c` : lecture par JSON Pointer sans arbre. **API minic** `studio_sg_*` sans patch upstream.
- ⚠️ Le plan disait `shotgun_api3` seul : écarté, il aurait fallu réécrire templates, schéma de dossiers et publish, ce que fait déjà la configuration Toolkit du studio.
- ⚠️ La configuration du studio doit gagner deux templates, `armorpaint_asset_work` et `armorpaint_shot_work` (noms réglables), voir `external/README.md`.
- ⚠️ Python : `STUDIO_SG_PYTHON`, défaut `python3` du `PATH`. Lancé depuis le Finder, le `PATH` est minimal : à régler au déploiement (le Python de ShotGrid Desktop est un bon candidat), à valider en C6.

**Tests** (`external/.tests/sg.sh`) : site ShotGrid simulé (mockgun avec le schéma d'un vrai site, vraie configuration Toolkit sur disque, session Desktop enregistrée, **vrai amorçage avec bascule de core**). 19 tests du sidecar (protocole, erreurs, auth, versions, publish, changement de projet, hook, pas d'invite), 221 contrôles du superviseur sous ASan/UBSan contre un faux sidecar qui se comporte mal sur commande (crash, silence, protocole, stdout fermé, réponses périmées, 8 Mo, annulation, table pleine, ni fuite ni zombie) puis de bout en bout, 373 contrôles du lecteur JSON (troncature à chaque octet, profondeur). Mutation testing sur le rejeu : détecté. Dans l'app : script minic en `--background --script` (auth, job, amorçage, changement de projet rejoué, sans Toolkit, sans Python). **Non validé contre un vrai site** : c'est la recette C6.

### C3 — UI Workfiles · **10 j** — ✅ **clos le 2026-09-26** (macOS ; validé sur site simulé ; les deux points restants sont abandonnés)

Changement de plan : **pas de panneau maison**. Les fenêtres sont celles de Toolkit (tk-multi-workfiles2), qu'on
connaît déjà et que la config du studio règle déjà, ouvertes par un moteur Toolkit propre à ArmorPaint.

- ✅ **Moteur `external/tk-armorpaint/`** (modèle tk-photoshopcc) : tourne dans le sidecar, qui l'amorce à la place de `tk-shell`, et affiche les fenêtres Qt des apps (PySide6 dans le Python du sidecar). Hook `scene_operation` de workfiles2 fourni par le moteur.
- ✅ **Requêtes host** (sidecar → ArmorPaint) dans le protocole : `current_path`, `open`, `save`, `save_as`, `new`, exécutées par `external/studio_workfiles.c` sur le thread de l'UI. Réveil de l'app au repos (`wake` → `iron_delay_idle_sleep`) : sans lui, IDLE_SLEEP d'iron bloquait les requêtes suivantes.
- ✅ **Open / Save / Save As / versions** : ceux de workfiles2 (version suivante, dossiers créés par le schéma, changement de contexte de tâche).
- ✅ **Menu ShotGrid** dans la barre de menus (le menu standard des moteurs Toolkit : contexte, Jump to Flow Production Tracking / File System, favoris `menu_favourites`, commandes des apps), ops `list_commands` / `show_app`. Raccourcis Ctrl/Cmd+Alt+O et Ctrl/Cmd+Alt+S. `STUDIO_SG_WORKFILES_ON_START=1` ouvre File Open au lancement.
- ✅ Contexte affiché dans le **titre de la fenêtre** (`robot.v003.arm - Demo / robot / Texture / texturing`) et en tête du menu.
- ~~Contexte dans la barre de statut (le plan), si le titre ne suffit pas.~~ Abandonné (décision du 2026-09-26) : le titre de la fenêtre suffit.
- ~~Verrouillage (version plus récente d'un autre utilisateur) : à vérifier dans ce que fait workfiles2, pas encore regardé.~~ Abandonné (décision du 2026-09-26).
- ✅ Freezes corrigés (roue arc-en-ciel) : le sidecar Python, application pour macOS dès sa première fenêtre, ne lisait plus ses événements entre deux requêtes (spindump : 33 s sans réponse) et recevait parfois le premier plan. Il fait maintenant tourner sa boucle Qt au repos, n'a plus d'icône dans le Dock et rend le premier plan à ArmorPaint.
- ⚠️ Python du sidecar : il lui faut **PySide6** (ou le Python de ShotGrid Desktop, qui l'embarque) : à valider en C6.

**Injection upstream** : 22 lignes dans `ui_menubar.c`, point d'injection générique `menubar-hook` (`external/PATCHES.md`). Rien dans `ui_statusbar.c`.

**Tests** : 3 tests du moteur dans `sg.sh` (menu avec et sans Qt, changement de contexte, moteur absent), 22 tests du sidecar en tout ; contrôles du réveil dans le superviseur (238). Démo `external/.tests/demo_workfiles.sh` : production simulée (Assets, Shots, versions créées par ArmorPaint en `--background`), File Open au démarrage ; open puis save vérifiés sur une instance au repos.

### C4 — Publish · **6 j** — ✅ **fait le 2026-09-27** (macOS ; validé sur site simulé)

Comme en C3, **pas de publish maison** : la fenêtre de tk-multi-publish2 (v2.11.2, vendorée dans `external/.shotgrid/`),
ouverte par le moteur tk-armorpaint, avec ses hooks (`external/tk-armorpaint/hooks/tk-multi-publish2/basic/`). Menu
ShotGrid > Publish..., Ctrl/Cmd+Alt+P ; op `publish` du sidecar pour publier sans fenêtre (scripts). Côté ArmorPaint,
`external/studio_publish.c` répond aux requêtes host des hooks.

- ✅ **Enchaînement** : collecte (préflight B5 fait par ArmorPaint, rien n'est écrit) → validation → export studio (B3/B4, une seule fois, textures cochées seulement) → copie dans le template de publish → `PublishedFile` → vignette → projet ouvert à sa version suivante.
- ✅ **Projet** : copie écrite par ArmorPaint (requête host `publish_copy`), ses chemins relatifs à la copie (patch `project-copy`) ; type `ArmorPaint Project`, celui de File Open.
- ✅ **Textures** : un publish par texture et par variante ; la **séquence UDIM en un seul publish** (`robot_base.<UDIM>.png`, type `UDIM Image`), dépendante du publish du projet. Réglages de l'export : ceux de la fenêtre d'export pour ce projet (son sidecar), sinon l'export courant, sinon le preset du réglage `Export Preset`.
- ✅ **Vignette** : l'image finale du viewport, comme Capture Screenshot (sans compas ni sphères, 1280 px au plus), au lieu de `viewport_save_texture_to_file`.
- ✅ **Garde-fous** : textures sans version décochées ; nom refusé par le template bloquant (tk-multi-publish2 le publierait en place sans rien dire) ; clé UDIM sans `default: "<UDIM>"` signalée ; projet d'une autre entité que le contexte refusé (ouvert par le menu File d'ArmorPaint, il garde le contexte de la dernière fenêtre : sans ce contrôle, ses publishes allaient à la tâche d'un autre asset) ; projet changé depuis la collecte ; version suivante déjà sur le disque.
- ✅ **Bugs upstream trouvés en route, corrigés** (patch `arm-relative-paths`) : après un enregistrement ailleurs, maillage et envmap gardaient en mémoire un chemin relatif à l'ancien dossier (Reimport Mesh ouvrait le sélecteur de fichiers, le Save As suivant écrivait des chemins faux) ; chemins `a/../src/…` non normalisés, perdus avec `a/`.
- Non fait : Version de review (plugin `upload_version.py` non configuré) ; publish en `--background` non testé.
- ⚠️ À valider en C6 sur le vrai site : envoi des vignettes (mockgun les ignore), types de publish, chargeurs de textures UDIM du studio.

**Injection upstream** : 6 lignes dans `export_arm.c` (`project-copy`), plus le correctif `arm-relative-paths` (`export_arm.c`, `iron_path.c`).

**Tests** : `sg.sh`, 9 tests du publish contre un faux ArmorPaint (séquences UDIM, clé sans default, nom refusé avec et sans Publish Template, sources des réglages, textures sans version, validation qui ne publie rien, projet d'un autre contexte, export en échec) ; 34 tests du sidecar en tout, 238 contrôles du superviseur. `publish_app.sh`, dans ArmorPaint : op `publish`, puis fenêtre Publish pilotée comme par l'artiste (Qt offscreen), copie publiée rouverte sans le dossier de travail ; 19 contrôles, dont 2 échouent sans `project-copy`. `arm_paths_app.sh` : 5 contrôles, dont 4 échouent sans `arm-relative-paths`.

### C5 — Contexte & résilience · **5 j**

- Persister le contexte SG dans le `.arm` (champ custom) — un projet rouvert sait d'où il vient
- **Mode dégradé obligatoire** : si ShotGrid est injoignable, l'app reste pleinement utilisable en mode fichier. Aucun blocage.
- File d'attente de publishes en échec, rejouables
- Journal d'audit local

### C6 — Intégration & recette · **5 j**

- Test sur un projet SG de test avec de vrais assets
- Aller-retour complet : open → paint → save → publish → réouverture depuis SG
- Documentation utilisateur (1 page) et runbook d'exploitation

| Chantier C — total | **37 j** |
|---|---|

---

# Chantier D — UDIM natifs (option B)

Décidé le 2026-09-26. Analyse, architecture et détail des phases : **`ARMORPAINT_UDIM.md`** (§6 et §7). Exception assumée au principe directeur : les phases 2 à 4 touchent le cœur (`slot_layer.c`, `util_layer.c`, `render_path_paint.c`, `make_mesh.c`, `export_texture.c`, `.arm`), la logique va dans un nouveau `paint/sources/util/util_udim.c` et les fichiers chauds ne reçoivent que les boucles par tuile (patches `udim-*` dans `PATCHES.md`).

| Lot | Contenu | Estimation |
|---|---|---|
| D0 | ✅ **fait le 2026-09-27** (macOS) — Correctifs sans regret : importeurs OBJ/FBX (UV négatives, u ≥ 10, triangles à cheval coupés, plafond FBX levé), erreur > 64 tuiles, conseil de résolution à l'import. Patches `udim-import`, `fbx-split`, `udim-tile-limit`. Reportés : UV matériau réelles en D1, gouttière entre cases en D2 (`ARMORPAINT_UDIM.md` §7) | 8-12 j |
| D1 | Tuile = donnée du projet, maillage à plages d'index (plus de split), stockage encore en atlas ; UV matériau réelles | 10-15 j |
| D2 | Jeu de textures par tuile, `layers_set_tile`, nouveau format `.arm` + migration — **point de non-retour du fork** | 20-30 j |
| D3 | Résolution par tuile, export natif | 6-10 j |
| D4 | Mémoire : allocation paresseuse, undo par tuile, éviction LZ4 | 12-20 j |

| Chantier D — total | **56-87 j** |
|---|---|

---

## Synthèse

| Chantier | Jours-homme | Dépendances |
|---|---|---|
| Lot 0 — squelette `external/` | 2 | — |
| **A — OCIO** | **33** (18 en voie B) | A0 avant tout |
| **B — Exporter studio** | **34** | B1 avant B2-B7 ; B3 profite de C2 |
| **C — ShotGrid** | **37** | C1+C2 avant C3-C4 ; C4 dépend de B3/B5 |
| **D — UDIM natifs** | **56-87** | D0 indépendant ; D2 avant D3-D4 ; C4 publie `<UDIM>` sans dépendre de D |
| **Total A-C** | **106 j-h** (~5,3 mois à 1 ETP) | |

### Chemin recommandé

**Phase 1 — fondations et gain immédiat · 3 semaines**
✅ `Lot 0` → ✅ `B1` (EXR) → ✅ `C1` (pont exec) → ✅ `B7` partiel (CLI, sans headless)
→ *Acquis : EXR compressé avec alpha et mono-canal (120 Mo → 4,7 Mo), import PIZ/B44/calques/multipart/DWA, CLI `--background --export-textures` réparée. Scripts et modules peuvent appeler les outils studio (macOS ; Linux non testé).*

**Phase 2 — la valeur pipeline · 6 semaines**
✅ `C2` → ✅ `B3` (nomenclature) → ✅ `C3` (workfiles) → ✅ `B4` + ✅ `B5` (+ ✅ `B2`)
→ *Un artiste ouvre depuis ShotGrid, peint, sauve en version, exporte au bon nom au bon endroit.*

**Phase 3 — couleur · 6 semaines**
✅ `A0` → ✅ `A1` → ✅ `A3` (export, prioritaire sur le viewport) → ✅ `A2` → ✅ `A4` → ✅ `A5` → `A6`
→ *Export colorimétriquement juste d'abord ; le viewport ensuite.*

**Phase 4 — industrialisation · 3 semaines**
`B7` (headless réel) → ✅ `C4` (publish) → `C5` → `B6` → `C6`

### Risques principaux

| Risque | Impact | Parade |
|---|---|---|
| Link OCIO C++ sur 3 OS | 🔴 bloque A | POC en A0 avant d'engager ; voie B en repli |
| Headless nécessite un GPU | 🟠 limite B7 | Nœuds de ferme équipés, ou lavapipe |
| ~~Upstream refuse la PR C1~~ | — | Sans objet : C1 fait dans `external/`, sans patch |
| Postes Windows | 🟠 bloque C2 sur Windows | `studio_proc` Windows à écrire et tester sur une machine Windows (le sidecar Python, lui, est portable) |
| Toolkit du studio | 🟠 C2 à C4 validés sur site simulé seulement | Recette C6 sur le vrai site : session Desktop, amorçage de la config de prod, templates ArmorPaint ajoutés, publish (vignettes, types, chargeurs UDIM) |
| Réécriture upstream (§1.1) | 🔴 | Tout dans `external/`, dépôt séparé, `PATCHES.md` |
| **Le mur VRAM reste entier** | 🔴 | Levé en partie par le chantier D (D4 : allocation par tuile, undo par tuile, éviction) — voir `ARMORPAINT_UDIM.md` |

### Ce que ces 110 jours ne règlent **pas**

- Le plafond VRAM (§5.3) : 3 render targets pleine résolution par layer, aucun streaming
- ~~L'UDIM à l'échelle~~ et ~~la résolution par tile UDIM~~ : repris par le chantier D (2026-09-26)
- La limite de 64 objets du path tracer / bake RT

Ces points relèvent d'une refonte du modèle de textures, hors périmètre.
