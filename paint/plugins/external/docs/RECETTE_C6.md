# Recette C6 — ArmorPaint studio sur le vrai site ShotGrid

Les lots C2 à C5 sont validés sur un site ShotGrid simulé (mockgun, vraie configuration Toolkit, vrai amorçage). Cette
recette les valide sur le site du studio, avec l'aller-retour complet : ouvrir, peindre, enregistrer, publier, puis
rouvrir depuis ShotGrid. Elle se fait **avant le lot D2**, qui change le format du `.arm` (point de non-retour du fork).

Il faut une demi-journée, avec quelqu'un qui peut modifier la configuration Toolkit du projet de test, et un artiste
texture.

## 0. Préparer

- [ ] **Projet ShotGrid de test** (pas de production), avec une configuration de pipeline copiée de la production.
- [ ] Dans cette configuration (détails et extraits YAML : `README.md`, sections C2, C3, C4 et B3) :
  - [ ] templates `armorpaint_asset_work`, `armorpaint_asset_textures`, `armorpaint_asset_publish`,
        `armorpaint_asset_textures_publish` (et `armorpaint_shot_*` si des shots sont texturés) ; clé UDIM avec
        `default: "<UDIM>"` ;
  - [ ] moteur `tk-armorpaint` dans les environnements de projet et de tâche, avec tk-multi-workfiles2
        (`hook_scene_operation` du moteur) et tk-multi-publish2 (collecteur et plugins du moteur) ;
  - [ ] `plugin_ids` de la PipelineConfiguration couvrant `basic.armorpaint` (`basic.*`).
- [ ] Un **asset** avec une tâche de texture assignée au compte de l'artiste ; son maillage (OBJ ou FBX, si possible
      avec des tuiles UDIM).
- [ ] **Poste** : ShotGrid Desktop installé et connecté au site ; ArmorPaint studio construit (`tools/armorpaint.sh`
      ou le build du studio) ; le lanceur du studio (rez…) règle `STUDIO_SG_PYTHON` sur un Python avec PySide6 (celui
      de ShotGrid Desktop convient) et `OCIO` si le show en a une.

## 1. Vérifier le poste et la configuration

Dans l'environnement du lanceur, depuis `paint/plugins/external` :

```sh
tools/recette_c6.py --project "<projet de test>"
tools/recette_c6.py --project "<projet de test>" --task <id de la tâche> --report recette_c6_config.md
```

Le premier appel ne fait que lire : poste (Python, PySide6), Toolkit, site, session Desktop, tâches de l'artiste,
configurations de pipeline du projet. Il liste les tâches pour choisir `--task`. Le second amorce Toolkit dans le
contexte de la tâche, comme ArmorPaint à la première ouverture du menu ShotGrid, et crée donc les dossiers de la
tâche. Il vérifie :

- le moteur et les apps ;
- les templates ;
- les réglages de tk-multi-publish2 ;
- les types de publish ;
- le nom des textures exportées.

**Objectif : 0 échec.** Chaque ligne `ÉCHEC` ou `attention` dit quoi corriger. Le journal du sidecar (chemin en fin de
sortie) montre l'amorçage de Toolkit.

## 2. Aller-retour dans ArmorPaint

Lancer ArmorPaint par le lanceur, en gardant son journal (`tools/armorpaint.sh` l'écrit dans
`$TMPDIR/armorpaint-studio.log`).

| # | Action | Attendu | OK |
|---|---|---|---|
| 1 | Ouvrir le menu ShotGrid | le contexte en tête, puis File Open..., File Save..., Publish... ; aucune erreur | |
| 2 | File Open... (Ctrl/Cmd+Alt+O) : la tâche, puis New File | fenêtre Workfiles au premier plan ; ArmorPaint repasse devant à sa fermeture | |
| 3 | Importer le maillage de l'asset (UDIM si possible), peindre quelques calques | | |
| 4 | File Save... (Ctrl/Cmd+Alt+S) | `…/work/armorpaint/<nom>.v001.arm` ; titre `<nom>.v001 - <projet> / <asset> / <étape> / <tâche> - ArmorPaint` | |
| 5 | File > Export > Textures : preset du studio, Export | fichiers nommés par le template des textures, une tuile par fichier en UDIM ; espace OCIO attendu | |
| 6 | Publish... (Ctrl/Cmd+Alt+P) : une description, Publish | projet copié dans `publish/`, textures publiées (une séquence `<UDIM>` = un publish) ; ArmorPaint passe à v002 | |
| 7 | Dans ShotGrid (web) | PublishedFiles du projet (`ArmorPaint Project`) et des textures (`Texture`, `UDIM Image`), textures dépendantes du projet, **vignettes présentes** | |
| 8 | Quitter ArmorPaint, le relancer ; File Open..., onglet des publishes : ouvrir le publish v001 | le projet publié s'ouvre, textures importées retrouvées (rien de rose), dans le contexte de la tâche | |
| 9 | Rouvrir le `.arm` de travail par File > Open Recent... | titre avec le contexte (gardé dans le `.arm`) ; Publish... va à la même tâche | |
| 10 | Charger la séquence UDIM publiée dans le DCC de lookdev du studio (Maya, Houdini, Katana…), par son chargeur | toutes les tuiles trouvées | |

## 3. Mode dégradé, réseau coupé pour de vrai

| # | Action | Attendu | OK |
|---|---|---|---|
| 11 | Couper le réseau (Wi-Fi, câble ou VPN), puis File Open... | une fois dans la console : `ShotGrid unreachable (…)` ; titre `(ShotGrid offline)` ; File Open refusé (la première fois après le délai du réseau, 30 s au plus) | |
| 12 | Peindre, File > Save, exporter | tout fonctionne ; textures aux noms d'ArmorPaint, dit une fois | |
| 13 | Rétablir le réseau et ne plus toucher ArmorPaint | en moins de 2 min : `ShotGrid is back`, titre normal | |
| 14 | Publish..., couper le réseau pendant le publish, après la validation | le publish aboutit ; menu ShotGrid : `N publishes waiting for ShotGrid` | |
| 15 | Rétablir le réseau | `ShotGrid: N queued publishes registered` ; dans ShotGrid, publishes, dépendances et vignettes complets, sans doublon | |
| 16 | Se déconnecter dans ShotGrid Desktop, puis ouvrir le menu ShotGrid | erreur `auth_required` claire, aucune invite de mot de passe ; se reconnecter, ShotGrid > Retry | |

Noter tout autre cas réseau rencontré : pas de DNS, proxy, site très lent.

## 4. Retour

À envoyer :

- `recette_c6_config.md`, le rapport de l'outil ;
- les tableaux ci-dessus remplis ;
- le journal d'ArmorPaint ;
- le journal d'audit (`studio_audit.jsonl`, dossier des préférences d'ArmorPaint, voir `EXPLOITATION.md`) ;
- une capture des PublishedFiles dans ShotGrid.
