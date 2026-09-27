#include "studio_env.h"
#include "studio.h"
#include "studio_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// iron (base/sources/iron_system.h, iron.h)
const char *iron_internal_save_path(void);
int         iron_get_arg_count(void);
char       *iron_get_arg(int index);

#ifdef _WIN32
#define LIST_SEP ';'
#else
#define LIST_SEP ':'
extern char **environ;
#endif

#define ENV_FILE "studio.env"

typedef struct {
	char *name;
	char *source; // fichier qui a donné la valeur
} origin_t;

static struct {
	origin_t *items;
	int       count, cap;
	char     *file; // studio_env_file
} eo;

typedef struct {
	char  *data;
	size_t len, cap;
} buf_t;

static char *dup_n(const char *s, size_t n) {
	char *d = malloc(n + 1);
	if (d != NULL) {
		memcpy(d, s, n);
		d[n] = 0;
	}
	return d;
}

static void buf_put(buf_t *b, const char *s, size_t n) {
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap > 0 ? b->cap : 64;
		while (b->len + n + 1 > cap) {
			cap *= 2;
		}
		char *data = realloc(b->data, cap);
		if (data == NULL) {
			return;
		}
		b->data = data;
		b->cap  = cap;
	}
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = 0;
}

static void buf_str(buf_t *b, const char *s) {
	buf_put(b, s, strlen(s));
}

