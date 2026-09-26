#include "studio_workfiles.h"
#include "iron_input.h"
#include "iron_string.h"
#include "iron_system.h"
#include "iron_ui.h"
#include "studio.h"
#include "studio_json.h"
#include "studio_sg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Cœur ArmorPaint (paint/sources/globals.h, functions.h, project.c, minic_impl.c, ui/ui_menu.c) ; ui_menubar_add_category
// est le point d'injection STUDIO-PATCH(menubar-hook) de ui/ui_menubar.c (PATCHES.md).
extern ui_t *g_ui;
extern char *manifest_title;
extern bool  ui_menu_show;
extern bool  ui_menu_show_first; // vrai : le menu ouvert est remesuré (hauteur de son fond) au prochain rendu
void         ui_menubar_add_category(char *label, void (*draw_items)(void));
bool         ui_menu_button(char *text, char *label, int icon);
void         ui_menu_separator(void);
void         ui_menu_label(char *text, char *shortcut);
void         project_open_on_file_picked(char *path);
void         export_arm_run_project(char *path);
char        *project_filepath_get(void);
void         project_filepath_set(char *s);
void         script_project_new(void);
void         console_error(char *s);

#define WF_ICON_NONE -1 // ICON_NONE (paint/sources/enums.h)

typedef struct {
	char *name; // "File Open...", nom de commande du moteur tk-armorpaint
	char *app;  // "Flow Production Tracking Workfiles", "" pour les commandes du moteur
	bool  context_menu;
	bool  favourite;
} wf_command_t;

// Une commande Toolkit à la fois : la requête show_app dure jusqu'à la fermeture de sa fenêtre (modale, dans le
// sidecar). Le contexte de la dernière (projet, tâche) est repris par la suivante, comme le ferait un DCC resté dans
// sa tâche.
static struct {
	int           job;        // show_app en cours, 0 sinon
	int           project_id; // 0 : inconnu (le sidecar prend le projet de lancement ou celui de la dernière tâche)
	int           task_id;
	char         *label;      // "Demo / robot / Texture / texturing", pour le titre de la fenêtre et le menu
	char         *request;    // requête host relevée, traitée à la frame suivante
	int           job_list;   // list_commands en cours
	wf_command_t *commands;   // le menu ShotGrid, dans l'ordre du moteur
	int           command_count;
	bool          listed;     // commands est à jour (sinon relu à la prochaine ouverture du menu)
	char         *menu_error; // échec de list_commands
	bool          menu_drawn; // le menu ShotGrid a été dessiné depuis la dernière frame
} wf;

static const char *basename_of(const char *path) {
	const char *slash = strrchr(path, '/');
#ifdef _WIN32
	const char *back = strrchr(path, '\\');
	if (back != NULL && (slash == NULL || back > slash)) {
		slash = back;
	}
#endif
	return slash != NULL ? slash + 1 : path;
}

// Texte de la valeur à `pointer` (malloc), "" si absente.
static char *json_text(const char *json, const char *pointer) {
	studio_json_value_t v = studio_json_find(json, pointer);
	char               *s = v.type == STUDIO_JSON_STRING ? studio_json_text(v) : NULL;
	return s != NULL ? s : strdup("");
}

// Contexte ShotGrid d'une réponse ou d'une requête ({"project_id","task_id","label",...} sous `pointer`).
static void remember_context(const char *json, const char *pointer) {
	if (studio_json_find(json, pointer).type != STUDIO_JSON_OBJECT) {
		return;
	}
	wf.project_id = (int)studio_json_int(studio_json_find(json, string_tmp("%s/project_id", pointer)), 0);
	wf.task_id    = (int)studio_json_int(studio_json_find(json, string_tmp("%s/task_id", pointer)), 0);
	free(wf.label);
	wf.label = json_text(json, string_tmp("%s/label", pointer));
}

static void set_title(const char *path) {
	const char *file = path[0] != 0 ? basename_of(path) : "untitled";
	if (wf.label != NULL && wf.label[0] != 0) {
		sys_title_set(string_tmp("%s - %s - %s", file, wf.label, manifest_title));
	}
	else {
		sys_title_set(string_tmp("%s - %s", file, manifest_title));
	}
}

// ---- Commandes Toolkit ----------------------------------------------------------------------------------------------

// Arguments d'une requête dans le contexte courant, avec en tête `first` (membres JSON, ou "").
static char *context_args(const char *first) {
	const char *sep = first[0] != 0 ? "," : "";
	return wf.task_id > 0      ? string_tmp("{%s%s\"task_id\":%d}", first, sep, wf.task_id)
	       : wf.project_id > 0 ? string_tmp("{%s%s\"project_id\":%d}", first, sep, wf.project_id)
	                           : string_tmp("{%s}", first);
}

