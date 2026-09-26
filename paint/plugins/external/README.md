# ArmorPaint — code studio

Ce dossier est compilé dans ArmorPaint par le hook upstream `paint/plugins/external/`
(`paint/plugins/project.c`, define `WITH_EXTERNAL`). `external_init()` est appelé au démarrage par
`plugins_init()` (`paint/plugins/plugins.c`). L'upstream ignore ce dossier ; la branche `studio` le suit (patch
`track-external`). Branches `upstream` (armory3d tel quel) et `studio` (nos patches et ce dossier) : voir
`PATCHES.md`.

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
- `.project/project.c` : réglages de build du code studio (bibliothèques, defines), évalués par amake grâce au patch
  `external-project`. Le code studio lui-même ne peut pas en ajouter.
- `.ocio/` : sources du shim C d'OpenColorIO (`studio_ocio_lib.cpp`, projet CMake), construit par
  `tools/build_ocio.sh` ; voir « Gestion couleur OCIO ».
- `.shotgrid/` : sources de Toolkit (tk-core, Workfiles et ses frameworks) pour la démo ShotGrid et les tests ;
  le venv PySide6 y est créé par `tools/setup_shotgrid.sh`, hors de git.
- `tools/armorpaint.sh` : lance la version studio (build compris) ; `--shotgrid-demo` pour le site simulé.

