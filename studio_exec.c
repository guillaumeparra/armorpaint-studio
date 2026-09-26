#include "studio_exec.h"
#include "studio.h"
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <time.h>
#endif

// Jobs ouverts au plus : un script qui oublie studio_exec_release() finit par être refusé plutôt que de fuir.
#define STUDIO_EXEC_MAX_JOBS 256
// À la sortie d'ArmorPaint, délai laissé aux jobs annulés pour tuer leur processus.
#define STUDIO_EXEC_SHUTDOWN_MS 2000

typedef struct exec_job {
	int                  id;
	char               **argv;
	char                *in;
	int                  timeout_ms;
	volatile int         cancel;
	int                  done;     // sous exec_lock()
	int                  released; // idem : libéré par studio_exec_release() pendant qu'il tournait
	studio_proc_result_t result;
} exec_job_t;

static exec_job_t *exec_jobs[STUDIO_EXEC_MAX_JOBS];
static int         exec_next_id = 1;

#ifdef _WIN32
// studio_proc n'est pas encore implémenté sous Windows : les jobs échouent aussitôt, sans thread.
static void exec_lock(void) {}
static void exec_unlock(void) {}
#else
static pthread_mutex_t exec_mutex = PTHREAD_MUTEX_INITIALIZER;

static void exec_lock(void) {
	pthread_mutex_lock(&exec_mutex);
}

static void exec_unlock(void) {
	pthread_mutex_unlock(&exec_mutex);
}
#endif

static void job_free(exec_job_t *j) {
	if (j->argv != NULL) {
		for (int i = 0; j->argv[i] != NULL; i++) {
			free(j->argv[i]);
		}
		free(j->argv);
	}
	free(j->in);
	studio_proc_result_free(&j->result);
	free(j);
}

static void job_run(exec_job_t *j) {
	studio_proc_run((const char *const *)j->argv, NULL, j->in, j->in != NULL ? strlen(j->in) : 0, j->timeout_ms, &j->cancel,
	                &j->result);
	if (!j->result.started) {
		iron_log("studio: exec failed: %s", j->result.error);
	}
	exec_lock();
	j->done       = 1;
	bool released = j->released != 0;
	exec_unlock();
	if (released) {
		job_free(j);
	}
}

#ifndef _WIN32
static void *job_thread(void *arg) {
	job_run(arg);
	return NULL;
}
#endif

// Crée le job et l'inscrit. NULL si la table est pleine.
static exec_job_t *job_create(const char *const *argv, const char *in, int timeout_ms) {
	int argc = 0;
	while (argv != NULL && argv[argc] != NULL) {
		argc++;
	}
	exec_job_t *j = calloc(1, sizeof(exec_job_t));
	if (j == NULL) {
		return NULL;
	}
	j->argv       = calloc((size_t)argc + 1, sizeof(char *));
	j->in         = in != NULL ? strdup(in) : NULL;
	j->timeout_ms = timeout_ms;
	bool ok       = j->argv != NULL && (in == NULL || j->in != NULL);
	for (int i = 0; ok && i < argc; i++) {
		j->argv[i] = strdup(argv[i]);
		ok         = j->argv[i] != NULL;
	}
	if (!ok) {
		job_free(j);
		return NULL;
	}

	exec_lock();
	int slot = -1;
	for (int i = 0; i < STUDIO_EXEC_MAX_JOBS && slot < 0; i++) {
		if (exec_jobs[i] == NULL) {
			slot = i;
		}
	}
	if (slot >= 0) {
		j->id           = exec_next_id++;
		exec_jobs[slot] = j;
	}
	exec_unlock();
	if (slot < 0) {
		iron_log("studio: exec refused, %d jobs open (studio_exec_release missing?)", STUDIO_EXEC_MAX_JOBS);
		job_free(j);
		return NULL;
	}
	return j;
}

