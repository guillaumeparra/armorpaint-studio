#include "studio_ocio_prefs.h"
#include "iron_array.h"
#include "iron_draw.h"
#include "iron_gpu.h"
#include "iron_map.h"
#include "iron_obj.h"
#include "iron_string.h"
#include "iron_system.h"
#include "iron_ui.h"
#include "engine.h"
#include "../../sources/types.h"
#include "studio.h"
#include "studio_json.h"
#include "studio_ocio.h"
#include "studio_ocio_import.h"
#include "studio_prefs.h"
#include "studio_project.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Cœur ArmorPaint (paint/sources/globals.h, ui/) ; box_preferences_add_tab est le point d'injection
// STUDIO-PATCH(preferences-tab) de ui/box_preferences.c (PATCHES.md).
extern ui_t *g_ui;
extern bool  ui_box_show;
extern bool  args_background;
extern i32   box_preferences_tab;
extern bool  ui_menu_keep_open;
i32          box_preferences_add_tab(char *label, void (*draw)(void));
void         box_preferences_show(void);
void         console_info(char *s);
void         console_error(char *s);
void         ui_box_show_message(char *title, char *text, bool copyable);
void         ui_files_show(char *filters, bool is_save, bool open_multiple, void (*files_done)(char *));
bool         ui_icon_button(char *text, int icon, int align);
bool         ui_menu_button(char *text, char *label, int icon);
bool         ui_menu_sub_button(char *text);
void         ui_menu_sub_begin(i32 items);
void         ui_menu_sub_end(void);

#define OP_ICON_NONE        -1 // paint/sources/enums.h
#define OP_ICON_FOLDER_OPEN 20
#define OP_ICON_REPLAY      122

#define PREF_CONFIG        "ocio.config"
#define PREF_WORKING_SPACE "ocio.working_space"
#define ENTRY              "studio_ocio.json" // entrée du paquet d'assets du .arm
#define TEXT_WIDTH         100                // caractères par ligne des textes longs de l'onglet
#define BOX_WIDTH          80                 // et de la boîte du démarrage (police à chasse fixe)

static struct {
	int             serial, generation; // projet et génération OCIO dans lesquels les textures ont été décodées
	bool            startup_done;
	int             tab;           // index de l'onglet Color des préférences, -1 sans
	char           *config_text;   // champ Config (ui_text_input le remplace sans libérer l'ancien)
	char           *config_synced; // préférence montrée dans le champ
	string_array_t *spaces;        // "Automatic", puis les espaces proposés comme espace de travail
	int             spaces_generation;
	bool            spaces_dirty;
	int             ws_choice; // valeur du combo (adresse stable : identifiant du widget)
} op = {.serial = -1, .generation = -1, .tab = -1, .spaces_generation = -1};

// console_* passe le message à iron_log comme chaîne de format : les '%' doivent être doublés.
static void report(bool error, const char *format, ...) {
	char    message[2048];
	char    escaped[4096];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	size_t j = 0;
	for (size_t i = 0; message[i] != 0 && j + 2 < sizeof(escaped); i++) {
		if (message[i] == '%') {
			escaped[j++] = '%';
		}
		escaped[j++] = message[i];
	}
	escaped[j] = 0;
	if (error) {
		console_error(escaped);
	}
	else {
		console_info(escaped);
	}
}

static char *dup(const char *s) {
	return strdup(s != NULL ? s : "");
}

// ── textures ──────────────────────────────────────────────────────────────────────────────────────────────────────

// La génération OCIO a changé dans le même projet (configuration rechargée, espace de travail changé) : les textures
// converties le sont à nouveau. Un projet qu'on ouvre ou qu'on crée est décodé directement dans son espace.
static void sync_textures(void) {
	studio_project_check();
	int generation = studio_ocio_generation();
	if (op.serial != studio_project_serial()) {
		op.serial     = studio_project_serial();
		op.generation = generation;
		return;
	}
	if (op.generation == generation) {
		return;
	}
	op.generation = generation;
	int n         = studio_ocio_import_reconvert();
	if (n > 0) {
		report(false, "studio: %d texture(s) converted to the working space \"%s\", layers kept as painted", n, studio_ocio_working_space());
	}
}

