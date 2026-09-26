#include "studio_export_naming.h"
#include "iron_string.h"
#include "iron_system.h"
#include "studio.h"
#include "studio_json.h"
#include "studio_naming.h"
#include "studio_sg.h"
#include "studio_workfiles.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

// Cœur ArmorPaint (paint/sources/project.c, console.c)
char *project_filepath_get(void);
void  console_info(char *s);
void  console_error(char *s);

// Le modèle du dernier projet vu. path sans job ni ready : pas demandé (ShotGrid pas encore utilisé), redemandé à
// l'export si ShotGrid sert entre-temps.
static struct {
	char *path;         // projet dont le modèle est demandé ou relu
	int   job;          // texture_pattern en cours, 0 sinon
	bool  ready;        // réponse relue pour path
	char *template;     // NULL : noms d'ArmorPaint
	char *pattern;      // sans tuile, NULL si le template exige une tuile
	char *pattern_udim; // avec tuile, NULL si le template n'en a pas
	bool  seen;         // ShotGrid sert dans la session
	bool  told;         // console : nommage (ou son absence) déjà signalé pour path
} nm;

static bool enabled(void) {
	const char *force = getenv("STUDIO_SG_NAMING");
	if (force != NULL && force[0] != 0) {
		return strcmp(force, "0") != 0;
	}
	if (!nm.seen) {
		const char *launch = getenv("SHOTGUN_ENTITY_TYPE");
		char       *hello  = studio_sg_hello();
		nm.seen            = (launch != NULL && launch[0] != 0) || hello != NULL;
		free(hello);
	}
	return nm.seen;
}

// Texte (malloc) de la chaîne à `pointer` dans la réponse, NULL si absente, null ou vide.
static char *reply_text(const char *json, const char *pointer) {
	studio_json_value_t v = studio_json_find(json, pointer);
	char               *s = v.type == STUDIO_JSON_STRING ? studio_json_text(v) : NULL;
	if (s != NULL && s[0] == 0) {
		free(s);
		s = NULL;
	}
	return s;
}

static void forget(void) {
	if (nm.job != 0) {
		studio_sg_release(nm.job);
		nm.job = 0;
	}
	free(nm.template), free(nm.pattern), free(nm.pattern_udim);
	nm.template = nm.pattern = nm.pattern_udim = NULL;
	nm.ready                                   = false;
	nm.told                                    = false;
}

static void track(const char *path, bool ask) {
	forget();
	free(nm.path);
	nm.path = strdup(path);
	if (!ask) {
		return;
	}
	char *quoted  = studio_json_quote(path);
	int   project = studio_workfiles_project_id();
	char *args    = project > 0 ? string_tmp("{\"path\":%s,\"project_id\":%d}", quoted, project) : string_tmp("{\"path\":%s}", quoted);
	free(quoted);
	nm.job = studio_sg_start("texture_pattern", args, 60000);
}

static void collect(void) {
	const char *reply = studio_sg_result(nm.job);
	nm.ready          = true;
	if (!studio_sg_ok(nm.job)) {
		console_error(string_tmp("ShotGrid texture naming: %s: %s (textures keep ArmorPaint names)", studio_sg_get(nm.job, "/error/code"),
		                         studio_sg_get(nm.job, "/error/message")));
		nm.told = true;
	}
	else {
		nm.template     = reply_text(reply, "/data/template");
		nm.pattern      = reply_text(reply, "/data/pattern");
		nm.pattern_udim = reply_text(reply, "/data/pattern_udim");
		if (nm.template != NULL) {
			iron_log("studio: naming: %s -> %s (%s)", nm.path, nm.pattern != NULL ? nm.pattern : nm.pattern_udim, nm.template);
		}
		else {
			char *reason = reply_text(reply, "/data/reason");
			iron_log("studio: naming: ArmorPaint names for %s: %s", nm.path, reason != NULL ? reason : "no template");
			free(reason);
		}
	}
	studio_sg_release(nm.job);
	nm.job = 0;
}

static void sleep_ms(int ms) {
#ifdef _WIN32
	Sleep((DWORD)ms);
#else
	struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
#endif
}