// Sous exec_lock().
static int job_slot(int id) {
	for (int i = 0; i < STUDIO_EXEC_MAX_JOBS; i++) {
		if (exec_jobs[i] != NULL && exec_jobs[i]->id == id) {
			return i;
		}
	}
	return -1;
}

int studio_exec_start(const char *const *argv, const char *in, int timeout_ms) {
	exec_job_t *j = job_create(argv, in, timeout_ms);
	if (j == NULL) {
		return 0;
	}
	int id = j->id;
#ifdef _WIN32
	job_run(j);
#else
	pthread_attr_t attr;
	pthread_t      thread;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&thread, &attr, job_thread, j) != 0) {
		job_run(j); // pas de thread disponible : on exécute ici plutôt que de perdre le job
	}
	pthread_attr_destroy(&attr);
#endif
	return id;
}

int studio_exec_run(const char *const *argv, const char *in, int timeout_ms) {
	exec_job_t *j = job_create(argv, in, timeout_ms);
	if (j == NULL) {
		return 0;
	}
	int id = j->id;
	job_run(j);
	return id;
}

int studio_exec_state(int job) {
	exec_lock();
	int slot  = job_slot(job);
	int state = slot < 0 ? STUDIO_EXEC_UNKNOWN : exec_jobs[slot]->done ? STUDIO_EXEC_DONE : STUDIO_EXEC_RUNNING;
	exec_unlock();
	return state;
}

const studio_proc_result_t *studio_exec_result(int job) {
	exec_lock();
	int                         slot = job_slot(job);
	const studio_proc_result_t *r    = slot >= 0 && exec_jobs[slot]->done ? &exec_jobs[slot]->result : NULL;
	exec_unlock();
	return r;
}

void studio_exec_cancel(int job) {
	exec_lock();
	int slot = job_slot(job);
	if (slot >= 0) {
		exec_jobs[slot]->cancel = 1;
	}
	exec_unlock();
}

void studio_exec_release(int job) {
	exec_lock();
	int         slot = job_slot(job);
	exec_job_t *j    = slot >= 0 ? exec_jobs[slot] : NULL;
	bool        now  = false;
	if (j != NULL) {
		exec_jobs[slot] = NULL;
		now             = j->done != 0;
		if (!now) {
			j->released = 1; // le thread le libérera
			j->cancel   = 1;
		}
	}
	exec_unlock();
	if (now) {
		job_free(j);
	}
}

// À la sortie d'ArmorPaint : les processus sont dans leur propre groupe et ne recevraient pas le signal du terminal,
// on les tue.
static void studio_exec_shutdown(void) {
	exec_lock();
	int running = 0;
	for (int i = 0; i < STUDIO_EXEC_MAX_JOBS; i++) {
		if (exec_jobs[i] != NULL && !exec_jobs[i]->done) {
			exec_jobs[i]->cancel = 1;
			running++;
		}
	}
	exec_unlock();
#ifndef _WIN32
	for (int waited = 0; running > 0 && waited < STUDIO_EXEC_SHUTDOWN_MS; waited += 10) {
		struct timespec ts = {0, 10 * 1000000};
		nanosleep(&ts, NULL);
		exec_lock();
		running = 0;
		for (int i = 0; i < STUDIO_EXEC_MAX_JOBS; i++) {
			running += exec_jobs[i] != NULL && !exec_jobs[i]->done;
		}
		exec_unlock();
	}
#endif
}

// ---- Scripting (minic) -----------------------------------------------------------------------------------------------

#ifdef WITH_EVAL

#include "iron_array.h"
#include "minic.h"