// ── projet ────────────────────────────────────────────────────────────────────────────────────────────────────────

static void load_entry(void) {
	char *json = studio_project_entry(ENTRY);
	if (json == NULL) { // projet d'avant le lot A5, ou d'ArmorPaint sans studio : ses couleurs sont celles d'ArmorPaint
		studio_ocio_set_project_working_space(STUDIO_OCIO_WS_AUTOMATIC, NULL);
		return;
	}
	char *config  = NULL;
	char *name    = NULL;
	bool  is_auto = true;
	if (studio_json_int(studio_json_find(json, "/version"), 0) != 1) {
		report(true, "studio: %s: unknown version, the project gets the automatic working space", ENTRY);
	}
	else {
		config  = studio_json_text(studio_json_find(json, "/config"));
		name    = studio_json_text(studio_json_find(json, "/working_space"));
		is_auto = name == NULL || name[0] == 0 || studio_json_truthy(studio_json_find(json, "/working_space_auto"));
	}
	studio_ocio_set_project_working_space(is_auto ? STUDIO_OCIO_WS_AUTOMATIC : STUDIO_OCIO_WS_NAMED, name);
	if (config != NULL && config[0] != 0 && studio_ocio_ready() && strcmp(config, studio_ocio_config_name()) != 0) {
		report(false, "studio: this project was made with the OCIO config \"%s\", current is \"%s\"", config, studio_ocio_config_name());
	}
	if (studio_ocio_ready() && studio_ocio_working_space() == NULL) {
		report(true, "studio: %s", studio_ocio_working_space_error());
	}
	free(config);
	free(name);
	free(json);
}

static void on_project_change(bool opened) {
	if (opened) {
		load_entry();
	}
	else {
		studio_ocio_set_project_working_space(STUDIO_OCIO_WS_DEFAULT, NULL);
	}
	op.serial       = studio_project_serial();
	op.generation   = studio_ocio_generation();
	op.spaces_dirty = true;
}

// Avant l'encodage du projet : l'espace de travail trouvé est écrit et figé pour ce projet.
static void on_save(string_array_t *texture_files) {
	(void)texture_files;
	if (!studio_ocio_ready() || studio_ocio_working_space() == NULL) {
		return; // rien de sûr à écrire : l'entrée du projet ouvert, s'il en a une, reste telle quelle
	}
	char ws[256];
	snprintf(ws, sizeof(ws), "%s", studio_ocio_working_space());
	bool  is_auto = studio_ocio_working_space_auto();
	char *config  = studio_json_quote(studio_ocio_config_name());
	char *source  = studio_json_quote(studio_ocio_config_source());
	char *name    = studio_json_quote(ws);
	char *json    = config != NULL && source != NULL && name != NULL
	                    ? string("{\n\t\"version\": 1,\n\t\"config\": %s,\n\t\"config_source\": %s,\n\t\"working_space\": %s,\n\t\"working_space_auto\": %s\n}\n",
	                             config, source, name, is_auto ? "true" : "false")
	                    : NULL;
	if (json == NULL || !studio_project_set_entry(ENTRY, json)) {
		report(true, "studio: working space not saved in the project (out of memory)");
	}
	else { // rouvert, le projet garde cet espace, quels que soient la préférence et l'environnement
		studio_ocio_set_project_working_space(is_auto ? STUDIO_OCIO_WS_AUTOMATIC : STUDIO_OCIO_WS_NAMED, ws);
	}
	free(json);
	free(config);
	free(source);
	free(name);
}

// ── réglages ──────────────────────────────────────────────────────────────────────────────────────────────────────

bool studio_ocio_prefs_set_config(const char *path) {
	const char *p = path != NULL ? path : "";
	studio_prefs_set_string(PREF_CONFIG, p[0] != 0 ? p : NULL);
	studio_ocio_set_config_preference(p);
	const char *env = getenv("OCIO");
	if (env != NULL && env[0] != 0) {
		report(false, "studio: OCIO config preference kept for sessions without $OCIO (%s)", env);
		return studio_ocio_ready();
	}
	bool ok = studio_ocio_reload(NULL);
	report(!ok, "studio: %s", studio_ocio_status());
	op.spaces_dirty = true;
	sync_textures();
	return ok;
}

