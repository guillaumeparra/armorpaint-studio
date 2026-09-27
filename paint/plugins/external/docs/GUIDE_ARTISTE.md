# ArmorPaint studio — guide de l'artiste

ArmorPaint studio est ArmorPaint avec le pipeline du studio : fichiers de travail et publishes ShotGrid, noms de
textures du pipeline, gestion couleur OCIO. Lancez-le par le lanceur du studio : il règle ShotGrid, Python et la
couleur pour le projet.

## Travailler dans une tâche ShotGrid

Le **menu ShotGrid** (après Help) montre en tête la tâche courante, puis les commandes du studio.

| Commande | Raccourci (macOS : Cmd au lieu de Ctrl) | Rôle |
|---|---|---|
| File Open... | Ctrl+Alt+O | ouvrir un fichier de travail ou un publish d'une tâche, ou en créer un (New File) |
| File Save... | Ctrl+Alt+S | enregistrer à la version suivante, au bon endroit, avec le bon nom |
| Publish... | Ctrl+Alt+P | publier le projet et ses textures dans ShotGrid |

Ces fenêtres sont celles de ShotGrid (les mêmes que dans Maya ou Nuke). Une seule à la fois : tant qu'elle est
ouverte, le menu est grisé.

Le titre de la fenêtre dit le fichier et la tâche : `robot.v003 - Demo / robot / Texture / texturing - ArmorPaint`.
La tâche est gardée dans le `.arm` : un fichier rouvert par le menu File d'ArmorPaint, même sur un autre poste,
retrouve sa tâche. File > Save enregistre sur le fichier courant ; pour une nouvelle version, File Save... du menu
ShotGrid.

## Exporter les textures

File > Export > Textures ouvre la fenêtre d'export : preset du studio, espace couleur de chaque texture, liste des
fichiers (décochez ceux à ne pas écrire), problèmes signalés avant l'export (préflight). Les fichiers sont nommés par
le pipeline, à côté du fichier de travail (par exemple `textures/v003/robot_base_color.1001.exr`). Ctrl+E refait
le dernier export.

## Publier

Publish... : vérifiez la liste, écrivez une description, cliquez Publish. Sont publiés :

- le **projet** : une copie dans le dossier de publish, avec une vignette du viewport ;
- les **textures** du dernier export de la fenêtre d'export (sinon de l'export courant), chacune liée au projet ; une
  texture UDIM est un seul publish pour toutes ses tuiles (`robot_base_color.<UDIM>.exr`).

Ensuite, ArmorPaint passe à la version suivante (v004) : continuez à peindre. Une texture sans version dans son nom
est décochée (l'export suivant écraserait le fichier publié). Un point bloquant (projet d'une autre tâche, projet
modifié depuis la collecte, nom refusé par le pipeline) s'affiche dans la fenêtre avec sa solution.

## Couleur

Le menu Color choisit l'affichage du viewport (display, view, look OCIO). Preferences > Color : configuration OCIO et
espace de travail des nouveaux projets. Chaque projet garde son espace de travail. Les textures importées y sont
converties depuis leur espace source, deviné d'après leur nom et leur format : pour le corriger, clic droit sur la
texture dans l'onglet Textures > Color Space..., ou menu Color > Texture Color Spaces... pour toutes.

## Sans ShotGrid (réseau coupé)

Si ShotGrid ne répond plus, **continuez à travailler** : ouvrir, peindre, enregistrer et exporter fonctionnent. Le
titre indique `(ShotGrid offline)`. File Open, File Save et Publish attendent le retour du site. Les textures exportées
gardent alors les noms d'ArmorPaint. Si la coupure arrive pendant un publish, il aboutit et ce qui reste à
enregistrer dans ShotGrid attend : le menu ShotGrid indique `N publishes waiting for ShotGrid`.

Le site est réessayé toutes les deux minutes ; ShotGrid > Check Connection le réessaie tout de suite. À son retour,
`ShotGrid is back` s'affiche et les publishes en attente sont enregistrés d'eux-mêmes.

## En cas de problème

Les messages s'affichent dans la console d'ArmorPaint (en bas). Pour le support : l'heure, ce que vous faisiez, le
message, et le journal d'ArmorPaint (le lanceur du studio dit où il est).

- `auth_required` : connectez-vous dans ShotGrid Desktop, puis ShotGrid > Retry.
- Menu ShotGrid vide ou en erreur : le projet n'a pas ArmorPaint dans sa configuration : prévenez le pipeline.
