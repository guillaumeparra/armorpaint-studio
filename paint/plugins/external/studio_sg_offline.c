#include "studio_sg_offline.h"
#include "iron_string.h"
#include "iron_system.h"
#include "studio.h"
#include "studio_audit.h"
#include "studio_fs.h"
#include "studio_json.h"
#include "studio_prefs.h"
#include "studio_sg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Cœur ArmorPaint (paint/sources/console.c, ui/ui_menu.c)
void console_info(char *s);
void console_error(char *s);
bool ui_menu_button(char *text, char *label, int icon);
void ui_menu_label(char *text, char *shortcut);

#define QUEUE_DIR         "studio_publish_queue"
#define COUNT_EVERY_S     2.0
#define CHECK_DEFAULT_S   120
#define CHECK_TIMEOUT_MS  60000
#define REPLAY_TIMEOUT_MS 900000 // vignettes à envoyer ; interrompu, le rejeu suivant reprend où il en était
#define WAVE_MAX          64     // projets rejoués par vague
#define OL_ICON_NONE      -1     // ICON_NONE (paint/sources/enums.h)

static struct {
	bool   queue_known;
	char  *queue;       // NULL : pas de file d'attente
	int    interval;    // secondes entre deux essais hors ligne, 0 : jamais
	int    serial;      // studio_sg_network_serial() déjà traité
	int    state;       // l'état réseau correspondant
	char  *reason;      // raison du passage hors ligne
	int    pending;     // publishes en file, au dernier relevé
	double counted;     // sys_time() du dernier relevé
	int    job_check;   // check_online en cours, ou programmé (job différé)
	bool   check_asked; // Check Connection : le résultat est dit dans la console
	int    job_replay;
	int    wave[WAVE_MAX]; // projets déjà rejoués dans la vague en cours
	int    wave_count;
	int    replayed;    // publishes enregistrés dans la vague
	int    failed;      // publishes restés en file sur une erreur (autre que réseau)
	char  *error;       // la première de ces erreurs
	int    other_sites; // publishes en file pour un autre site ShotGrid (dernier rejeu)
} ol;

// Texte court pour la console ou le menu : au plus max octets, sans couper un caractère UTF-8 (chaîne temporaire).
static char *shorten(const char *text, size_t max) {
	size_t n = strlen(text);
	if (n <= max) {
		return string_tmp("%s", text);
	}
	while (max > 0 && ((unsigned char)text[max] & 0xC0) == 0x80) {
		max--;
	}
	return string_tmp("%.*s...", (int)max, text);
}

const char *studio_sg_queue_dir(void) {
	if (!ol.queue_known) {
		ol.queue_known  = true;
		const char *env = getenv("STUDIO_SG_QUEUE");
		if (env != NULL && env[0] != 0) {
			ol.queue = strdup(env);
		}
		else {
			char *dir = studio_prefs_dir();
			if (dir != NULL) {
#ifdef _WIN32
				ol.queue = strdup(string_tmp("%s\\%s", dir, QUEUE_DIR));
#else
				ol.queue = strdup(string_tmp("%s/%s", dir, QUEUE_DIR));
#endif
			}
			free(dir);
		}
	}
	return ol.queue;
}

bool studio_sg_offline(void) {
	return studio_sg_network(NULL) == STUDIO_SG_NET_OFFLINE;
}

static void count_pending(bool now) {
	double t = sys_time();
	if (!now && t - ol.counted < COUNT_EVERY_S) {
		return;
	}
	ol.counted      = t;
	const char *dir = studio_sg_queue_dir();
	int         n   = dir != NULL ? studio_fs_count(dir, ".json") : 0;
	ol.pending      = n > 0 ? n : 0;
}

int studio_sg_pending(void) {
	return ol.pending;
}

// ---- Rejeu de la file ---------------------------------------------------------------------------------------------------
// Un appel par projet (le sidecar tient un projet Toolkit à la fois) : le premier prend le projet du sidecar s'il en a
// en file, sinon le plus ancien ; sa réponse liste les autres projets qui en ont, rejoués ensuite dans la même vague.

