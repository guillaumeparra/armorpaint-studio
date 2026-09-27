#include "studio_prefs.h"
#include "iron_path.h"
#include "studio.h"
#include "studio_fs.h"
#include "studio_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// iron (base/sources/iron_system.h, iron.h)
const char *iron_internal_save_path(void);
int         iron_get_arg_count(void);
char       *iron_get_arg(int index);

#define PREFS_FILE    "studio_prefs.json"
#define PREFS_VERSION 1

typedef struct {
	char *key;
	char *literal; // valeur JSON telle qu'écrite dans le fichier
	char *text;    // chaîne décodée si c'en est une, NULL sinon
} pref_t;

static struct {
	pref_t *items;
	int     count, cap;
	char    path[4096];
	bool    explicit_path; // $STUDIO_PREFS
	bool    ignored;       // --background sans $STUDIO_PREFS : ni lues ni écrites
	bool    unreadable;    // fichier présent mais illisible : mis de côté avant la première écriture
	char    error[1024];
} sp;

static const char *skip_ws(const char *p) {
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
		p++;
	}
	return p;
}

// path_data() donne ".../out//./data//" : séparateurs doublés et "/./" retirés (un "//" en tête, chemin UNC, reste).
static void clean_path(char *path) {
	char *w = path;
	for (char *r = path; *r != 0; r++) {
		bool sep = *r == '/' || *r == '\\';
		if (sep && r[1] == '.' && (r[2] == '/' || r[2] == '\\')) {
			r++;
			continue;
		}
		if (sep && (r[1] == '/' || r[1] == '\\') && r > path) {
			continue;
		}
		*w++ = *r;
	}
	*w = 0;
}

static pref_t *find(const char *key) {
	for (int i = 0; i < sp.count; i++) {
		if (strcmp(sp.items[i].key, key) == 0) {
			return &sp.items[i];
		}
	}
	return NULL;
}

static void item_free(pref_t *p) {
	free(p->key);
	free(p->literal);
	free(p->text);
}

static void clear(void) {
	for (int i = 0; i < sp.count; i++) {
		item_free(&sp.items[i]);
	}
	sp.count = 0;
}

// Prend possession de literal.
static bool put(const char *key, char *literal) {
	char *text = NULL;
	if (literal[0] == '"') {
		text = studio_json_text(studio_json_find(literal, ""));
		if (text == NULL) {
			free(literal);
			return false;
		}
	}
	pref_t *p = find(key);
	if (p == NULL) {
		if (sp.count == sp.cap) {
			int     cap   = sp.cap > 0 ? sp.cap * 2 : 16;
			pref_t *grown = realloc(sp.items, sizeof(pref_t) * (size_t)cap);
			if (grown == NULL) {
				free(literal);
				free(text);
				return false;
			}
			sp.items = grown;
			sp.cap   = cap;
		}
		char *k = strdup(key);
		if (k == NULL) {
			free(literal);
			free(text);
			return false;
		}
		p  = &sp.items[sp.count++];
		*p = (pref_t){.key = k};
	}
	else {
		free(p->literal);
		free(p->text);
	}
	p->literal = literal;
	p->text    = text;
	return true;
}

static void remove_key(const char *key) {
	pref_t *p = find(key);
	if (p != NULL) {
		item_free(p);
		int i = (int)(p - sp.items);
		memmove(p, p + 1, sizeof(pref_t) * (size_t)(sp.count - i - 1));
		sp.count--;
	}
}

// Objet plat : chaque membre gardé tel qu'écrit (valeur JSON brute), sauf "version".
static bool parse(const char *json) {
	const char *p = skip_ws(json);
	if (*p != '{') {
		return false;
	}
	p = skip_ws(p + 1);
	if (*p == '}') {
		return true;
	}
	for (;;) {
		studio_json_type_t type;
		const char        *end = studio_json_skip(p, &type);
		if (end == NULL || type != STUDIO_JSON_STRING) {
			return false;
		}
		char *key = studio_json_text((studio_json_value_t){.start = p, .len = (size_t)(end - p), .type = STUDIO_JSON_STRING});
		if (key == NULL) {
			return false;
		}
		p = skip_ws(end);
		if (*p != ':') {
			free(key);
			return false;
		}
		p   = skip_ws(p + 1);
		end = studio_json_skip(p, &type);
		if (end == NULL) {
			free(key);
			return false;
		}
		if (strcmp(key, "version") != 0) {
			char *literal = strndup(p, (size_t)(end - p));
			if (literal == NULL || !put(key, literal)) {
				free(key);
				return false;
			}
		}
		free(key);
		p = skip_ws(end);
		if (*p == ',') {
			p = skip_ws(p + 1);
			continue;
		}
		return *p == '}';
	}
}

static void load(void) {
	size_t size = 0;
	char  *json = studio_fs_read(sp.path, &size);
	if (json == NULL) {
		if (studio_fs_exists(sp.path)) {
			snprintf(sp.error, sizeof(sp.error), "%s is not readable, defaults used", sp.path);
			sp.unreadable = true;
		}
		return;
	}
	if (!parse(json)) {
		clear();
		snprintf(sp.error, sizeof(sp.error), "%s is not a JSON object of preferences, defaults used", sp.path);
		sp.unreadable = true;
	}
	free(json);
}