static bool is_name_start(char c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool is_name_char(char c) {
	return is_name_start(c) || (c >= '0' && c <= '9');
}

static const char *home(void) {
#ifdef _WIN32
	const char *h = getenv("USERPROFILE");
#else
	const char *h = getenv("HOME");
#endif
	return h != NULL ? h : "";
}

static bool env_set(const char *name, const char *value) {
#ifdef _WIN32
	return _putenv_s(name, value) == 0;
#else
	return setenv(name, value, 1) == 0;
#endif
}

// $VAR ou ${VAR} en p (qui pointe sur '$') : valeur ajoutée à b, rend le caractère qui suit. Sans nom valide, '$' est
// pris tel quel.
static const char *expand(buf_t *b, const char *p) {
	const char *start = p + 1;
	const char *end   = start;
	bool        brace = *start == '{';
	if (brace) {
		start++;
		end = start;
	}
	if (!is_name_start(*end)) {
		buf_put(b, "$", 1);
		return p + 1;
	}
	while (is_name_char(*end)) {
		end++;
	}
	if (brace && *end != '}') {
		buf_put(b, "$", 1);
		return p + 1;
	}
	char       *name  = dup_n(start, (size_t)(end - start));
	const char *value = name != NULL ? getenv(name) : NULL;
	if (value != NULL) {
		buf_str(b, value);
	}
	free(name);
	return brace ? end + 1 : end;
}

static void set_origin(const char *name, const char *source) {
	if (eo.count == eo.cap) {
		int       cap   = eo.cap > 0 ? eo.cap * 2 : 16;
		origin_t *items = realloc(eo.items, (size_t)cap * sizeof(origin_t));
		if (items == NULL) {
			return;
		}
		eo.items = items;
		eo.cap   = cap;
	}
	eo.items[eo.count++] = (origin_t){.name = dup_n(name, strlen(name)), .source = dup_n(source, strlen(source))};
}

const char *studio_env_file(void) {
	return eo.file != NULL ? eo.file : "";
}

const char *studio_env_origin(const char *name) {
	if (name == NULL || getenv(name) == NULL) {
		return NULL;
	}
	for (int i = 0; i < eo.count; i++) {
		if (strcmp(eo.items[i].name, name) == 0) {
			return eo.items[i].source;
		}
	}
	return "environment";
}

typedef struct {
	const char *source;
	int         taken;
	buf_t       names; // variables prises, pour le log
	buf_t       kept;  // variables déjà définies, gardées
} load_t;

static void warn(const char *source, int line, const char *what) {
	iron_log("studio: env %s:%d: %s, line ignored", source, line, what);
}

// Reste de la ligne après une valeur entre guillemets : espaces, puis rien ou un commentaire.
static bool only_comment(const char *p) {
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	return *p == 0 || *p == '#';
}

static void parse_line(load_t *ld, char *line, int no) {
	size_t n = strlen(line);
	if (n > 0 && line[n - 1] == '\r') {
		line[--n] = 0;
	}
	const char *p = line;
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	if (*p == 0 || *p == '#') {
		return;
	}
	if (strncmp(p, "export", 6) == 0 && (p[6] == ' ' || p[6] == '\t')) {
		p += 6;
		while (*p == ' ' || *p == '\t') {
			p++;
		}
	}
	const char *name_start = p;
	if (!is_name_start(*p)) {
		warn(ld->source, no, "not a NAME=value line");
		return;
	}
	while (is_name_char(*p)) {
		p++;
	}
	const char *name_end = p;
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	if (*p != '=') {
		warn(ld->source, no, "not a NAME=value line");
		return;
	}
	p++;
	while (*p == ' ' || *p == '\t') {
		p++;
	}

	buf_t value = {0};
	buf_put(&value, "", 0);
	if (*p == '\'') {
		const char *close = strchr(p + 1, '\'');
		if (close == NULL || !only_comment(close + 1)) {
			warn(ld->source, no, close == NULL ? "unterminated quote" : "text after the closing quote");
			free(value.data);
			return;
		}
		buf_put(&value, p + 1, (size_t)(close - p - 1));
	}
	else if (*p == '"') {
		p++;
		while (*p != 0 && *p != '"') {
			if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$')) {
				buf_put(&value, p + 1, 1);
				p += 2;
			}
			else if (*p == '$') {
				p = expand(&value, p);
			}
			else {
				buf_put(&value, p, 1);
				p++;
			}
		}
		if (*p != '"' || !only_comment(p + 1)) {
			warn(ld->source, no, *p != '"' ? "unterminated quote" : "text after the closing quote");
			free(value.data);
			return;
		}
	}
	else {
		const char *end = p;
		while (*end != 0 && !(*end == '#' && end > p && (end[-1] == ' ' || end[-1] == '\t'))) {
			end++;
		}
		while (end > p && (end[-1] == ' ' || end[-1] == '\t')) {
			end--;
		}
		if (*p == '~' && (p + 1 == end || p[1] == '/')) {
			buf_str(&value, home());
			p++;
		}
		while (p < end) {
			if (*p == '$') {
				const char *next = expand(&value, p);
				p                = next <= end ? next : end;
			}
			else {
				buf_put(&value, p, 1);
				p++;
			}
		}
	}

	char *name = dup_n(name_start, (size_t)(name_end - name_start));
	if (name != NULL && value.data != NULL) {
		buf_t *list = &ld->kept;
		if (getenv(name) == NULL) {
			if (env_set(name, value.data)) {
				set_origin(name, ld->source);
				ld->taken++;
				list = &ld->names;
			}
			else {
				warn(ld->source, no, "could not set the variable");
				list = NULL;
			}
		}
		if (list != NULL) {
			buf_str(list, list->len > 0 ? ", " : "");
			buf_str(list, name);
		}
	}
	free(name);
	free(value.data);
}

static int load_text(const char *text, const char *source) {
	load_t      ld = {.source = source};
	const char *p  = text;
	int         no = 0;
	while (*p != 0) {
		no++;
		size_t len  = strcspn(p, "\n");
		char  *line = dup_n(p, len);
		if (line != NULL) {
			parse_line(&ld, line, no);
			free(line);
		}
		p += len;
		if (*p == '\n') {
			p++;
		}
	}
	iron_log("studio: env %s: %d variable%s%s%s%s%s%s", source, ld.taken, ld.taken == 1 ? "" : "s", ld.names.len > 0 ? " (" : "",
	         ld.names.len > 0 ? ld.names.data : "", ld.names.len > 0 ? ")" : "", ld.kept.len > 0 ? "; already set, kept: " : "",
	         ld.kept.len > 0 ? ld.kept.data : "");
	free(ld.names.data);
	free(ld.kept.data);
	return ld.taken;
}

int studio_env_load_text(const char *text, const char *source) {
	return load_text(text != NULL ? text : "", source != NULL ? source : "(text)");
}

int studio_env_load(const char *path) {
	size_t size = 0;
	char  *text = studio_fs_read(path, &size);
	if (text == NULL) {
		iron_log("studio: env %s: cannot read the file", path);
		return -1;
	}
	int taken = load_text(text, path);
	free(text);
	return taken;
}

