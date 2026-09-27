#pragma once

// Fenêtres Qt du studio (export du lot B6, subzero du chantier E) : un processus Python à part, lancé à l'ouverture et
// arrêté à sa fermeture ; il s'arrête aussi quand ArmorPaint quitte (EOF sur son stdin). Le script Python s'appuie sur
// assets/studio_qt.py.
//
// Protocole, une ligne JSON par message sur stdin et stdout :
//   requête de la fenêtre   {"id":N,"op":"...","args":{...}}
//   réponse d'ArmorPaint    {"id":N,"ok":true,"data":...} ou {"id":N,"ok":false,"error":{"code","message"}}
//   évènement d'ArmorPaint  {"event":"...", ...}
// Les requêtes sont traitées une à une, à la frame suivante (sys_notify_on_next_frame), hors du dessin. stderr de la
// fenêtre est recopié dans le log ("studio: <nom>: ...").
//
// Interpréteur : STUDIO_PYTHON, sinon STUDIO_SG_PYTHON, sinon python3 dans le PATH ; il lui faut PySide6. La fenêtre
// reçoit PYTHONDONTWRITEBYTECODE=1 (pas de __pycache__ dans le paquet signé de l'app) et STUDIO_UI_SCALE (échelle de
// l'interface d'ArmorPaint). Pas encore sous Windows (studio_proc). Thread principal.

#include <stdbool.h>

typedef struct studio_window studio_window_t;

typedef struct {
	const char *name;       // "export window" : log et messages
	const char *title;      // "Export window" : titre des boîtes
	const char *script;     // script dans data/ ("studio_export_window.py")
	const char *script_env; // variable qui le remplace ("STUDIO_EXPORT_WINDOW"), NULL : aucune
	// Requête de la fenêtre, à la frame suivante ; op est NULL si la requête n'en a pas (répondre bad_request).
	void (*on_request)(studio_window_t *w, int id, const char *op, const char *req);
	void (*on_closed)(studio_window_t *w); // optionnel : fenêtre fermée, processus terminé
} studio_window_desc_t;

// Crée l'hôte (une fenêtre à la fois) et l'accroche à la boucle d'ArmorPaint. desc est copié ; ses chaînes doivent
// vivre aussi longtemps que l'hôte (littéraux).
studio_window_t *studio_window_create(const studio_window_desc_t *desc);

// Lance la fenêtre. env : variables en plus ("NOM=valeur", terminé par NULL), NULL : aucune. Faux si elle est déjà
// ouverte ou si le lancement échoue (boîte montrée à l'artiste).
bool studio_window_open(studio_window_t *w, const char *const *env);
bool studio_window_is_open(studio_window_t *w);

void studio_window_send(studio_window_t *w, const char *line); // évènement ou message déjà formé, sans '\n'
void studio_window_reply(studio_window_t *w, int id, const char *data_json);
void studio_window_reply_error(studio_window_t *w, int id, const char *code, const char *message);
