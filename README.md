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
| `studio_shotgrid` | C | à faire |
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

## Tests

```sh
.tests/run.sh [dossier de travail]     # EXR (B1) ; FUZZ=<n> pour le nombre de mutations
.tests/proc.sh [dossier de travail]    # processus (C1)
```

`run.sh` requiert les outils OpenEXR (`brew install openexr`) comme référence indépendante. La couche jobs et
l'API de scripting se vérifient dans l'app : `ArmorPaint --background --script <fichier>` avec un script comme
celui ci-dessus.
