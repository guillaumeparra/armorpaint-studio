#include "studio_fs.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

#ifdef _WIN32
// Chemin UTF-8 en UTF-16 (malloc), NULL si invalide.
static wchar_t *wide(const char *path) {
	int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
	if (n <= 0) {
		return NULL;
	}
	wchar_t *w = malloc(sizeof(wchar_t) * (size_t)n);
	if (w != NULL && MultiByteToWideChar(CP_UTF8, 0, path, -1, w, n) == 0) {
		free(w);
		return NULL;
	}
	return w;
}
#endif

static bool is_sep(char c) {
#ifdef _WIN32
	return c == '/' || c == '\\';
#else
	return c == '/';
#endif
}

FILE *studio_fopen(const char *path, const char *mode) {
#ifdef _WIN32
	wchar_t *wpath = wide(path);
	wchar_t *wmode = wide(mode);
	FILE    *f     = wpath != NULL && wmode != NULL ? _wfopen(wpath, wmode) : NULL;
	free(wpath);
	free(wmode);
	return f;
#else
	return fopen(path, mode);
#endif
}

bool studio_fs_rename(const char *from, const char *to) {
#ifdef _WIN32
	wchar_t *wfrom = wide(from);
	wchar_t *wto   = wide(to);
	bool     ok    = wfrom != NULL && wto != NULL && MoveFileExW(wfrom, wto, MOVEFILE_REPLACE_EXISTING) != 0;
	free(wfrom);
	free(wto);
	return ok;
#else
	return rename(from, to) == 0;
#endif
}

bool studio_fs_remove(const char *path) {
#ifdef _WIN32
	wchar_t *w  = wide(path);
	bool     ok = w != NULL && DeleteFileW(w) != 0;
	free(w);
	return ok;
#else
	return remove(path) == 0;
#endif
}