// Le modèle du projet ouvert, relu si besoin : false s'il n'y en a pas (noms d'ArmorPaint).
static bool ensure(void) {
	char *path = project_filepath_get();
	if (path == NULL || path[0] == 0) {
		return false;
	}
	if (nm.path == NULL || strcmp(nm.path, path) != 0 || (!nm.ready && nm.job == 0)) {
		if (!enabled()) {
			return false;
		}
		track(path, true);
	}
	if (!nm.ready) {
		if (nm.job == 0) {
			return false; // plus de place pour un job : le superviseur l'a déjà signalé
		}
		if (studio_sg_state(nm.job) != STUDIO_SG_DONE && studio_workfiles_busy()) {
			// Le job attend derrière la fenêtre ShotGrid, dont les requêtes passent par ce thread : attendre ici la
			// bloquerait.
			if (!nm.told) {
				console_error("ShotGrid texture naming: close the ShotGrid window first (textures keep ArmorPaint names)");
				nm.told = true;
			}
			return false;
		}
		while (studio_sg_state(nm.job) != STUDIO_SG_DONE) { // borné par le délai du job
			sleep_ms(10);
		}
		collect();
	}
	return nm.template != NULL;
}

char *studio_export_texture_path(char *variant, char *channel, char *udim, char *ext) {
	if (!ensure()) {
		return NULL;
	}
	const char *tile    = udim[0] == '.' ? udim + 1 : udim;
	const char *pattern = tile[0] != 0 ? nm.pattern_udim : nm.pattern;
	if (pattern == NULL) {
		if (!nm.told) {
			console_error(string_tmp("ShotGrid texture naming: template %s has %s UDIM key (textures keep ArmorPaint names)", nm.template,
			                         tile[0] != 0 ? "no" : "a required"));
			nm.told = true;
		}
		return NULL;
	}
	const char *dot   = strrchr(ext, '.');
	const char *plain = dot != NULL ? dot + 1 : ext;
	char       *name  = variant[0] != 0 ? string_tmp("%s_%s", variant, channel) : channel;
	char       *path  = studio_naming_expand(pattern, name, tile, plain);
	if (path == NULL) {
		return NULL;
	}
	if (!studio_naming_make_parent(path)) {
		console_error(string_tmp("ShotGrid texture naming: cannot create the folder of %s", path));
	}
	if (!nm.told) {
		console_info(string_tmp("Textures named by ShotGrid template %s", nm.template));
		nm.told = true;
	}
	iron_log("studio: naming: %s", path);
	char *out = string_copy(path);
	free(path);
	return out;
}

int studio_export_naming_path(const char *variant, const char *channel, const char *udim, const char *ext, char **path,
                              char **error) {
	*path  = NULL;
	*error = NULL;
	if (!ensure()) {
		return 0;
	}
	const char *pattern = udim[0] != 0 ? nm.pattern_udim : nm.pattern;
	if (pattern == NULL) {
		*error = strdup(string_tmp("ShotGrid template %s %s a UDIM key: export %s", nm.template,
		                           udim[0] != 0 ? "has no" : "requires",
		                           udim[0] != 0 ? "without per-tile mode or fix the template" : "per UDIM tile"));
		return -1;
	}
	char *name = variant[0] != 0 ? string_tmp("%s_%s", variant, channel) : (char *)channel;
	*path      = studio_naming_expand(pattern, name, udim, ext);
	return *path != NULL ? 1 : 0;
}

const char *studio_export_naming_template(void) {
	return ensure() ? nm.template : NULL;
}

// Le modèle est demandé dès que le projet ouvert change, pour ne pas attendre le sidecar à l'export.
static void naming_update(void *data) {
	(void)data;
	if (nm.job != 0 && studio_sg_state(nm.job) == STUDIO_SG_DONE) {
		collect();
	}
	char *path = project_filepath_get();
	if (path != NULL && path[0] != 0 && (nm.path == NULL || strcmp(nm.path, path) != 0)) {
		track(path, enabled());
	}
}

void studio_export_naming_init(void) {
	sys_notify_on_update(naming_update, NULL);
}
