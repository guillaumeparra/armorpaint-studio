// Banc du superviseur ShotGrid (studio_sg.c, lot C2), hors de l'app, sous ASan/UBSan : lancé par sg.sh.
// Contre sg_fake.py (pannes à la demande), puis de bout en bout contre le vrai sidecar sur le site simulé
// (sg_fixture.py) si STUDIO_TEST_TK_CORE est défini.
#include "../studio_json.h"
#include "../studio_sg.h"
#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int checks   = 0;
static int failures = 0;

#define CHECK(cond, ...)                                                                                               \
	do {                                                                                                               \
		checks++;                                                                                                      \
		if (!(cond)) {                                                                                                 \
			failures++;                                                                                                \
			printf("  FAIL ");                                                                                         \
			printf(__VA_ARGS__);                                                                                       \
			printf("\n");                                                                                              \
		}                                                                                                              \
	} while (0)

static const char *tests_dir;
static const char *workdir;

static double now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void sleep_ms(int ms) {
	struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000};
	nanosleep(&ts, NULL);
}

static int open_fds(void) {
	int  n = 0;
	DIR *d = opendir("/dev/fd");
	while (d != NULL && readdir(d) != NULL) {
		n++;
	}
	if (d != NULL) {
		closedir(d);
	}
	return n;
}

// ---- Log capturé (le thread des jobs logue aussi) ---------------------------------------------------------------------

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static char            log_buf[1 << 20];
static size_t          log_len;

static void capture_log(const char *line) {
	pthread_mutex_lock(&log_mutex);
	size_t n = strlen(line);
	if (log_len + n + 2 < sizeof(log_buf)) {
		memcpy(log_buf + log_len, line, n);
		log_len += n;
		log_buf[log_len++] = '\n';
		log_buf[log_len]   = 0;
	}
	if (getenv("SG_TEST_VERBOSE") != NULL) {
		printf("    log: %s\n", line);
	}
	pthread_mutex_unlock(&log_mutex);
}

static bool log_has(const char *text) {
	pthread_mutex_lock(&log_mutex);
	bool found = strstr(log_buf, text) != NULL;
	pthread_mutex_unlock(&log_mutex);
	return found;
}

static void log_clear(void) {
	pthread_mutex_lock(&log_mutex);
	log_len    = 0;
	log_buf[0] = 0;
	pthread_mutex_unlock(&log_mutex);
}

static volatile int wakes; // appels de config.wake

static void count_wake(void) {
	__sync_fetch_and_add(&wakes, 1);
}

static void use_fake(const char *hello, int hello_ms, int call_ms) {
	char script[1024], mode[64];
	snprintf(script, sizeof(script), "%s/sg_fake.py", tests_dir);
	snprintf(mode, sizeof(mode), "FAKE_HELLO=%s", hello);
	const char        *argv[] = {"python3", script, NULL};
	const char        *env[]  = {mode, "PYTHONDONTWRITEBYTECODE=1", NULL};
	studio_sg_config_t config = {.argv = argv, .env = env, .hello_timeout_ms = hello_ms, .call_timeout_ms = call_ms,
	                             .log = capture_log, .wake = count_wake};
	studio_sg_configure(&config);
}

// ---- Réponses ---------------------------------------------------------------------------------------------------------

static char *text_at(const char *reply, const char *pointer) {
	char *t = studio_json_text(studio_json_find(reply, pointer));
	return t != NULL ? t : strdup("(missing)");
}

// Appel synchrone : vérifie le code d'erreur attendu (NULL = succès), rend la réponse (à libérer).
static char *expect(const char *op, const char *args, int timeout_ms, const char *code, const char *what) {
	char *reply = studio_sg_call(op, args, timeout_ms, NULL);
	CHECK(reply != NULL, "%s: NULL reply", what);
	if (reply == NULL) {
		return strdup("{}");
	}
	char *got = text_at(reply, "/error/code");
	bool  ok  = studio_json_truthy(studio_json_find(reply, "/ok"));
	CHECK(code == NULL ? ok : (!ok && strcmp(got, code) == 0), "%s: expected %s, got %.300s", what, code ? code : "ok",
	      reply);
	free(got);
	return reply;
}

static long long pid_of_sidecar(void) {
	char     *reply = expect("pid", NULL, 0, NULL, "pid");
	long long pid   = studio_json_int(studio_json_find(reply, "/data"), -1);
	free(reply);
	return pid;
}