// ---- Démarrage ------------------------------------------------------------------------------------------------------

// args_background n'est pas encore lu quand external_init() tourne (args_parse, dans base_init).
static bool in_background(void) {
	for (int i = 1; i < iron_get_arg_count(); i++) {
		if (strcmp(iron_get_arg(i), "--background") == 0) {
			return true;
		}
	}
	return false;
}

static bool is_studio_name(const char *name, size_t len) {
	return (len == 4 && strncmp(name, "OCIO", 4) == 0) || strncmp(name, "STUDIO_", 7) == 0 || strncmp(name, "SUBZERO_", 8) == 0;
}

static bool is_secret(const char *name) {
	const char *words[] = {"KEY", "SECRET", "TOKEN", "PASSWORD"};
	for (int i = 0; i < 4; i++) {
		if (strstr(name, words[i]) != NULL) {
			return true;
		}
	}
	return false;
}

static int compare_text(const void *a, const void *b) {
	return strcmp(*(char *const *)a, *(char *const *)b);
}

// Environnement studio effectif, une ligne par variable, avec son origine ; les secrets sont masqués.
static void log_summary(void) {
#ifndef _WIN32
	int    count = 0;
	char **lines = NULL;
	for (char **e = environ; e != NULL && *e != NULL; e++) {
		const char *eq = strchr(*e, '=');
		if (eq == NULL || !is_studio_name(*e, (size_t)(eq - *e))) {
			continue;
		}
		char **grown = realloc(lines, (size_t)(count + 1) * sizeof(char *));
		if (grown == NULL) {
			break;
		}
		lines       = grown;
		char *name  = dup_n(*e, (size_t)(eq - *e));
		buf_t line  = {0};
		const char *origin = name != NULL ? studio_env_origin(name) : NULL;
		buf_str(&line, name != NULL ? name : "");
		buf_str(&line, "=");
		buf_str(&line, name != NULL && is_secret(name) ? "***" : eq + 1);
		buf_str(&line, " (");
		buf_str(&line, origin != NULL ? origin : "environment");
		buf_str(&line, ")");
		lines[count++] = line.data;
		free(name);
	}
	qsort(lines, (size_t)count, sizeof(char *), compare_text);
	for (int i = 0; i < count; i++) {
		iron_log("studio: env %s", lines[i] != NULL ? lines[i] : "");
		free(lines[i]);
	}
	free(lines);
#endif
}

void studio_env_init(void) {
	const char *list = getenv("STUDIO_ENV");
	if (list != NULL) {
		char *copy = dup_n(list, strlen(list));
		int   read = 0;
		for (char *p = copy; p != NULL && *p != 0;) {
			char *sep = strchr(p, LIST_SEP);
			if (sep != NULL) {
				*sep = 0;
			}
			if (*p != 0) {
				buf_t path = {0};
				if (p[0] == '~' && (p[1] == 0 || p[1] == '/')) {
					buf_str(&path, home());
					buf_str(&path, p + 1);
				}
				else {
					buf_str(&path, p);
				}
				if (path.data != NULL) {
					studio_env_load(path.data);
					if (read++ == 0) {
						free(eo.file);
						eo.file   = path.data;
						path.data = NULL;
					}
				}
				free(path.data);
			}
			p = sep != NULL ? sep + 1 : NULL;
		}
		free(copy);
		if (read == 0) {
			iron_log("studio: env: STUDIO_ENV is empty, no %s read", ENV_FILE);
		}
	}
	else {
		const char *dir  = iron_internal_save_path();
		buf_t       path = {0};
		buf_str(&path, dir != NULL ? dir : "");
		buf_str(&path, ENV_FILE);
		if (path.data == NULL) {
			return;
		}
		if (!studio_fs_exists(path.data)) {
			iron_log("studio: env: no %s (%s), environment only", ENV_FILE, path.data);
		}
		else if (in_background()) {
			iron_log("studio: env %s ignored in --background (set STUDIO_ENV to read a file)", path.data);
		}
		else {
			studio_env_load(path.data);
		}
		free(eo.file);
		eo.file = path.data;
	}
	log_summary();
}
