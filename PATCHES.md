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