static void replay_start(int project_id) {
	ol.job_replay = studio_sg_start("replay_publishes", project_id > 0 ? string_tmp("{\"project_id\":%d}", project_id) : "{}", REPLAY_TIMEOUT_MS);
}

void studio_sg_replay_pending(void) {
	if (ol.job_replay != 0) {
		return;
	}
	count_pending(true);
	if (ol.pending == 0) {
		console_info("No publish is waiting for ShotGrid");
		return;
	}
	ol.wave_count = 0;
	ol.replayed   = 0;
	ol.failed     = 0;
	free(ol.error);
	ol.error = NULL;
	iron_log("studio: shotgrid: %d queued publish(es) to register", ol.pending);
	replay_start(0);
}

static bool in_wave(int project_id) {
	for (int i = 0; i < ol.wave_count; i++) {
		if (ol.wave[i] == project_id) {
			return true;
		}
	}
	return false;
}

static void on_replay_done(void) {
	int job       = ol.job_replay;
	ol.job_replay = 0;
	int next      = 0;
	if (!studio_sg_ok(job)) {
		const char *code = studio_sg_get(job, "/error/code");
		if (strcmp(code, "sg_unreachable") != 0) { // hors ligne : dit par on_network
			console_error(string_tmp("ShotGrid: queued publishes not registered: %s: %s", code, shorten(studio_sg_get(job, "/error/message"), 300)));
		}
	}
	else {
		int project = (int)studio_sg_int(job, "/data/project_id", 0);
		int done    = studio_sg_length(job, "/data/replayed");
		int failed  = studio_sg_length(job, "/data/failed");
		ol.replayed += done > 0 ? done : 0;
		ol.failed += failed > 0 ? failed : 0;
		if (failed > 0 && ol.error == NULL) {
			ol.error = strdup(studio_sg_get(job, "/data/failed/0/error"));
		}
		ol.other_sites = (int)studio_sg_int(job, "/data/other_sites", 0);
		if (project > 0 && ol.wave_count < WAVE_MAX) {
			ol.wave[ol.wave_count++] = project;
		}
		bool stopped = studio_json_find(studio_sg_result(job), "/data/stopped").type == STUDIO_JSON_STRING;
		for (int i = 0; !stopped && next == 0 && i < studio_sg_length(job, "/data/projects"); i++) {
			int p = (int)studio_sg_int(job, string_tmp("/data/projects/%d", i), 0);
			next  = p > 0 && !in_wave(p) && ol.wave_count < WAVE_MAX ? p : 0;
		}
		iron_log("studio: shotgrid: replay of project %d: %d registered, %d failed%s", project, done, failed,
		         stopped ? ", stopped (ShotGrid unreachable)" : "");
	}
	studio_sg_release(job);
	if (next > 0) {
		replay_start(next);
		return;
	}
	count_pending(true);
	if (ol.replayed > 0) {
		console_info(string_tmp("ShotGrid: %d queued publish%s registered", ol.replayed, ol.replayed > 1 ? "es" : ""));
	}
	if (ol.failed > 0) {
		console_error(string_tmp("ShotGrid: %d queued publish%s not registered, still waiting: %s", ol.failed, ol.failed > 1 ? "es" : "",
		                         shorten(ol.error != NULL ? ol.error : "?", 300)));
	}
	if (ol.other_sites > 0) {
		iron_log("studio: shotgrid: %d queued publish(es) for another ShotGrid site, replayed from a session on that site", ol.other_sites);
	}
}

// ---- Connexion ----------------------------------------------------------------------------------------------------------
// Hors ligne, le site est essayé toutes les ol.interval secondes par un job différé du superviseur : il part à l'heure
// même quand ArmorPaint est au repos (sans entrée, iron ne fait plus de frames), et sa réponse le réveille.

static void check_start(int delay_s) {
	ol.job_check = studio_sg_start_after("check_online", NULL, CHECK_TIMEOUT_MS, delay_s * 1000);
}