static bool alive(long long pid) {
	return pid > 0 && kill((pid_t)pid, 0) == 0;
}

// ---- Faux sidecar -----------------------------------------------------------------------------------------------------

static void test_calls(void) {
	use_fake("noise", 0, 0);
	char *r = expect("echo", " {\"a\" :\n [1, \"Élodie\\n\\\"x\\\"\"],\r\n \"b\":{}} \n", 0, NULL, "echo");
	char *a = text_at(r, "/data/a/1");
	CHECK(strcmp(a, "Élodie\n\"x\"") == 0, "echo round trip: %s", a);
	free(a);
	free(r);
	CHECK(log_has("ignored before handshake: Welcome to Python!"), "noise before the handshake is logged");
	free(expect("echo", NULL, 0, NULL, "NULL args"));
	free(expect("echo", "  ", 0, NULL, "blank args"));
	free(expect("echo", "[1]", 0, "bad_request", "array args"));
	free(expect("echo", "{\"a\":1} x", 0, "bad_request", "trailing garbage"));
	free(expect("echo", "{\"a\":", 0, "bad_request", "truncated args"));
	free(expect("", NULL, 0, "bad_request", "empty op"));
	free(expect("nope", NULL, 0, "unknown_op", "sidecar error passed through"));

	char *hello = studio_sg_hello();
	CHECK(hello != NULL && studio_json_int(studio_json_find(hello, "/data/protocol"), 0) == 1, "hello kept");
	free(hello);

	long long pid = pid_of_sidecar();
	CHECK(pid_of_sidecar() == pid, "sidecar kept between calls");
	studio_sg_stop();
	CHECK(!alive(pid), "stop ends the sidecar");
	CHECK(studio_sg_hello() == NULL, "no hello once stopped");
	CHECK(log_has("exiting"), "stop is an EOF: the sidecar exits on its own");
	long long pid2 = pid_of_sidecar();
	CHECK(pid2 != pid && pid2 > 0, "next call restarts it");

	r = expect("stale", NULL, 0, NULL, "stale reply");
	char *d = text_at(r, "/data");
	CHECK(strcmp(d, "fresh") == 0 && log_has("discarding unexpected reply") && log_has("this is not JSON"),
	      "stale and non-JSON replies discarded: %s", d);
	free(d);
	free(r);

	r = expect("huge", "{\"n\":8000000}", 0, NULL, "8 MB reply");
	CHECK(studio_json_find(r, "/data").len == 8000002, "8 MB reply intact");
	free(r);
	printf("  ok   calls: args normalised and checked, errors passed through, sidecar kept, stale replies, 8 MB\n");
}

static void test_failures(void) {
	use_fake("ok", 0, 0);
	long long pid = pid_of_sidecar();
	char     *r   = expect("crash", NULL, 0, "sidecar_died", "crash");
	char     *m   = text_at(r, "/error/message");
	CHECK(strstr(m, "exit code 3") != NULL && strstr(m, "boom: the fake sidecar crashes") != NULL, "crash message: %s", m);
	free(m);
	free(r);
	CHECK(pid_of_sidecar() != pid, "restarted after a crash");

	// Mort pendant l'attente : kill externe.
	pid = pid_of_sidecar();
	kill((pid_t)pid, SIGKILL);
	sleep_ms(200);
	free(expect("pid", NULL, 0, NULL, "sidecar killed while idle: restarted transparently"));

	double t0 = now_s();
	r         = expect("sleep", "{\"s\":10}", 300, "timeout", "timeout");
	CHECK(now_s() - t0 < 2.0, "timeout honoured: %.2f s", now_s() - t0);
	free(r);
	CHECK(log_has("killing the sidecar: timeout"), "timeout kills the sidecar");
	free(expect("echo", NULL, 0, NULL, "works after a timeout"));

	t0 = now_s();
	free(expect("close_stdout", NULL, 20000, "sidecar_failed", "stdout closed"));
	CHECK(now_s() - t0 < 3.0, "stdout closed detected without waiting the timeout: %.2f s", now_s() - t0);

	// Relance demandée : rejouée une fois, de façon transparente.
	r        = expect("project", "{\"p\":1}", 0, NULL, "project 1");
	long long p1 = studio_json_int(studio_json_find(r, "/data/pid"), -1);
	free(r);
	log_clear();
	r            = expect("project", "{\"p\":2}", 0, NULL, "project switch replayed");
	long long p2 = studio_json_int(studio_json_find(r, "/data/pid"), -1);
	CHECK(p2 != p1 && studio_json_int(studio_json_find(r, "/data/project"), -1) == 2, "replayed on a new sidecar");
	CHECK(log_has("sidecar restarting: switching to project 2") && log_has("exit code 0"), "restart logged");
	free(r);
	free(expect("always_restart", NULL, 0, "restart_required", "restart replayed once only"));

	// Journal : ligne par ligne, la ligne sans fin relayée à l'arrêt.
	log_clear();
	free(expect("log", "{\"n\":3}", 0, NULL, "log"));
	CHECK(log_has("line 0\nline 1\nline 2\n") && !log_has("partial without newline"), "stderr relayed by line");
	studio_sg_stop();
	CHECK(log_has("partial without newline"), "partial line flushed at stop");
	printf("  ok   failures: crash (stderr tail), killed while idle, timeout, closed stdout, restart replayed once\n");
}