static bool save(void) {
	if (sp.ignored) {
		return true;
	}
	size_t cap = 64;
	for (int i = 0; i < sp.count; i++) {
		cap += strlen(sp.items[i].key) * 6 + strlen(sp.items[i].literal) + 16;
	}
	char *out = malloc(cap);
	if (out == NULL) {
		return false;
	}
	size_t len = (size_t)snprintf(out, cap, "{\n\t\"version\": %d", PREFS_VERSION);
	for (int i = 0; i < sp.count; i++) {
		char *key = studio_json_quote(sp.items[i].key);
		if (key == NULL) {
			free(out);
			return false;
		}
		len += (size_t)snprintf(out + len, cap - len, ",\n\t%s: %s", key, sp.items[i].literal);
		free(key);
	}
	len += (size_t)snprintf(out + len, cap - len, "\n}\n");
	char aside[4200], tmp[4200];
	snprintf(aside, sizeof(aside), "%s.bak", sp.path);
	snprintf(tmp, sizeof(tmp), "%s.tmp", sp.path);
	if (sp.unreadable && studio_fs_exists(sp.path)) {
		studio_fs_rename(sp.path, aside);
		iron_log("studio: preferences: unreadable %s kept as %s", sp.path, aside);
	}
	sp.unreadable = false;
	bool ok       = studio_fs_write(tmp, out, len) && studio_fs_rename(tmp, sp.path);
	free(out);
	if (!ok) {
		studio_fs_remove(tmp);
		snprintf(sp.error, sizeof(sp.error), "cannot write %s", sp.path);
		iron_log("studio: preferences: %s", sp.error);
		return false;
	}
	sp.error[0] = 0;
	return true;
}

// ── API ────────────────────────────────────────────────────────────────────────────────────────────────────────────

// args_background n'est pas encore lu quand external_init() tourne (args_parse, dans base_init).
static bool in_background(void) {
	for (int i = 1; i < iron_get_arg_count(); i++) {
		if (strcmp(iron_get_arg(i), "--background") == 0) {
			return true;
		}
	}
	return false;
}

void studio_prefs_init(void) {
	const char *env = getenv("STUDIO_PREFS");
	if (env != NULL && env[0] != 0) {
		snprintf(sp.path, sizeof(sp.path), "%s", env);
		sp.explicit_path = true;
	}
	else { // dossier de config.json (config_save, paint/sources/config.c)
		const char *dir = path_is_protected() ? iron_internal_save_path() : path_data();
		if (!studio_fs_can_write_in(dir)) {
			// App installée en lecture seule (par l'IT, comme au studio ; macOS ne la dit jamais protégée) : ni les
			// préférences, ni le journal d'audit, ni la file des publishes n'y seraient écrits.
			dir = iron_internal_save_path();
			iron_log("studio: preferences: the app folder is read-only, studio files go to %s", dir);
		}
		snprintf(sp.path, sizeof(sp.path), "%s%s%s", dir, PATH_SEP, PREFS_FILE);
		clean_path(sp.path);
	}
	if (!sp.explicit_path && in_background()) {
		sp.ignored = true;
		iron_log("studio: preferences %s ignored in --background (set STUDIO_PREFS to use a file)", sp.path);
		return;
	}
	load();
	iron_log("studio: preferences %s (%d)%s%s", sp.path, sp.count, sp.error[0] ? ": " : "", sp.error);
}

const char *studio_prefs_path(void) {
	return sp.path;
}

const char *studio_prefs_error(void) {
	return sp.error;
}

char *studio_prefs_dir(void) {
	if (sp.ignored || sp.path[0] == 0) {
		return NULL;
	}
	char *dir = studio_fs_dirname(sp.path);
	if (dir != NULL && dir[0] == 0) {
		free(dir);
		dir = strdup("."); // $STUDIO_PREFS sans dossier
	}
	return dir;
}

bool studio_prefs_has(const char *key) {
	return find(key) != NULL;
}

const char *studio_prefs_string(const char *key, const char *fallback) {
	pref_t *p = find(key);
	return p != NULL && p->text != NULL ? p->text : fallback;
}

bool studio_prefs_bool(const char *key, bool fallback) {
	pref_t *p = find(key);
	if (p == NULL) {
		return fallback;
	}
	return strcmp(p->literal, "true") == 0 ? true : strcmp(p->literal, "false") == 0 ? false : fallback;
}

bool studio_prefs_set_string(const char *key, const char *value) {
	pref_t *p = find(key);
	if (value == NULL) {
		if (p == NULL) {
			return true;
		}
		remove_key(key);
		return save();
	}
	if (p != NULL && p->text != NULL && strcmp(p->text, value) == 0) {
		return true;
	}
	char *literal = studio_json_quote(value);
	if (literal == NULL || !put(key, literal)) {
		return false;
	}
	return save();
}

bool studio_prefs_set_bool(const char *key, bool value) {
	pref_t *p = find(key);
	if (p != NULL && strcmp(p->literal, value ? "true" : "false") == 0) {
		return true;
	}
	char *literal = strdup(value ? "true" : "false");
	if (literal == NULL || !put(key, literal)) {
		return false;
	}
	return save();
}