bool studio_ocio_prefs_set_working_space(const char *name) {
	const char *n = name != NULL ? name : "";
	if (n[0] != 0 && studio_ocio_ready() && studio_ocio_resolve(n) == NULL) {
		report(true, "studio: unknown OCIO color space \"%s\", working space preference not changed", n);
		return false;
	}
	studio_prefs_set_string(PREF_WORKING_SPACE, n[0] != 0 ? n : NULL);
	studio_ocio_set_working_space_preference(n);
	op.spaces_dirty = true;
	sync_textures();
	return true;
}

bool studio_ocio_prefs_set_project_working_space(int mode, const char *name) {
	if (mode != STUDIO_OCIO_WS_DEFAULT && mode != STUDIO_OCIO_WS_AUTOMATIC && mode != STUDIO_OCIO_WS_NAMED) {
		report(true, "studio: unknown working space mode %d", mode);
		return false;
	}
	if (mode == STUDIO_OCIO_WS_NAMED && (name == NULL || name[0] == 0 || (studio_ocio_ready() && studio_ocio_resolve(name) == NULL))) {
		report(true, "studio: unknown OCIO color space \"%s\", working space of the project not changed", name != NULL ? name : "");
		return false;
	}
	studio_project_check();
	studio_ocio_set_project_working_space(mode, mode == STUDIO_OCIO_WS_NAMED ? name : NULL);
	op.spaces_dirty = true;
	sync_textures();
	if (studio_ocio_working_space() != NULL) {
		report(false, "studio: working space of the project: \"%s\" (%s)", studio_ocio_working_space(), studio_ocio_working_space_origin());
	}
	else {
		report(true, "studio: working space of the project unknown: %s", studio_ocio_working_space_error());
	}
	return true;
}

// Les widgets changent les réglages à la frame suivante, hors du dessin de l'interface.
enum {
	ACT_CONFIG,
	ACT_WORKING_SPACE,
	ACT_PROJECT
};

typedef struct {
	int   kind, mode;
	char *value;
} action_t;

static void run_action(void *data) {
	action_t *a = data;
	if (a->kind == ACT_CONFIG) {
		studio_ocio_prefs_set_config(a->value);
	}
	else if (a->kind == ACT_WORKING_SPACE) {
		studio_ocio_prefs_set_working_space(a->value);
	}
	else {
		studio_ocio_prefs_set_project_working_space(a->mode, a->value);
	}
	free(a->value);
	free(a);
}

static void schedule(int kind, int mode, const char *value) {
	action_t *a = malloc(sizeof(action_t));
	if (a != NULL) {
		*a = (action_t){.kind = kind, .mode = mode, .value = value != NULL ? dup(value) : NULL};
		sys_notify_on_next_frame(run_action, a);
	}
}

// ── interface ─────────────────────────────────────────────────────────────────────────────────────────────────────

static void push_space(const char *name) {
	if (name == NULL || name[0] == 0) {
		return;
	}
	const char *canonical = studio_ocio_resolve(name);
	const char *shown     = canonical != NULL ? canonical : name;
	for (int i = 1; i < (int)op.spaces->length; i++) {
		if (strcmp(op.spaces->buffer[i], shown) == 0) {
			return;
		}
	}
	string_array_push(op.spaces, dup(shown));
}