static void test_startup(void) {
	use_fake("bad_protocol", 0, 0);
	char *r = expect("echo", NULL, 0, "sidecar_failed", "protocol mismatch");
	char *m = text_at(r, "/error/message");
	CHECK(strstr(m, "protocol 2") != NULL, "protocol message: %s", m);
	free(m);
	free(r);

	use_fake("fail", 0, 0);
	r = expect("echo", NULL, 0, "sidecar_failed", "startup error");
	m = text_at(r, "/error/message");
	CHECK(strstr(m, "sidecar startup failed: boom") != NULL, "startup message: %s", m);
	free(m);
	free(r);

	use_fake("exit", 0, 0);
	r = expect("echo", NULL, 0, "sidecar_failed", "exit at startup");
	m = text_at(r, "/error/message");
	CHECK(strstr(m, "exit code 4") != NULL && strstr(m, "no module named sgtk") != NULL, "exit message: %s", m);
	free(m);
	free(r);

	use_fake("silent", 500, 0);
	double t0 = now_s();
	free(expect("echo", NULL, 0, "sidecar_failed", "no handshake"));
	CHECK(now_s() - t0 < 2.0, "handshake timeout: %.2f s", now_s() - t0);

	const char        *argv[] = {"/nonexistent/python3", "x.py", NULL};
	studio_sg_config_t config = {.argv = argv, .log = capture_log};
	studio_sg_configure(&config);
	r = expect("echo", NULL, 0, "sidecar_failed", "missing interpreter");
	m = text_at(r, "/error/message");
	CHECK(strstr(m, "/nonexistent/python3") != NULL, "missing interpreter message: %s", m);
	free(m);
	free(r);
	printf("  ok   startup: protocol mismatch, startup error, exit with stderr tail, silent sidecar, no interpreter\n");
}

static int wait_done(int job, double seconds) {
	double t0 = now_s();
	while (studio_sg_state(job) != STUDIO_SG_DONE && now_s() - t0 < seconds) {
		sleep_ms(5);
	}
	return studio_sg_state(job);
}

static void *cancel_later(void *arg) {
	sleep_ms(300);
	*(volatile int *)arg = 1;
	return NULL;
}

