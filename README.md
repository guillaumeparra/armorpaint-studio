# ArmorPaint — code studio

Ce dossier est compilé dans ArmorPaint par le hook upstream `paint/plugins/external/`
(`paint/plugins/project.c`, define `WITH_EXTERNAL`). `external_init()` est appelé au démarrage par
`plugins_init()` (`paint/plugins/plugins.c`). Le dossier est ignoré par le `.gitignore` upstream : c'est un
dépôt git séparé.

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

Modules, un par lot (voir `ARMORPAINT_LOTS.md` à la racine d'armorpaint) :

| Module | Lot | État |
|---|---|---|
| `studio_exr` | B1 | ✅ lecture et écriture EXR par tinyexr |
| `studio_proc`, `studio_exec` | C1 | ✅ exécution de processus, jobs asynchrones, API de scripting (macOS ; Linux écrit, non testé) |
| `studio_export` | B2-B5 | à faire |
| `studio_ocio` | A | à faire |
| `studio_sg`, `studio_shotgrid`, `studio_json`, `assets/studio_sg_*.py` | C2 | ✅ sidecar ShotGrid (Toolkit), superviseur, API de scripting (macOS ; validé sur site simulé) |
| `studio_shotgrid` (UI) | C3-C5 | à faire |
| `studio_ui` | B6/C3 | à faire |

Build : `../base/make --compile` depuis `paint/`. Le log de démarrage affiche `studio: external <version>
loaded`, puis une ligne par module.

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
| `STUDIO_SG_ENGINE` | moteur démarré après l'amorçage (défaut `tk-shell`, `none` pour aucun ; sans moteur en repli s'il échoue) |
| `STUDIO_SG_PLUGIN_ID` | plugin id de l'amorçage (défaut `basic.armorpaint`) |
| `STUDIO_SG_BASE_CONFIGURATION` | descripteur de configuration si le projet n'a pas de PipelineConfiguration |
| `STUDIO_SG_WORK_TEMPLATES` | templates de travail par type d'entité (défaut `Asset=armorpaint_asset_work,Shot=armorpaint_shot_work`) |
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
| `ping`, `shutdown` | — | `{}` |

Codes d'erreur (`/error/code`) : du sidecar, `bad_request`, `unknown_op`, `auth_required` (pas de session Desktop,
ou expirée : jamais d'invite de mot de passe), `sg_unreachable`, `sg_error`, `toolkit_unavailable` (tk-core
introuvable), `toolkit_error`, `not_found`, `template_missing` (la config n'a pas le template de travail),
`internal` ; du superviseur, `sidecar_failed`, `sidecar_died`, `timeout`, `cancelled`. `restart_required` n'arrive
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

## Tests

```sh
.tests/run.sh [dossier de travail]     # EXR (B1) ; FUZZ=<n> pour le nombre de mutations
.tests/proc.sh [dossier de travail]    # processus (C1)
STUDIO_TEST_TK_CORE=<tk-core> .tests/sg.sh [dossier de travail]   # ShotGrid (C2)
```

`sg.sh` fait tourner le sidecar sur un site ShotGrid simulé (`sg_fixture.py` : mockgun avec le schéma d'un vrai
site, la configuration Toolkit de `sg_fixture/config`, une session Desktop enregistrée, un vrai amorçage). Il lui
faut un clone de tk-core (`git clone --depth 1 --branch v0.24.2 https://github.com/shotgunsoftware/tk-core` ;
**pas** le paquet PyPI `tk-core`, qui n'est pas celui d'Autodesk). Sans `STUDIO_TEST_TK_CORE`, seuls le lecteur
JSON et le superviseur (contre `sg_fake.py`) tournent. Dans l'app, le site simulé remplace le vrai sidecar avec
`STUDIO_SG_BRIDGE=<external>/.tests/sg_fixture.py STUDIO_TEST_WORKDIR=<dossier> STUDIO_TEST_TK_CORE=<tk-core>`.

`run.sh` requiert les outils OpenEXR (`brew install openexr`) comme référence indépendante. La couche jobs et
l'API de scripting se vérifient dans l'app : `ArmorPaint --background --script <fichier>` avec un script comme
celui ci-dessus.