Modules, un par lot (voir `ARMORPAINT_LOTS.md` à la racine d'armorpaint) :

| Module | Lot | État |
|---|---|---|
| `studio_exr` | B1 | ✅ lecture et écriture EXR par tinyexr |
| `studio_proc`, `studio_exec` | C1 | ✅ exécution de processus, jobs asynchrones, API de scripting (macOS ; Linux écrit, non testé) |
| `studio_naming`, `studio_export_naming` | B3 | ✅ noms des textures exportées par le template Toolkit du fichier de travail (macOS ; validé sur site simulé) |
| `studio_export`, `studio_preset`, `studio_expr`, `studio_image`, `studio_preflight`, `studio_sha256`, `studio_fs`, `assets/studio_preset_*.json` | B2, B4, B5 | ✅ presets d'export studio : PNG 8/16, TIFF 8/16/32 (none/LZW/deflate), JPG, EXR 16/32 ; canaux calculés ; préflight ; écriture atomique ; manifeste (macOS) |
| `studio_ocio`, `studio_ocio_lib.h`, `.ocio/`, `tools/build_ocio.sh` | A0, A1 | ✅ OpenColorIO 2.5.2 embarqué : configuration, espaces, displays/views, transformations CPU, API de scripting (macOS ; Linux écrit, non testé) |
| `studio_ocio`, `studio_export` (color_space OCIO) | A3 | ✅ export : chaque texture convertie vers un espace OCIO de la configuration, préflight, manifeste (macOS) |
| `studio_ocio_view`, `studio_ocio_lut` | A2 | ✅ viewport : transformation display/view/look d'OCIO en LUT 3D dans le compositeur, menu Color (macOS) |
| `studio_ocio` (import, préférences, shader exact) | A4-A6 | à faire |
| `studio_sg`, `studio_shotgrid`, `studio_json`, `assets/studio_sg_*.py` | C2 | ✅ sidecar ShotGrid (Toolkit), superviseur, API de scripting (macOS ; validé sur site simulé) |
| `studio_workfiles`, `tk-armorpaint/` | C3 | ✅ Workfiles (tk-multi-workfiles2) et menu ShotGrid, par le moteur Toolkit tk-armorpaint (macOS ; validé sur site simulé) |
| `studio_shotgrid` (UI) | C4-C5 | à faire |
| `studio_ui` | B6/C3 | à faire |

Build : `../base/make --compile` depuis `paint/`. Le log de démarrage affiche `studio: external <version>
loaded`, puis une ligne par module.

Lancer la version studio, toujours à jour (construit libstudio_ocio si elle manque, recompile, lance le build
Release détaché du terminal ; journal dans `$TMPDIR/armorpaint-studio.log`) :

```sh
tools/armorpaint.sh [projet.arm]
tools/armorpaint.sh --shotgrid-demo [dossier]   # sur le site ShotGrid simulé, voir « Workfiles et menu ShotGrid (C3) »
```

## Variables d'environnement

Réglages de secours, lus au démarrage. Ils existent pour diagnostiquer et pour revenir au comportement
upstream sans rebuild ; les réglages destinés aux artistes passeront par l'UI et les presets (lots B4/B6).

| Variable | Effet |
|---|---|
| `STUDIO_EXR_COMPRESSION` | `none`, `rle`, `zips`, `zip` (défaut), `piz`, `pxr24`, `b44`, `b44a`, `htj2k256`, `htj2k32` |
| `STUDIO_EXR_WRITER=legacy` | rend l'écriture EXR au writer upstream |
| `STUDIO_EXR_READER=legacy` | rend la lecture EXR à l'importeur upstream |
| `STUDIO_EXR_THREADS` | nombre de threads de codec (défaut : cœurs matériels) |
| `STUDIO_SG_*`, `STUDIO_SGTK_PATH` | sidecar ShotGrid, voir « ShotGrid (C2) » |
| `STUDIO_EXPORT_PRESETS` | presets d'export studio : dossiers ou fichiers `.json`, séparés par `:` (`;` sous Windows), prioritaires sur les presets livrés |
| `STUDIO_EXPORT_THREADS` | threads de composition et d'encodage des exports studio (défaut : cœurs matériels) |
| `OCIO` | configuration OpenColorIO (fichier `.ocio`, archive `.ocioz` ou URI `ocio://`) ; défaut : `ocio://studio-config-latest` |
| `STUDIO_OCIO_WORKING_SPACE` | espace de travail d'ArmorPaint dans la configuration OCIO (nom, alias ou rôle), quand elle n'en a pas de reconnaissable ; voir « Gestion couleur OCIO » |
| `STUDIO_OCIO_LIB` | chemin de `libstudio_ocio` pour un build sans liaison statique (bancs de test, Linux) |
| `STUDIO_OCIO_VIEWPORT=0` | viewport au tonemap filmique d'ArmorPaint au démarrage (réactivable par le menu Color) |
| `STUDIO_OCIO_DISPLAY`, `STUDIO_OCIO_VIEW`, `STUDIO_OCIO_LOOK` | transformation d'affichage du viewport au démarrage ; défaut : display et view par défaut de la configuration, sans look. Une valeur inconnue est journalisée et remplacée par le défaut |

## Exécution (C1)

`studio_proc` lance un programme **sans shell** : `argv` est transmis tel quel (pas d'injection par un nom de
fichier ou un champ ShotGrid), le programme est cherché dans le `PATH` s'il ne contient pas de `/`. Le processus
a son propre groupe : un timeout, une annulation ou la fermeture d'ArmorPaint tuent aussi ses descendants. stdin,
stdout et stderr sont des pipes lus en parallèle (pas d'interblocage), sortie plafonnée (64 Mio par flux par
défaut). Pour un dialogue ligne à ligne (sidecar du lot C2) : `studio_proc_spawn` / `studio_proc_write` /
`studio_proc_read_line`.

`studio_exec` en fait des jobs identifiés par un entier, exécutés sur un thread, et les expose aux scripts minic
(enregistrés au démarrage par `minic_register`, sans patch upstream ; absents de `--api`) :

| Fonction | Rôle |
|---|---|
| `int studio_exec_run(any_array_t *argv, char *stdin, int timeout_ms)` | lance et attend ; retourne le job |
| `int studio_exec_start(any_array_t *argv, char *stdin, int timeout_ms)` | lance sans attendre (0 : plus de 256 jobs ouverts) |
| `int studio_exec_state(int job)` | `0` en cours, `1` terminé, `-1` inconnu ou libéré |
| `int studio_exec_exit_code(int job)` | code de sortie ; `-1` si tué, introuvable ou en cours |
| `int studio_exec_timed_out(int job)` | `1` si tué par le timeout |
| `char *studio_exec_stdout(int job)`, `studio_exec_stderr`, `studio_exec_error` | sorties, et l'erreur de lancement ou de timeout ; valides jusqu'à `release` |
| `void studio_exec_cancel(int job)` | tue le processus (SIGTERM, puis SIGKILL après 500 ms) |
| `void studio_exec_release(int job)` | libère le job, l'annule s'il tourne encore |

`stdin` : `NULL` pour rien. `timeout_ms < 0` : sans limite. Exemple :

```c
int main() {
	int j = studio_exec_run(string_split("oiiotool\n--info\n/chemin/avec espaces.exr", "\n"), NULL, 10000);
	console_log(studio_exec_stdout(j));
	studio_exec_release(j);
	return 0;
}
```

⚠️ **Windows : non implémenté.** Les fonctions existent mais chaque job échoue aussitôt avec
`studio_proc: not implemented on Windows yet` (pas de toolchain Windows pour le développer et le tester).

## ShotGrid (C2)

ArmorPaint ne parle pas à ShotGrid lui-même : un **sidecar Python** (`assets/studio_sg_bridge.py` et
`studio_sg_session.py`, copiés dans `data/`) le fait avec **Toolkit**, l'intégration du studio. Il réutilise
tk-core (qui embarque `shotgun_api3`), la session de ShotGrid Desktop et la configuration de pipeline du projet :
templates, schéma de dossiers, hooks. Rien n'est à installer sur le poste au-delà de ShotGrid Desktop.

`studio_sg.c` supervise le sidecar : lancé au premier appel, gardé ensuite (l'amorçage Toolkit coûte des
secondes), relancé s'il meurt, tué s'il ne répond pas dans le délai ou si l'appel est annulé. Son stderr
(progression de l'amorçage, erreurs) arrive dans le log d'ArmorPaint, préfixé `studio: shotgrid:`.

**Un projet Toolkit par processus** : tk-core ne sait pas changer de projet à chaud. Une requête sur un autre
projet redémarre le sidecar, et le superviseur la rejoue ; l'appelant ne voit qu'un appel un peu plus long.

### Réglages

| Variable | Effet |
|---|---|
| `STUDIO_SG_PYTHON` | interpréteur (défaut `python3` du `PATH` ; lancé depuis le Finder, le `PATH` est minimal : le régler au déploiement) |
| `STUDIO_SG_BRIDGE` | script du sidecar (défaut `data/studio_sg_bridge.py`) |
| `STUDIO_SGTK_PATH` | dossier contenant le paquet `sgtk` ; sinon `SHOTGUN_BUNDLE_CACHE_PATH`, puis le cache de ShotGrid Desktop (`$SHOTGUN_HOME` ou `~/Library/Caches/Shotgun`, `bundle_cache/app_store/tk-core`, version la plus récente) |
| `STUDIO_SG_SCRIPT_NAME`, `STUDIO_SG_SCRIPT_KEY`, `STUDIO_SG_HOST` | connexion par script (ferme de rendu) au lieu de la session Desktop ; `SHOTGUN_SITE` à défaut de `STUDIO_SG_HOST` |
| `STUDIO_SG_ENGINE` | moteur démarré après l'amorçage (défaut `tk-armorpaint`, voir C3 ; `none` pour aucun ; sans moteur en repli s'il échoue) |
| `STUDIO_SG_PLUGIN_ID` | plugin id de l'amorçage (défaut `basic.armorpaint`) |
| `STUDIO_SG_BASE_CONFIGURATION` | descripteur de configuration si le projet n'a pas de PipelineConfiguration |
| `STUDIO_SG_WORK_TEMPLATES` | templates de travail par type d'entité (défaut `Asset=armorpaint_asset_work,Shot=armorpaint_shot_work`) |
| `STUDIO_SG_TEXTURE_TEMPLATES` | templates des textures exportées par type d'entité (défaut `Asset=armorpaint_asset_textures,Shot=armorpaint_shot_textures`), voir B3 |
| `STUDIO_SG_TEXTURE_KEYS` | noms des clés de ces templates (défaut `channel=channel,udim=UDIM,ext=texture_ext`) |
| `STUDIO_SG_NAMING` | `1` : nommage des textures par template, `0` : jamais ; par défaut dès que ShotGrid sert dans la session |
| `STUDIO_SG_PUBLISH_TYPE` | type de publish (défaut `ArmorPaint Project`) |
| `STUDIO_SG_DEFAULT_NAME` | nom de fichier de travail par défaut (défaut `scene`) |
| `STUDIO_SG_HOOK` | fichier Python du studio avec `setup(session)`, qui peut ajouter des opérations (`session.ops["nom"] = fonction(args)`) |
| `STUDIO_SG_DEBUG` | log détaillé, tk-core compris |

La configuration Toolkit du studio doit définir les templates de travail, avec les clés `name` et `version` :

```yaml
# core/templates.yml
paths:
    armorpaint_asset_work: '@asset_root/work/armorpaint/{name}.v{version}.arm'
    armorpaint_shot_work: '@shot_root/work/armorpaint/{name}.v{version}.arm'
```

Le nom de publish suit la convention Toolkit : le nom de fichier sans sa version (`robot.v003.arm` → `robot.arm`).

### Opérations

| Op | args | data |
|---|---|---|
| `auth` | — | `{method, login, user}` ; `method` : `session` (Desktop) ou `script` |
| `context` | — | tâche ou entité de lancement (`SHOTGUN_ENTITY_TYPE`/`ID` posés par Desktop), ou `null` |
| `list_projects` | — | projets actifs (ni archivés, ni modèles) |
| `list_entities` | `project_id`, `entity_type` (`Asset` défaut, `Shot`) | entités du projet |
| `list_assets` | `project_id` | idem, `Asset` |
| `list_tasks` | `project_id` ?, `entity` ? (`{type, id}`), `mine` ? (vrai par défaut sans `entity`) | tâches ; `entity` porte `name` (et `sg_asset_type`), plus `step.Step.code`, `project.Project.name` |
| `list_workfiles` | `task_id`, `name` ? | `{template, files, publishes}` : fichiers du template de travail sur disque, publishes ArmorPaint de la tâche |
| `next_version` | `task_id`, `name` ? | `{name, version, path, publish_name}` : première version libre (fichiers et publishes) ; crée les dossiers de la tâche |
| `register_publish` | `task_id`, `path`, `name` ?, `version` ? (défaut : lue dans le nom), `type` ?, `description` ?, `thumbnail_path` ?, `dependency_paths` ? | le PublishedFile créé |
| `upload_thumbnail` | `entity` (`{type, id}`), `path` | `{attachment_id}` |
| `list_commands` | `task_id` ? ou `project_id` ? | `{context, commands, favourites, has_ui}` : le menu du moteur tk-armorpaint, voir C3 |
| `show_app` | `command`, `task_id` ? ou `project_id` ? | `{command, context}` à la fin de la commande (fermeture de la fenêtre) |
| `texture_pattern` | `path`, `project_id` ? | `{template, pattern, pattern_udim}` : chemin des textures exportées depuis le fichier de travail `path`, marqueurs `<channel>`, `<UDIM>`, `<ext>` ; `template` null (et `reason`) hors pipeline. Voir B3 |
| `ping`, `shutdown` | — | `{}` |

Codes d'erreur (`/error/code`) : du sidecar, `bad_request`, `unknown_op`, `auth_required` (pas de session Desktop,
ou expirée : jamais d'invite de mot de passe), `sg_unreachable`, `sg_error`, `toolkit_unavailable` (tk-core
introuvable), `toolkit_error`, `not_found`, `template_missing` (la config n'a pas le template de travail ou de textures),
`internal`, `app_unavailable` (moteur tk-armorpaint absent de la config, ou commande inconnue : Qt manquant…) ; du superviseur, `sidecar_failed`, `sidecar_died`, `timeout`, `cancelled`. `restart_required` n'arrive
à l'appelant que si le rejeu échoue lui aussi.

### Scripts (minic)

Comme `studio_exec`, des jobs identifiés par un entier, enregistrés par `minic_register` (absents de `--api`) :

| Fonction | Rôle |
|---|---|
| `int studio_sg_run(char *op, char *args, int timeout_ms)` | appelle et attend ; retourne le job |
| `int studio_sg_start(char *op, char *args, int timeout_ms)` | sans attendre, sur le thread ShotGrid (0 : plus de 256 jobs ouverts) |
| `int studio_sg_state(int job)` | `1` en file, `2` en cours, `3` fini, `0` inconnu ou libéré |
| `int studio_sg_ok(int job)` | `1` si la réponse est un succès |
| `char *studio_sg_result(int job)` | réponse JSON complète (`""` tant que le job n'est pas fini) |
| `char *studio_sg_get(int job, char *pointer)` | valeur au [JSON Pointer](https://www.rfc-editor.org/rfc/rfc6901) `pointer` (`"/data/0/code"`, `"/error/message"`) : chaîne décodée, ou le JSON de la valeur ; `""` si absente |
| `int studio_sg_length(int job, char *pointer)` | éléments d'un tableau ou d'un objet, `-1` sinon |
| `int studio_sg_int(int job, char *pointer, int fallback)` | entier, `fallback` si absent |
| `void studio_sg_cancel(int job)` | en file : ne part pas ; en cours : sidecar tué, réponse `cancelled` |
| `void studio_sg_release(int job)` | libère le job et ses textes, l'annule s'il tourne encore |
| `void studio_sg_restart()` | arrête le sidecar, relancé au prochain appel (après une connexion dans Desktop…) |
| `char *studio_json_quote(char *s)` | littéral JSON d'une chaîne, pour composer `args` |

`args` : un objet JSON, `NULL` pour `{}`. `timeout_ms` : `0` pour le défaut (300 s : un premier amorçage peut
télécharger la configuration du projet), `< 0` sans limite. Les chaînes rendues vivent jusqu'au `release`.
`studio_sg_run` bloque le thread appelant, y compris derrière un job en cours : l'UI utilise `studio_sg_start`.

```c
int main() {
	int j = studio_sg_run("next_version", "{\"task_id\":1234,\"name\":\"robot\"}", 0);
	if (studio_sg_ok(j)) {
		console_log(studio_sg_get(j, "/data/path"));
	}
	else {
		console_log(string("%s: %s", studio_sg_get(j, "/error/code"), studio_sg_get(j, "/error/message")));
	}
	studio_sg_release(j);
	return 0;
}
```

Le C appelle `studio_sg_call()` (`studio_sg.h`), ou les mêmes jobs.

⚠️ **Windows** : le sidecar est portable, mais `studio_proc` n'y est pas implémenté (voir C1) : tout appel échoue en
`sidecar_failed`.

## Workfiles et menu ShotGrid (C3)

Les fenêtres sont **celles de Toolkit**, pas une réimplémentation. `tk-armorpaint/` est un moteur Toolkit, sur le
modèle des moteurs Adobe (tk-photoshopcc) : il tourne dans le sidecar, qui l'amorce, et les apps du studio
(tk-multi-workfiles2…) y ouvrent leurs fenêtres Qt. Il faut donc PySide6 dans l'interpréteur du sidecar
(`STUDIO_SG_PYTHON`) ; sans lui (ou avec `STUDIO_SG_NO_UI=1`), les apps à fenêtre ne se chargent pas et
`show_app` répond `app_unavailable`.

Une fois une fenêtre ouverte, le sidecar est une application pour le système. Entre deux requêtes, il fait donc
tourner sa boucle d'événements Qt (`studio_sg_bridge.IDLE`, toutes les 50 ms) : sinon macOS le déclare bloqué, et
s'il reçoit le premier plan, il retient clavier et souris (roue arc-en-ciel sur ArmorPaint). Sur macOS, c'est une
application d'arrière-plan, absente du Dock et de Cmd+Tab, qui rend le premier plan à ArmorPaint à la fermeture de
chaque fenêtre.

Ce que les apps font sur la scène revient à ArmorPaint par des **requêtes host**, dans l'autre sens du protocole :
le sidecar écrit `{"host":N,"op","args"}`, ArmorPaint exécute l'op à sa frame suivante et répond
`{"host":N,"ok":true,"data":…}` ou `{"host":N,"ok":false,"error":{code,message}}`. Ops de `studio_workfiles.c` :
`current_path`, `open` (`path`), `save`, `save_as` (`path`), `new`. Le hook `scene_operation` de workfiles2
fourni par le moteur (`tk-armorpaint/hooks/tk-multi-workfiles2/scene_operation.py`) les appelle. Côté C, le
superviseur les met en file ; le thread de l'UI les relève par `studio_sg_host_take()` et y répond par
`studio_sg_host_reply()`. Un appel qui déclenche des requêtes host doit donc tourner en job (`studio_sg_start`),
jamais en `studio_sg_run` sur le thread de l'UI. Le réglage `wake` du superviseur (`iron_delay_idle_sleep`)
réveille l'app quand une requête arrive : au repos, iron ne rend plus de frame et les callbacks d'update ne
tournent plus.

Dans ArmorPaint :

- **menu ShotGrid**, après Help (point d'injection `menubar-hook`, voir `PATCHES.md`). On y trouve le contexte
  courant, la section contexte du moteur (Jump to Flow Production Tracking, Jump to File System), les favoris,
  puis les commandes des apps. Le menu est chargé à la première ouverture (`list_commands`) et rechargé après
  chaque fenêtre, qui a pu changer de contexte ;
- **Ctrl+Alt+O / Ctrl+Alt+S** (Cmd+Alt sur macOS) : File Open / File Save de workfiles2 ;
- `STUDIO_SG_WORKFILES_ON_START=1` ouvre File Open au démarrage ;
- le titre de la fenêtre porte le fichier et le contexte (`robot.v003.arm - Demo / robot / Texture / texturing`).

Une fenêtre à la fois : tant qu'une fenêtre Toolkit est ouverte, les entrées du menu sont grisées. Le contexte
d'une fenêtre est la dernière tâche connue (celle de la fenêtre précédente), sinon le projet de lancement.

Configuration Toolkit du studio, dans les environnements où ArmorPaint doit avoir ses apps :

```yaml
engines:
  tk-armorpaint:
    location: {type: path, path: <external>/tk-armorpaint}   # ou dev/git selon le déploiement
    menu_favourites:
      - {app_instance: tk-multi-workfiles2, name: File Open...}
      - {app_instance: tk-multi-workfiles2, name: File Save...}
    apps:
      tk-multi-workfiles2:
        location: {type: app_store, name: tk-multi-workfiles2, version: v0.15.x}
        hook_scene_operation: '{engine}/tk-multi-workfiles2/scene_operation.py'
        template_work: armorpaint_asset_work      # et les autres réglages habituels de workfiles2
```

Démo sur le site simulé, avec des versions créées au premier lancement :

```sh
STUDIO_TEST_TK_CORE=<tk-core> STUDIO_TEST_TK_APPS=<clones> STUDIO_SG_PYTHON=<python avec PySide6> \
    .tests/demo_workfiles.sh [dossier]
```

(`STUDIO_TEST_TK_APPS` : tk-multi-workfiles2, tk-framework-shotgunutils et tk-framework-qtwidgets clonés depuis
github.com/shotgunsoftware ; voir l'en-tête du script.) Ces sources sont versionnées dans `.shotgrid/` (tags dans
`.shotgrid/README.md`) : `tools/armorpaint.sh --shotgrid-demo` lance la démo avec elles, et crée au besoin le python
PySide6 (`.shotgrid/venv`, propre à la machine, par `tools/setup_shotgrid.sh`).

## Nommage des exports (B3)

Quand le projet ouvert est un fichier de travail ShotGrid, les textures exportées prennent le chemin du template
Toolkit des textures, rempli avec les champs du fichier : entité, étape, nom, version. Pour `robot.v003.arm`, la
texture `base` du preset va dans `…/work/armorpaint/textures/v003/robot_base.png`. Chaque version du projet a donc
ses textures, et le dossier choisi dans la boîte d'export est ignoré. La console l'indique à la première texture :
« Textures named by ShotGrid template … ».

```yaml
# core/templates.yml
keys:
    channel: {type: str}                    # nom de texture du preset d'export (base, nor, occ…)
    UDIM: {type: sequence, format_spec: "04"}
    texture_ext: {type: str, choices: [png, jpg, exr]}
paths:
    armorpaint_asset_textures: '@asset_root/work/armorpaint/textures/v{version}/{name}_{channel}[.{UDIM}].{texture_ext}'
    armorpaint_shot_textures: '@shot_root/work/armorpaint/textures/v{version}/{name}_{channel}.{texture_ext}'
```

- **Clés obligatoires.** `{channel}` et `{texture_ext}` le sont. Sans `{UDIM}`, ou avec un `{UDIM}` obligatoire,
  l'export qui ne correspond pas garde les noms d'ArmorPaint, et la console le signale.
- **Export par objet ou par atlas.** L'objet se place devant le canal : `robot_head_base.png`.
- **Valeurs non validées.** Canal, tuile et extension viennent d'ArmorPaint : leur type dans `templates.yml`
  (`filter_by`, `choices`) n'est pas vérifié, mais `/`, `\` et `:` y deviennent `_`.

**Déroulé.** Quand le projet ouvert change (sauvegarde, ouverture, fenêtre Workfiles), `studio_export_naming.c`
demande le modèle au sidecar (op `texture_pattern`, en job). Le projet Toolkit se déduit du chemin :
racine de stockage puis `tank_name`. L'export ne fait ensuite que remplacer les marqueurs et créer les dossiers.

- Si le modèle n'est pas encore arrivé, l'export l'attend.
- Si une fenêtre ShotGrid est ouverte, il ne peut pas l'attendre : il garde les noms d'ArmorPaint et le dit.
- Hors pipeline (projet jamais sauvé, fichier hors des templates, pas de template pour le type d'entité) ou en cas
  d'erreur, les noms restent ceux d'ArmorPaint.

**Activation.** Par défaut, le nommage n'est actif que si ShotGrid sert déjà dans la session (sidecar lancé, ou
ArmorPaint lancé par Desktop). En ligne de commande (ferme), il faut le forcer :

```sh
STUDIO_SG_NAMING=1 ArmorPaint --background robot.v003.arm --export-textures png generic /ignored
```

Point d'injection upstream : `STUDIO-PATCH(export-naming)` (PATCHES.md). L'export « bake material » n'est pas
couvert.

## Export studio (B2, B4, B5)

Un **preset studio** est un preset d'export plus riche que ceux d'ArmorPaint. Pour chaque texture, il fixe le format,
la profondeur, la compression, l'espace colorimétrique, le nom de fichier et des canaux calculés. L'export vérifie
tout avant d'écrire, écrit de façon atomique et laisse un manifeste.

**Presets livrés** (`assets/studio_preset_<nom>.json`, copiés dans `data/`) : `arnold`, `karma`, `renderman`,
`vray` et `redshift`. Suffixes des paramètres du shader de chaque moteur. Cartes de couleur en EXR half linéaire,
données brutes, hauteur en float 32 bits. `STUDIO_EXPORT_PRESETS` en ajoute ou en remplace : le nom du preset est
celui du fichier, et le premier trouvé l'emporte.

**Installation.** Au démarrage, chaque preset studio valide reçoit un preset upstream du même nom dans
`data/export_presets/`, avec les 4 canaux et le `color_space` les plus proches. Il apparaît ainsi dans la boîte
d'export et en ligne de commande. Le registre `data/studio_presets_generated.txt` note ces fichiers et leur
empreinte.
- Un preset upstream du même nom n'est jamais écrasé : le preset studio n'est pas installé et la console le dit.
- Un fichier que le studio a écrit est retiré quand son preset disparaît, sauf s'il a été modifié depuis.
- Un preset invalide est signalé au démarrage, avec la clé en faute (`textures[2].bits: …`).

Le preset est relu à chaque export : une modification prend effet sans relancer ArmorPaint.

### Format

```json
{
	"version": 1,
	"description": "Lookdev maison",
	"naming": "<file>[_<variant>]_<channel>[.<UDIM>].<ext>",
	"resolutions": [2048, "4096x4096"],
	"overwrite": "warn",
	"manifest": true,
	"defaults": { "format": "exr", "bits": 16, "color_space": "raw",
	              "compression": { "exr": "zip", "tiff": "lzw", "png": "default" } },
	"textures": [
		{ "name": "base", "suffix": "BaseColor", "channels": ["base_r", "base_g", "base_b"], "color_space": "linear" },
		{ "name": "orm", "channels": ["occ", "rough", "metal"], "format": "png", "bits": 16 },
		{ "name": "mask", "channels": ["clamp(1 - metal * 2, 0, 1)"], "format": "tiff", "bits": 32, "compression": "deflate" },
		{ "name": "preview", "channels": ["base_r", "base_g", "base_b"], "format": "jpg", "quality": 85,
		  "naming": "<file>/preview/<file>.<ext>" }
	]
}
```

La lecture est stricte : une clé inconnue, un type faux ou une combinaison impossible est une erreur.

| Clé | Valeurs |
|---|---|
| `format` | `png` (8, 16 bits), `jpg` (8), `tiff` (8, 16, 32 float), `exr` (16 half, 32 float) |
| `compression` | png `fast`, `default`, `best` ; tiff `none`, `lzw`, `deflate` (défaut) ; exr `none`, `rle`, `zips`, `zip`, `piz`, `pxr24`, `b44`, `b44a`, `htj2k256`, `htj2k32` (défaut : `STUDIO_EXR_COMPRESSION`) ; jpg : `quality` 1-100 (défaut 90) |
| `color_space` | `raw` : valeurs d'ArmorPaint telles quelles (le `linear` des presets upstream) ; `linear` : couleurs décodées (gamma 2.2 → linéaire) ; `srgb` : décodées, calculées, puis réencodées. Les données, les constantes et l'alpha ne sont jamais transformés. Tout autre nom est un espace OCIO (nom, alias ou rôle : `ACEScg`, `sRGB - Texture`, `Raw`…) : voir « Export (A3) ». Les trois mots-clés s'écrivent en minuscules ; `Raw` est l'espace OCIO |
| `channels` | 1, 3 ou 4 expressions (pas de 4e canal en JPG) |
| `naming` | marqueurs `<file>`, `<variant>` (objet ou atlas), `<channel>` (`suffix`, sinon `name`), `<UDIM>`, `<ext>`. Une section `[…]` disparaît si un marqueur qu'elle contient est vide. Un chemin relatif part du dossier choisi ; des sous-dossiers sont permis |
| `resolutions` | résolutions admises par le show : une autre est bloquante |
| `overwrite` | `warn` (défaut : remplace et le signale), `allow`, `block` |

**Expressions.** Variables : `base_r`, `base_g`, `base_b`, `opac`, `nor_r`, `nor_g`, `nor_g_directx`, `nor_b`,
`occ`, `rough`, `smooth`, `metal`, `height`, `emis`, `subs`, `diff_r/g/b`, `spec_r/g/b`. Ce sont les canaux des
presets upstream, avec les mêmes définitions.
- **Opérateurs** : `+ - * /` et les parenthèses.
- **Fonctions** : `abs`, `sqrt`, `min`, `max`, `pow`, `step(edge, x)`, `clamp(x, lo, hi)`, `mix(a, b, t)`.
- **Garde-fous** : une division par zéro, la racine d'un négatif ou un NaN donnent 0.
- **Exemples** : un masque d'émission `base_r * emis`, un ORM `occ`, `rough`, `metal`, une carte de brillance
  `1 - rough`.

### Déroulé d'un export

1. **Plan.** Passe à blanc de l'export upstream. Ses boucles (objets, atlas, tuiles UDIM, masques d'objet) donnent la
   liste exacte des fichiers, avec le nom ShotGrid (B3) s'il s'applique.
2. **Préflight.** Le rapport liste chaque fichier et sa taille estimée (non compressée), et sépare les erreurs
   bloquantes des avertissements. En cas de blocage, rien n'est écrit et le rapport s'affiche.

   | Bloquant | Avertissement |
   |---|---|
   | nommage impossible (template ShotGrid), aucun calque visible, aucun fichier, résolution hors show, chemins en double, fichier existant (`block`), dossier non inscriptible, espace disque insuffisant, PNG > 2 Gio, TIFF non compressé > 4 Gio, espace OCIO inconnu ou OCIO indisponible (A3) | calques masqués ignorés, profondeur supérieure à celle des calques, hauteur en 8 bits, données en JPG, EXR `srgb` ou non linéaire, couleur linéaire en 8 bits, données converties vers un espace couleur et inversement (A3), maillage UDIM exporté hors mode tuile, tuiles UDIM manquantes, fichier existant (`warn`), marge disque faible |

3. **Écriture.** Pour chaque variante, les calques aplatis sont relus une fois. Chaque texture est composée en
   parallèle (`STUDIO_EXPORT_THREADS`), encodée, puis écrite en `<fichier>.studio-tmp`. Une texture uniforme est
   signalée (« nothing painted? »), c'est le cas typique du calque vide.
4. **Mise en place.** Quand tout est écrit, les fichiers sont renommés, puis le manifeste est écrit de la même
   façon. Une erreur avant ce point supprime les temporaires : aucun fichier existant n'est remplacé.

**Manifeste** `<file>.export.json`, dans le dossier commun des fichiers. Il contient :
- le schéma ;
- date UTC, utilisateur, machine ;
- les versions d'ArmorPaint (avec le commit) et du studio ;
- le projet ;
- le preset (nom, chemin, SHA-256) ;
- la résolution, la profondeur des calques, le mode et le template ShotGrid ;
- pour chaque fichier : chemin relatif, texture, variante, tuile, format, profondeur, compression, espace, canaux,
  taille, SHA-256 ;
- la provenance OCIO si une texture en dépend (A3) ;
- les avertissements.

PNG et TIFF sont écrits par `studio_image.c` : filtres adaptatifs, deflate et LZW parallèles, prédicteur TIFF
(flottant pour 32 bits). L'EXR passe par tinyexr (B1), et le JPG par stb. Le mode « pack into project » garde
l'écriture upstream.

### Scripts (minic)

```c
int   studio_export_run(char *dir, char *preset, int mode);        // 1 si écrit ; mode : 0 visible, 1 sélection, 2 par objet, 3 par tuile, -1 inchangé
char *studio_export_preflight(char *dir, char *preset, int mode);  // rapport JSON, sans rien écrire
char *studio_export_last_report(void);                              // rapport JSON du dernier export
int   studio_export_is_studio_preset(char *name);
```

Le rapport JSON contient `ok`, `preset`, `destination`, la résolution, `total_size`, `manifest`, `files[]` (chemin,
texture, format, profondeur, compression, taille estimée, `exists`) et `issues[]` (`level`, `code`, `message`). En
ligne de commande, `ArmorPaint projet.arm --background --export-textures exr16 arnold <dossier>` utilise le preset
studio. Chaque texture garde son propre format. Le type fixe seulement la profondeur de travail des calques
aplatis (`png`/`jpg` : 8 bits, `exr16` : half, `exr32` : float), comme dans l'upstream. Pour une sortie 16 ou
32 bits utile, prendre `exr16` ou `exr32` ; le préflight avertit sinon.

Point d'injection upstream : `STUDIO-PATCH(studio-export)` (PATCHES.md).

## Gestion couleur OCIO (A0, A1, A2, A3)

**Décision A0 : le vrai OpenColorIO**, pas une réimplémentation. Version épinglée dans `.ocio/CMakeLists.txt`
(`STUDIO_OCIO_TAG`, aujourd'hui `v2.5.2`, VFX Reference Platform CY2026), construite depuis ses sources avec ses
dépendances (expat, yaml-cpp, pystring, Imath, zlib, minizip-ng) par `tools/build_ocio.sh`. Premier build : 2 à
3 minutes, réseau requis. Les produits sont ignorés par git (`.ocio/build/`, `.ocio/lib/`) : relancer le script
sur une machine neuve, puis rebuild d'ArmorPaint.

OCIO est en C++ et amake ne compile que du C : le shim `.ocio/studio_ocio_lib.cpp` expose une ABI C
(`studio_ocio_lib.h`, 26 fonctions, version `STUDIO_OCIO_LIB_ABI`). Aucune exception ne la traverse : chaque
erreur devient un retour NULL/0/-1 et un message (`studio_ocio_lib_error()`, par thread).

- **macOS : liaison statique.** `build_ocio.sh` fusionne le shim, OCIO et ses dépendances en un seul objet
  (`ld -r`) où seuls les `studio_ocio_lib_*` restent visibles : aucune collision possible avec l'app. L'archive
  `.ocio/lib/libstudio_ocio.a` est liée par `.project/project.c` (define `WITH_STUDIO_OCIO_STATIC`). Pas de
  dlopen : ArmorPaint est signé ad hoc avec le hardened runtime, dont la validation des bibliothèques refuse une
  dylib d'une autre « équipe » (« different Team IDs »). Coût : environ 7 Mo sur le binaire.
- **Linux : dlopen** de `data/libstudio_ocio.so` (copié dans `assets/` par le script). Écrit, non testé.
- **Windows : à faire.**
- Sans bibliothèque, ArmorPaint se construit et démarre sans OCIO ; `studio_ocio_status()` dit pourquoi.

**Configuration**, dans l'ordre : chemin explicite (`studio_ocio_reload`, préférences du lot A5), `$OCIO`, puis la
configuration studio intégrée à OCIO (`ocio://studio-config-latest`, ACES 2.0). Un `$OCIO` invalide est une erreur,
sans repli : un repli cacherait au studio que sa configuration n'est pas chargée. Le log de démarrage l'affiche :

```
studio: OCIO 2.5.2, config "studio-config-v4.0.0_aces-v2.0_ocio-v2.5" from ocio://studio-config-latest (built-in default), 55 colorspaces, display "sRGB - Display", working space "Linear Rec.709 (sRGB)"
```

`studio_ocio_transform()` convertit des pixels float32 (RGB ou RGBA) sur place, avec le processeur CPU par défaut
d'OCIO (celui d'`ocioconvert` et de PyOpenColorIO). L'alpha n'est jamais transmis à OCIO, dont certaines vues
d'affichage le modifient. Les processeurs sont gardés en cache par couple (src, dst), et l'appel est sûr depuis
plusieurs threads sur des bandes disjointes. La liste des espaces contient aussi les espaces d'affichage
(`sRGB - Display`…).

**Espace de travail.** ArmorPaint stocke ses couleurs encodées gamma 2.2 en primaires Rec.709 ; décodées, elles sont
en Rec.709 linéaire. C'est l'espace de travail : la source de toutes les conversions. Il est cherché dans la
configuration par son équivalence avec l'espace intégré `lin_rec709_srgb` d'OCIO (`IdentifyBuiltinColorSpace`) :
dans les configurations ACES studio et CG, `Linear Rec.709 (sRGB)`. Cette recherche demande les rôles d'échange
(`aces_interchange`, `cie_xyz_d65_interchange`). Une configuration maison qui ne les a pas doit le nommer par
`STUDIO_OCIO_WORKING_SPACE` (nom, alias ou rôle, par exemple `scene_linear` si c'est bien du Rec.709 linéaire). Sans
lui, les conversions sont bloquées et le statut dit pourquoi. Le projet le portera au lot A5.

### Viewport (A2)

En modes **Lit** et **Path Trace**, le compositeur du viewport remplace son tonemap filmique par la transformation
d'affichage d'OCIO : de l'espace de travail vers un display et une view, avec un look facultatif. Le shader ne peut
pas exécuter le code GPU d'OCIO ; la transformation est donc calculée sur le CPU par le processeur exact d'OCIO, en
une **LUT 3D de 129³ nœuds** (RGBA half, texture de 17 Mo, gardée aussi sur le CPU pour `studio_ocio_view_apply_rgb`), que le shader interpole (patch `ocio-viewport`). Son
entrée est mise en forme par la courbe ACEScct : de 0,0069 sous le noir à 222 au-dessus du blanc, logarithmique,
comme les LUT de viewport des DCC. Calcul : environ 0,1 s sur tous les cœurs, au démarrage et à chaque changement.
Les autres modes (couleur de base, normales, rugosité…) montrent des données et restent inchangés. Une LUT `.cube`
chargée dans les préférences s'applique après OCIO, sur les valeurs d'affichage.

**Menu Color** (barre de menus, après ShotGrid) : case « OCIO Viewport » (décochée : le tonemap filmique
d'ArmorPaint), sous-menus Display, View et Look. Une view HDR est marquée « (HDR) » : le viewport est un affichage
SDR 8 bits, elle y est sans intérêt. La transformation suit un rechargement de la configuration : si la view choisie
n'y existe plus, celle par défaut la remplace (journalisé). Sans OCIO ou sans espace de travail, le viewport reste
filmique et le statut dit pourquoi. Le choix n'est pas encore mémorisé : variables `STUDIO_OCIO_DISPLAY`/`_VIEW`/
`_LOOK` en attendant les préférences du lot A5.

**Précision de la LUT** contre OCIO exact, en 1/255 après écrêtage à [0, 1], sur 400 000 couleurs (gris HDR,
couleurs saturées, 19 stops, albédos éclairés ; `.tests/ocio_view_test.c`, configurations studio et CG) :

| Display / view | gris | 99,9 % | max |
|---|---|---|---|
| sRGB / ACES 2.0 SDR | 0,06 | 2,3 | 8,9 |
| Display P3 / ACES 2.0 SDR | 0,06 | 2,2 | 9,3 |
| Gamma 2.2 Rec.709 / ACES 2.0 SDR | 0,11 | 2,6 | 11,7 |
| Rec.1886 Rec.709 / ACES 2.0 SDR | 0,19 | 3,4 | 14,2 |
| P3-D65 (gamma 2,6) / ACES 2.0 SDR | 0,31 | 4,0 | 17,5 |
| sRGB / Un-tone-mapped, Video | 0,08 | 0,08 | 0,08 |
| sRGB / Raw | 0,26 | 0,25 | 0,26 |
| vues HDR (PQ, HLG, Display P3 HDR) | 0,12 | 21 | 36 |

Les gris et les couleurs courantes sont exacts à mieux qu'un code 8 bits. Les écarts maximaux viennent des couleurs
très saturées au bord du gamut : la vue ACES 2.0 y compresse le gamut selon la teinte, une courbe que l'interpolation
d'une LUT lisse. Dans l'app, le compositeur (GPU) est à 0,14/255 d'OCIO sur les couleurs sondées. La traduction
exacte du shader GPU d'OCIO en kong reste une piste (lot A6), si un show en a besoin.

**Non couvert.** Les sphères d'environnement (menu Viewport, désactivées par défaut) gardent le tonemap filmique.

### Export (A3)

Un `color_space` qui n'est pas `raw`, `linear` ou `srgb` est un espace OCIO. Les couleurs sont décodées comme en
`linear`, les expressions calculées, puis le RGB composé est converti de l'espace de travail vers cet espace, ligne
par ligne dans les threads de composition, avant la quantification. L'alpha ne passe jamais par OCIO. Une texture à
1 canal est convertie comme un gris (v, v, v), dont on garde R. Un espace de donnée (`isdata`, `Raw`) écrit les
valeurs telles quelles, comme `raw`. `Linear Rec.709 (sRGB)` donne donc, au bit près, le même fichier que `linear`.

```json
{ "name": "base", "channels": ["base_r", "base_g", "base_b"], "color_space": "ACEScg" },
{ "name": "base_srgb", "channels": ["base_r", "base_g", "base_b"], "format": "png", "bits": 8, "color_space": "sRGB - Texture" },
{ "name": "rough", "channels": ["rough"], "color_space": "Raw" }
```

Le nom est validé à l'export, contre la configuration chargée à ce moment : un preset peut viser les espaces d'une
configuration de show. Les presets livrés restent en `raw`/`linear`, qui marchent sans OCIO.

| Code du préflight | Niveau | Cas |
|---|---|---|
| `ocio_unavailable` | bloquant | OCIO non chargé (`$OCIO` invalide, build sans bibliothèque) |
| `ocio_unknown_colorspace` | bloquant | nom absent de la configuration |
| `ocio_no_working_space` | bloquant | pas d'espace de travail (voir plus haut) |
| `ocio_no_transform` | bloquant | OCIO ne sait pas construire la conversion |
| `ocio_color_as_data` | avertissement | canaux de couleur écrits dans un espace de donnée : valeurs encodées gamma 2.2 |
| `ocio_data_as_color` | avertissement | canaux de donnée (rugosité…) convertis vers un espace couleur |
| `exr_not_linear` | avertissement | EXR dans un espace non linéaire (encodage OCIO autre que `scene-linear`, par exemple ACEScct) |
| `linear_8bit` | avertissement | espace linéaire en 8 bits (banding) |

Le manifeste note, pour chaque fichier OCIO, l'espace canonique (un alias est résolu) et la conversion :
`"ocio": {"colorspace": "ACEScg", "from": "Linear Rec.709 (sRGB)", "transform": "<cache id OCIO>"}`, ou
`{"colorspace": "Raw", "data": true}`. Au niveau du manifeste, `"ocio"` donne la version d'OCIO, le nom et la source
de la configuration et l'espace de travail ; `null` si aucune texture n'en dépend. L'identifiant de conversion
change si la configuration change : deux exports au même identifiant ont subi la même conversion.

### Scripts (minic)

```c
char *studio_ocio_status(void);
int   studio_ocio_reload(char *config);          // "" : ordre par défaut ; 1 si chargée
int   studio_ocio_colorspace_count(void);
char *studio_ocio_colorspace_name(int i);
char *studio_ocio_display_default(void);
char *studio_ocio_view_default(char *display);
char *studio_ocio_working_space(void);           // espace de travail ; "" si la configuration n'en a pas
char *studio_ocio_resolve(char *name);           // nom canonique d'un espace, alias ou rôle ; "" si inconnu
int   studio_ocio_is_data(char *colorspace);     // 1 donnée, 0 couleur, -1 inconnu
char *studio_ocio_colorspace_from_path(char *path);  // file_rules de la configuration
char *studio_ocio_transform_rgb(char *src, char *dst, float r, float g, float b);  // "r g b" (%.9g) ; "" si échec

// Viewport (A2)
int   studio_ocio_view_enabled(void);            // choix de l'utilisateur (case du menu Color)
int   studio_ocio_view_active(void);             // 1 si le viewport passe par OCIO (LUT prête)
int   studio_ocio_view_set_enabled(int on);      // rend studio_ocio_view_active()
int   studio_ocio_view_set(char *display, char *view, char *look);  // "" : défaut (look : aucun) ; 0 si inconnu, rien ne change
char *studio_ocio_view_display(void);
char *studio_ocio_view_view(void);
char *studio_ocio_view_look(void);               // "" : aucun
char *studio_ocio_view_status(void);
char *studio_ocio_view_apply_rgb(float r, float g, float b);  // la LUT sur une couleur de l'espace de travail : "r g b" ; "" si inactif
char *studio_ocio_view_probe(int argb);          // diagnostic : 0xAARRGGBB passé par le vrai compositeur (grain et vignette coupés) : "r g b"
```

## Tests

```sh
.tests/run.sh [dossier de travail]     # EXR (B1) ; FUZZ=<n> pour le nombre de mutations
.tests/proc.sh [dossier de travail]    # processus (C1)
.tests/obj.sh [dossier de travail]     # import OBJ (patch obj-global-indices) ; FUZZ=<n> pour le nombre de mutations
STUDIO_TEST_TK_CORE=<tk-core> .tests/sg.sh [dossier de travail]   # ShotGrid (C2), noms des exports (B3) ; STUDIO_TEST_TK_APPS pour le moteur (C3)
.tests/export.sh [dossier de travail]  # export studio (B2, B4, B5) hors app : encodeurs relus par libpng/libtiff, expressions, presets, préflight ; FUZZ=<n>
.tests/export_app.sh [dossier]         # export studio dans l'app : scripts, --export-textures, pixels = upstream, manifeste, atomicité
.tests/ocio.sh [dossier de travail]    # OCIO (A1) hors app : ABI, module, références PyOpenColorIO, threads, erreurs ; OCIO_TEST_CONFIGS=<configs>
.tests/ocio_app.sh [dossier]           # OCIO dans l'app : configuration par défaut, $OCIO, $OCIO invalide, API de scripting
.tests/ocio_export_app.sh [dossier]    # export OCIO (A3) dans l'app : pixels = PyOpenColorIO, préflight, manifeste, cas bloquants
.tests/ocio_view_app.sh [dossier]      # viewport OCIO (A2) dans l'app : compositeur = PyOpenColorIO, menu (API), rechargement, variables
```

`ocio.sh` compare au bit près à PyOpenColorIO, la roue officielle à la même version, installée dans `.ocio/venv`
au premier lancement. Pour chaque configuration (celle du banc avec LUT, looks et file_rules, puis
`ocio://studio-config-latest` et `ocio://cg-config-latest`), il vérifie les listes, les rôles, les alias et les
file_rules, puis chaque espace depuis et vers scene_linear et chaque display/view dans les deux sens, sur des pixels
HDR, négatifs et nuls. Il lance aussi `ocio_view_test.c` (A2) : conversion half, courbe ACEScct, disposition des
tuiles, interpolation exacte d'une LUT linéaire, puis la précision de la LUT de chaque view (tableau de « Viewport
(A2) »), avec des tolérances pour les vues SDR. Le harnais est construit deux fois, lié en statique comme l'app et
chargé par dlopen, avec de fausses bibliothèques (ABI différente, symbole manquant).

`ocio_export_app.sh` exporte une texture procédurale (1,7 million de couleurs) en EXR 32 et half, TIFF 16 et
PNG 8, à 1, 3 et 4 canaux, vers ACEScg, ACEScct, sRGB - Texture, Raw et des alias. `ocio_export_check.py` relit
chaque fichier et le compare à PyOpenColorIO appliqué à la texture brute : identique au bit près (au code près en
8 et 16 bits). Il demande, dans `.ocio/venv` : `pip install numpy OpenEXR tifffile pillow`.

`export.sh` requiert libpng et libtiff (`brew install libpng libtiff`) ; `export_app.sh`, Pillow et `tiffinfo`.

`sg.sh` fait tourner le sidecar sur un site ShotGrid simulé (`sg_fixture.py` : mockgun avec le schéma d'un vrai
site, la configuration Toolkit de `sg_fixture/config`, une session Desktop enregistrée, un vrai amorçage). Il lui
faut un clone de tk-core (`git clone --depth 1 --branch v0.24.2 https://github.com/shotgunsoftware/tk-core` ;
**pas** le paquet PyPI `tk-core`, qui n'est pas celui d'Autodesk). Sans `STUDIO_TEST_TK_CORE`, seuls le lecteur
JSON et le superviseur (contre `sg_fake.py`) tournent. Dans l'app, le site simulé remplace le vrai sidecar avec
`STUDIO_SG_BRIDGE=<external>/.tests/sg_fixture.py STUDIO_TEST_WORKDIR=<dossier> STUDIO_TEST_TK_CORE=<tk-core>`.

`run.sh` requiert les outils OpenEXR (`brew install openexr`) comme référence indépendante. La couche jobs et
l'API de scripting se vérifient dans l'app : `ArmorPaint --background --script <fichier>` avec un script comme
celui ci-dessus.