static void test_jobs(void) {
	use_fake("ok", 0, 0);
	// Dans l'ordre, sur un seul sidecar.
	int ids[8];
	for (int i = 0; i < 8; i++) {
		char args[64];
		snprintf(args, sizeof(args), "{\"i\":%d,\"name\":\"n%d\",\"list\":[1,2,3]}", i, i);
		ids[i] = studio_sg_start("echo", args, 0);
		CHECK(ids[i] > 0, "start %d", i);
	}
	CHECK(wait_done(ids[7], 20) == STUDIO_SG_DONE, "jobs done");
	for (int i = 0; i < 8; i++) {
		char name[16];
		snprintf(name, sizeof(name), "n%d", i);
		CHECK(studio_sg_state(ids[i]) == STUDIO_SG_DONE && studio_sg_ok(ids[i]), "job %d ok", i);
		CHECK(strcmp(studio_sg_get(ids[i], "/data/name"), name) == 0, "job %d get", i);
		CHECK(studio_sg_int(ids[i], "/data/i", -1) == i && studio_sg_length(ids[i], "/data/list") == 3, "job %d int", i);
		CHECK(strcmp(studio_sg_get(ids[i], "/data/nope"), "") == 0, "job %d get missing", i);
		studio_sg_release(ids[i]);
		CHECK(studio_sg_state(ids[i]) == STUDIO_SG_UNKNOWN && studio_sg_result(ids[i]) == NULL, "job %d released", i);
	}

	// Annulation : en cours (sidecar tué), en file (ne part pas) ; libération en cours et en file.
	int running = studio_sg_start("sleep", "{\"s\":10}", 0);
	int queued  = studio_sg_start("echo", NULL, 0);
	int dropped = studio_sg_start("echo", NULL, 0);
	sleep_ms(300);
	CHECK(studio_sg_state(running) == STUDIO_SG_RUNNING && studio_sg_state(queued) == STUDIO_SG_QUEUED, "states");
	studio_sg_release(dropped);
	studio_sg_cancel(queued);
	double t0 = now_s();
	studio_sg_cancel(running);
	CHECK(wait_done(running, 5) == STUDIO_SG_DONE && now_s() - t0 < 2.0, "cancel running: %.2f s", now_s() - t0);
	CHECK(strcmp(studio_sg_get(running, "/error/code"), "cancelled") == 0, "running job cancelled");
	CHECK(wait_done(queued, 5) == STUDIO_SG_DONE && strcmp(studio_sg_get(queued, "/error/code"), "cancelled") == 0,
	      "queued job cancelled: %s", studio_sg_result(queued));
	studio_sg_release(running);
	studio_sg_release(queued);
	int orphan = studio_sg_start("sleep", "{\"s\":10}", 0);
	sleep_ms(300);
	studio_sg_release(orphan); // en cours : annulé et libéré par le thread (ASan : pas de fuite, pas d'accès après free)
	int after = studio_sg_run("echo", "{\"x\":1}", 0);
	CHECK(studio_sg_state(after) == STUDIO_SG_DONE && studio_sg_int(after, "/data/x", 0) == 1, "run after release");
	studio_sg_release(after);

	int bad = studio_sg_start("echo", "nope", 0);
	CHECK(wait_done(bad, 5) == STUDIO_SG_DONE && strcmp(studio_sg_get(bad, "/error/code"), "bad_request") == 0,
	      "bad args job");
	studio_sg_release(bad);

	// Annulation d'un appel synchrone par un autre thread.
	volatile int cancel = 0;
	pthread_t    thread;
	pthread_create(&thread, NULL, cancel_later, (void *)&cancel);
	t0      = now_s();
	char *r = studio_sg_call("sleep", "{\"s\":10}", 0, &cancel);
	pthread_join(thread, NULL);
	char *code = text_at(r, "/error/code");
	CHECK(strcmp(code, "cancelled") == 0 && now_s() - t0 < 2.0, "sync cancel: %s in %.2f s", code, now_s() - t0);
	free(code);
	free(r);

	// Table pleine : refus propre.
	int many[256], n = 0;
	for (int i = 0; i < 256; i++) {
		many[i] = studio_sg_run("echo", NULL, 0);
		n += many[i] > 0;
	}
	CHECK(n == 256 && studio_sg_run("echo", NULL, 0) == 0 && log_has("job refused"), "job table full: %d", n);
	for (int i = 0; i < 256; i++) {
		studio_sg_release(many[i]);
	}
	printf("  ok   jobs: FIFO, get/int/length, cancel running/queued/sync, release running/queued, table full\n");
}

static void test_no_leak(void) {
	use_fake("ok", 0, 0);
	free(expect("echo", NULL, 0, NULL, "warm up"));
	studio_sg_stop();
	int before = open_fds();
	for (int i = 0; i < 10; i++) {
		free(expect("echo", NULL, 0, NULL, "cycle"));
		free(expect("crash", NULL, 0, "sidecar_died", "cycle crash"));
		free(expect("sleep", "{\"s\":5}", 50, "timeout", "cycle timeout"));
	}
	studio_sg_stop();
	CHECK(open_fds() == before, "fd leak: %d before, %d after", before, open_fds());
	char cmd[128];
	snprintf(cmd, sizeof(cmd), "ps -A -o ppid=,stat= | awk '$1 == %d && $2 ~ /Z/' | wc -l", (int)getpid());
	FILE *ps      = popen(cmd, "r");
	int   zombies = -1;
	if (ps != NULL && fscanf(ps, "%d", &zombies) != 1) {
		zombies = -1;
	}
	if (ps != NULL) {
		pclose(ps);
	}
	CHECK(zombies == 0, "zombies: %d", zombies);
	printf("  ok   30 sidecars started and stopped: no fd leak, no zombie\n");
}

