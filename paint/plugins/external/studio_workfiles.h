#pragma once

// Lot C3 : Workfiles de ShotGrid. Les fenêtres File Open et File Save sont celles de tk-multi-workfiles2, ouvertes par
// le moteur Toolkit tk-armorpaint dans le sidecar (op show_app, voir tk-armorpaint/engine.py) ; ce qu'elles font sur
// la scène revient ici par les requêtes host du sidecar (studio_sg.h) : current_path, open, save, save_as, new.
// Ctrl+Alt+O (Cmd+Alt+O sur macOS) ouvre File Open, Ctrl+Alt+S File Save ; File Open s'ouvre aussi au démarrage si
// STUDIO_SG_WORKFILES_ON_START=1.

#include <stdbool.h>

void studio_workfiles_init(void);

// Vrai tant qu'une fenêtre ShotGrid est ouverte : un appel au sidecar attendrait sa fermeture.
bool studio_workfiles_busy(void);

// Lot B6 (fenêtre d'export) : enregistre le projet ouvert comme la version suivante de son fichier de travail : op
// next_version du sidecar pour la tâche du contexte et name (champ du template de travail), puis sauvegarde à ce
// chemin, comme File Save. Chemin du nouveau fichier (malloc), ou NULL et *error (malloc).
char *studio_workfiles_version_up(const char *name, char **error);
