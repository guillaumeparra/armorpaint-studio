#include "studio_naming.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define naming_mkdir(p) _mkdir(p)
#define strncasecmp     _strnicmp
#define S_ISDIR(m)      (((m) & S_IFMT) == S_IFDIR)
#else
#include <strings.h>
#define naming_mkdir(p) mkdir(p, 0777)
#endif

static bool is_sep(char c) {
#ifdef _WIN32
	return c == '/' || c == '\\';
#else
	return c == '/';
#endif
}

typedef struct {
	char  *data;
	size_t len;
	size_t cap;
	bool   failed;
} buf_t;

static void put(buf_t *b, const char *s, size_t n, bool value) {
	if (b->failed) {
		return;
	}
	if (b->len + n + 1 > b->cap) {
		size_t cap  = (b->len + n + 1) * 2;
		char  *grow = realloc(b->data, cap);
		if (grow == NULL) {
			b->failed = true;
			return;
		}
		b->data = grow;
		b->cap  = cap;
	}
	for (size_t i = 0; i < n; ++i) {
		char c                = s[i];
		b->data[b->len++] = value && (c == '/' || c == '\\' || c == ':') ? '_' : c;
	}
	b->data[b->len] = 0;
}

char *studio_naming_expand(const char *pattern, const char *channel, const char *udim, const char *ext) {
	static const char *markers[] = {"<channel>", "<UDIM>", "<ext>"};
	const char        *values[]  = {channel, udim, ext};
	buf_t              b         = {0};
	put(&b, "", 0, false);
	const char *p = pattern;
	while (*p != 0) {
		const char *lt = strchr(p, '<');
		if (lt == NULL) {
			put(&b, p, strlen(p), false);
			break;
		}
		put(&b, p, (size_t)(lt - p), false);
		size_t used = 0;
		for (int i = 0; i < 3 && used == 0; ++i) {
			size_t n = strlen(markers[i]);
			if (strncmp(lt, markers[i], n) == 0) {
				put(&b, values[i], strlen(values[i]), true);
				used = n;
			}
		}
		if (used == 0) { // un '<' qui n'ouvre pas de marqueur
			put(&b, lt, 1, false);
			used = 1;
		}
		p = lt + used;
	}
	if (b.failed) {
		free(b.data);
		return NULL;
	}
	return b.data;
}

// Développe pattern[begin, end) dans b. *empty passe à true si un marqueur connu y vaut "".
static void format_range(buf_t *b, const char *p, const char *end, const char *const *names, const char *const *values,
                         int count, bool *empty) {
	while (p < end) {
		const char *lt = memchr(p, '<', (size_t)(end - p));
		if (lt == NULL) {
			put(b, p, (size_t)(end - p), false);
			return;
		}
		put(b, p, (size_t)(lt - p), false);
		const char *gt   = memchr(lt, '>', (size_t)(end - lt));
		size_t      used = 0;
		for (int i = 0; gt != NULL && i < count && used == 0; ++i) {
			size_t n = strlen(names[i]);
			if ((size_t)(gt - lt - 1) == n && strncasecmp(lt + 1, names[i], n) == 0) {
				const char *v = values[i] != NULL ? values[i] : "";
				put(b, v, strlen(v), true);
				*empty = *empty || v[0] == 0;
				used   = n + 2;
			}
		}
		if (used == 0) { // un '<' qui n'ouvre pas de marqueur connu
			put(b, lt, 1, false);
			used = 1;
		}
		p = lt + used;
	}
}

char *studio_naming_format(const char *pattern, const char *const *names, const char *const *values, int count) {
	buf_t b = {0};
	put(&b, "", 0, false);
	const char *p   = pattern;
	const char *end = pattern + strlen(pattern);
	while (p < end) {
		const char *open  = memchr(p, '[', (size_t)(end - p));
		const char *close = open != NULL ? memchr(open, ']', (size_t)(end - open)) : NULL;
		bool        empty = false;
		if (close == NULL) { // pas (ou plus) de section complète : le reste est littéral
			format_range(&b, p, end, names, values, count, &empty);
			break;
		}
		format_range(&b, p, open, names, values, count, &empty);
		buf_t section = {0};
		put(&section, "", 0, false);
		empty = false;
		format_range(&section, open + 1, close, names, values, count, &empty);
		if (section.failed) {
			b.failed = true;
		}
		else if (!empty) {
			put(&b, section.data, section.len, false);
		}
		free(section.data);
		p = close + 1;
	}
	if (b.failed) {
		free(b.data);
		return NULL;
	}
	return b.data;
}