static int fs_stat(const char *path, bool *is_dir, int64_t *size) {
#ifdef _WIN32
	wchar_t *w = wide(path);
	WIN32_FILE_ATTRIBUTE_DATA data;
	BOOL     ok = w != NULL && GetFileAttributesExW(w, GetFileExInfoStandard, &data);
	free(w);
	if (!ok) {
		return -1;
	}
	*is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	*size   = ((int64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
	return 0;
#else
	struct stat st;
	if (stat(path, &st) != 0) {
		return -1;
	}
	*is_dir = S_ISDIR(st.st_mode);
	*size   = (int64_t)st.st_size;
	return 0;
#endif
}

bool studio_fs_exists(const char *path) {
	bool    dir;
	int64_t size;
	return fs_stat(path, &dir, &size) == 0;
}

bool studio_fs_is_dir(const char *path) {
	bool    dir  = false;
	int64_t size = 0;
	return fs_stat(path, &dir, &size) == 0 && dir;
}

int64_t studio_fs_size(const char *path) {
	bool    dir  = false;
	int64_t size = 0;
	return fs_stat(path, &dir, &size) == 0 ? size : -1;
}

char *studio_fs_dirname(const char *path) {
	size_t end = strlen(path);
	while (end > 0 && !is_sep(path[end - 1])) {
		--end;
	}
	while (end > 1 && is_sep(path[end - 1])) {
		--end;
	}
	char *dir = malloc(end + 1);
	if (dir != NULL) {
		memcpy(dir, path, end);
		dir[end] = 0;
	}
	return dir;
}

char *studio_fs_existing_ancestor(const char *path) {
	char *dir = strdup(path[0] != 0 ? path : ".");
	while (dir != NULL && !studio_fs_is_dir(dir)) {
		char *parent = studio_fs_dirname(dir);
		if (parent == NULL || strcmp(parent, dir) == 0 || parent[0] == 0) {
			bool relative = parent != NULL && parent[0] == 0;
			free(parent);
			free(dir);
			return relative ? strdup(".") : NULL;
		}
		free(dir);
		dir = parent;
	}
	return dir;
}

int64_t studio_fs_free_space(const char *path) {
	char *dir = studio_fs_existing_ancestor(path);
	if (dir == NULL) {
		return -1;
	}
	int64_t result = -1;
#ifdef _WIN32
	wchar_t       *w = wide(dir);
	ULARGE_INTEGER avail;
	if (w != NULL && GetDiskFreeSpaceExW(w, &avail, NULL, NULL)) {
		result = (int64_t)avail.QuadPart;
	}
	free(w);
#else
	struct statvfs st;
	if (statvfs(dir, &st) == 0) {
		result = (int64_t)st.f_bavail * (int64_t)st.f_frsize;
	}
#endif
	free(dir);
	return result;
}

bool studio_fs_can_write_in(const char *path) {
	char *dir = studio_fs_existing_ancestor(path);
	if (dir == NULL) {
		return false;
	}
#ifdef _WIN32
	// Les ACL rendent _waccess peu fiable : on essaie de créer un fichier.
	size_t n     = strlen(dir);
	char  *probe = malloc(n + 32);
	bool   ok    = false;
	if (probe != NULL) {
		snprintf(probe, n + 32, "%s\\.studio_write_%lu", dir, (unsigned long)GetCurrentProcessId());
		FILE *f = studio_fopen(probe, "wb");
		if (f != NULL) {
			fclose(f);
			studio_fs_remove(probe);
			ok = true;
		}
		free(probe);
	}
#else
	bool ok = access(dir, W_OK | X_OK) == 0;
#endif
	free(dir);
	return ok;
}

bool studio_fs_write(const char *path, const void *data, size_t size) {
	FILE *f = studio_fopen(path, "wb");
	if (f == NULL) {
		return false;
	}
	bool ok = fwrite(data, 1, size, f) == size;
	ok      = fclose(f) == 0 && ok;
	return ok;
}

char *studio_fs_read(const char *path, size_t *size) {
	FILE *f = studio_fopen(path, "rb");
	if (f == NULL) {
		return NULL;
	}
	size_t cap = 4096, len = 0;
	char  *data = malloc(cap);
	while (data != NULL) {
		if (len + 1 >= cap) {
			char *grow = cap > SIZE_MAX / 2 ? NULL : realloc(data, cap * 2);
			if (grow == NULL) {
				free(data);
				data = NULL;
				break;
			}
			data = grow;
			cap *= 2;
		}
		size_t n = fread(data + len, 1, cap - len - 1, f);
		len += n;
		if (n == 0) {
			break;
		}
	}
	bool failed = ferror(f) != 0;
	fclose(f);
	if (data == NULL || failed) {
		free(data);
		return NULL;
	}
	data[len] = 0;
	if (size != NULL) {
		*size = len;
	}
	return data;
}

bool studio_fs_append(const char *path, const void *data, size_t size) {
#ifdef _WIN32
	wchar_t *w = wide(path);
	HANDLE   h = w != NULL
	                 ? CreateFileW(w, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)
	                 : INVALID_HANDLE_VALUE;
	free(w);
	if (h == INVALID_HANDLE_VALUE) {
		return false;
	}
	DWORD written = 0;
	bool  ok      = size <= 0xffffffffu && WriteFile(h, data, (DWORD)size, &written, NULL) && written == size;
	CloseHandle(h);
	return ok;
#else
	int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0) {
		return false;
	}
	ssize_t written = write(fd, data, size);
	bool    ok      = written >= 0 && (size_t)written == size;
	ok              = close(fd) == 0 && ok;
	return ok;
#endif
}

static bool has_suffix(const char *name, const char *suffix) {
	size_t n = strlen(name), k = strlen(suffix);
	return name[0] != '.' && n >= k && strcmp(name + n - k, suffix) == 0;
}

int studio_fs_count(const char *dir, const char *suffix) {
	int count = 0;
#ifdef _WIN32
	size_t   len     = strlen(dir);
	char    *pattern = malloc(len + 3);
	wchar_t *w       = NULL;
	if (pattern != NULL) {
		memcpy(pattern, dir, len);
		memcpy(pattern + len, "\\*", 3);
		w = wide(pattern);
		free(pattern);
	}
	WIN32_FIND_DATAW found;
	HANDLE           h = w != NULL ? FindFirstFileW(w, &found) : INVALID_HANDLE_VALUE;
	free(w);
	if (h == INVALID_HANDLE_VALUE) {
		return -1;
	}
	do {
		char name[1024];
		if (WideCharToMultiByte(CP_UTF8, 0, found.cFileName, -1, name, sizeof(name), NULL, NULL) > 0 && has_suffix(name, suffix)) {
			count++;
		}
	} while (FindNextFileW(h, &found));
	FindClose(h);
#else
	DIR *d = opendir(dir);
	if (d == NULL) {
		return -1;
	}
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		count += has_suffix(e->d_name, suffix);
	}
	closedir(d);
#endif
	return count;
}
