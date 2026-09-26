#include "studio_naming.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define naming_mkdir(p) _mkdir(p)
#else
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

static bool is_dir(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
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