static void show(const char *command) {
	if (wf.job != 0) {
		iron_log("studio: workfiles: a ShotGrid window is already open");
		return;
	}
	char *name = studio_json_quote(command);
	char *args = context_args(string_tmp("\"command\":%s", name));
	free(name);
	// Sans délai : la fenêtre reste ouverte tant que l'artiste travaille dedans. Un job, jamais studio_sg_run : les
	// requêtes host de la fenêtre sont traitées par ce thread (workfiles_update).
	wf.job = studio_sg_start("show_app", args, -1);
	iron_log("studio: workfiles: show_app %s", args);
}

static void on_show_done(int job) {
	wf.listed = false; // le contexte a pu changer, et avec lui les commandes de l'environnement
	if (studio_sg_ok(job)) {
		remember_context(studio_sg_result(job), "/data/context");
		return;
	}
	const char *code = studio_sg_get(job, "/error/code");
	if (strcmp(code, "cancelled") != 0) {
		console_error(string_tmp("ShotGrid Workfiles: %s: %s", code, studio_sg_get(job, "/error/message")));
	}
}

static void clear_commands(void) {
	for (int i = 0; i < wf.command_count; ++i) {
		free(wf.commands[i].name), free(wf.commands[i].app);
	}
	free(wf.commands);
	wf.commands      = NULL;
	wf.command_count = 0;
}

static void list_commands(void) {
	if (wf.job_list == 0) {
		wf.job_list = studio_sg_start("list_commands", context_args(""), 0);
	}
}

static void on_commands(int job) {
	const char *result = studio_sg_result(job);
	free(wf.menu_error);
	wf.menu_error = NULL;
	if (!studio_sg_ok(job)) {
		wf.menu_error = strdup(string_tmp("%s: %s", studio_sg_get(job, "/error/code"), studio_sg_get(job, "/error/message")));
		wf.listed     = true; // pas de nouvel essai à chaque frame : Retry dans le menu
		return;
	}
	remember_context(result, "/data/context");
	clear_commands();
	int count   = studio_sg_length(job, "/data/commands");
	wf.commands = calloc(count > 0 ? count : 1, sizeof(wf_command_t));
	for (int i = 0; i < count; ++i) {
		wf_command_t *c = &wf.commands[wf.command_count++];
		c->name         = json_text(result, string_tmp("/data/commands/%d/name", i));
		c->app          = json_text(result, string_tmp("/data/commands/%d/app", i));
		c->context_menu = strcmp(studio_sg_get(job, string_tmp("/data/commands/%d/type", i)), "context_menu") == 0;
		for (int f = 0; f < studio_sg_length(job, "/data/favourites"); ++f) {
			if (strcmp(studio_sg_get(job, string_tmp("/data/favourites/%d", f)), c->name) == 0) {
				c->favourite = true;
			}
		}
	}
	wf.listed = true;
}

// ---- Menu ShotGrid ---------------------------------------------------------------------------------------------------
// Celui des moteurs Toolkit des DCC : le contexte et ses commandes, les favoris de la config (menu_favourites), puis
// les autres commandes des apps. Dessiné par ui_menubar_draw_category_items (STUDIO-PATCH(menubar-hook)).

static const char *shortcut_of(const char *command) {
	return strcmp(command, "File Open...") == 0 ? "ctrl+alt+o" : strcmp(command, "File Save...") == 0 ? "ctrl+alt+s" : "";
}

static void menu_commands(bool context_menu, bool favourite) {
	for (int i = 0; i < wf.command_count; ++i) {
		wf_command_t *c = &wf.commands[i];
		if (c->context_menu == context_menu && (context_menu || c->favourite == favourite)) {
			if (ui_menu_button(c->name, (char *)shortcut_of(c->name), WF_ICON_NONE)) {
				show(c->name);
			}
		}
	}
}

static int menu_count(bool context_menu, bool favourite) {
	int n = 0;
	for (int i = 0; i < wf.command_count; ++i) {
		n += wf.commands[i].context_menu == context_menu && (context_menu || wf.commands[i].favourite == favourite);
	}
	return n;
}

static void shotgrid_menu_draw(void) {
	wf.menu_drawn = true;
	if (!wf.listed && wf.job == 0) {
		list_commands(); // à la première ouverture, puis après chaque commande (le contexte a pu changer)
	}
	ui_menu_label(wf.label != NULL && wf.label[0] != 0 ? wf.label : "ShotGrid", NULL);
	if (wf.menu_error != NULL && wf.command_count == 0) {
		ui_menu_label(wf.menu_error, NULL);
		if (ui_menu_button("Retry", "", WF_ICON_NONE)) {
			studio_sg_stop(); // relance le sidecar : la session ShotGrid a pu être ouverte entre-temps
			free(wf.menu_error);
			wf.menu_error = NULL;
			list_commands();
		}
		return;
	}
	if (wf.command_count == 0) {
		ui_menu_label("Loading ShotGrid...", NULL);
		return;
	}
	bool enabled  = g_ui->enabled;
	g_ui->enabled = wf.job == 0; // une fenêtre Toolkit est ouverte : elle est modale
	menu_commands(true, false);
	if (menu_count(false, true) > 0) {
		ui_menu_separator();
		menu_commands(false, true);
	}
	if (menu_count(false, false) > 0) {
		ui_menu_separator();
		menu_commands(false, false);
	}
	g_ui->enabled = enabled;
	if (wf.job != 0) {
		ui_menu_label("A ShotGrid window is open", NULL);
	}
}

