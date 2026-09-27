#include "studio_export_naming.h"
#include "iron_string.h"
#include "iron_system.h"
#include "studio.h"
#include "studio_json.h"
#include "studio_naming.h"
#include "studio_sg.h"
#include "studio_sg_context.h"
#include "studio_sg_offline.h"
#include "studio_workfiles.h"
#include <stdio.h>
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
	char *info;         // lot B6, pour la fenêtre d'export : objet JSON (studio_export_naming_json), NULL sans réponse
	bool  seen;         // ShotGrid sert dans la session
	bool  told;         // console : nommage (ou son absence) déjà signalé pour path
	int   net_serial;   // studio_sg_network_serial() déjà vu
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
	free(nm.template), free(nm.pattern), free(nm.pattern_udim), free(nm.info);
	nm.template = nm.pattern = nm.pattern_udim = nm.info = NULL;
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
	// Projet du contexte (studio_sg_context.h), avec son site : le sidecar l'ignore s'il est d'un autre site.
	char *quoted  = studio_json_quote(path);
	char *site    = studio_json_quote(studio_sg_context_site());
	int   project = studio_sg_context_project_id();
	char *args    = project > 0 ? string_tmp("{\"path\":%s,\"project_id\":%d,\"site\":%s}", quoted, project, site) : string_tmp("{\"path\":%s}", quoted);
	free(quoted);
	free(site);
	nm.job = studio_sg_start("texture_pattern", args, 60000);
}

// Mode dégradé (studio_sg_offline.h) : les textures gardent les noms d'ArmorPaint, dit une fois par projet.
static void tell_offline(void) {
	if (!nm.told) {
		console_error("ShotGrid texture naming: ShotGrid is unreachable, textures keep ArmorPaint names until it is back");
		nm.told = true;
	}
}

// Membre de l'objet à base, en JSON brut (malloc) ; "null" s'il manque.
static char *raw(const char *json, const char *base, const char *name) {
	studio_json_value_t v = studio_json_find(json, string_tmp("%s/%s", base, name));
	return v.type != STUDIO_JSON_INVALID ? strndup(v.start, v.len) : strdup("null");
}

// Ce que la fenêtre d'export montre (lot B6) : réponse de texture_pattern (base "/data") ou son erreur ("/error").
static void keep_info(const char *reply, const char *base, const char *error) {
	const char *members[] = {"template", "default_template", "chosen", "templates", "name", "version"};
	size_t      size      = 64;
	char       *parts[6];
	for (int i = 0; i < 6; ++i) {
		parts[i] = raw(reply, base, members[i]);
		size += strlen(members[i]) + strlen(parts[i]) + 4;
	}
	char *quoted = studio_json_quote(error != NULL ? error : "");
	size += strlen(quoted);
	free(nm.info);
	nm.info = malloc(size);
	size_t n = (size_t)snprintf(nm.info, size, "{");
	for (int i = 0; i < 6; ++i) {
		n += (size_t)snprintf(nm.info + n, size - n, "\"%s\":%s,", members[i], parts[i]);
		free(parts[i]);
	}
	snprintf(nm.info + n, size - n, "\"error\":%s}", error != NULL ? quoted : "null");
	free(quoted);
}

static void collect(void) {
	const char *reply = studio_sg_result(nm.job);
	nm.ready          = true;
	if (!studio_sg_ok(nm.job) && strcmp(studio_sg_get(nm.job, "/error/code"), "sg_unreachable") == 0) {
		// Pas de réponse : redemandé au retour du site ; l'export d'ici là le dit (ensure).
		nm.ready = false;
		iron_log("studio: naming: ShotGrid unreachable, template of %s asked again when it is back", nm.path);
	}
	else if (!studio_sg_ok(nm.job)) {
		console_error(string_tmp("ShotGrid texture naming: %s: %s (textures keep ArmorPaint names)", studio_sg_get(nm.job, "/error/code"),
		                         studio_sg_get(nm.job, "/error/message")));
		nm.told = true;
		keep_info(reply, "/error", studio_sg_get(nm.job, "/error/message"));
	}
	else {
		keep_info(reply, "/data", NULL);
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
	bool offline = studio_sg_offline();
	if (nm.path == NULL || strcmp(nm.path, path) != 0 || (!nm.ready && nm.job == 0)) {
		if (!enabled()) {
			return false;
		}
		if (offline) { // pas de requête qui attendrait le délai du réseau
			if (nm.path == NULL || strcmp(nm.path, path) != 0) {
				track(path, false);
			}
			tell_offline();
			return false;
		}
		track(path, true);
	}
	if (!nm.ready) {
		if (nm.job == 0) {
			return false; // plus de place pour un job : le superviseur l'a déjà signalé
		}
		if (studio_sg_state(nm.job) != STUDIO_SG_DONE && offline) {
			tell_offline(); // la réponse arrivera plus tard, le prochain export en profitera
			return false;
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
		if (!nm.ready) {
			tell_offline(); // ShotGrid injoignable pendant la requête
		}
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

char *studio_export_naming_json(void) {
	ensure();
	return strdup(nm.info != NULL ? nm.info : "null");
}

void studio_export_naming_refresh(void) {
	char *path = project_filepath_get();
	if (path != NULL && path[0] != 0 && enabled() && !studio_sg_offline()) {
		track(path, true);
	}
}

static char *text_or_null(const char *s) {
	return s != NULL && s[0] != 0 ? strdup(s) : NULL;
}

void studio_export_naming_set(const char *path, const char *template, const char *pattern, const char *pattern_udim) {
	track(path, false);
	nm.template     = text_or_null(template);
	nm.pattern      = nm.template != NULL ? text_or_null(pattern) : NULL;
	nm.pattern_udim = nm.template != NULL ? text_or_null(pattern_udim) : NULL;
	nm.ready        = true;
	nm.seen         = true;

	const char *shown = nm.pattern != NULL ? nm.pattern : nm.pattern_udim;
	iron_log("studio: naming: %s -> %s (%s, given)", path, shown != NULL ? shown : "ArmorPaint names",
	         nm.template != NULL ? nm.template : "no template");
}

// Le modèle est demandé dès que le projet ouvert change, pour ne pas attendre le sidecar à l'export.
static void naming_update(void *data) {
	(void)data;
	if (nm.job != 0 && studio_sg_state(nm.job) == STUDIO_SG_DONE) {
		collect();
	}
	char *path    = project_filepath_get();
	bool  offline = studio_sg_offline();
	if (path != NULL && path[0] != 0 && (nm.path == NULL || strcmp(nm.path, path) != 0)) {
		track(path, enabled() && !offline);
	}
	// ShotGrid de retour : le modèle resté sans réponse est redemandé.
	int serial = studio_sg_network_serial();
	if (serial != nm.net_serial) {
		nm.net_serial = serial;
		if (studio_sg_network(NULL) == STUDIO_SG_NET_ONLINE && nm.path != NULL && !nm.ready && nm.job == 0 && enabled()) {
			char *again = strdup(nm.path);
			track(again, true);
			free(again);
		}
	}
}

void studio_export_naming_init(void) {
	sys_notify_on_update(naming_update, NULL);
}