// ---- Bout en bout : vrai sidecar, site simulé -------------------------------------------------------------------------

static void test_end_to_end(void) {
	if (getenv("STUDIO_TEST_TK_CORE") == NULL) {
		printf("  skip end to end (STUDIO_TEST_TK_CORE not set)\n");
		return;
	}
	char script[1024], dir[1024];
	snprintf(script, sizeof(script), "%s/sg_fixture.py", tests_dir);
	snprintf(dir, sizeof(dir), "%s/e2e", workdir);
	const char        *argv[] = {"python3", script, dir, NULL};
	const char        *env[]  = {"PYTHONDONTWRITEBYTECODE=1", NULL};
	studio_sg_config_t config = {.argv = argv, .env = env, .log = capture_log};
	studio_sg_configure(&config);
	log_clear();

	char *r = expect("auth", NULL, 0, NULL, "e2e auth");
	char *u = text_at(r, "/data/user/name");
	CHECK(strcmp(u, "Ada Artist") == 0, "e2e user: %s", u);
	free(u);
	free(r);

	int job = studio_sg_start("list_tasks", NULL, 0);
	CHECK(wait_done(job, 30) == STUDIO_SG_DONE && studio_sg_ok(job) && studio_sg_length(job, "/data") == 2, "e2e tasks: %s",
	      studio_sg_result(job));
	CHECK(strcmp(studio_sg_get(job, "/data/0/content"), "texturing") == 0, "e2e task content");
	studio_sg_release(job);

	double t0 = now_s();
	r         = expect("next_version", "{\"task_id\":1,\"name\":\"robot\"}", 0, NULL, "e2e next_version (bootstrap)");
	char *path = text_at(r, "/data/path");
	CHECK(strstr(path, "/storage/demo/assets/Prop/robot/TXT/work/armorpaint/robot.v001.arm") != NULL, "e2e path: %s", path);
	free(path);
	free(r);
	CHECK(log_has("bootstrap") && log_has("Toolkit ready for project 1"), "e2e bootstrap progress relayed to the log");
	printf("  ok   end to end: auth, list_tasks (job), next_version with Toolkit bootstrap (%.1f s)\n", now_s() - t0);

	// Tâche d'un autre projet : le sidecar se relance, la requête est rejouée, l'appelant ne voit rien.
	t0 = now_s();
	r  = expect("list_workfiles", "{\"task_id\":3}", 0, NULL, "e2e project switch");
	char *tpl = text_at(r, "/data/template");
	CHECK(strcmp(tpl, "armorpaint_asset_work") == 0, "e2e project 2 workfiles: %s", tpl);
	free(tpl);
	free(r);
	CHECK(log_has("sidecar restarting") && log_has("Toolkit ready for project 2"), "e2e restart logged");
	printf("  ok   end to end: project switch restarts the sidecar and replays the request (%.1f s)\n", now_s() - t0);

	studio_sg_shutdown();
	r = expect("auth", NULL, 0, "sidecar_failed", "after shutdown");
	free(r);
	CHECK(studio_sg_hello() == NULL, "shutdown stops the sidecar");
	printf("  ok   shutdown: sidecar stopped, later calls refused\n");
}

// Thread d'interface simulé : répond aux requêtes host (en majuscules pour "upper", en erreur pour "fail").
static volatile int host_stop;
static volatile int host_answered;

static void *host_ui(void *arg) {
	(void)arg;
	while (!host_stop) {
		char *request = studio_sg_host_take();
		if (request == NULL) {
			sleep_ms(2);
			continue;
		}
		long long n     = studio_json_int(studio_json_find(request, "/host"), -1);
		char     *op    = text_at(request, "/op");
		char     *text  = text_at(request, "/args/text");
		char     *quote = studio_json_quote(text);
		char      reply[512];
		if (strcmp(op, "fail") == 0) {
			snprintf(reply, sizeof(reply), "{\"host\":%lld,\"ok\":false,\"error\":{\"code\":\"no_scene\",\"message\":\"m\"}}", n);
		}
		else {
			snprintf(reply, sizeof(reply), "{\"host\":%lld,\n\"ok\":true,\"data\":{\"op\":\"%s\",\"text\":%s}}", n, op, quote);
		}
		studio_sg_host_reply(reply);
		host_answered++;
		free(op), free(text), free(quote), free(request);
	}
	return NULL;
}

