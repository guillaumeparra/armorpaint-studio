#include "studio_audit.h"
#include "iron_string.h"
#include "iron_system.h"
#include "studio.h"
#include "studio_fs.h"
#include "studio_json.h"
#include "studio_naming.h"
#include "studio_prefs.h"
#include "studio_project.h"
#include "studio_publish.h"
#include "studio_sg_context.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

// Cœur ArmorPaint (paint/sources/project.c)
char *project_filepath_get(void);

#define AUDIT_FILE   "studio_audit.jsonl"
#define AUDIT_ROTATE ((int64_t)10 << 20)

static struct {
	char *path; // NULL : journal désactivé
	char *host; // littéraux JSON des champs communs
	char *user;
	long  pid;
	bool  failed;       // échec d'écriture signalé, jusqu'à la prochaine réussite
	bool  open_pending; // project_open à écrire, une fois le contexte ShotGrid du projet posé
	char *saved;        // project_save à écrire, une fois le fichier écrit
} au;

static void now_utc(char *out, size_t size) {
	struct timespec ts;
	timespec_get(&ts, TIME_UTC);
	time_t    seconds = ts.tv_sec;
	struct tm tm;
#ifdef _WIN32
	gmtime_s(&tm, &seconds);
#else
	gmtime_r(&seconds, &tm);
#endif
	char base[32];
	strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tm);
	snprintf(out, size, "%s.%03dZ", base, (int)(ts.tv_nsec / 1000000));
}

// Comme getpass.getuser() du sidecar : LOGNAME, USER, LNAME, USERNAME, puis le compte du processus.
static char *user_name(void) {
	const char *vars[] = {"LOGNAME", "USER", "LNAME", "USERNAME"};
	for (int i = 0; i < 4; i++) {
		const char *v = getenv(vars[i]);
		if (v != NULL && v[0] != 0) {
			return studio_json_quote(v);
		}
	}
#ifndef _WIN32
	struct passwd *pw = getpwuid(getuid());
	if (pw != NULL && pw->pw_name != NULL) {
		return studio_json_quote(pw->pw_name);
	}
#endif
	return studio_json_quote("");
}

static char *host_name(void) {
	char host[256] = "";
#ifdef _WIN32
	DWORD size = sizeof(host);
	if (!GetComputerNameA(host, &size)) {
		host[0] = 0;
	}
#else
	if (gethostname(host, sizeof(host) - 1) != 0) {
		host[0] = 0;
	}
	host[sizeof(host) - 1] = 0;
#endif
	return studio_json_quote(host);
}

void studio_audit(const char *event, const char *fields) {
	if (au.path == NULL) {
		return;
	}
	char time[40];
	now_utc(time, sizeof(time));
	char       *name  = studio_json_quote(event);
	const char *extra = fields != NULL ? fields : "";
	size_t      size  = strlen(extra) + strlen(au.host) + strlen(au.user) + (name != NULL ? strlen(name) : 0) + 128;
	char       *line  = name != NULL ? malloc(size) : NULL;
	int         n     = -1;
	if (line != NULL) {
		n = snprintf(line, size, "{\"time\":\"%s\",\"source\":\"armorpaint\",\"pid\":%ld,\"host\":%s,\"user\":%s,\"event\":%s%s%s}\n", time, au.pid, au.host,
		             au.user, name, extra[0] != 0 ? "," : "", extra);
	}
	bool ok = n > 0 && (size_t)n < size;
	if (ok) {
		for (int i = 0; i < n - 1; i++) { // une ligne : hors des chaînes JSON, un saut de ligne n'est qu'un espace
			if (line[i] == '\n' || line[i] == '\r') {
				line[i] = ' ';
			}
		}
		ok = studio_fs_append(au.path, line, (size_t)n);
	}
	if (!ok && !au.failed) {
		iron_log("studio: audit: cannot write %s (event %s)", au.path, event);
	}
	au.failed = !ok;
	free(name);
	free(line);
}

// ---- Évènements du projet ---------------------------------------------------------------------------------------------

// ",\"sg\":{...}" : le contexte ShotGrid du projet, "" sans contexte (chaîne temporaire, string_tmp).
static const char *sg_field(void) {
	const char *context = studio_sg_context_json();
	return context != NULL ? string_tmp(",\"sg\":%s", context) : "";
}

static void flush(void) {
	if (au.open_pending) {
		au.open_pending = false;
		char *path      = studio_json_quote(project_filepath_get());
		studio_audit("project_open", string_tmp("\"path\":%s%s", path, sg_field()));
		free(path);
	}
	if (au.saved != NULL) {
		char *path = studio_json_quote(au.saved);
		studio_audit("project_save", string_tmp("\"path\":%s,\"size\":%lld%s", path, (long long)studio_fs_size(au.saved), sg_field()));
		free(path);
		free(au.saved);
		au.saved = NULL;
	}
}

// Écrits à la frame suivante : un .arm ouvert par Workfiles reçoit ensuite le contexte de la fenêtre, et un projet
// enregistré n'est écrit qu'après les abonnés (studio_project_save_entries).
static void on_change(bool opened) {
	au.open_pending = opened;
}

static void on_save(string_array_t *texture_files) {
	(void)texture_files;
	if (studio_project_copy()) {
		return; // copie publiée : project_copy, écrit par studio_publish.c
	}
	const char *path = project_filepath_get();
	if (path != NULL && path[0] != 0) {
		free(au.saved);
		au.saved = strdup(path);
	}
}

static void audit_update(void *data) {
	(void)data;
	flush();
}

const char *studio_audit_path(void) {
	return au.path;
}

void studio_audit_init(void) {
	const char *env = getenv("STUDIO_AUDIT_LOG");
	if (env != NULL && env[0] != 0) {
		au.path = strdup(env);
	}
	else {
		char *dir = studio_prefs_dir();
		if (dir != NULL) {
			size_t size = strlen(dir) + sizeof(AUDIT_FILE) + 2;
			au.path     = malloc(size);
			if (au.path != NULL) {
#ifdef _WIN32
				snprintf(au.path, size, "%s\\%s", dir, AUDIT_FILE);
#else
				snprintf(au.path, size, "%s/%s", dir, AUDIT_FILE);
#endif
			}
		}
		free(dir);
	}
	if (au.path == NULL) {
		iron_log("studio: audit log off in --background (set STUDIO_AUDIT_LOG or STUDIO_PREFS to write one)");
		return;
	}
	studio_naming_make_parent(au.path);
	if (studio_fs_size(au.path) > AUDIT_ROTATE) {
		char *old = strdup(string_tmp("%s.1", au.path));
		if (old != NULL && studio_fs_rename(au.path, old)) {
			iron_log("studio: audit log rotated to %s", old);
		}
		free(old);
	}
	au.host = host_name();
	au.user = user_name();
#ifdef _WIN32
	au.pid = (long)GetCurrentProcessId();
#else
	au.pid = (long)getpid();
#endif
	if (au.host == NULL || au.user == NULL) {
		free(au.path);
		au.path = NULL;
		return;
	}
	studio_project_on_change(on_change);
	studio_project_on_save(on_save);
	sys_notify_on_update(audit_update, NULL);
	atexit(flush); // enregistré puis quitté dans la même frame (Save & Quit)
	iron_log("studio: audit log %s", au.path);
}
