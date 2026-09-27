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
- `.shotgrid/` : sources de Toolkit (tk-core, Workfiles, Publish et leurs frameworks) pour la démo ShotGrid et les
  tests ; le venv PySide6 y est créé par `tools/setup_shotgrid.sh`, hors de git.
- `tools/armorpaint.sh` : lance la version studio (build compris) ; `--shotgrid-demo` pour le site simulé. Sans
  `STUDIO_PYTHON`, il prend le venv PySide6 de `.shotgrid/` (créé au besoin) pour la fenêtre d'export.
- `tools/recette_c6.py` : vérifie un poste et la configuration Toolkit d'un projet sur le vrai site ShotGrid, par le
  sidecar d'ArmorPaint (recette C6, diagnostic d'un poste).
- `docs/` : documentation d'exploitation, en français : `GUIDE_ARTISTE.md` (une page pour l'artiste),
  `EXPLOITATION.md` (déploiement, fichiers d'un poste, surveillance, incidents, mises à jour) et `RECETTE_C6.md`
  (recette sur le vrai site ShotGrid).

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
| `studio_ocio_import` | A4 | ✅ textures importées : espace source par texture (nom, file_rule, format, choix de l'artiste), pixels convertis vers l'espace de travail au décodage, espaces dans le `.arm` (macOS) |
| `studio_prefs`, `studio_project`, `studio_ocio_prefs` | A5 | ✅ réglages couleur : préférences studio (`studio_prefs.json`, onglet Color), espace de travail gardé dans le `.arm`, textures reconverties quand il change (macOS) |
| `.tests/ocio_validation_app.sh`, `.tests/ocio_validation.py` | A6 | ✅ validation de la chaîne couleur contre oiiotool, ociobakelut et Nuke 17 : mires, calques 8/16/32 bits, espace de travail ACEScg, viewport ; export upstream en EXR corrigé sur Metal (macOS) |
| `studio_sg`, `studio_shotgrid`, `studio_json`, `assets/studio_sg_*.py` | C2 | ✅ sidecar ShotGrid (Toolkit), superviseur, API de scripting (macOS ; validé sur site simulé) |
| `studio_workfiles`, `tk-armorpaint/` | C3 | ✅ Workfiles (tk-multi-workfiles2) et menu ShotGrid, par le moteur Toolkit tk-armorpaint (macOS ; validé sur site simulé) |
| `studio_publish`, `tk-armorpaint/hooks/tk-multi-publish2/` | C4 | ✅ publish ShotGrid par tk-multi-publish2 : projet copié par ArmorPaint, textures exportées puis publiées, séquences `<UDIM>`, vignette du viewport (macOS ; validé sur site simulé) |
| `studio_sg_context`, `studio_sg_offline`, `studio_audit`, `assets/studio_sg_queue.py`, `assets/studio_audit.py` | C5 | ✅ contexte ShotGrid gardé dans le `.arm`, mode dégradé hors ligne, file d'attente des publishes rejouée au retour du site, journal d'audit (macOS ; validé sur site simulé) |
| `tools/recette_c6.py`, `docs/` | C6 | 🟡 kit de recette prêt (outil de vérification, fiche, guide de l'artiste, runbook) ; recette sur le vrai site à faire |
| `studio_export_window`, `assets/studio_export_window.py`, `studio_atlas` | B6 | ✅ fenêtre d'export Qt (PySide6) à la place de la boîte d'export : presets studio, espace OCIO, préflight en direct, nom des fichiers, jeux de textures, template et version ShotGrid (macOS) |
| `studio_batch` | B7 | ✅ mode batch : `--background` sans fenêtre, options `--export-mode`, `--export-naming`, `--export-color-space`, rapport JSON (`--report`), code de retour 0/1/2 (macOS) |

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
upstream sans rebuild ; les réglages destinés aux artistes passeront par l'UI et les presets (lots B4/B6). Les
réglages couleur des artistes sont dans les préférences studio (« Réglages (A5) ») : les variables OCIO
l'emportent sur elles, pour le lanceur d'un show.

| Variable | Effet |
|---|---|
| `STUDIO_EXR_COMPRESSION` | `none`, `rle`, `zips`, `zip` (défaut), `piz`, `pxr24`, `b44`, `b44a`, `htj2k256`, `htj2k32` |
| `STUDIO_EXR_WRITER=legacy` | rend l'écriture EXR au writer upstream |
| `STUDIO_EXR_READER=legacy` | rend la lecture EXR à l'importeur upstream |
| `STUDIO_EXR_THREADS` | nombre de threads de codec (défaut : cœurs matériels) |
| `STUDIO_SG_*`, `STUDIO_SGTK_PATH` | sidecar ShotGrid, voir « ShotGrid (C2) » |
| `STUDIO_AUDIT_LOG` | journal d'audit à la place de `studio_audit.jsonl` des préférences ; voir « Contexte et mode dégradé ShotGrid (C5) » |
| `STUDIO_EXPORT_PRESETS` | presets d'export studio : dossiers ou fichiers `.json`, séparés par `:` (`;` sous Windows), prioritaires sur les presets livrés |
| `STUDIO_EXPORT_COLOR_SPACE` | espace OCIO imposé aux textures de couleur des exports studio au démarrage (vide : celui du preset) ; voir « Fenêtre d'export (B6) » |
| `STUDIO_PYTHON` | interpréteur Python 3 avec PySide6 de la fenêtre d'export (défaut : `STUDIO_SG_PYTHON`, puis `python3`) |
| `STUDIO_EXPORT_WINDOW` | script de la fenêtre d'export à la place de `data/studio_export_window.py` |
| `STUDIO_EXPORT_THREADS` | threads de composition et d'encodage des exports studio (défaut : cœurs matériels) |
| `STUDIO_PREFS` | fichier des préférences studio à la place de `studio_prefs.json` ; en `--background`, seul moyen de les lire et de les écrire ; voir « Réglages (A5) » |
| `OCIO` | configuration OpenColorIO (fichier `.ocio`, archive `.ocioz` ou URI `ocio://`), à la place de la préférence ; défaut : la préférence, sinon `ocio://studio-config-latest` |
| `STUDIO_OCIO_WORKING_SPACE` | espace de travail des nouveaux projets (nom, alias ou rôle), à la place de la préférence ; un projet enregistré garde le sien. Sert aussi de repli à l'automatique quand la configuration n'a pas d'espace reconnaissable ; voir « Gestion couleur OCIO » |
| `STUDIO_OCIO_LIB` | chemin de `libstudio_ocio` pour un build sans liaison statique (bancs de test, Linux) |
| `STUDIO_OCIO_VIEWPORT=0` | viewport au tonemap filmique d'ArmorPaint au démarrage (réactivable par le menu Color), à la place du choix gardé dans les préférences ; une autre valeur : OCIO |
| `STUDIO_OCIO_DISPLAY`, `STUDIO_OCIO_VIEW`, `STUDIO_OCIO_LOOK` | transformation d'affichage du viewport au démarrage, à la place du dernier choix du menu Color ; dès que l'une est définie, les trois viennent de l'environnement. Défaut : display et view par défaut de la configuration, sans look. Une valeur inconnue est journalisée et remplacée par le défaut |
| `STUDIO_OCIO_IMPORT=0` | textures importées sans conversion OCIO (upstream) ; l'entrée des espaces d'un `.arm` n'est ni lue ni réécrite ; voir « Textures importées (A4) » |

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
| `STUDIO_SG_NET_TIMEOUT` | délai des connexions au site, en secondes (défaut 30) ; voir C5 |
| `STUDIO_SG_CHECK_INTERVAL` | hors ligne, secondes entre deux essais du site (défaut 120, `0` : jamais) ; voir C5 |
| `STUDIO_SG_QUEUE` | dossier de la file d'attente des publishes (défaut `studio_publish_queue/` des préférences) ; voir C5 |
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
| `texture_pattern` | `path`, `project_id` ?, `site` ?, `template` ? | `{template, pattern, pattern_udim}` : chemin des textures exportées depuis le fichier de travail `path`, marqueurs `<channel>`, `<UDIM>`, `<ext>` ; `template` null (et `reason`) hors pipeline. Voir B3. Template : `template`, sinon celui choisi pour le projet (`texture_template` de son sidecar), sinon celui du réglage ; la réponse donne aussi `default_template`, `chosen`, `templates` (ceux qui peuvent nommer ses textures) et `name`, `version` du fichier (voir « Fenêtre d'export ») |
| `publish` | `description` ?, `skip` ?, `task_id` ? ou `project_id` ? | `{context, publishes, skipped, finalized, messages}` : publish du projet ouvert sans fenêtre, voir C4 ; `queued` : publishes mis en file, voir C5 |
| `check_online` | — | `{online, site, version}` : le site répond, sans amorçage ; sinon `sg_unreachable`. Voir C5 |
| `pending_publishes` | — | `{queue, count, publishes}` : la file d'attente, sans requête au site |
| `replay_publishes` | `project_id` ? | `{project_id, replayed, failed, stopped, pending, projects, other_sites}` : enregistre les publishes en file d'un projet |
| `discard_publishes` | `keys` | `{discarded, missing}` : publishes écartés de la file |
| `ping`, `shutdown` | — | `{}` |

Codes d'erreur (`/error/code`) : du sidecar, `bad_request`, `unknown_op`, `auth_required` (pas de session Desktop,
ou expirée : jamais d'invite de mot de passe), `sg_unreachable`, `sg_error`, `toolkit_unavailable` (tk-core
introuvable), `toolkit_error`, `not_found`, `template_missing` (la config n'a pas le template de travail ou de textures ; pour `texture_pattern`, l'erreur porte
aussi `templates`, `default_template`…, de quoi en choisir un autre),
`internal`, `app_unavailable` (moteur tk-armorpaint absent de la config, ou commande inconnue : Qt manquant…),
`publish_invalid`, `publish_failed`, `nothing_to_publish` (op `publish`) ; du superviseur, `sidecar_failed`,
`sidecar_died`, `timeout`, `cancelled`. `restart_required` n'arrive à l'appelant que si le rejeu échoue lui aussi.

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
      - {app_instance: tk-multi-publish2, name: Publish...}      # voir C4
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

## Publish ShotGrid (C4)

Le publish est celui de Toolkit : la fenêtre de **tk-multi-publish2**, ouverte dans le sidecar comme File Open (menu
ShotGrid > Publish..., **Ctrl+Alt+P**, Cmd+Alt sur macOS), avec les hooks du moteur
(`tk-armorpaint/hooks/tk-multi-publish2/basic/`). L'op `publish` fait la même chose sans fenêtre.

Ce qui est publié, dans le contexte ShotGrid courant (la tâche de la dernière fenêtre, ou `task_id`) :

- **le projet** (`collector.py`, `publish_session.py`) : enregistré, copié à l'emplacement du template de publish,
  enregistré dans ShotGrid avec une vignette du viewport, puis ouvert à sa version suivante, comme dans les moteurs
  des DCC. Type de publish : `STUDIO_SG_PUBLISH_TYPE` (`ArmorPaint Project`), celui que liste File Open. Sans
  template de publish, le projet est publié en place ;
- **les textures** (`publish_textures.py`), sous le projet : une par texture du preset et par variante (objet, jeu de
  textures). En export par tuile, une **séquence `<UDIM>`** : un seul publish pour toutes ses tuiles, de type
  `UDIM Image` (`Texture` sinon), dépendant du publish du projet. ArmorPaint exporte au publish, une seule fois, les
  textures cochées, nommées par le template des textures (B3) ; chaque fichier est ensuite copié à l'emplacement du
  `Publish Template` du plugin (sans lui, publié en place).

La copie du projet est écrite par ArmorPaint (patch `project-copy`, `PATCHES.md`) : un `.arm` désigne ses fichiers
(textures importées, maillage source, envmap) par des chemins relatifs à son dossier, qu'une simple copie du fichier
perdrait. La copie publiée les retrouve, même sur un poste sans le dossier de travail de l'artiste, sauf les fichiers
rangés dans ce dossier.

**Réglages de l'export**, le premier qui existe :

1. le dernier export de la fenêtre d'export pour ce projet (sidecar du projet, voir « Export studio ») ;
2. l'export courant de la session, celui que refait Ctrl+E ;
3. le preset studio du réglage `Export Preset` du collecteur, couches visibles, par tuile si le maillage a des tuiles
   UDIM.

Sans aucun, le projet est publié seul. L'export courant n'est pas changé. La collecte n'écrit rien : ArmorPaint fait
le préflight (B5), et un problème bloquant arrête le publish à la validation.

**Garde-fous.**

- Une texture sans version dans son nom est décochée : l'export suivant réécrirait le fichier publié. Il faut un
  template des textures avec `{version}`, ou un nom de fichier avec `<file>` ou `<version>`.
- Un nom que le template des textures refuse (canal `base_color` face à `filter_by: alphanumeric`, extension hors des
  `choices`) bloque le publish quand le plugin a un `Publish Template`. Sans, c'est un avertissement : tk-multi-publish2
  publierait le fichier en place sans rien dire.
- Une clé UDIM sans `default: "<UDIM>"` est signalée : ShotGrid enregistrerait la séquence en `%04d`, que les
  chargeurs de textures UDIM ne reconnaissent pas.
- Un projet d'une autre entité ou étape que le contexte est refusé, textures comprises : ouvert autrement que par
  File Open (menu File d'ArmorPaint, fichiers récents), il garde le contexte de la dernière fenêtre, et ses publishes
  iraient à la tâche d'un autre élément. File Open l'ouvre dans le contexte de sa tâche (bouton dans la fenêtre).
  Contrôle d'après le path cache de Toolkit : un dossier qu'il ne connaît pas n'est pas contrôlé.
- Un projet jamais enregistré, ou changé depuis la collecte, est une erreur (bouton Save As dans la fenêtre). Un
  projet hors du template de travail est signalé. Une version suivante déjà sur le disque est une erreur, avec un
  bouton qui enregistre à la première version libre.

Configuration Toolkit du studio, ici pour les assets (de même pour les shots) :

```yaml
# core/templates.yml, en plus de ceux de C2 et B3
paths:
    armorpaint_asset_publish: '@asset_root/publish/armorpaint/{name}.v{version}.arm'
    armorpaint_asset_textures_publish: '@asset_root/publish/armorpaint/textures/v{version}/{name}_{channel}[.{UDIM}].{texture_ext}'
```

```yaml
# environnements de tâche : engines.tk-armorpaint.apps
tk-multi-publish2:
  location: {type: app_store, name: tk-multi-publish2, version: v2.11.2}
  collector: '{self}/collector.py:{engine}/tk-multi-publish2/basic/collector.py'
  collector_settings: {Work Template: armorpaint_asset_work, Export Preset: <preset studio>}
  publish_plugins:
    - name: Publish to Flow Production Tracking      # fichiers glissés dans la fenêtre
      hook: '{self}/publish_file.py'
      settings: {}
    - name: Publish ArmorPaint Project
      hook: '{self}/publish_file.py:{engine}/tk-multi-publish2/basic/publish_session.py'
      settings: {Publish Template: armorpaint_asset_publish}
    - name: Publish Textures
      hook: '{self}/publish_file.py:{engine}/tk-multi-publish2/basic/publish_textures.py'
      settings: {Publish Template: armorpaint_asset_textures_publish, Publish Type: Texture, UDIM Publish Type: UDIM Image}
```

**Op `publish`**, sans fenêtre :

- args : `description` ? (le commentaire de chaque publish), `skip` ? (éléments à ne pas publier, par nom,
  `robot_base.<UDIM>.png`, ou par type, `armorpaint.texture`), `task_id` ? ou `project_id` ? (contexte, comme
  `show_app`) ;
- data : `{context, publishes, skipped, finalized, messages}`. `publishes` : `[{item, type, id, name, path,
  version_number, published_file_type}]` ; `messages` : avertissements et erreurs des hooks, `[{level, message}]` ;
  `finalized` faux : tout est publié, mais la finalisation (version suivante du projet) a échoué, voir `messages` ;
- erreurs : `publish_invalid` (à la validation, rien n'est publié), `publish_failed` (pendant le publish ; le message
  dit ce qui l'est déjà), `nothing_to_publish`, `app_unavailable` (tk-multi-publish2 absent de l'environnement).

Comme la fenêtre, l'op agit sur le projet par des requêtes host : elle tourne en job (`studio_sg_start`), et l'app
doit rendre ses frames jusqu'à la fin. Script testé dans ArmorPaint avec son interface (`--background` ne l'est pas) :

```c
int job = 0;
void poll() {
	if (job != 0 && studio_sg_state(job) == 3) {
		console_log(string("publish ok=%d %s", studio_sg_ok(job), studio_sg_get(job, "/error/message")));
		studio_sg_release(job);
		job = 0;
	}
}
int main() {
	job = studio_sg_start("publish", "{\"description\":\"Lookdev v1\"}", 0);
	script_notify_on_update(poll);
	return 0;
}
```

**Requêtes host** du publish (`studio_publish.c`), en plus de celles de Workfiles :

| Op | args | data |
|---|---|---|
| `publish_preflight` | `naming`, `choices` ?, `skip` ? | `{path, naming_template, export: {ok, report, error}}` : préflight de l'export, rien n'est écrit |
| `publish_export` | idem | idem, après l'export |
| `publish_thumbnail` | `path` | `{path, width, height}` : PNG du viewport (1280 px au plus), quelques frames plus tard |
| `publish_copy` | `path` | `{path, size}` : copie du projet enregistré (`.arm`), écrite à côté puis renommée |

`naming` : le modèle des noms de textures (`{template, pattern, pattern_udim}` de l'op `texture_pattern`), calculé
par le sidecar, qui ne peut pas répondre à ArmorPaint tant que la fenêtre est ouverte. `choices` : les réglages d'un
export de la fenêtre d'export ; absent, l'export courant. `skip` : les fichiers à ne pas écrire,
`[[texture, variante, tuile], …]`. Dans `report`, chaque fichier d'une tuile porte `sequence`, son chemin avec
`<UDIM>` à la place de la tuile. Erreurs : `not_saved`, `bad_request`, `write_failed`, `no_viewport`, `busy`.

**Limites connues.**

- Validé sur le site simulé seulement : l'envoi des vignettes (mockgun les ignore), les types de publish et les
  chargeurs de textures UDIM du studio sont à vérifier à la recette (C6).
- Pas de Version de review : le plugin `upload_version.py` de tk-multi-publish2 n'est pas configuré.
- La vignette est l'image du viewport, sans compas ni sphères d'environnement.
- Un fichier rangé dans le dossier de travail (texture importée depuis `work/`) est désigné là par la copie
  publiée : il manque sur un poste sans ce dossier.
- Windows : pas de sidecar (voir C2).

## Contexte et mode dégradé ShotGrid (C5)

### Contexte du projet

Le contexte ShotGrid (projet, entité, étape, tâche) est celui de la dernière fenêtre Toolkit (File Open, File Save,
Publish...) ou du menu ShotGrid. Il est **enregistré dans le `.arm`**, entrée `studio_shotgrid.json` (patch
`project-entries`, lot A5), avec son site :

```json
{"version":1,"context":{"site":"https://studio.shotgrid.autodesk.com","project_id":12,"project":"Demo",
 "entity_type":"Asset","entity_id":34,"entity":"robot","step":"Texture","task_id":56,"task":"texturing",
 "label":"Demo / robot / Texture / texturing"}}
```

Un projet rouvert par le menu File d'ArmorPaint, sur un autre poste ou après un redémarrage, reprend donc sa tâche :
la prochaine fenêtre ShotGrid s'y ouvre, le publish y va. Ouvrir un `.arm` prend son contexte, ou aucun s'il n'en a
pas (`studio: shotgrid context cleared: the opened project has none` au log) ; un nouveau projet garde le contexte
courant ; File Open de Workfiles donne celui de sa fenêtre, qui l'emporte sur celui du fichier. Le contexte d'un autre
site ShotGrid reste dans le fichier, mais le sidecar ignore ses identifiants (`context of another ShotGrid site
ignored` au log) : projet et tâche viennent alors du chemin du fichier.

Le titre de la fenêtre le montre : `robot.v003 - Demo / robot / Texture / texturing - ArmorPaint`.

### Mode dégradé

Si le site ShotGrid ne répond plus, ArmorPaint reste utilisable **en mode fichier** : ouvrir, peindre, enregistrer et
exporter ne dépendent pas de ShotGrid. Ce qui en dépend échoue vite et le dit :

- le passage hors ligne est dit une fois dans la console (`ShotGrid unreachable (<raison>): ArmorPaint keeps working
  on files, publishes are queued until ShotGrid is back`), puis dans le titre (`… (ShotGrid offline) - ArmorPaint`)
  et en tête du menu ShotGrid (`ShotGrid offline: <raison>`, **Check Connection**) ;
- les fenêtres Toolkit sont refusées aussitôt (`ShotGrid is offline: File Open needs the ShotGrid site`), sans
  attendre le délai du réseau ;
- les textures exportées gardent les noms d'ArmorPaint au lieu de ceux du template (B3), dit une fois par projet ;
- un publish en cours met ses enregistrements restants en file d'attente (ci-dessous).

Le sidecar sait si le site a répondu : chaque réponse d'une requête qui lui a parlé porte `"sg"`, `{"online":true}` ou
`{"online":false,"reason":…}`, comme l'erreur `sg_unreachable`, et ses connexions ont un délai
(`STUDIO_SG_NET_TIMEOUT`, 30 s). Le superviseur en tient l'état (`studio_sg_network`), dit au log à chaque changement.
Hors ligne, le site est de nouveau essayé (op `check_online`) toutes les `STUDIO_SG_CHECK_INTERVAL` secondes (120 par
défaut, `0` : jamais), **ArmorPaint au repos compris** : l'essai est un job différé du superviseur
(`studio_sg_start_after`), qui part à l'heure sans frame et réveille l'app par sa réponse. Tout appel réussi compte
aussi. Au retour du site (`ShotGrid is back`), le titre et les noms des templates reviennent, et les publishes en file
sont rejoués.

En ligne, rien ne sonde le site : une coupure est découverte au prochain appel à ShotGrid (menu, fenêtre, nommage des
textures, publish).

### File d'attente des publishes

Quand le site devient injoignable pendant un publish, ses fichiers sont déjà écrits (copie du projet, textures) : leurs
enregistrements dans ShotGrid (`register_publish`) sont gardés dans une file d'attente, un fichier JSON par publish et
sa vignette, dans `STUDIO_SG_QUEUE` (défaut : `studio_publish_queue/` à côté des préférences studio). Le publish
aboutit : l'op `publish` rend, pour chaque publish en file, `queued` (sa clé) à la place de `id`, et la liste de ces
clés dans `queued`. Sans file (`--background` sans `STUDIO_SG_QUEUE` ni `STUDIO_PREFS`), un publish interrompu échoue
(`publish_failed`).

Au retour du site, la file est **rejouée d'elle-même**, projet par projet (op `replay_publishes` ; un projet Toolkit
par processus), dans l'ordre : le projet avant ses textures, qui en dépendent. Un publish créé avant l'échec (réponse
perdue) n'est pas recréé, il est complété (vignette, dépendances). Le menu ShotGrid montre ceux qui restent
(`2 publishes waiting for ShotGrid`, **Replay Pending Publishes**). Seuls ceux du site de la session sont rejoués ;
ceux d'un autre site attendent une session sur ce site. L'op `discard_publishes` en écarte (fichiers déplacés dans
`discarded/` du dossier de la file).

⚠️ Sur macOS, les préférences studio sont dans l'app quand elle est inscriptible (`data/`, à côté de `config.json`) :
remplacer l'app efface alors la file. Le lanceur règle `STUDIO_SG_QUEUE` (et `STUDIO_PREFS`, `STUDIO_AUDIT_LOG`) vers
un dossier de l'utilisateur ; voir `docs/EXPLOITATION.md`, qui dit aussi comment écarter un publish ArmorPaint fermé.

### Journal d'audit

Une ligne JSON par évènement (JSON Lines) dans `studio_audit.jsonl`, à côté des préférences studio, ou dans
`STUDIO_AUDIT_LOG` ; rien sans l'un ni l'autre (`--background` sans `STUDIO_PREFS`). ArmorPaint et le sidecar écrivent
dans le même fichier, chaque ligne d'une seule écriture en ajout. Au démarrage, un journal de plus de 10 Mo passe en
`.1`. Rien n'est envoyé au site.

Champs communs : `time` (UTC, `2026-09-27T10:04:05.123Z`), `source` (`armorpaint` ou `sidecar`), `pid`, `host`,
`user`, `event`. Les évènements `publish*` du sidecar portent aussi `site`, `sg_login`, `project_id`, `task_id`,
`path`, `name` et `version_number`. Puis :

| Source | Évènement | Champs |
|---|---|---|
| armorpaint | `project_open` | `path`, `sg` (contexte du projet, s'il en a un) |
| | `project_save` | `path`, `size`, `sg` |
| | `project_copy` | `path`, `from` (le projet copié), `size` : la copie publiée du projet (C4) |
| | `sg_offline` | `reason` |
| | `sg_online` | `pending` : publishes en file |
| sidecar | `publish` | `id` |
| | `publish_queued` | `key`, `id` (négatif s'il n'a pas pu être créé), `depends_on`, `error` |
| | `publish_replayed` | `key`, `id`, `queued_at` |
| | `publish_failed` | `error`, `replay` (pendant un rejeu) |
| | `publish_discarded` | `key` |

```sh
jq -c 'select(.event | startswith("publish"))' studio_audit.jsonl
```

### Scripts (minic)

Les ops `check_online`, `pending_publishes`, `replay_publishes` et `discard_publishes` sont dans le tableau des
opérations de C2.

| Fonction | Rôle |
|---|---|
| `char *studio_sg_context()` | contexte courant (objet JSON sur une ligne), `""` sans contexte |
| `int studio_sg_context_set_json(char *json)` | pose le contexte (objet avec `project_id`, `task_id`, `label`, `site`…), `""` le retire ; `1` s'il y en a un |
| `int studio_sg_offline()` | `1` si la dernière réponse de ShotGrid disait le site injoignable |
| `int studio_sg_pending()` | publishes en file d'attente |
| `int studio_sg_offline_busy()` | `1` pendant un essai demandé ou en cours, ou un rejeu |
| `void studio_sg_check_connection()` | essaie le site maintenant, comme Check Connection |
| `void studio_sg_replay_pending()` | rejoue la file, comme Replay Pending Publishes |
| `char *studio_sg_queue_dir()` | dossier de la file, `""` sans file |
| `int studio_sg_show(char *command)` | ouvre une fenêtre Toolkit comme le menu ShotGrid (`"open"`, `"save"`, `"Publish..."` ou le nom d'une commande du moteur) ; `0` si une autre est ouverte ou si ShotGrid est hors ligne |

Côté C : `studio_sg_network()` et `studio_sg_network_serial()` (état du site), `studio_sg_start_after()` (job
différé) dans `studio_sg.h` ; `studio_sg_context.h`, `studio_sg_offline.h`, `studio_audit.h`.

**Limites connues.**

- Validé sur le site simulé, la coupure simulée par le sidecar de test (connexion refusée) : DNS, proxy, site très
  lent et session expirée pendant la coupure sont à vérifier à la recette (C6).
- En ligne, pas de sonde : une coupure est découverte au prochain appel à ShotGrid.
- Windows : pas de sidecar (voir C2).

## Nommage des exports (B3)

Quand le projet ouvert est un fichier de travail ShotGrid, les textures exportées prennent le chemin du template
Toolkit des textures, rempli avec les champs du fichier : entité, étape, nom, version. Pour `robot.v003.arm`, la
texture `base` du preset va dans `…/work/armorpaint/textures/v003/robot_base.png`. Chaque version du projet a donc
ses textures, et le dossier choisi dans la fenêtre d'export est ignoré. La console l'indique à la première texture :
« Textures named by ShotGrid template … ». Le template est celui de `STUDIO_SG_TEXTURE_TEMPLATES` pour le type
d'entité, ou un autre choisi pour le projet dans la fenêtre d'export (B6).

```yaml
# core/templates.yml
keys:
    channel: {type: str}                    # nom de texture du preset d'export (base, nor, occ…)
    UDIM: {type: sequence, format_spec: "04", default: "<UDIM>"}   # default : pour le publish (C4)
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

**Presets livrés** (`assets/studio_preset_<nom>.json`, copiés dans `data/`) :
- `basic` : tous les canaux d'ArmorPaint, une carte chacun, noms génériques, pour un moteur sans preset ;
- `arnold` et `karma` : suffixes des paramètres du shader (aiStandardSurface, MaterialX Standard Surface) ;
- `cycles` : Principled BSDF, suffixes reconnus par Node Wrangler (Ctrl+Shift+T) ;
- `unreal_engine` : préfixe `T_`, PNG 8 bits, couleur en sRGB, ORM groupé, normales DirectX (`unreal` est déjà un
  preset d'ArmorPaint).

Sauf `unreal_engine`, les cartes de couleur sont en EXR half linéaire, les données brutes, la hauteur en float
32 bits et les normales OpenGL. `STUDIO_EXPORT_PRESETS` en ajoute ou en remplace : le nom du preset est
celui du fichier, et le premier trouvé l'emporte.

**Lecture seule.** Les presets studio sont seulement lus : rien n'est installé ni écrit à côté de l'app (au studio,
son dossier n'est pas inscriptible). Les dossiers sont relus à l'ouverture de la fenêtre d'export et à chaque export :
une modification prend effet sans relancer ArmorPaint. Un preset invalide est signalé, avec la clé en faute
(`textures[2].bits: …`). La ligne de commande (`--export-textures <type> <preset>`) et Ctrl+E (refaire le dernier
export) les trouvent par leur nom. Les presets propres à un projet vont dans son sidecar : voir « Fenêtre d'export
(B6) ».

### Format

```json
{
	"version": 1,
	"description": "Lookdev maison",
	"naming": "<file>[_<variant>]_<channel>[.<UDIM>].<ext>",
	"resolutions": [2048, "4096x4096"],
	"overwrite": "warn",
	"manifest": true,
	"defaults": { "format": "exr", "bits": 16, "color_space": "raw", "resolution": 0,
	              "compression": { "exr": "zip", "tiff": "lzw", "png": "default" } },
	"textures": [
		{ "name": "base", "suffix": "BaseColor", "channels": ["base_r", "base_g", "base_b"], "color_space": "linear" },
		{ "name": "orm", "channels": ["occ", "rough", "metal"], "format": "png", "bits": 16, "resolution": 1024 },
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
| `naming` | variables (sans égard à la casse) : `<file>` (fichier du projet, `robot_v003`), `<name>` (sans sa version, `robot`), `<version>` (`003`, vide sans version), `<variant>` (objet ou jeu exporté à part), `<object>` (Per Object, ou l'objet du calque en Selected, le jeu d'un calque limité à un jeu), `<atlas>` (jeu de textures, en Visible et en Per UDIM Tile), `<channel>` (`suffix`, sinon `name`), `<UDIM>`, `<resolution>` (largeur du fichier), `<bits>`, `<preset>`, `<ext>`. Une variable inconnue est refusée. Une section `[…]` disparaît si une variable qu'elle contient est vide. Un chemin relatif part du dossier choisi ; des sous-dossiers sont permis |
| `resolution` | côté le plus long du fichier, proportions gardées (1 à 65536) ; `0` : celle du projet (défaut). Par texture ou dans `defaults`. Réduire moyenne les pixels par zone ; agrandir est bilinéaire et donne une note `upscaled` |
| `resolutions` | résolutions admises par le show, vérifiées pour chaque texture à sa taille de sortie : une autre est bloquante |
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
   | nommage impossible (template ShotGrid), aucun calque visible, aucun fichier, aucun fichier coché (B6), résolution hors show, chemins en double, fichier existant (`block`), dossier non inscriptible, espace disque insuffisant, PNG > 2 Gio, TIFF non compressé > 4 Gio, espace OCIO inconnu ou OCIO indisponible (A3) | maillage UDIM exporté hors mode tuile, tuiles UDIM manquantes, fichier existant (`warn`), marge disque faible, TIFF compressé > 4 Gio |

   Les **notes** (niveau `note`) décrivent des choix du preset, pas des erreurs : calques masqués ignorés, hauteur en
   8 bits, données en JPG, EXR `srgb` ou non linéaire, couleur linéaire en 8 bits, données converties vers un espace
   couleur et inversement (A3). Elles vont dans le rapport et le manifeste, pas à l'écran. Une profondeur supérieure
   à celle des calques n'est plus signalée.

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
- le preset (nom, chemin, SHA-256, `overrides` : bits et résolution changés dans la fenêtre, sinon `null`) ;
- la résolution, la profondeur des calques, le mode et le template ShotGrid ;
- pour chaque fichier : chemin relatif, texture, variante, tuile, `resolution` `[l, h]`, format, profondeur,
  compression, espace, canaux, taille, SHA-256 ;
- la provenance OCIO si une texture en dépend (A3) ;
- les avertissements.

PNG et TIFF sont écrits par `studio_image.c` : filtres adaptatifs, deflate et LZW parallèles, prédicteur TIFF
(flottant pour 32 bits). L'EXR passe par tinyexr (B1), et le JPG par stb. Le mode « pack into project » garde
l'écriture upstream.

### Fenêtre d'export (B6)

File > Export > Textures, l'icône d'export de la barre d'outils, Export du menu d'un calque et les raccourcis ouvrent
une fenêtre Qt à la place de la boîte d'export d'ArmorPaint (patch `export-window`). C'est un processus Python à part
(`assets/studio_export_window.py`, PySide6) : l'environnement Python est fourni par le déploiement (rez).

- **Preset** : les presets studio seulement : ceux du projet (sidecar) d'abord, puis ceux de `STUDIO_EXPORT_PRESETS`,
  puis les presets livrés. « Save as preset… » enregistre les réglages en cours comme preset du projet (vérifié par
  ArmorPaint avant d'être gardé) ; un nom de preset studio est refusé.
- **Color space** : « From preset », ou un espace OCIO imposé aux textures de couleur ; les données restent brutes.
- **Layers** : Visible, Selected, Per Object, Per UDIM Tile, et la marge (padding) de l'upstream.
- **Destination** : dossier ; défaut : celui du dernier export, sinon celui du projet.
- **ShotGrid** (fichier de travail dans un contexte, nommage actif : voir « Nommage des exports (B3) ») : le template
  des textures, parmi ceux de la config qui ont les clés du canal et de l'extension et se remplissent avec ce fichier
  (celui du réglage en premier), la version du fichier et « Version Up ». Le template choisi est gardé pour le projet
  dans son sidecar (`texture_template`, retiré si c'est celui du réglage) : l'export, le `--background` et le publish
  (op `texture_pattern`) le suivent. « Version Up » enregistre le projet en version suivante (op `next_version`, comme
  le Save de workfiles, avec le journal d'audit et le titre) ; les noms suivent la nouvelle version. Export est grisé
  pendant la demande au sidecar ; une erreur s'affiche sous la ligne.
- **File names** : le modèle de nom du preset (`naming`), modifiable pour cet export. « Variables » insère une
  variable au curseur (liste ci-dessus, avec leur description) ou met la sélection entre `[ ]` (section retirée si une
  de ses variables est vide). Sous le champ, un nom de fichier de cet export ; une variable inconnue est signalée tout
  de suite et bloque l'export. Le modèle est gardé par preset dans le sidecar et fait partie d'un « Save as
  preset » ; une map qui a son propre `naming` dans le preset le garde. Quand un template ShotGrid nomme les
  textures (B3), le champ montre le template et reste figé.
- **Texture sets** (projet à plusieurs objets) : les jeux de textures, les « atlas » d'ArmorPaint. Chaque objet
  (les tuiles UDIM d'un objet ensemble) est dans un jeu ; « New set… » en crée un, « Rename… » en renomme un, « One
  set » les réunit. En Visible, chaque jeu utilisé devient un jeu de maps : calques partagés, calques des objets du
  jeu et calques limités au jeu, nommé par `<atlas>` (et `<variant>`). Les jeux changent dans le projet d'ArmorPaint
  et sont enregistrés avec le `.arm`, pas dans le sidecar. Un calque limité à un jeu y reste quand le jeu est
  renommé ; vider un tel jeu de ses objets est refusé. Les objets gardent leurs UV : un jeu ne réagence rien, il
  choisit quels calques vont dans quels fichiers. En Per UDIM Tile, chaque jeu donne ses maps pour chacune de ses
  tuiles seulement (`<atlas>` et `<UDIM>`), avec une note dans la section ; les jeux d'une tuile y partagent sa case
  de la grille (voir **UDIM**).
- **Maps** : une ligne par texture du preset, avec une case (map désactivée : aucun de ses fichiers n'est écrit), sa
  profondeur (les valeurs permises par son format) et sa résolution (celle du projet, ou 8192 à 256), puis la taille
  de sortie. « Set all bits » et « Set all resolutions » les règlent pour toutes les maps (une map dont le format n'a
  pas cette profondeur prend la plus proche en dessous) ; une valeur changée est en bleu, « Reset » rend celles du preset. Ces réglages ne changent
  pas le preset : ils valent pour l'export, le temps de s'en faire un.
- **Files** : le préflight, recalculé à chaque changement, sans rien écrire. Avec plusieurs jeux (tuiles UDIM, objets
  ou atlas), chaque fichier se coche ou se décoche avant l'export ; un fichier décoché n'est ni vérifié ni écrit. La
  liste (sans les maps désactivées) est groupée par map ou par objet / tuile, avec une case par groupe (décocher `height` retire toutes ses
  tuiles). En tête, ce que représentent les fichiers (« 9 maps × 7 UDIM tiles », ou avec des jeux en Per UDIM Tile
  « 9 maps × 6 texture set tiles (2 sets, 6 UDIM tiles) ») ; la ligne du projet donne ses objets
  et ses tuiles. Un fichier existant est en orange et compté dans le résumé.
- **Problèmes** : les bloquants (qui désactivent Export) et quelques avertissements utiles (tuiles UDIM manquantes,
  maillage UDIM hors mode par tuile, place disque). Les choix du preset que les graphistes connaissent (profondeur,
  8 bits, JPG, espace) sont des **notes** : dans le rapport et le manifeste, jamais affichées.

Un export abouti devient l'export courant d'ArmorPaint (dossier, preset, mode, marge, espace, fichiers décochés) :
Ctrl+E le refait
sans rouvrir la fenêtre. Cancel ne change rien. Rouvrir la fenêtre pendant qu'elle est ouverte la ramène devant. Elle
se ferme quand ArmorPaint quitte. Sa taille suit l'échelle de l'interface d'ArmorPaint (`STUDIO_UI_SCALE`, transmis
par l'app). Si elle ne démarre pas (Python sans PySide6), ArmorPaint le dit, avec la fin de la sortie d'erreur.

**Sidecar du projet.** `<dossier du .arm>/<nom sans .arm ni version>.export_settings.json`, commun aux versions
(`robot_v003.arm` → `robot.export_settings.json`). La fenêtre seule le lit et l'écrit, après un export abouti, un
« Save as preset » et à sa fermeture ; rien pour un projet jamais enregistré. Un sidecar illisible est signalé dans le
log et jamais écrasé.

```json
{
	"version": 1,
	"texture_template": "armorpaint_asset_textures_publish",
	"presets": { "robot_hero": { "description": "…", "textures": [ … ] } },
	"export": {
		"preset": "project:robot_hero", "color_space": "", "mode": 3, "padding": false, "dir": "/show/…", "group": "map",
		"presets": { "file:karma": { "overrides": { "normal": { "bits": 32 } }, "disabled": ["opacity"],
		                             "skip": [["height", "", "1002"]], "naming": "<name>_<atlas>_<channel>.<ext>" } }
	}
}
```

`presets` a le format des presets studio. `texture_template` : le template ShotGrid des textures choisi pour le
projet (absent : celui du réglage). `export` garde les choix, remis à la réouverture ; par preset (`file:<nom>`
pour ceux des dossiers, `project:<nom>` pour ceux du sidecar), les réglages des maps, les maps désactivées, les
fichiers décochés et le modèle de nom s'il diffère de celui du preset. Un preset du projet est envoyé en JSON à ArmorPaint à chaque export et noté au manifeste avec le
chemin du sidecar.

**Un jeu de maps par matériau.** À l'import OBJ ou FBX, « Split By: Material » crée un objet par matériau. En mode
Per Object, ArmorPaint écrit un jeu par objet qui a au moins un calque qui lui est propre (masque d'objet du calque) ;
les calques partagés vont dans chaque jeu. Sans calque propre à un objet, Per Object donne un seul jeu. Le nom du
matériau est alors le nom de l'objet : `<object>` (ArmorPaint ne garde pas d'affectation de shader par objet). Autre
voie : un jeu de textures par matériau (Texture sets), export Visible, `<atlas>`. Avec des UDIM : « Material » et
« UDIM Tiles » ensemble (voir **UDIM**).

**UDIM.** À l'import, la case « UDIM Tiles » (à côté de « Split By: Object / Material ») fait un objet par objet du
fichier et par tuile (`body.1001`, `body.1002`…) ; avec « Material », un objet par matériau et par tuile (`skin.1001`…,
rassemblé d'un objet du fichier à l'autre) et un jeu de textures par matériau (patches udim-import et fbx-split). Un
triangle à cheval sur deux tuiles est coupé le long de la frontière.
Un fichier dont une uv sort des tuiles 1001-1990 (uv négative, u ≥ 10) ou qui a plus de 64 tuiles est refusé avec un
message, et le projet garde son maillage. Un objet sans uv est importé sans tuile, avec un message : les calques UDIM
ne le peignent pas. ArmorPaint peint toutes les tuiles dans chaque texture de calque, rangées en grille (côté : racine
carrée du nombre de tuiles arrondie au-dessus, 8 au plus, soit 64 tuiles) : une tuile a la résolution du projet divisée
par ce côté (projet 4096, 6 tuiles → grille 3 × 3 → 1365 pixels par tuile). Après l'import, la barre d'état le dit,
avec la résolution de projet qu'il faut pour 2048 et 4096 pixels par tuile et la mémoire d'un calque
(`studio_udim.c` ; dans la console seule si l'import a affiché un avertissement). Per UDIM Tile découpe la tuile et la
remet à la résolution de la map : les fichiers ont la taille demandée (`<resolution>`), pas plus de détail. En
Visible, un maillage UDIM donne la grille entière (avertissement du préflight). Au-delà de 64 tuiles (objets
`nom.1xxx` ajoutés ou renommés), les suivantes ne sont ni peintes ni exportées : message, et le préflight bloque
l'export studio (patch udim-tile-limit).

Les matériaux lisent les uv UDIM du fichier : un bruit procédural ou un dégradé est continu d'une tuile à sa voisine,
comme dans le DCC, sur un calque partagé comme sur le calque d'une tuile (patch udim-material-uv ; en vue 2D, ils
gardent l'espace de la case). Per UDIM Tile avec des jeux de textures : les maps de chaque jeu pour ses tuiles
(`<fichier>_<jeu>_<canal>.<tuile>` à l'export upstream, `<atlas>` et `<UDIM>` dans un preset studio ; patch
udim-export-sets). Renommer un objet dans l'onglet Meshes garde sa tuile : le champ ne montre que le nom devant
`.1011`, et `torso` tapé pour `skin.1011` donne `torso.1011` (patch udim-tile-name).

**Attention, cases de la grille** (L17 de `ARMORPAINT_UDIM.md`, corrigée au lot D2). La case d'une tuile dépend de la
liste des tuiles du projet. Ajouter un maillage qui apporte une tuile, supprimer le dernier objet d'une tuile ou passer
de 4 à 5 tuiles (grille 2 × 2 → 3 × 3) déplace les cases : la peinture déjà faite sur les calques partagés passe à une
autre tuile. Importer toutes les tuiles avant de peindre.

L'onglet Presets (éditeur de presets upstream) et « Pack into project » de la boîte d'export ne sont plus
accessibles ; l'onglet Atlases est remplacé par la section Texture sets. La résolution du projet (celle des calques) ne change pas : chaque map est mise à sa résolution
à l'export. Pas encore sous Windows (`studio_proc`).

**Protocole**, une ligne JSON par message sur stdin et stdout de la fenêtre (détail dans `studio_export_window.h`) :
requêtes `state` (`args.presets` : presets du sidecar à vérifier ; la réponse donne aussi `atlases`,
`naming_template`, `naming_variables` et `sg_naming` : `{template, default_template, chosen, templates, name, version,
error}` sous un template ShotGrid, sinon null), `naming` (`args.refresh` : redemandé au sidecar ; réponse :
`sg_naming`), `version_up` (réponse `{path, sg_naming}`), `inspect` (un preset JSON), `atlases` (`{"names", "objects"}` : jeux de
textures du projet), `preflight` et `export` (`args` : `preset`, `preset_json` et `preset_source` pour un preset du
projet, `overrides`, `naming`, `color_space`, `dir`, `mode`, `padding`, `skip`), réponses
`{"id", "ok", "data" | "error"}`, évènement `{"event": "raise", "mode"}`. Un autre outil peut remplacer la fenêtre
par `STUDIO_EXPORT_WINDOW`. `STUDIO_EXPORT_WINDOW_TEST=<choix.json>` la fait tourner sans intervention (bancs).

L'espace imposé est noté `color_space_override` dans le manifeste (`preset`) et dans le rapport du préflight, le
modèle de nom de l'export dans `naming` (`null` : celui du preset).

### Scripts (minic)

```c
int   studio_export_run(char *dir, char *preset, int mode);        // 1 si écrit ; mode : 0 visible, 1 sélection, 2 par objet, 3 par tuile, -1 inchangé
char *studio_export_preflight(char *dir, char *preset, int mode);  // rapport JSON, sans rien écrire
char *studio_export_last_report(void);                              // rapport JSON du dernier export
int   studio_export_is_studio_preset(char *name);
char *studio_export_color_space(void);              // espace imposé aux couleurs ; "" : celui du preset
int   studio_export_set_color_space(char *name);     // "" : celui du preset ; 0 si inconnu d'OCIO (gardé, l'export sera bloqué)
char *studio_export_skip(void);                      // fichiers décochés : lignes "<texture>\t<variant>\t<tuile>"
void  studio_export_set_skip(char *keys);            // "" : tous les fichiers ; "<texture>\t*\t*" : toute la map ; gardé pour les exports suivants
void  studio_export_set_preset(char *name, char *json, char *source); // preset courant (Ctrl+E) ; json : preset hors des dossiers, source : son fichier
void  studio_export_set_overrides(char *json);       // {"<texture>": {"bits": 16, "resolution": 1024}} pour ce preset ; "" : ceux du preset
void  studio_export_set_naming(char *pattern);       // modèle de nom pour ce preset (pour tous sans preset choisi) ; "" : celui du preset
char *studio_atlases(void);                          // {"names", "objects": [{"name", "atlas"}], "layers": [{"layer", "atlas"}]}
char *studio_set_atlases(char *json);                // {"names": [...], "objects": [jeu de chaque objet]} ; "" si fait, sinon la raison
int   studio_layer_set_atlas(int atlas);             // limite le calque courant à un jeu ; -1 : partagé
int   studio_layer_set_object(int index);            // limite le calque courant à un objet (ordre de studio_atlases) ; -1 : partagé
int   studio_layer_new(void);                        // nouveau calque de peinture vide, au-dessus du courant, devenu courant
char *studio_object_rename(int index, char *name);   // comme l'onglet Meshes : le suffixe de tuile UDIM reste (body.1001 → torso.1001) ; nouveau nom
int   studio_import_mesh(char *path, int split);     // remplace le maillage ; split : 0 objet, 1 matériau, 2 tuile UDIM, 3 matériau et tuile UDIM ; fini au retour
int   studio_meshes_sort(void);                      // Edit > Sort de l'onglet Meshes : objets triés par nom
int   studio_mesh_set_parent(char *child, char *parent); // champ Parent de l'onglet Meshes ; "" : aucun ; la liste est réordonnée
char *studio_layer_masks(void);                      // masques d'objet des calques : "Layer 1=0 Layer 2=5 filter=0" (bancs)
char *studio_dialog_title(void);                     // titre de la boîte ouverte (« Unwrap Mesh »...), "" sans boîte
char *studio_status_message(void);                   // message de la barre d'état, "" s'il est effacé
void  studio_export_window_show(void);               // ouvre la fenêtre d'export, ou la ramène devant
int   studio_export_window_is_open(void);
```

Le rapport JSON contient `ok`, `preset`, `color_space_override`, `destination`, la résolution, `total_size`, `manifest`, `files[]` (chemin,
texture, `variant`, `udim`, `width`, `height`, format, profondeur, compression, taille estimée, `exists`, `skip`) et `issues[]` (`level` :
`blocking`, `warning` ou `note` ; `code`, `message`). En
ligne de commande, `ArmorPaint projet.arm --background --export-textures exr16 arnold <dossier>` utilise le preset
studio. Chaque texture garde son propre format. Le type fixe seulement la profondeur de travail des calques
aplatis (`png`/`jpg` : 8 bits, `exr16` : half, `exr32` : float), comme dans l'upstream. Pour une sortie 16 ou
32 bits utile, prendre `exr16` ou `exr32` ; le préflight avertit sinon.

Point d'injection upstream : `STUDIO-PATCH(studio-export)` (PATCHES.md). Mode, nom des fichiers et espace OCIO en
ligne de commande, rapport et code de retour : voir « Mode batch (B7) ».

## Mode batch (B7)

`ArmorPaint --background` tourne sans fenêtre (patch `headless`) : aucune fenêtre à l'écran, pas d'icône dans le Dock,
le focus n'est pas pris. Sous macOS ; les backends Linux et Windows savaient déjà garder la fenêtre d'iron cachée,
`--background` la leur demande désormais aussi (non testé). Un GPU reste nécessaire (Metal). Plusieurs processus peuvent tourner en parallèle.

Options du studio (l'upstream ignore les arguments qu'il ne connaît pas) :

| Option | Effet |
|---|---|
| `--report <fichier.json>` | rapport du run, voir plus bas ; écrit dès le démarrage (`"status": "running"`) puis à la fin |
| `--export-mode <mode>` | mode de `--export-textures` : `visible` (défaut, celui de l'upstream), `selected`, `per-object`, `per-tile` (alias `udim`) |
| `--export-naming <modèle>` | nom des fichiers (variables de `naming`, voir « Format »), à la place de celui du preset ; preset studio seulement |
| `--export-color-space <espace>` | espace OCIO des textures de couleur, comme `STUDIO_EXPORT_COLOR_SPACE` ; preset studio seulement |

```sh
ArmorPaint robot_v003.arm --background --export-textures exr16 arnold /shows/demo/robot/textures \
    --export-mode per-tile --export-naming "<name>_v<version>_<channel>.<UDIM>.<ext>" --report /tmp/robot.json
echo $?   # 0, 1 ou 2
```

**Code de retour** d'un run `--background` :
- `0` : tout a abouti ;
- `1` : une erreur pendant le run : message d'erreur d'ArmorPaint (`console_error` : fichier introuvable, texture
  manquante d'un `.arm`…), erreur de script minic (`<script>:<ligne>: error: …`), export bloqué par le préflight ou en
  échec, export sans fichier écrit, fichier annoncé mais absent ;
- `2` : ligne de commande fausse, rien n'est lancé (sortie dès le démarrage) : type, dossier ou preset de
  `--export-textures` (l'upstream prenait en silence le premier preset, ou n'écrivait rien ; un preset studio crée le
  dossier qui manque, seul un chemin qui n'est pas un dossier est refusé), `--export-textures`
  incomplet, option du studio sans valeur, mode inconnu, modèle de nom ou espace refusé, options `--export-*` sans
  `--export-textures`, `--export-naming` ou `--export-color-space` avec un preset d'ArmorPaint, script ou dossier du
  rapport introuvable ;
- après un plantage, le code du signal ; le rapport reste `"running"`.

**Rapport** (écrit à côté puis renommé : jamais lu à moitié) :

```json
{
  "version": 1, "status": "ok", "exit_code": 0, "studio_version": "0.8.0", "pid": 4242,
  "started": "2026-09-27T10:09:51Z", "duration_s": 0.51, "argv": ["…"],
  "exports": [
    {"dir": "…/textures", "preset": "arnold", "studio_preset": true, "mode": "per-tile", "destination": "disk",
     "preflight_only": false, "ok": true, "errors": 0, "manifest": "…/robot_v003.export.json",
     "files": ["…/robot_v003_baseColor.1001.exr", "…"]}
  ],
  "outputs": {"version": "13"},
  "errors": [],
  "log_dropped": 0,
  "log": [{"level": "info", "message": "Textures exported with arnold: 54 files, 1.2 GB, manifest …"}]
}
```

`exports` : chaque export du run (`--export-textures` et ceux des scripts) et ses fichiers : relevés au moment de
l'écriture pour un preset d'ArmorPaint (patch `batch-report`), liste exacte de l'écriture atomique pour un preset
studio (manifeste à part). `ok` vaut `null` pour un export pas fini. `errors` : messages d'erreur (200 au plus) ;
`log` : messages de la console d'ArmorPaint (2000 au plus, `log_dropped` compte les suivants).

Dans un script (`--background --script`), `console_error("…")` fait échouer le run, et :

```c
void studio_batch_output(char *key, char *value); // "outputs" du rapport : ce que le script rend à l'appelant
int  studio_batch_exit_code(void);                // code de retour si le run s'arrêtait maintenant (0 ou 1)
```

Hors `--background`, les options `--export-*` valent pour l'export de `--export-textures` (une ligne fausse est
signalée au journal et les options ignorées) ; ni rapport ni code de retour.

**Limites.** Un GPU reste nécessaire (sur ferme : nœuds équipés ; Linux sans écran : X virtuel, non testé). Les
fichiers de `--export-mesh` et `--export-material` ne sont pas dans le rapport. Une erreur signalée par `iron_log`
seul, hors console, ne compte pas : celles de `--export-textures` et `--script` sont vérifiées au démarrage.

## Gestion couleur OCIO (A0 à A6)

**Décision A0 : le vrai OpenColorIO**, pas une réimplémentation. Version épinglée dans `.ocio/CMakeLists.txt`
(`STUDIO_OCIO_TAG`, aujourd'hui `v2.5.2`, VFX Reference Platform CY2026), construite depuis ses sources avec ses
dépendances (expat, yaml-cpp, pystring, Imath, zlib, minizip-ng) par `tools/build_ocio.sh`. Premier build : 2 à
3 minutes, réseau requis. Les produits sont ignorés par git (`.ocio/build/`, `.ocio/lib/`) : relancer le script
sur une machine neuve, puis rebuild d'ArmorPaint.

OCIO est en C++ et amake ne compile que du C : le shim `.ocio/studio_ocio_lib.cpp` expose une ABI C
(`studio_ocio_lib.h`, 27 fonctions, version `STUDIO_OCIO_LIB_ABI`). Aucune exception ne la traverse : chaque
erreur devient un retour NULL/0/-1 et un message (`studio_ocio_lib_error()`, par thread).

- **macOS : liaison statique.** `build_ocio.sh` fusionne le shim, OCIO et ses dépendances en un seul objet
  (`ld -r`) où seuls les `studio_ocio_lib_*` restent visibles : aucune collision possible avec l'app. L'archive
  `.ocio/lib/libstudio_ocio.a` est liée par `.project/project.c` (define `WITH_STUDIO_OCIO_STATIC`). Pas de
  dlopen : ArmorPaint est signé ad hoc avec le hardened runtime, dont la validation des bibliothèques refuse une
  dylib d'une autre « équipe » (« different Team IDs »). Coût : environ 7 Mo sur le binaire.
- **Linux : dlopen** de `data/libstudio_ocio.so` (copié dans `assets/` par le script). Écrit, non testé.
- **Windows : à faire.**
- Sans bibliothèque, ArmorPaint se construit et démarre sans OCIO ; `studio_ocio_status()` dit pourquoi.

**Configuration**, dans l'ordre : chemin explicite (`studio_ocio_reload`, scripts), `$OCIO`, la préférence
(« Réglages (A5) »), puis la configuration studio intégrée à OCIO (`ocio://studio-config-latest`, ACES 2.0).
`$OCIO` passe avant la préférence : c'est le lanceur du show qui la choisit. Un `$OCIO` ou une préférence invalides
sont une erreur, sans repli : un repli cacherait au studio que sa configuration n'est pas chargée. Le log de
démarrage l'affiche :

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
lui, les conversions sont bloquées et le statut dit pourquoi. Il se choisit aussi dans les préférences et le projet
le garde : voir « Réglages (A5) ».

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
filmique et le statut dit pourquoi. Le choix (case, display, view, look) est gardé dans les préférences studio
d'une session à l'autre ; `STUDIO_OCIO_VIEWPORT` et `STUDIO_OCIO_DISPLAY`/`_VIEW`/`_LOOK` l'emportent.

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
d'une LUT lisse. Dans l'app, le compositeur (GPU) est à 0,14/255 d'OCIO sur les couleurs sondées. La LUT est plus
précise qu'une LUT d'affichage baked par `ociobakelut` (voir « Validation (A6) ») ; traduire le shader GPU d'OCIO en
kong reste une piste, si un show en a besoin.

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

### Textures importées (A4)

Chaque texture importée a un **espace source** OCIO. Ses pixels sont convertis vers l'espace de travail au
décodage, avant l'envoi au GPU (patch `ocio-import`), et rangés comme ArmorPaint les attend. En 8 bits, ils restent
encodés gamma 2.2, la convention des couleurs d'ArmorPaint : décodés depuis l'espace source, convertis, réencodés et
arrondis au code le plus proche. En flottant (EXR, HDR), ils restent linéaires. Un espace de donnée (`Raw`) n'est
pas converti ; l'alpha ne passe jamais par OCIO. La conversion tourne sur tous les cœurs, par bandes de 65 536
pixels.

**Espace source**, dans l'ordre :

1. le choix enregistré pour la texture : celui de l'artiste, ou l'espace retenu à son premier import. Un projet
   rouvert est décodé comme il l'a été, même si la configuration a changé ses règles entre-temps ;
2. `.k` (format d'ArmorPaint) : pas de conversion ;
3. un nom de donnée : les étiquettes de l'import de dossier d'ArmorPaint (`normal`, `nor`, `ao`, `rough`,
   `metallic`, `height`, `opacity`…, `iron_path.c`) et `mask`, sur le nom sans extension ni numéro de tuile UDIM :
   l'espace de donnée de la configuration (rôle `data`, `Raw`). Une étiquette de couleur de base l'emporte
   (`wood_basecolor_normal` reste une couleur) ;
4. une `file_rule` de la configuration autre que `Default` : dans les configurations studio et CG, un `.exr` est
   en `ACES2065-1` ;
5. le défaut du format : 8 bits → `sRGB Encoded Rec.709 (sRGB)`, l'équivalent de `srgb_tx` trouvé comme l'espace
   de travail (le rôle `texture_paint` le désigne aussi dans ces configurations, mais une configuration de show peut
   le mettre ailleurs : il n'est pas lu) ; `.hdr` → `Linear Rec.709 (sRGB)` ; autre flottant → `scene_linear`
   (`ACEScg`).

La règle `Default` n'est pas suivie : dans les configurations studio et CG, elle désigne `sRGB - Display`, un espace
d'affichage qu'OCIO convertirait par la vue inverse (ACES 2.0), pas par la courbe sRGB. Le log le dit pour chaque
texture :

```
studio: texture /shows/demo/tex/wood_normal.png: Raw (name), data, not converted
```

**Choisir l'espace.** Clic droit sur une texture de l'onglet Textures, « Color Space... », ou menu Color, « Texture
Color Spaces... » pour toutes les textures du projet. Chaque ligne donne l'espace retenu et sa source (`chosen`,
`data name`, `file rule`, `default` ; « not converted » sans conversion) et un menu : « Automatic » (l'ordre
ci-dessus) ou un espace de la configuration. La texture est rechargée depuis son fichier et convertie avec son
nouvel espace ; environnement, matériaux et calques de remplissage suivent. L'infobulle d'une texture dans l'onglet
ajoute son espace et sa source. Le choix se fait par texture et non par nœud : ce sont les pixels qui sont convertis
au chargement.

**Nœud Image Texture.** Son option d'espace (Auto, Linear, sRGB) n'est qu'une conversion gamma 2.2 d'ArmorPaint,
sans OCIO. Elle convertissait à l'envers : Linear sur la couleur de base décodait au lieu d'encoder, sRGB sur une
autre entrée encodait au lieu de décoder (patch `image-texture-color-space`). Corrigée, elle change le rendu des
projets qui la réglaient à Linear ou sRGB. Auto sur une texture qui garde des couleurs linéaires (flottante, d'un
espace couleur) vaut désormais Linear : un EXR de couleur de base n'a plus besoin de l'option.

**Dans le `.arm`.** Les espaces voyagent dans le projet, dans l'entrée `studio_color_spaces.json` du paquet
d'assets, qu'ArmorPaint sans studio garde telle quelle. Chemins relatifs au `.arm`, comme ses textures :

```json
{
	"version": 1,
	"config": "studio-config-v4.0.0_aces-v2.0_ocio-v2.5",
	"working_space": "Linear Rec.709 (sRGB)",
	"textures": [
		{"file": "ramp.png", "color_space": "ACEScg", "source": "user", "float": false, "linear": false}
	]
}
```

Rouvert avec une autre configuration, le projet le signale dans la console ; un espace absent de la configuration
repasse en automatique, signalé aussi.

**Pack Assets** (menu File). Un projet enregistré avec ses assets relit ses textures du paquet, dont les pixels sont
déjà convertis : elles ne sont pas converties une seconde fois. Sans le fichier sur le disque, leur espace ne peut
plus changer (« packed » dans la fenêtre, réimporter le fichier) ; avec lui, changer l'espace retire la copie du
paquet, que le prochain enregistrement avec Pack Assets refait. Le paquet d'ArmorPaint redessine chaque texture en
RGBA 8 bits : une texture flottante y perd sa précision (upstream). Ce chemin débordait le tas à l'encodage des PNG,
patch `encode-image-overflow`.

**Désactiver** : `STUDIO_OCIO_IMPORT=0` au lancement. Pas de conversion, et l'entrée du `.arm` n'est ni lue ni
réécrite (celle d'un projet ouvert est gardée telle quelle).

**Non couvert.**
- Les couleurs hors de [0, 1] après conversion sont bornées en 8 bits (une couleur ACEScg saturée hors du gamut
  Rec.709, par exemple) ; en half, bornées à ±65 504 ; une valeur non finie devient 0.
- Les calques d'un PSD prennent l'espace du fichier à l'import ; leur espace ne se change pas ensuite (pas de
  fichier à relire).
- Les textures des matériaux ou brosses importés d'un autre `.arm` sont résolues comme un import : les choix
  enregistrés dans cet autre `.arm` ne sont pas lus.
- Une texture déjà dans le cache d'ArmorPaint (capture du viewport) n'est pas décodée à nouveau : ses pixels
  restent tels quels. Les vignettes du navigateur de fichiers ne sont pas converties.

### Réglages (A5)

**Préférences studio.** Les réglages couleur sont gardés d'une session à l'autre dans `studio_prefs.json`, à côté du
`config.json` d'ArmorPaint (dossier `data/` de l'app, ou dossier de sauvegarde du système quand l'app est dans un
dossier protégé ou en lecture seule : `~/Library/Application Support/ArmorPaint/` sur macOS), ou dans le fichier
`$STUDIO_PREFS`. Un fichier à part : `config_save()` d'ArmorPaint réécrit
`config.json` champ par champ et y perdrait les clés du studio. Objet JSON plat, clés inconnues gardées (celles d'une
autre version du studio), écrit à chaque changement par un fichier temporaire renommé. Illisible, il est journalisé,
ignoré, et mis de côté en `.bak` à la première écriture. En `--background`, il n'est ni lu ni écrit sans
`$STUDIO_PREFS` : un script ne dépend pas des réglages de l'artiste et ne les change pas.

| Clé | Réglage | Variable qui l'emporte |
|---|---|---|
| `ocio.config` | configuration OCIO (fichier `.ocio`, archive `.ocioz`, URI `ocio://`) ; absente : l'intégrée | `OCIO` |
| `ocio.working_space` | espace de travail des nouveaux projets (nom, alias ou rôle) ; absente : l'automatique | `STUDIO_OCIO_WORKING_SPACE` |
| `ocio.viewport` | viewport par OCIO (`true`) ou filmique | `STUDIO_OCIO_VIEWPORT` |
| `ocio.display`, `ocio.view`, `ocio.look` | dernier choix du menu Color | `STUDIO_OCIO_DISPLAY`, `_VIEW`, `_LOOK` |

**Preferences > Color** (onglet ajouté par le patch `preferences-tab` ; menu Color, « Color Preferences... ») :
configuration (champ, Browse, Built-in pour revenir à l'intégrée) et son statut ; espace de travail des nouveaux
projets (« Automatic » ou un espace de la configuration) ; espace du projet courant et son origine ; chemin du
fichier des préférences. Avec `$OCIO`, le champ est grisé : la préférence vaut pour les sessions sans lui.

**Dans le projet.** L'espace de travail voyage dans l'entrée `studio_ocio.json` du paquet d'assets du `.arm`, avec la
configuration qui l'a donné. ArmorPaint sans studio garde l'entrée telle quelle. Les entrées sont écrites juste avant
l'encodage du projet (patch `project-entries`, module `studio_project`) :

```json
{
	"version": 1,
	"config": "studio-config-v4.0.0_aces-v2.0_ocio-v2.5",
	"config_source": "ocio://studio-config-latest",
	"working_space": "Linear Rec.709 (sRGB)",
	"working_space_auto": true
}
```

- Un nouveau projet prend `$STUDIO_OCIO_WORKING_SPACE`, sinon la préférence, sinon l'automatique, jusqu'à son
  enregistrement, qui fige l'espace trouvé : rouvert, il garde cet espace, quels que soient la préférence et
  l'environnement.
- `"working_space_auto": true` : l'automatique, cherché à nouveau dans la configuration de la session.
- Sans entrée (projet d'avant A5, ou enregistré par ArmorPaint sans studio) : l'automatique, les couleurs
  d'ArmorPaint.
- Rouvert avec une autre configuration : signalé dans la console.
- Un espace nommé que la configuration ne connaît pas : erreur, sans repli ; les conversions sont bloquées jusqu'à
  un autre choix.

**Menu Color, Working Space** : l'espace du projet courant, « Automatic » ou un espace de la configuration, gardé au
prochain enregistrement. Un espace de travail autre que l'automatique (ACEScg, par exemple) change le sens de toutes
les valeurs d'ArmorPaint : le sélecteur de couleur et les couleurs des nœuds ne passent pas par OCIO, leurs valeurs
sont lues dans cet espace.

**Changement en cours de session** (configuration ou espace de travail) : les textures converties sont rechargées
depuis leurs fichiers et converties à nouveau, chacune avec son espace source ; celles qui ne sont que dans le paquet
d'assets gardent leurs pixels (signalé). Viewport et exports suivent. Les calques ne sont pas convertis : ce qui est
peint garde ses valeurs.

**Au démarrage**, hors `--background`, une boîte « Color Management » signale OCIO indisponible (configuration
refusée, bibliothèque absente) ou un espace de travail introuvable, et dit où le corriger.

**Non couvert.**
- Les calques ne sont pas convertis quand l'espace de travail change : ArmorPaint n'a pas de conversion de calque.
- Les couleurs de l'interface et du sélecteur ne passent pas par OCIO : ce sont les valeurs de l'espace de travail,
  affichées telles quelles.
- Deux instances d'ArmorPaint écrivent le même fichier de préférences sans verrou : la dernière écriture l'emporte.

### Validation (A6)

`.tests/ocio_validation_app.sh` fait passer des mires par toute la chaîne (import, calque, export) et compare les
fichiers écrits à des outils qui ne partagent pas le code d'ArmorPaint : `oiiotool --colorconvert` (OpenImageIO 3.1,
avec son propre OCIO 2.5.1), `ociobakelut`, et Nuke 17.1 s'il est installé (OCIO 2.4, configuration
`fn-nuke_studio-config-v3.0.0_aces-v2.0_ocio-v2.4`, de la même famille ACES 2.0).

**Mires.** Rampe sRGB 8 bits (256 niveaux), ColorChecker 24 en sRGB 8 bits, 32 valeurs HDR linéaires (gris de
0,0028 à 92, couleurs saturées jusqu'à 64), ColorChecker en ACEScg (EXR float), 8 couleurs ACEScg hors du gamut
Rec.709. Chaque mire est importée dans son espace (`studio_texture_set_color_space`), posée sur un calque de
remplissage et exportée en EXR 32 vers l'espace de travail, ACEScg, sRGB - Texture et Linear P3-D65 : calques 32,
16 et 8 bits (`studio_project_set_bits`) en Linear Rec.709, puis calques 32 bits en espace de travail ACEScg.

**Écarts mesurés** (2026-09-27, maximum sur tous les espaces d'export) :

| Mire | 32 bits | 16 bits | 8 bits | 32 bits, espace ACEScg |
|---|---|---|---|---|
| Rampe et ColorChecker sRGB 8 bits (/255, après la courbe sRGB) | 0,56 | 0,58 | 0,56 | 0,56, sauf 4,7 en sRGB - Texture ¹ |
| HDR et ColorChecker ACEScg (écart relatif) | 1,6·10⁻⁴ | 1,8·10⁻³ (half) | — | 1,6·10⁻⁴ |
| Couleurs ACEScg hors gamut Rec.709 | faussées ² | faussées ² | — | 4,7·10⁻⁴ |

| Autres contrôles | Écart max |
|---|---|
| Aller-retour : ColorChecker sRGB → ArmorPaint → export ACEScg → Nuke → sRGB | 0,52/255 |
| Viewport (LUT 129³) contre OCIO exact : gris, ColorChecker, HDR de 0,0028 à 92 | 0,06, 0,09, 1,6 /255 |
| Même vue en LUT `ociobakelut` 65³ (entrée ACEScct), pour comparaison | 0,14, 0,99, 3,7 /255 |
| Affichage ACES 2.0 de Nuke contre oiiotool | 0,0007/255 |

Les textures 8 bits importées sont ré-encodées en gamma 2.2 sur 8 bits dans l'espace de travail : l'écart de
0,56/255 est ce demi-pas de quantification. Les couleurs flottantes et les valeurs > 1 passent sans écrêtage en
calques 16 et 32 bits (en 8 bits, le calque écrête à 1).

1. En espace de travail ACEScg, cette quantification se fait en primaires AP1. Revenue en sRGB, elle passe par la
   matrice AP1 → Rec.709 et par la pente de la courbe sRGB près du noir : le canal rouge du cyan du ColorChecker
   (8/255) ressort à 12,7/255.
2. **Limite.** Le calque stocke la couleur de base encodée gamma 2.2, sans signe : en espace de travail Rec.709,
   une couleur hors de son gamut perd ses composantes négatives. Le rouge ACEScg (1, 0, 0) ressort en
   (1,09, 0,24, 0,07). **Un show qui peint avec des textures à gamut large (ACEScg) doit prendre ACEScg comme espace
   de travail** (préférences, projet ou `STUDIO_OCIO_WORKING_SPACE`) : ces couleurs sont alors exactes.

Le banc a aussi trouvé un bug d'upstream sur Metal : l'export `--export-textures exr16`/`exr32` et l'aplatissement
des calques 16/32 bits écrivaient des valeurs aberrantes (patch `metal-target-format`, voir `PATCHES.md`).

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

// Textures importées (A4) ; file : chemin de la texture (asset), comme dans le projet
int   studio_ocio_import_enabled(void);          // 0 avec STUDIO_OCIO_IMPORT=0
char *studio_texture_color_space(char *file);    // espace source retenu ; "" sans conversion (.k, OCIO indisponible, désactivé)
char *studio_texture_color_space_source(char *file);  // "user", "name", "rule", "default" ; "" sans conversion
int   studio_texture_is_linear(char *file);      // 1 : couleurs gardées linéaires (flottante, espace couleur)
int   studio_texture_set_color_space(char *file, char *color_space);  // nom, alias ou rôle ; "" : automatique. Recharge
                                                 // la texture ; 0 si l'espace est inconnu ou le fichier absent. Sans
                                                 // texture de ce chemin dans le projet, le choix vaut pour son import
int   studio_texture_dump(char *file, char *out);  // pour les bancs : pixels relus du GPU ("STX1", largeur, hauteur, format)
int   studio_project_pack_assets(int on);        // option Pack Assets du menu File : 1/0, -1 pour lire ; rend la valeur d'avant
int   studio_project_bits(void);                 // profondeur des calques : 8, 16 ou 32
int   studio_project_set_bits(int bits);         // calques recopiés à cette profondeur, valeurs gardées (bornées à [0, 1] en 8
                                                 // bits) ; 0 pour une autre valeur que 8, 16 ou 32

// Réglages (A5)
char *studio_prefs_path(void);                   // fichier des préférences studio (ignoré en --background sans $STUDIO_PREFS)
char *studio_ocio_config_preference(void);       // "" : aucune (configuration intégrée)
int   studio_ocio_set_config_preference(char *path);  // "" : aucune. Enregistrée et rechargée (gardée seulement avec $OCIO) ;
                                                 // rend 1 si OCIO est prêt
char *studio_ocio_config_origin(void);           // "explicit", "$OCIO", "preferences", "built-in default"
char *studio_ocio_working_space_preference(void);  // espace des nouveaux projets ; "" : automatique
int   studio_ocio_set_working_space_preference(char *name);  // "" : automatique ; 0 si inconnu, rien ne change
char *studio_ocio_working_space_origin(void);    // "project", "project, automatic", "$STUDIO_OCIO_WORKING_SPACE",
                                                 // "preferences", "automatic" ; "" sans espace de travail
char *studio_ocio_working_space_error(void);     // pourquoi il n'y a pas d'espace de travail ; "" sinon
int   studio_ocio_project_working_space_mode(void);  // 0 nouveau projet (variable, préférence, automatique), 1 automatique, 2 nommé
char *studio_ocio_project_working_space_name(void);  // nom enregistré pour le projet ; "" sinon
int   studio_ocio_set_project_working_space(int mode, char *name);  // gardé au prochain enregistrement ; 0 si inconnu, rien ne change
int   studio_ocio_prefs_show(void);              // ouvre Preferences > Color ; 0 sans onglet
int   studio_ocio_import_reconvert(void);        // recharge et reconvertit les textures converties ; rend leur nombre
```

## Tests

```sh
.tests/run.sh [dossier de travail]     # EXR (B1) ; FUZZ=<n> pour le nombre de mutations
.tests/proc.sh [dossier de travail]    # processus (C1)
.tests/obj.sh [dossier de travail]     # import OBJ (patches obj-global-indices, obj-float-exponent, udim-import) ; FUZZ=<n> pour le nombre de mutations
.tests/fbx.sh [dossier de travail]     # import FBX (patch fbx-split) : splits = upstream, UDIM, matériaux, fuzz ; FUZZ=<n>
.tests/udim_app.sh [dossier]           # UDIM dans l'app (lot D0) : import OBJ/FBX, refus, ajout, 70 tuiles (atlas, peinture, export), conseil de résolution
.tests/sg.sh [dossier de travail]     # ShotGrid hors app : sidecar (C2), noms des exports (B3), moteur (C3), publish (C4), mode dégradé et file d'attente (C5) ; Toolkit de .shotgrid/
.tests/publish_app.sh [dossier]        # publish ShotGrid (C4) dans l'app, site simulé : op publish, puis fenêtre Publish (Qt offscreen) ; copie publiée rouverte sans le dossier de travail ; PY=<python PySide6>
.tests/sg_context_app.sh [dossier]     # contexte et mode dégradé ShotGrid (C5) dans l'app, site simulé : contexte gardé dans le .arm, site coupé (mode fichier, retour trouvé au repos), coupure en plein publish (file rejouée), journal d'audit
.tests/arm_paths_app.sh [dossier]      # chemins enregistrés dans les .arm (patch arm-relative-paths) : relatifs, normalisés, rouverts sans l'ancien dossier
python3 .tests/arm_paths.py <projet.arm> [fichier...]   # chemins des fichiers d'un .arm ; avec des fichiers, vérifie qu'il les désigne, relatifs et normalisés
.tests/export.sh [dossier de travail]  # export studio (B2, B4, B5) hors app : encodeurs relus par libpng/libtiff (patches sdefl-bound, encode-image-overflow), expressions, presets, préflight, conseil de résolution UDIM ; FUZZ=<n>
.tests/export_app.sh [dossier]         # export studio dans l'app : scripts, --export-textures, pixels = upstream, manifeste, atomicité
.tests/batch_app.sh [dossier]          # mode batch (B7) : sans fenêtre ni Dock, --export-mode/-naming/-color-space, rapport JSON, codes de retour 0/1/2
.tests/ocio.sh [dossier de travail]    # OCIO (A1) hors app : ABI, module, références PyOpenColorIO, threads, erreurs ; OCIO_TEST_CONFIGS=<configs>
.tests/ocio_app.sh [dossier]           # OCIO dans l'app : configuration par défaut, $OCIO, $OCIO invalide, API de scripting
.tests/ocio_export_app.sh [dossier]    # export OCIO (A3) dans l'app : pixels = PyOpenColorIO, préflight, manifeste, cas bloquants
.tests/ocio_view_app.sh [dossier]      # viewport OCIO (A2) dans l'app : compositeur = PyOpenColorIO, menu (API), rechargement, variables
.tests/ocio_import_app.sh [dossier]    # textures importées (A4) dans l'app : pixels = PyOpenColorIO par format, choix par texture, .arm rouvert et paquet d'assets, STUDIO_OCIO_IMPORT=0, nœud Image Texture
.tests/ocio_validation_app.sh [dossier]  # validation couleur (A6) dans l'app : mires importées, peintes, exportées (8/16/32 bits, espace ACEScg) = oiiotool ; viewport = oiiotool et ociobakelut ; aller-retour Nuke ; export upstream exr16/exr32 = png (patch metal-target-format) ; NUKE=<binaire>
.tests/ocio_prefs_app.sh [dossier]     # réglages couleur (A5) dans l'app : fichier des préférences, espace de travail du projet (.arm rouvert, autre configuration, erreurs), textures reconverties = PyOpenColorIO, viewport
.tests/export_window_app.sh [dossier]  # fenêtre d'export (B6) dans l'app, Qt offscreen : état, préflight, export, fichiers décochés, blocage, réouverture, bits et résolution par map, sidecar, preset du projet ; PY=<python PySide6>
<python PySide6> .tests/export_window_ui.py [dossier]  # fenêtre d'export contre un faux ArmorPaint : liste groupée (tuiles UDIM, objets), cases, décomptes, problèmes affichés, tableau des maps, sidecar, nom des fichiers, jeux de textures
.tests/sets_app.sh [dossier]           # jeux de textures, nom des fichiers et UDIM dans l'app, avec les maillages de udim_gen.py ; PY=<python PySide6>
.tests/atlas_order_app.sh [dossier]    # jeux de textures quand l'onglet Meshes trie, réapparente ou supprime des objets (patch atlas-objects-order)
python3 .tests/udim_gen.py [dossier]   # maillages de test (défaut .tests/meshes) : udim_cube.obj (une face par tuile, 1001-1003 et 1011-1013 ; matériaux body et cap), props.obj (crate, barrel, ball)
```

Les maillages de `.tests/meshes` s'ouvrent aussi à la main dans ArmorPaint (File > Import Mesh, « Split By » : UDIM
Tile, Material ou Object) pour essayer peinture et export.

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

`ocio_import_app.sh` écrit des images de test avec `ocio_import_check.py` (rampes et bruit en PNG, TIFF, PSD à plat
et à calques ; EXR float et half aux valeurs extrêmes, NaN et infinis compris ; `.hdr`), les importe dans ArmorPaint,
relit les pixels envoyés au GPU (`studio_texture_dump`) et les compare à PyOpenColorIO appliqué au fichier : au code
près en 8 bits (99 % exacts), à 1 ulp en half. Il vérifie aussi le `.arm` (entrée des espaces, réouverture, paquet
d'assets sans les fichiers) et le nœud Image Texture (export d'un gris linéaire par un calque de remplissage). Même
venv que `ocio_export_app.sh`.

`ocio_prefs_app.sh` fait tourner l'app avec un fichier de préférences à lui (`$STUDIO_PREFS`) : lecture, écriture,
clé inconnue, `$OCIO` prioritaire, préférence invalide, fichier illisible, et le fichier de l'artiste intact en
`--background`. Il enregistre des projets et relit leur entrée `studio_ocio.json` : espace figé à l'enregistrement,
repris à la réouverture quels que soient la préférence et l'environnement, automatique sans entrée, autre
configuration signalée, espace inconnu en erreur. Il change l'espace de travail (ACEScg, puis l'automatique) et
compare les pixels des textures reconverties à PyOpenColorIO (`ocio_import_check.py --working=`). Même venv que
`ocio_export_app.sh`.

`ocio_validation_app.sh` écrit ses mires, son maillage (un plan d'UV 0..1), son preset et ses scripts avec
`ocio_validation.py make`, lance ArmorPaint quatre fois (calques 32, 16, 8 bits, puis 32 bits en espace de travail
ACEScg), puis compare avec `ocio_validation.py check`, qui écrit `report.md` (écarts mesurés, tableau de
« Validation (A6) »). Les références sont calculées par `oiiotool` et `ociobakelut` de `.ocio/venv`
(`pip install OpenImageIO` ; `ociobakelut` vient de la roue OpenColorIO). Nuke est cherché dans
`/Applications/Nuke17*` ; `NUKE=` (vide) s'en passe, `NUKE=<binaire>` en désigne un autre (Nuke 17 ou plus, pour
ACES 2.0). Environ 10 s avec Nuke.

`export.sh` requiert libpng et libtiff (`brew install libpng libtiff`) ; `export_app.sh`, Pillow et `tiffinfo`.

`sg.sh` fait tourner le sidecar sur un site ShotGrid simulé (`sg_fixture.py` : mockgun avec le schéma d'un vrai
site, la configuration Toolkit de `sg_fixture/config`, une session Desktop enregistrée, un vrai amorçage). tk-core
et les apps viennent de `.shotgrid/` ; `STUDIO_TEST_TK_CORE` et `STUDIO_TEST_TK_APPS` en désignent d'autres (un
autre tk-core : `git clone --depth 1 --branch <tag> https://github.com/shotgunsoftware/tk-core`, **pas** le paquet
PyPI `tk-core`, qui n'est pas celui d'Autodesk). Python du sidecar : `.shotgrid/venv` s'il a PySide6, sinon
`python3`, et les tests des fenêtres et du publish sont sautés. Les tests du publish (`TestPublish`, contre un faux
ArmorPaint) essaient des variantes du site : `STUDIO_TEST_UDIM_DEFAULT=0` (clé UDIM sans `default: "<UDIM>"`),
`STUDIO_TEST_CHANNEL_FILTER=<filtre>` (`filter_by` de la clé du canal), `STUDIO_TEST_TEXTURES_COPY=0` (textures
publiées en place). Dans l'app, le site simulé remplace le vrai sidecar avec
`STUDIO_SG_BRIDGE=<external>/.tests/sg_fixture.py STUDIO_TEST_WORKDIR=<dossier> STUDIO_TEST_TK_CORE=<tk-core>`.
Le site simulé coupe aussi le réseau à la demande (C5) : `STUDIO_TEST_OFFLINE_FLAG=<fichier>`, injoignable tant que
ce fichier existe ; `STUDIO_TEST_OFFLINE_AT=create:PublishedFile:2`, le deuxième de ces appels échoue et crée le
fichier (`STUDIO_TEST_LOST_AT` : pareil, mais l'appel a eu lieu, réponse perdue).

`publish_app.sh` lance ArmorPaint sur ce site (scénario `demo`, avec tk-multi-publish2), le hook du sidecar
`publish_app_hook.py` relevant ce qui est publié ; la fenêtre Publish y est pilotée comme par l'artiste. Le site
simulé range `work/` et `publish/` à la même profondeur, comme tk-config-default2 : le banc importe aussi une texture
rangée dans le dossier de travail, seul cas où les chemins de la copie publiée dépendent du patch `project-copy`.

`run.sh` requiert les outils OpenEXR (`brew install openexr`) comme référence indépendante. La couche jobs et
l'API de scripting se vérifient dans l'app : `ArmorPaint --background --script <fichier>` avec un script comme
celui ci-dessus.
