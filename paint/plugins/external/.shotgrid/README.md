# Toolkit pour la démo ShotGrid

Sources de Toolkit pour `tools/armorpaint.sh --shotgrid-demo`, `.tests/demo_workfiles.sh` et `.tests/sg.sh`,
versionnées pour qu'un clone du dépôt ait tout. Sans leur `.git`, leurs `docs/`, `tests/`, `developer/` ni les
maquettes `.graffle` :

| dossier | tag (github.com/shotgunsoftware) |
|---|---|
| `tk-core` | v0.24.2 |
| `tk-multi-workfiles2` | v0.18.1 |
| `tk-framework-shotgunutils` | v5.11.3 |
| `tk-framework-qtwidgets` | v2.12.8 |

Seule exception dans les tests : `tk-core/tests/fixtures/mockgun/schema*.pickle`, le schéma d'un vrai site
ShotGrid que charge le site simulé (`.tests/sg_fixture.py`).

`venv/` (Python avec PySide6, le python du sidecar) est propre à la machine et ignoré par git :
`tools/setup_shotgrid.sh` le crée, et `--shotgrid-demo` l'appelle s'il manque.

Changer de version : supprimer le dossier, le recloner sur le nouveau tag
(`git clone --depth 1 --branch <tag> https://github.com/shotgunsoftware/<dépôt>.git`), retirer son `.git` et ce qui est
exclu ci-dessus (en gardant les deux `.pickle`), mettre à jour ce tableau.