// Espaces proposés : linéaires de scène (ou d'encodage non déclaré), pas de donnée ; plus ceux en usage.
static void ensure_spaces(void) {
	int generation = studio_ocio_generation();
	if (op.spaces != NULL && op.spaces_generation == generation && !op.spaces_dirty) {
		return;
	}
	if (op.spaces != NULL) {
		for (int i = 0; i < (int)op.spaces->length; i++) {
			free(op.spaces->buffer[i]);
		}
		array_free(op.spaces);
		free(op.spaces);
	}
	op.spaces            = string_array_create(0);
	op.spaces_generation = generation;
	op.spaces_dirty      = false;
	string_array_push(op.spaces, dup("Automatic"));
	for (int i = 0; i < studio_ocio_colorspace_count(); i++) {
		const char *name     = studio_ocio_colorspace_name(i);
		const char *encoding = name != NULL ? studio_ocio_encoding(name) : NULL;
		if (name != NULL && studio_ocio_is_data(name) != 1 && (encoding == NULL || encoding[0] == 0 || strcmp(encoding, "scene-linear") == 0)) {
			push_space(name);
		}
	}
	push_space(studio_ocio_working_space_preference());
	push_space(studio_ocio_project_working_space_name());
	push_space(studio_ocio_working_space());
}

// Index dans op.spaces, 0 (Automatic) pour un nom vide.
static int space_index(const char *name) {
	if (name == NULL || name[0] == 0) {
		return 0;
	}
	const char *canonical = studio_ocio_resolve(name);
	const char *shown     = canonical != NULL ? canonical : name;
	for (int i = 1; i < (int)op.spaces->length; i++) {
		if (strcmp(op.spaces->buffer[i], shown) == 0) {
			return i;
		}
	}
	return 0;
}

// ui_text ne passe pas à la ligne : coupé aux fins de ligne, puis aux espaces.
static void text_lines(const char *text) {
	const char *p = text != NULL ? text : "";
	while (*p != 0) {
		size_t len = strcspn(p, "\n");
		size_t cut = len;
		if (len > TEXT_WIDTH) {
			cut = TEXT_WIDTH;
			while (cut > 0 && p[cut] != ' ') {
				cut--;
			}
			if (cut == 0) {
				cut = TEXT_WIDTH;
			}
		}
		ui_text(string_tmp("%.*s", (int)cut, p), UI_ALIGN_LEFT, 0);
		p += cut;
		while (*p == ' ' || *p == '\n') {
			p++;
		}
	}
}

// Comme box_preferences_section (static dans ui/box_preferences.c).
static void section(char *title) {
	ui_separator(8, false);
	ui_text(title, UI_ALIGN_LEFT, 0x00000000);
	ui_separator(2, true);
	ui_separator(4, false);
}

static void config_picked(char *path) {
	schedule(ACT_CONFIG, 0, path);
}

static void tab_draw(void) {
	const char *pref = studio_ocio_config_preference();
	if (op.config_synced == NULL || strcmp(op.config_synced, pref) != 0) {
		free(op.config_synced);
		free(op.config_text); // malloc, ou calloc de ui_text_input (string_copy)
		op.config_synced = dup(pref);
		op.config_text   = dup(pref);
	}
	const char *env    = getenv("OCIO");
	bool        locked = env != NULL && env[0] != 0;

	section("OCIO Config");
	ui_row(f32_array_create_from_raw_tmp((f32[]){0.6f, 0.2f, 0.2f}, 3));
	g_ui->enabled = !locked;
	ui_text_input(&op.config_text, "Config", UI_ALIGN_LEFT, !locked, false);
	if (!locked && ui_item_changed()) {
		schedule(ACT_CONFIG, 0, op.config_text);
	}
	if (ui_icon_button("Browse", OP_ICON_FOLDER_OPEN, UI_ALIGN_CENTER) && !locked) {
		ui_files_show("ocio,ocioz", false, false, config_picked);
	}
	if (ui_icon_button("Built-in", OP_ICON_REPLAY, UI_ALIGN_CENTER) && !locked) {
		schedule(ACT_CONFIG, 0, "");
	}
	g_ui->enabled = true;
	if (locked) {
		text_lines(string_tmp("$OCIO is set (%s): the config above is used in sessions without it.", env));
	}
	else if (op.config_text[0] == 0) {
		text_lines("Empty: the config built into OpenColorIO, " STUDIO_OCIO_DEFAULT_CONFIG ".");
	}
	text_lines(studio_ocio_status());

	section("Working Space");
	if (!studio_ocio_ready()) {
		text_lines("Chosen once an OCIO config is loaded.");
	}
	else {
		ensure_spaces();
		op.ws_choice = space_index(studio_ocio_working_space_preference());
		ui_combo(&op.ws_choice, op.spaces, "New Projects", true, UI_ALIGN_LEFT, true);
		if (ui_item_changed()) {
			schedule(ACT_WORKING_SPACE, 0, op.ws_choice > 0 ? op.spaces->buffer[op.ws_choice] : "");
		}
		const char *ws_env = getenv("STUDIO_OCIO_WORKING_SPACE");
		if (ws_env != NULL && ws_env[0] != 0) {
			text_lines(string_tmp("$STUDIO_OCIO_WORKING_SPACE is set (%s): new projects use it instead.", ws_env));
		}
		const char *ws = studio_ocio_working_space();
		text_lines(ws != NULL ? string_tmp("Current project: \"%s\" (%s).", ws, studio_ocio_working_space_origin())
		                      : string_tmp("Current project: working space unknown: %s", studio_ocio_working_space_error()));
		text_lines("A project keeps its working space once saved; change it in the Color menu (Working Space). Textures are then "
		           "converted again, layers are kept as painted.");
	}

	section("Studio Preferences");
	text_lines(string_tmp("%s%s%s", studio_prefs_path(), studio_prefs_error()[0] != 0 ? ": " : "", studio_prefs_error()));
}