// Retire l'essai programmé qui n'est pas encore parti (pas celui qu'a demandé l'artiste).
static void check_unschedule(void) {
	if (ol.job_check != 0 && !ol.check_asked && studio_sg_state(ol.job_check) == STUDIO_SG_QUEUED) {
		studio_sg_release(ol.job_check);
		ol.job_check = 0;
	}
}

// Un essai est en cours, ou demandé : pas seulement programmé.
static bool checking(void) {
	return ol.job_check != 0 && (ol.check_asked || studio_sg_state(ol.job_check) == STUDIO_SG_RUNNING);
}

void studio_sg_check_connection(void) {
	check_unschedule(); // remplacé par un essai tout de suite
	if (ol.job_check == 0) {
		check_start(0);
	}
	ol.check_asked = true;
}

static void on_check_done(void) {
	int job = ol.job_check;
	if (ol.check_asked) {
		const char *code = studio_sg_get(job, "/error/code");
		if (studio_sg_ok(job)) {
			if (ol.state != STUDIO_SG_NET_OFFLINE) { // sinon, on_network dit le retour du site
				console_info(string_tmp("ShotGrid is reachable (%s)", studio_sg_get(job, "/data/site")));
			}
		}
		else if (strcmp(code, "sg_unreachable") == 0) {
			console_error(string_tmp("ShotGrid still unreachable: %s", shorten(studio_sg_get(job, "/error/message"), 300)));
		}
		else {
			console_error(string_tmp("ShotGrid: %s: %s", code, shorten(studio_sg_get(job, "/error/message"), 300)));
		}
	}
	ol.check_asked = false;
	studio_sg_release(job);
	ol.job_check = 0;
	if (ol.state == STUDIO_SG_NET_OFFLINE && ol.interval > 0) {
		check_start(ol.interval); // toujours hors ligne (on_network est passé avant) : essai suivant
	}
}

// Changement d'état relevé par le superviseur (studio_sg.h) : console, journal, et au retour du site, rejeu.
static void on_network(void) {
	int serial = studio_sg_network_serial();
	if (serial == ol.serial) {
		return;
	}
	ol.serial    = serial;
	char *reason = NULL;
	int   was    = ol.state;
	ol.state     = studio_sg_network(&reason);
	if (ol.state == STUDIO_SG_NET_OFFLINE) {
		free(ol.reason);
		ol.reason = reason != NULL ? reason : strdup("no reason given");
		reason    = NULL;
		console_error(string_tmp("ShotGrid unreachable (%s): ArmorPaint keeps working on files, publishes are queued "
		                         "until ShotGrid is back",
		                         shorten(ol.reason, 300)));
		char *q = studio_json_quote(ol.reason);
		studio_audit("sg_offline", string_tmp("\"reason\":%s", q));
		free(q);
		if (ol.job_check == 0 && ol.interval > 0) {
			check_start(ol.interval);
		}
	}
	else if (ol.state == STUDIO_SG_NET_ONLINE) {
		check_unschedule(); // le site a répondu à autre chose
		count_pending(true);
		if (was == STUDIO_SG_NET_OFFLINE) {
			console_info(ol.pending > 0 ? string_tmp("ShotGrid is back: registering %d queued publish%s", ol.pending, ol.pending > 1 ? "es" : "")
			                            : "ShotGrid is back");
			studio_audit("sg_online", string_tmp("\"pending\":%d", ol.pending));
		}
		free(ol.reason);
		ol.reason = NULL;
		if (ol.pending > ol.other_sites) {
			studio_sg_replay_pending();
		}
	}
	free(reason);
}

static void offline_update(void *data) {
	(void)data;
	on_network();
	count_pending(false);
	if (ol.job_check != 0 && studio_sg_state(ol.job_check) == STUDIO_SG_DONE) {
		on_check_done();
	}
	if (ol.job_replay != 0 && studio_sg_state(ol.job_replay) == STUDIO_SG_DONE) {
		on_replay_done();
	}
}