static void test_host(void) {
	use_fake("ok", 0, 0);
	studio_sg_host_reply("{\"id\":1}"); // sans "host" : refusée
	CHECK(log_has("host reply without"), "host reply without host refused");
	studio_sg_host_reply("{\"host\":42,\"ok\":true}"); // d'un autre appel : jetée au début du suivant
	pthread_t ui;
	host_stop = 0;
	pthread_create(&ui, NULL, host_ui, NULL);
	double t0 = now_s();
	char  *r  = expect("host", "{\"ops\":[\"open\",\"stray\",\"fail\",\"save\"]}", 5000, NULL, "host");
	CHECK(now_s() - t0 < 1.0, "host round trips are quick: %.2f s", now_s() - t0);
	CHECK(studio_json_length(studio_json_find(r, "/data")) == 3, "three host replies: %.300s", r);
	char *a = text_at(r, "/data/0/data/op"), *b = text_at(r, "/data/0/data/text"), *c = text_at(r, "/data/1/error/code");
	char *d = text_at(r, "/data/2/data/op");
	CHECK(strcmp(a, "open") == 0 && strcmp(b, "é\n\"x\"") == 0, "host reply data: %s %s", a, b);
	CHECK(studio_json_int(studio_json_find(r, "/data/0/host"), -1) == 1, "host id kept");
	CHECK(strcmp(c, "no_scene") == 0 && strcmp(d, "save") == 0, "host error and order: %s %s", c, d);
	free(a), free(b), free(c), free(d), free(r);
	host_stop = 1;
	pthread_join(ui, NULL);
	// Sans thread d'interface : une requête restée sans preneur est jetée à la fin de l'appel.
	free(expect("host", "{\"ops\":[\"stray\"]}", 5000, NULL, "stray"));
	CHECK(studio_sg_host_take() == NULL, "stray host request dropped at the end of the call");
	// Pendant un job (le cas de l'app : show_app) ; le thread d'interface répond avec du retard. L'app est réveillée
	// à la requête, puis régulièrement tant qu'elle n'a pas répondu, et à la fin du job.
	wakes     = 0;
	int job   = studio_sg_start("host", "{\"ops\":[\"open\"]}", -1);
	char *req = NULL;
	for (int i = 0; i < 2000 && req == NULL; i++) {
		req = studio_sg_host_take();
		sleep_ms(1);
	}
	CHECK(req != NULL && studio_sg_state(job) == STUDIO_SG_RUNNING, "job waits for the host reply");
	sleep_ms(300);
	CHECK(studio_sg_state(job) == STUDIO_SG_RUNNING, "still waiting");
	CHECK(wakes >= 10, "app woken while the host request waits: %d", wakes);
	studio_sg_host_reply("{\"host\":1,\"ok\":true,\"data\":7}");
	CHECK(wait_done(job, 5) == STUDIO_SG_DONE && studio_sg_int(job, "/data/0/data", -1) == 7, "job done after reply: %s",
	      studio_sg_result(job));
	int after = wakes;
	sleep_ms(100);
	CHECK(wakes == after, "no wake once replied and done: %d then %d", after, wakes);
	studio_sg_release(job);
	free(req);
	// Annulé pendant qu'il attend l'app : sidecar tué, rien ne reste.
	job = studio_sg_start("host", "{\"ops\":[\"open\"]}", -1);
	for (int i = 0; i < 2000 && (req = studio_sg_host_take()) == NULL; i++) {
		sleep_ms(1);
	}
	studio_sg_cancel(job);
	CHECK(wait_done(job, 3) == STUDIO_SG_DONE && strcmp(studio_sg_get(job, "/error/code"), "cancelled") == 0,
	      "cancel while waiting for the app");
	studio_sg_release(job);
	free(req);
	studio_sg_stop();
	printf("  ok   host: requests to the app, replies in order, errors, stray and stale lines, job, cancel\n");
}

int main(int argc, char **argv) {
	if (argc != 3) {
		fprintf(stderr, "usage: sg_super_test <tests dir> <workdir>\n");
		return 2;
	}
	signal(SIGPIPE, SIG_DFL);
	tests_dir = argv[1];
	workdir   = argv[2];
	test_calls();
	test_failures();
	test_startup();
	test_jobs();
	test_host();
	test_no_leak();
	test_end_to_end();
	printf("%d checks, %d failures\n", checks, failures);
	return failures > 0;
}