void studio_ocio_prefs_working_space_menu_draw(void) {
	if (!studio_ocio_ready() || !ui_menu_sub_button("Working Space")) {
		return;
	}
	ensure_spaces();
	int n = (int)op.spaces->length;
	ui_menu_sub_begin(n);
	const char *ws       = studio_ocio_working_space();
	int         selected = ws == NULL ? -1 : studio_ocio_working_space_auto() ? 0 : space_index(ws);
	for (int i = 0; i < n; i++) {
		ui_radio(&selected, i, op.spaces->buffer[i], "");
		if (ui_item_changed()) {
			schedule(ACT_PROJECT, i == 0 ? STUDIO_OCIO_WS_AUTOMATIC : STUDIO_OCIO_WS_NAMED, i == 0 ? NULL : op.spaces->buffer[i]);
			ui_menu_keep_open = true;
		}
	}
	ui_menu_sub_end();
}

bool studio_ocio_prefs_show(void) {
	if (op.tab < 0) {
		return false;
	}
	box_preferences_tab = op.tab;
	box_preferences_show();
	return true;
}

void studio_ocio_prefs_menu_draw(void) {
	if (op.tab >= 0 && ui_menu_button("Color Preferences...", "", OP_ICON_NONE)) {
		studio_ocio_prefs_show();
	}
}

// Une fois, hors --background, quand aucune autre boîte n'est ouverte.
static void startup_check(void) {
	if (args_background) {
		op.startup_done = true;
		return;
	}
	if (ui_box_show) {
		return;
	}
	op.startup_done     = true;
	const char *problem = !studio_ocio_ready() ? studio_ocio_status() : studio_ocio_working_space() == NULL ? studio_ocio_working_space_error() : NULL;
	if (problem == NULL) {
		return;
	}
	char text[4096];
	snprintf(text, sizeof(text),
	         "%s\n\nColor management is off until this is fixed: the viewport uses the filmic tonemap of ArmorPaint, imported textures are "
	         "not converted, exports to OCIO color spaces are refused.\n\nChoose an OCIO config and a working space in Preferences > Color "
	         "(Color menu > Color Preferences...).",
	         problem);
	// Le cadre de la boîte ne passe pas à la ligne : coupé aux espaces.
	for (size_t start = 0, i = 0, space = 0; text[i] != 0; i++) {
		if (text[i] == '\n') {
			start = i + 1;
			space = 0;
		}
		else if (text[i] == ' ') {
			space = i;
		}
		if (i - start >= BOX_WIDTH && space > start) {
			text[space] = '\n';
			start       = space + 1;
			space       = 0;
		}
	}
	report(true, "studio: %s", problem);
	ui_box_show_message("Color Management", text, true);
}