// ---- Menu ShotGrid ----------------------------------------------------------------------------------------------------

int studio_sg_offline_menu(void) {
	int lines = 0;
	if (ol.state == STUDIO_SG_NET_OFFLINE) {
		ui_menu_label(string_tmp("ShotGrid offline: %s", shorten(ol.reason != NULL ? ol.reason : "", 60)), NULL);
		if (checking()) {
			ui_menu_label("Checking the connection...", NULL);
		}
		else if (ui_menu_button("Check Connection", "", OL_ICON_NONE)) {
			studio_sg_check_connection();
		}
		lines += 2;
	}
	if (ol.pending > 0 || ol.job_replay != 0) {
		ui_menu_label(string_tmp("%d publish%s waiting for ShotGrid", ol.pending, ol.pending > 1 ? "es" : ""), NULL);
		if (ol.job_replay != 0) {
			ui_menu_label("Registering queued publishes...", NULL);
		}
		else if (ol.state != STUDIO_SG_NET_OFFLINE && ui_menu_button("Replay Pending Publishes", "", OL_ICON_NONE)) {
			studio_sg_replay_pending();
		}
		lines += ol.job_replay != 0 || ol.state != STUDIO_SG_NET_OFFLINE ? 2 : 1;
	}
	return lines;
}

// ---- Scripting (minic) -----------------------------------------------------------------------------------------------

#ifdef WITH_EVAL

#include "minic.h"

static minic_val_t mn_studio_sg_offline(minic_val_t *a, int n) {
	(void)a, (void)n;
	return minic_val_int(studio_sg_offline() ? 1 : 0);
}

static minic_val_t mn_studio_sg_pending(minic_val_t *a, int n) {
	(void)a, (void)n;
	count_pending(true);
	return minic_val_int(ol.pending);
}

static minic_val_t mn_studio_sg_busy(minic_val_t *a, int n) {
	(void)a, (void)n;
	return minic_val_int(checking() || ol.job_replay != 0 ? 1 : 0);
}

static minic_val_t mn_studio_sg_check_connection(minic_val_t *a, int n) {
	(void)a, (void)n;
	studio_sg_check_connection();
	return minic_val_int(0);
}

static minic_val_t mn_studio_sg_replay_pending(minic_val_t *a, int n) {
	(void)a, (void)n;
	studio_sg_replay_pending();
	return minic_val_int(0);
}

static minic_val_t mn_studio_sg_queue_dir(minic_val_t *a, int n) {
	(void)a, (void)n;
	const char *dir = studio_sg_queue_dir();
	return minic_val_ptr((void *)(dir != NULL ? dir : ""));
}

static void offline_register_script_api(void) {
	minic_register("studio_sg_offline", "i()", mn_studio_sg_offline);
	minic_register("studio_sg_pending", "i()", mn_studio_sg_pending);
	minic_register("studio_sg_offline_busy", "i()", mn_studio_sg_busy);
	minic_register("studio_sg_check_connection", "v()", mn_studio_sg_check_connection);
	minic_register("studio_sg_replay_pending", "v()", mn_studio_sg_replay_pending);
	minic_register("studio_sg_queue_dir", "p()", mn_studio_sg_queue_dir);
}

#endif

void studio_sg_offline_init(void) {
	const char *env = getenv("STUDIO_SG_CHECK_INTERVAL");
	ol.interval     = env != NULL && env[0] != 0 ? atoi(env) : CHECK_DEFAULT_S;
	if (ol.interval < 0) {
		ol.interval = 0;
	}
	ol.serial = studio_sg_network_serial();
	ol.state  = studio_sg_network(NULL);
	count_pending(true);
	const char *queue = studio_sg_queue_dir();
	iron_log("studio: shotgrid publish queue: %s (%d waiting), connection retried every %d s when offline", queue != NULL ? queue : "none", ol.pending,
	         ol.interval);
	sys_notify_on_update(offline_update, NULL);
#ifdef WITH_EVAL
	offline_register_script_api();
#endif
}