// ---- Requêtes host ---------------------------------------------------------------------------------------------------

static void reply_ok(int host, const char *data_json) {
	studio_sg_host_reply(string_tmp("{\"host\":%d,\"ok\":true,\"data\":%s}", host, data_json));
}

static void reply_error(int host, const char *code, const char *message) {
	char *m = studio_json_quote(message);
	studio_sg_host_reply(string_tmp("{\"host\":%d,\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":%s}}", host, code, m));
	free(m);
}

static bool is_arm(const char *path) {
	size_t n = strlen(path);
	return n > 4 && strcmp(path + n - 4, ".arm") == 0;
}

// Même moment que la sauvegarde d'ArmorPaint (project_save_on_next_frame) : hors du dessin de l'interface.
static void handle_request(void *data) {
	(void)data;
	char *req   = wf.request;
	wf.request  = NULL;
	int   host  = (int)studio_json_int(studio_json_find(req, "/host"), 0);
	char *op    = json_text(req, "/op");
	char *path  = json_text(req, "/args/path");
	remember_context(req, "/args/context");

	if (strcmp(op, "current_path") == 0) {
		char *p = studio_json_quote(project_filepath_get());
		reply_ok(host, string_tmp("{\"path\":%s}", p));
		free(p);
	}
	else if (strcmp(op, "open") == 0) {
		if (!is_arm(path)) {
			reply_error(host, "bad_request", string_tmp("not an ArmorPaint project (.arm): %s", path));
		}
		else {
			iron_log("studio: workfiles: open %s", path);
			project_open_on_file_picked(path); // synchrone ; filepath et titre posés par l'import
			set_title(path);
			reply_ok(host, "null");
		}
	}
	else if (strcmp(op, "save") == 0 || strcmp(op, "save_as") == 0) {
		if (strcmp(op, "save_as") == 0) {
			if (!is_arm(path)) {
				reply_error(host, "bad_request", string_tmp("not an ArmorPaint project (.arm): %s", path));
				goto done;
			}
			project_filepath_set(path);
		}
		char *target = project_filepath_get();
		if (target[0] == 0) {
			reply_error(host, "no_path", "the project has never been saved: use File Save");
			goto done;
		}
		iron_log("studio: workfiles: save %s", target);
		export_arm_run_project(target); // synchrone : le fichier est écrit quand workfiles2 reçoit la réponse
		set_title(target);
		reply_ok(host, "null");
	}
	else if (strcmp(op, "new") == 0) {
		iron_log("studio: workfiles: new project");
		script_project_new();
		set_title("");
		reply_ok(host, "null");
	}
	else {
		reply_error(host, "bad_request", string_tmp("unknown host operation: %s", op));
	}
done:
	free(op);
	free(path);
	free(req);
}

// ---- Frame ------------------------------------------------------------------------------------------------------------

static void workfiles_update(void *data) {
	(void)data;
	if (wf.job != 0 && studio_sg_state(wf.job) == STUDIO_SG_DONE) {
		on_show_done(wf.job);
		studio_sg_release(wf.job);
		wf.job = 0;
	}
	bool menu_open = ui_menu_show && wf.menu_drawn;
	wf.menu_drawn  = false;
	if (wf.job_list != 0 && studio_sg_state(wf.job_list) == STUDIO_SG_DONE) {
		on_commands(wf.job_list);
		studio_sg_release(wf.job_list);
		wf.job_list = 0;
		if (menu_open) {
			ui_menu_show_first = true; // les commandes remplacent "Loading ShotGrid..."
		}
	}
	if (wf.request == NULL && (wf.request = studio_sg_host_take()) != NULL) {
		sys_notify_on_next_frame(handle_request, NULL);
	}

	if (keyboard_down("control") && keyboard_down("alt") && !g_ui->is_typing) {
		if (keyboard_started("o")) {
			show("open");
		}
		else if (keyboard_started("s")) {
			show("save");
		}
	}
}

static void workfiles_on_start(void *data) {
	(void)data;
	show("open");
}

void studio_workfiles_init(void) {
	ui_menubar_add_category("ShotGrid", shotgrid_menu_draw);
	sys_notify_on_update(workfiles_update, NULL);
	const char *on_start = getenv("STUDIO_SG_WORKFILES_ON_START");
	if (on_start != NULL && strcmp(on_start, "1") == 0) {
		sys_notify_on_next_frame(workfiles_on_start, NULL); // après base_init() (et sa boîte des projets récents)
	}
}