static void on_update(void *data) {
	(void)data;
	sync_textures();
	if (!op.startup_done) {
		startup_check();
	}
}

// ── scripts ───────────────────────────────────────────────────────────────────────────────────────────────────────

#ifdef WITH_EVAL
#include "minic.h"

static minic_val_t mn_prefs_path(minic_val_t *a, int n) {
	return minic_val_ptr((void *)studio_prefs_path());
}

static minic_val_t mn_config_preference(minic_val_t *a, int n) {
	return minic_val_ptr((void *)studio_ocio_config_preference());
}

static minic_val_t mn_set_config_preference(minic_val_t *a, int n) {
	return minic_val_int(studio_ocio_prefs_set_config(minic_arg_p(a, n, 0)) ? 1 : 0);
}

static minic_val_t mn_config_origin(minic_val_t *a, int n) {
	return minic_val_ptr((void *)studio_ocio_config_origin());
}

static minic_val_t mn_working_space_preference(minic_val_t *a, int n) {
	return minic_val_ptr((void *)studio_ocio_working_space_preference());
}

static minic_val_t mn_set_working_space_preference(minic_val_t *a, int n) {
	return minic_val_int(studio_ocio_prefs_set_working_space(minic_arg_p(a, n, 0)) ? 1 : 0);
}

static minic_val_t mn_working_space_origin(minic_val_t *a, int n) {
	studio_project_check();
	return minic_val_ptr((void *)studio_ocio_working_space_origin());
}

static minic_val_t mn_working_space_error(minic_val_t *a, int n) {
	studio_project_check();
	return minic_val_ptr((void *)studio_ocio_working_space_error());
}

// Les lectures de l'état du projet détectent d'abord un projet ouvert ou créé dans la même frame.
static minic_val_t mn_project_mode(minic_val_t *a, int n) {
	studio_project_check();
	return minic_val_int(studio_ocio_project_working_space_mode());
}

static minic_val_t mn_project_name(minic_val_t *a, int n) {
	studio_project_check();
	return minic_val_ptr((void *)studio_ocio_project_working_space_name());
}

static minic_val_t mn_set_project(minic_val_t *a, int n) {
	return minic_val_int(studio_ocio_prefs_set_project_working_space(minic_arg_i(a, n, 0), minic_arg_p(a, n, 1)) ? 1 : 0);
}

static minic_val_t mn_show(minic_val_t *a, int n) {
	return minic_val_int(studio_ocio_prefs_show() ? 1 : 0);
}

static void register_script_api(void) {
	// Signatures au format de minic_register ; la référence est dans README.md.
	minic_register("studio_prefs_path", "p()", mn_prefs_path);
	minic_register("studio_ocio_config_preference", "p()", mn_config_preference);
	minic_register("studio_ocio_set_config_preference", "i(p)", mn_set_config_preference);
	minic_register("studio_ocio_config_origin", "p()", mn_config_origin);
	minic_register("studio_ocio_working_space_preference", "p()", mn_working_space_preference);
	minic_register("studio_ocio_set_working_space_preference", "i(p)", mn_set_working_space_preference);
	minic_register("studio_ocio_working_space_origin", "p()", mn_working_space_origin);
	minic_register("studio_ocio_working_space_error", "p()", mn_working_space_error);
	minic_register("studio_ocio_project_working_space_mode", "i()", mn_project_mode);
	minic_register("studio_ocio_project_working_space_name", "p()", mn_project_name);
	minic_register("studio_ocio_set_project_working_space", "i(i,p)", mn_set_project);
	minic_register("studio_ocio_prefs_show", "i()", mn_show);
}
#endif

void studio_ocio_prefs_init(void) {
	studio_ocio_set_config_preference(studio_prefs_string(PREF_CONFIG, ""));
	studio_ocio_set_working_space_preference(studio_prefs_string(PREF_WORKING_SPACE, ""));
	studio_project_on_change(on_project_change);
	studio_project_on_save(on_save);
	sys_notify_on_update(on_update, NULL);
	op.tab = box_preferences_add_tab("Color", tab_draw);
#ifdef WITH_EVAL
	register_script_api();
#endif
}
