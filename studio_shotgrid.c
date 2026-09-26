#include "studio_shotgrid.h"
#include "studio.h"
#include "studio_json.h"
#include "studio_sg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Cœur ArmorPaint / iron
char *path_data(void);

static void shotgrid_log(const char *line) {
	iron_log("studio: shotgrid: %s", line);
}

// Commande du sidecar : STUDIO_SG_PYTHON (défaut python3, cherché dans le PATH) et STUDIO_SG_BRIDGE (défaut
// data/studio_sg_bridge.py, copié depuis external/assets/ par le build).
static void shotgrid_configure(void) {
	const char *python = getenv("STUDIO_SG_PYTHON");
	const char *bridge = getenv("STUDIO_SG_BRIDGE");
	char        path[4096];
	if (python == NULL || python[0] == 0) {
		python = "python3";
	}
	if (bridge == NULL || bridge[0] == 0) {
		snprintf(path, sizeof(path), "%sstudio_sg_bridge.py", path_data());
		bridge = path;
	}
	const char *argv[] = {python, bridge, NULL};
	// Pas de __pycache__ dans data/ : le paquet de l'app est signé (macOS), et ce dossier peut être en lecture seule.
	const char        *env[]  = {"PYTHONDONTWRITEBYTECODE=1", NULL};
	studio_sg_config_t config = {.argv = argv, .env = env, .log = shotgrid_log};
	studio_sg_configure(&config);
	iron_log("studio: shotgrid ready (sidecar: %s %s, started on first call)", python, bridge);
}

// ---- Scripting (minic) -----------------------------------------------------------------------------------------------

#ifdef WITH_EVAL

#include "minic.h"

static minic_val_t mn_studio_sg_start(minic_val_t *a, int n) {
	return minic_val_int(studio_sg_start(minic_arg_p(a, n, 0), minic_arg_p(a, n, 1), minic_arg_i(a, n, 2)));
}

static minic_val_t mn_studio_sg_run(minic_val_t *a, int n) {
	return minic_val_int(studio_sg_run(minic_arg_p(a, n, 0), minic_arg_p(a, n, 1), minic_arg_i(a, n, 2)));
}

static minic_val_t mn_studio_sg_state(minic_val_t *a, int n) {
	return minic_val_int(studio_sg_state(minic_arg_i(a, n, 0)));
}

static minic_val_t mn_studio_sg_ok(minic_val_t *a, int n) {
	return minic_val_int(studio_sg_ok(minic_arg_i(a, n, 0)) ? 1 : 0);
}

static minic_val_t mn_studio_sg_result(minic_val_t *a, int n) {
	const char *r = studio_sg_result(minic_arg_i(a, n, 0));
	return minic_val_ptr((void *)(r != NULL ? r : ""));
}

static minic_val_t mn_studio_sg_get(minic_val_t *a, int n) {
	return minic_val_ptr((void *)studio_sg_get(minic_arg_i(a, n, 0), minic_arg_p(a, n, 1)));
}

static minic_val_t mn_studio_sg_length(minic_val_t *a, int n) {
	return minic_val_int(studio_sg_length(minic_arg_i(a, n, 0), minic_arg_p(a, n, 1)));
}

static minic_val_t mn_studio_sg_int(minic_val_t *a, int n) {
	long long fallback = minic_arg_i(a, n, 2);
	long long v        = studio_sg_int(minic_arg_i(a, n, 0), minic_arg_p(a, n, 1), fallback);
	return minic_val_int(v >= -2147483647LL - 1 && v <= 2147483647LL ? (int)v : (int)fallback);
}

static minic_val_t mn_studio_sg_cancel(minic_val_t *a, int n) {
	studio_sg_cancel(minic_arg_i(a, n, 0));
	return minic_val_int(0);
}

static minic_val_t mn_studio_sg_release(minic_val_t *a, int n) {
	studio_sg_release(minic_arg_i(a, n, 0));
	return minic_val_int(0);
}

static minic_val_t mn_studio_sg_restart(minic_val_t *a, int n) {
	(void)a, (void)n;
	studio_sg_stop();
	return minic_val_int(0);
}

// Pour composer args : "{\"path\":" + studio_json_quote(path) + "}". Comme les chaînes de string(), jamais libérée.
static minic_val_t mn_studio_json_quote(minic_val_t *a, int n) {
	return minic_val_ptr(studio_json_quote(minic_arg_p(a, n, 0)));
}

static void shotgrid_register_script_api(void) {
	// Signatures au format de minic_register (sans les noms de paramètres) ; la référence est dans README.md.
	minic_register("studio_sg_start", "i(p,p,i)", mn_studio_sg_start);
	minic_register("studio_sg_run", "i(p,p,i)", mn_studio_sg_run);
	minic_register("studio_sg_state", "i(i)", mn_studio_sg_state);
	minic_register("studio_sg_ok", "i(i)", mn_studio_sg_ok);
	minic_register("studio_sg_result", "p(i)", mn_studio_sg_result);
	minic_register("studio_sg_get", "p(i,p)", mn_studio_sg_get);
	minic_register("studio_sg_length", "i(i,p)", mn_studio_sg_length);
	minic_register("studio_sg_int", "i(i,p,i)", mn_studio_sg_int);
	minic_register("studio_sg_cancel", "v(i)", mn_studio_sg_cancel);
	minic_register("studio_sg_release", "v(i)", mn_studio_sg_release);
	minic_register("studio_sg_restart", "v()", mn_studio_sg_restart);
	minic_register("studio_json_quote", "p(p)", mn_studio_json_quote);
}

#endif

void studio_shotgrid_init(void) {
	shotgrid_configure();
	atexit(studio_sg_shutdown);
#ifdef WITH_EVAL
	shotgrid_register_script_api();
#endif
}
