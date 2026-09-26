#pragma once

// Lot C3 : Workfiles de ShotGrid. Les fenêtres File Open et File Save sont celles de tk-multi-workfiles2, ouvertes par
// le moteur Toolkit tk-armorpaint dans le sidecar (op show_app, voir tk-armorpaint/engine.py) ; ce qu'elles font sur
// la scène revient ici par les requêtes host du sidecar (studio_sg.h) : current_path, open, save, save_as, new.
// Ctrl+Alt+O (Cmd+Alt+O sur macOS) ouvre File Open, Ctrl+Alt+S File Save ; File Open s'ouvre aussi au démarrage si
// STUDIO_SG_WORKFILES_ON_START=1.

#include <stdbool.h>

void studio_workfiles_init(void);

// Projet ShotGrid du contexte courant (dernière fenêtre Workfiles), 0 si inconnu.
int studio_workfiles_project_id(void);

// Vrai tant qu'une fenêtre ShotGrid est ouverte : un appel au sidecar attendrait sa fermeture.
bool studio_workfiles_busy(void);