const studio_naming_variable_t studio_naming_variables[] = {
    {"file", "project file name (robot_v003)"},
    {"name", "project name without its version (robot)"},
    {"version", "version number of the project file (003), empty if none"},
    {"variant", "object or texture set exported apart, empty otherwise"},
    {"object", "object exported apart (Per Object, or the layer's object in Selected)"},
    {"atlas", "texture set (Visible, when the project has texture sets)"},
    {"channel", "map suffix from the preset (BaseColor)"},
    {"UDIM", "UDIM tile (Per UDIM Tile)"},
    {"resolution", "width in pixels of the file (2048)"},
    {"bits", "bit depth of the file (8, 16 or 32)"},
    {"preset", "export preset name"},
    {"ext", "file extension (exr)"},
};
const int studio_naming_variable_count = (int)(sizeof(studio_naming_variables) / sizeof(studio_naming_variables[0]));

bool studio_naming_check(const char *pattern, char *err, size_t err_size) {
	if (err != NULL && err_size > 0) {
		err[0] = 0;
	}
	if (pattern == NULL || pattern[0] == 0) {
		if (err != NULL) {
			snprintf(err, err_size, "empty name");
		}
		return false;
	}
	for (const char *lt = strchr(pattern, '<'); lt != NULL; lt = strchr(lt + 1, '<')) {
		const char *gt = strpbrk(lt + 1, "<>"); // comme studio_naming_format : "a<b<name>" a le marqueur <name>
		if (gt == NULL) {
			break;
		}
		if (*gt == '<') {
			continue;
		}
		size_t n     = (size_t)(gt - lt - 1);
		bool   known = false;
		for (int i = 0; i < studio_naming_variable_count && !known; ++i) {
			known = strlen(studio_naming_variables[i].name) == n && strncasecmp(lt + 1, studio_naming_variables[i].name, n) == 0;
		}
		if (!known && err != NULL) {
			size_t len = 0;
			len += (size_t)snprintf(err, err_size, "unknown variable %.*s (", (int)(n + 2 > 40 ? 40 : n + 2), lt);
			for (int i = 0; i < studio_naming_variable_count && len < err_size; ++i) {
				len += (size_t)snprintf(err + len, err_size - len, "%s%s", i > 0 ? ", " : "", studio_naming_variables[i].name);
			}
			if (len < err_size) {
				snprintf(err + len, err_size - len, ")");
			}
		}
		if (!known) {
			return false;
		}
	}
	return true;
}

bool studio_naming_split_version(const char *file, char **name, char **version) {
	size_t len    = strlen(file);
	size_t digits = 0;
	while (digits < len && file[len - 1 - digits] >= '0' && file[len - 1 - digits] <= '9') {
		++digits;
	}
	size_t cut = len; // début du "_v003", len si pas de version
	if (digits > 0 && len >= digits + 2) {
		char v   = file[len - digits - 1];
		char sep = file[len - digits - 2];
		if ((v == 'v' || v == 'V') && (sep == '_' || sep == '.' || sep == '-') && len > digits + 2) {
			cut = len - digits - 2;
		}
	}
	*name = malloc(cut + 1);
	if (*name != NULL) {
		memcpy(*name, file, cut);
		(*name)[cut] = 0;
	}
	*version = strdup(cut < len ? file + len - digits : "");
	if (*name == NULL || *version == NULL) {
		free(*name);
		free(*version);
		*name = *version = NULL;
		return false;
	}
	return true;
}

static bool is_dir(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode); // S_IFMT est XSI : absent en _POSIX_C_SOURCE (Linux)
}

bool studio_naming_make_parent(const char *path) {
	char *dir = strdup(path);
	if (dir == NULL) {
		return false;
	}
	size_t end = strlen(dir);
	while (end > 0 && !is_sep(dir[end - 1])) {
		--end;
	}
	while (end > 1 && is_sep(dir[end - 1])) {
		--end;
	}
	dir[end] = 0;
	bool ok  = end == 0 || is_dir(dir);
	// Chaque préfixe, de la racine au parent. Les échecs intermédiaires (lecteur "C:", dossier sans droits d'un
	// chemin qui existe) sont ignorés : seul compte le parent à la fin.
	for (size_t i = 1; !ok && i <= end; ++i) {
		if (i == end || is_sep(dir[i])) {
			char c = dir[i];
			dir[i] = 0;
			(void)naming_mkdir(dir);
			dir[i] = c;
		}
	}
	ok = ok || is_dir(dir);
	free(dir);
	return ok;
}