// argv d'un script : un any_array_t de chaînes (string_split("oiiotool\n-i\n…", "\n"), ou any_array_push).
static int exec_from_script(minic_val_t *a, int n, bool wait) {
	any_array_t *arr = minic_arg_p(a, n, 0);
	const char  *in  = minic_arg_p(a, n, 1);
	int          ms  = minic_arg_i(a, n, 2);
	int          len = arr != NULL ? (int)arr->length : 0;
	const char **argv = calloc((size_t)len + 1, sizeof(char *));
	if (argv == NULL) {
		return 0;
	}
	for (int i = 0; i < len; i++) {
		argv[i] = arr->buffer[i];
		if (argv[i] == NULL) { // tableau troué : on s'arrête là plutôt que de lire un NULL comme fin de argv plus loin
			break;
		}
	}
	int job = wait ? studio_exec_run(argv, in, ms) : studio_exec_start(argv, in, ms);
	free(argv);
	return job;
}

static minic_val_t mn_studio_exec_start(minic_val_t *a, int n) {
	return minic_val_int(exec_from_script(a, n, false));
}

static minic_val_t mn_studio_exec_run(minic_val_t *a, int n) {
	return minic_val_int(exec_from_script(a, n, true));
}

static minic_val_t mn_studio_exec_state(minic_val_t *a, int n) {
	return minic_val_int(studio_exec_state(minic_arg_i(a, n, 0)));
}

static minic_val_t mn_studio_exec_exit_code(minic_val_t *a, int n) {
	const studio_proc_result_t *r = studio_exec_result(minic_arg_i(a, n, 0));
	return minic_val_int(r != NULL ? r->exit_code : -1);
}

static minic_val_t mn_studio_exec_timed_out(minic_val_t *a, int n) {
	const studio_proc_result_t *r = studio_exec_result(minic_arg_i(a, n, 0));
	return minic_val_int(r != NULL && r->timed_out ? 1 : 0);
}

static minic_val_t mn_studio_exec_stdout(minic_val_t *a, int n) {
	const studio_proc_result_t *r = studio_exec_result(minic_arg_i(a, n, 0));
	return minic_val_ptr(r != NULL ? r->out : "");
}

static minic_val_t mn_studio_exec_stderr(minic_val_t *a, int n) {
	const studio_proc_result_t *r = studio_exec_result(minic_arg_i(a, n, 0));
	return minic_val_ptr(r != NULL ? r->err : "");
}

static minic_val_t mn_studio_exec_error(minic_val_t *a, int n) {
	const studio_proc_result_t *r = studio_exec_result(minic_arg_i(a, n, 0));
	return minic_val_ptr((void *)(r != NULL ? r->error : ""));
}

static minic_val_t mn_studio_exec_cancel(minic_val_t *a, int n) {
	studio_exec_cancel(minic_arg_i(a, n, 0));
	return minic_val_int(0);
}

static minic_val_t mn_studio_exec_release(minic_val_t *a, int n) {
	studio_exec_release(minic_arg_i(a, n, 0));
	return minic_val_int(0);
}

static void studio_exec_register_script_api(void) {
	// Signatures au format de minic_register (sans les noms de paramètres) ; la référence est dans README.md.
	minic_register("studio_exec_start", "i(p,p,i)", mn_studio_exec_start);
	minic_register("studio_exec_run", "i(p,p,i)", mn_studio_exec_run);
	minic_register("studio_exec_state", "i(i)", mn_studio_exec_state);
	minic_register("studio_exec_exit_code", "i(i)", mn_studio_exec_exit_code);
	minic_register("studio_exec_timed_out", "i(i)", mn_studio_exec_timed_out);
	minic_register("studio_exec_stdout", "p(i)", mn_studio_exec_stdout);
	minic_register("studio_exec_stderr", "p(i)", mn_studio_exec_stderr);
	minic_register("studio_exec_error", "p(i)", mn_studio_exec_error);
	minic_register("studio_exec_cancel", "v(i)", mn_studio_exec_cancel);
	minic_register("studio_exec_release", "v(i)", mn_studio_exec_release);
}

#endif

void studio_exec_init(void) {
	atexit(studio_exec_shutdown);
#ifdef WITH_EVAL
	studio_exec_register_script_api();
	iron_log("studio: exec ready, scripting API registered");
#else
	iron_log("studio: exec ready (no scripting: WITH_EVAL not defined)");
#endif
}
