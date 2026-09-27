#include "studio_sg_context.h"
#include "iron_array.h"
#include "iron_string.h"
#include "iron_system.h"
#include "studio.h"
#include "studio_json.h"
#include "studio_project.h"
#include "studio_sg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Cœur ArmorPaint (paint/sources/globals.h)
extern char *manifest_title;

#define CONTEXT_ENTRY   "studio_shotgrid.json"
#define CONTEXT_VERSION 1
#define TITLE_MAX       1000 // _sys_window_title : 1024 octets (base/sources/iron_system.c)
#define LABEL_MAX       300

static struct {
	char *json; // NULL : pas de contexte
	int   project_id;
	int   task_id;
	char *label;
	char *site;
	char  base[1024];  // titre posé par ArmorPaint, sans le contexte
	char  shown[1024]; // titre posé ici
} cx;

// Coupe s à au plus max octets sans couper un caractère UTF-8.
static void utf8_cut(char *s, size_t max) {
	size_t n = strlen(s);
	if (n <= max) {
		return;
	}
	while (max > 0 && ((unsigned char)s[max] & 0xC0) == 0x80) {
		max--;
	}
	s[max] = 0;
}

static char *string_at(const char *json, const char *pointer) {
	studio_json_value_t v = studio_json_find(json, pointer);
	char               *s = v.type == STUDIO_JSON_STRING ? studio_json_text(v) : NULL;
	return s != NULL ? s : strdup("");
}

void studio_sg_context_clear(void) {
	free(cx.json), free(cx.label), free(cx.site);
	cx.json       = NULL;
	cx.label      = NULL;
	cx.site       = NULL;
	cx.project_id = 0;
	cx.task_id    = 0;
}

void studio_sg_context_set(const char *json, const char *pointer) {
	studio_json_value_t v = studio_json_find(json, pointer);
	if (v.type != STUDIO_JSON_OBJECT) {
		return;
	}
	char *object  = studio_json_text(v);
	int   project = (int)studio_json_int(studio_json_find(object, "/project_id"), 0);
	if (object == NULL || project <= 0) {
		free(object);
		studio_sg_context_clear();
		return;
	}
	for (char *c = object; *c; c++) { // une ligne (journal d'audit, entrée du projet)
		if (*c == '\n' || *c == '\r') {
			*c = ' ';
		}
	}
	bool same = cx.json != NULL && strcmp(cx.json, object) == 0;
	studio_sg_context_clear();
	cx.json       = object;
	cx.project_id = project;
	cx.task_id    = (int)studio_json_int(studio_json_find(object, "/task_id"), 0);
	cx.label      = string_at(object, "/label");
	cx.site       = string_at(object, "/site");
	if (!same) {
		iron_log("studio: shotgrid context: %s (project %d, task %d, %s)", cx.label, cx.project_id, cx.task_id, cx.site[0] != 0 ? cx.site : "site unknown");
	}
}

const char *studio_sg_context_json(void) {
	return cx.json;
}

int studio_sg_context_project_id(void) {
	return cx.project_id;
}

int studio_sg_context_task_id(void) {
	return cx.task_id;
}

const char *studio_sg_context_label(void) {
	return cx.label != NULL ? cx.label : "";
}

const char *studio_sg_context_site(void) {
	return cx.site != NULL ? cx.site : "";
}

char *studio_sg_context_args(const char *first) {
	const char *sep  = first[0] != 0 ? "," : "";
	char       *site = cx.site != NULL && cx.site[0] != 0 ? studio_json_quote(cx.site) : NULL;
	char       *ids  = cx.task_id > 0      ? string_tmp("%s\"task_id\":%d", sep, cx.task_id)
	                   : cx.project_id > 0 ? string_tmp("%s\"project_id\":%d", sep, cx.project_id)
	                                       : "";
	char       *args = string_tmp("{%s%s%s%s}", first, ids, site != NULL ? ",\"site\":" : "", site != NULL ? site : "");
	free(site);
	return args;
}

// ---- Projet -----------------------------------------------------------------------------------------------------------

static void on_change(bool opened) {
	if (!opened) {
		return; // nouveau projet : il garde le contexte
	}
	char *entry = studio_project_entry(CONTEXT_ENTRY);
	if (entry != NULL && studio_json_int(studio_json_find(entry, "/version"), 0) == CONTEXT_VERSION &&
	    studio_json_find(entry, "/context").type == STUDIO_JSON_OBJECT) {
		studio_sg_context_set(entry, "/context");
	}
	else {
		if (entry != NULL) {
			iron_log("studio: shotgrid context of the project ignored (unknown version): %.200s", entry);
		}
		if (cx.json != NULL) {
			iron_log("studio: shotgrid context cleared: the opened project has none");
		}
		studio_sg_context_clear();
	}
	free(entry);
}

static void on_save(string_array_t *texture_files) {
	(void)texture_files;
	char *entry = cx.json != NULL ? string_tmp("{\"version\":%d,\"context\":%s}", CONTEXT_VERSION, cx.json) : NULL;
	if (!studio_project_set_entry(CONTEXT_ENTRY, entry)) {
		iron_log("studio: shotgrid context not saved in the project (out of memory)");
	}
}

// ---- Titre de la fenêtre ------------------------------------------------------------------------------------------------
// ArmorPaint réécrit le titre à l'ouverture, à l'enregistrement et à chaque action (« robot.v003* - ArmorPaint ») : le
// contexte est ajouté à chaque frame, avant « - ArmorPaint ». Sur mobile, le titre sert de nom de fichier : pas touché.

static void decorate(const char *base, char *out, size_t size) {
	char label[LABEL_MAX + 1] = "";
	if (cx.label != NULL && cx.label[0] != 0) {
		snprintf(label, sizeof(label), "%s", cx.label);
		utf8_cut(label, LABEL_MAX);
	}
	bool offline = studio_sg_network(NULL) == STUDIO_SG_NET_OFFLINE;
	char suffix[LABEL_MAX + 32];
	snprintf(suffix, sizeof(suffix), "%s%s%s", label[0] != 0 ? " - " : "", label, offline ? " (ShotGrid offline)" : "");
	size_t app = strlen(manifest_title), n = strlen(base);
	if (suffix[0] == 0) {
		snprintf(out, size, "%s", base);
	}
	else if (n > app + 3 && strcmp(base + n - app, manifest_title) == 0 && strncmp(base + n - app - 3, " - ", 3) == 0) {
		snprintf(out, size, "%.*s%s - %s", (int)(n - app - 3), base, suffix, manifest_title);
	}
	else {
		snprintf(out, size, "%s%s", base, suffix);
	}
	utf8_cut(out, TITLE_MAX);
}

static void context_update(void *data) {
	(void)data;
#if !defined(IRON_ANDROID) && !defined(IRON_IOS)
	const char *title = sys_title();
	if (strcmp(title, cx.shown) != 0) {
		snprintf(cx.base, sizeof(cx.base), "%s", title); // posé par ArmorPaint
	}
	char want[1024];
	decorate(cx.base, want, sizeof(want));
	if (strcmp(want, title) != 0) {
		sys_title_set(want);
	}
	snprintf(cx.shown, sizeof(cx.shown), "%s", want);
#endif
}

// ---- Scripting (minic) -----------------------------------------------------------------------------------------------

#ifdef WITH_EVAL

#include "minic.h"

static minic_val_t mn_studio_sg_context(minic_val_t *a, int n) {
	(void)a, (void)n;
	return minic_val_ptr((void *)(cx.json != NULL ? cx.json : ""));
}

// studio_sg_context_set_json("{\"project_id\":12,...}") pose le contexte, "" le retire.
static minic_val_t mn_studio_sg_context_set_json(minic_val_t *a, int n) {
	const char *json = minic_arg_p(a, n, 0);
	if (json == NULL || json[0] == 0) {
		studio_sg_context_clear();
	}
	else {
		studio_sg_context_set(json, "");
	}
	return minic_val_int(cx.json != NULL ? 1 : 0);
}

static void context_register_script_api(void) {
	minic_register("studio_sg_context", "p()", mn_studio_sg_context);
	minic_register("studio_sg_context_set_json", "i(p)", mn_studio_sg_context_set_json);
}

#endif

void studio_sg_context_init(void) {
	studio_project_on_change(on_change);
	studio_project_on_save(on_save);
	sys_notify_on_update(context_update, NULL);
#ifdef WITH_EVAL
	context_register_script_api();
#endif
}
