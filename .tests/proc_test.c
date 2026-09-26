// Banc de test du lot C1 (studio_proc.c), hors de l'app, sous ASan/UBSan : lancé par proc_run.sh.
// Les processus de test sont des outils POSIX de base (sh, cat, printf, sleep, head, yes).
#include "../studio_proc.h"
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
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

static double now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int open_fds(void) {
	int  n = 0;
	DIR *d = opendir("/dev/fd");
	if (d == NULL) {
		return -1;
	}
	while (readdir(d) != NULL) {
		n++;
	}
	closedir(d);
	return n;
}

static bool run(const char *const *argv, const studio_proc_opts_t *opts, const char *in, int timeout_ms, studio_proc_result_t *r) {
	return studio_proc_run(argv, opts, in, in != NULL ? strlen(in) : 0, timeout_ms, NULL, r);
}

static void test_basics(void) {
	studio_proc_result_t r;

	bool done = run((const char *[]){"echo", "hello", NULL}, NULL, NULL, 5000, &r);
	CHECK(done && r.exit_code == 0 && strcmp(r.out, "hello\n") == 0 && r.err_size == 0, "echo: done %d code %d out \"%s\"", done,
	      r.exit_code, r.out);
	studio_proc_result_free(&r);

	done = run((const char *[]){"sh", "-c", "exit 3", NULL}, NULL, NULL, 5000, &r);
	CHECK(done && r.exit_code == 3 && r.signal == 0, "exit code: %d", r.exit_code);
	studio_proc_result_free(&r);

	done = run((const char *[]){"sh", "-c", "echo out; echo err 1>&2", NULL}, NULL, NULL, 5000, &r);
	CHECK(done && strcmp(r.out, "out\n") == 0 && strcmp(r.err, "err\n") == 0, "stderr apart: \"%s\" / \"%s\"", r.out, r.err);
	studio_proc_result_free(&r);

	studio_proc_opts_t merge = {.merge_stderr = true};
	done = run((const char *[]){"sh", "-c", "echo a; echo b 1>&2; echo c", NULL}, &merge, NULL, 5000, &r);
	CHECK(done && strcmp(r.out, "a\nb\nc\n") == 0 && r.err_size == 0, "merge_stderr: \"%s\"", r.out);
	studio_proc_result_free(&r);

	done = run((const char *[]){"sh", "-c", "kill -9 $$", NULL}, NULL, NULL, 5000, &r);
	CHECK(done && r.exit_code == -1 && r.signal == SIGKILL, "signal: code %d signal %d", r.exit_code, r.signal);
	studio_proc_result_free(&r);

	done = run((const char *[]){"studio-no-such-program-xyz", NULL}, NULL, NULL, 5000, &r);
	CHECK(!done && !r.started && strstr(r.error, "studio-no-such-program-xyz") != NULL && r.out != NULL && r.err != NULL,
	      "not found: started %d error \"%s\"", r.started, r.error);
	studio_proc_result_free(&r);

	done = run((const char *[]){NULL}, NULL, NULL, 5000, &r);
	CHECK(!done && !r.started && r.error[0] != 0, "empty argv");
	studio_proc_result_free(&r);

	printf("  ok   basics: output, exit code, stderr, merge, signal, not found\n");
}

// Arguments transmis tels quels : pas de shell, donc ni découpage, ni globbing, ni substitution.
static void test_arguments(void) {
	const char *args[] = {"printf", "[%s]", "", "a b", "\"quoted\"", "'single'", "$HOME", "*", "`id`", "; rm -rf /tmp/x",
	                      "é漢字", "{\"op\":\"auth\",\"user\":\"a b\"}", "back\\slash", NULL};
	studio_proc_result_t r;
	bool                 done = run(args, NULL, NULL, 5000, &r);
	char                 expected[512] = "";
	for (int i = 2; args[i] != NULL; i++) {
		snprintf(expected + strlen(expected), sizeof(expected) - strlen(expected), "[%s]", args[i]);
	}
	CHECK(done && strcmp(r.out, expected) == 0, "arguments:\n    got      %s\n    expected %s", r.out, expected);
	studio_proc_result_free(&r);
	printf("  ok   arguments passed verbatim (spaces, quotes, $, *, backticks, UTF-8, JSON)\n");
}

static void test_env_cwd(void) {
	studio_proc_result_t r;
	const char          *env[]  = {"STUDIO_TEST_VAR=42 x", "PATH=/usr/bin:/bin", NULL};
	studio_proc_opts_t   opts   = {.env = env};
	bool                 done   = run((const char *[]){"sh", "-c", "printf '%s|%s' \"$STUDIO_TEST_VAR\" \"$PATH\"", NULL}, &opts, NULL, 5000, &r);
	CHECK(done && strcmp(r.out, "42 x|/usr/bin:/bin") == 0, "env: \"%s\"", r.out);
	studio_proc_result_free(&r);

	// Le reste de l'environnement est hérité.
	setenv("STUDIO_TEST_INHERITED", "yes", 1);
	done = run((const char *[]){"sh", "-c", "printf %s \"$STUDIO_TEST_INHERITED\"", NULL}, &opts, NULL, 5000, &r);
	CHECK(done && strcmp(r.out, "yes") == 0, "env inherited: \"%s\"", r.out);
	studio_proc_result_free(&r);

	char tmp[1024];
	if (realpath("/tmp", tmp) == NULL) {
		strcpy(tmp, "/tmp");
	}
	studio_proc_opts_t cwd = {.cwd = "/tmp"};
	done = run((const char *[]){"pwd", "-P", NULL}, &cwd, NULL, 5000, &r);
	char expected[1100];
	snprintf(expected, sizeof(expected), "%s\n", tmp);
	CHECK(done && strcmp(r.out, expected) == 0, "cwd: \"%s\"", r.out);
	studio_proc_result_free(&r);
	printf("  ok   environment (override, inherited) and working directory\n");
}

// Entrée et sortie volumineuses en même temps : cat ne lit plus si personne ne vide sa sortie.
static void test_large_io(void) {
	size_t n  = 8 << 20;
	char  *in = malloc(n);
	for (size_t i = 0; i < n; i++) {
		in[i] = (char)('a' + (i * 7919) % 26);
	}
	studio_proc_result_t r;
	double               t0   = now_s();
	bool                 done = studio_proc_run((const char *[]){"cat", NULL}, NULL, in, n, 20000, NULL, &r);
	CHECK(done && r.out_size == n && memcmp(r.out, in, n) == 0 && !r.out_truncated, "cat 8 Mio: done %d size %zu", done, r.out_size);
	printf("  ok   stdin -> cat -> stdout, 8 MiB round trip in %.2f s\n", now_s() - t0);
	studio_proc_result_free(&r);

	// Le processus n'attend pas son entrée et quitte : l'écriture s'arrête, sans SIGPIPE (qui tuerait ce banc).
	// poll() voit la fermeture avant l'écriture ; la protection SIGPIPE d'studio_proc.c couvre la fenêtre de course
	// entre poll() et write(), que ce banc ne sait pas provoquer (400 essais sans succès, 2026-09-26).
	done = studio_proc_run((const char *[]){"true", NULL}, NULL, in, n, 5000, NULL, &r);
	CHECK(done && r.exit_code == 0, "stdin ignored: done %d code %d", done, r.exit_code);
	studio_proc_result_free(&r);
	printf("  ok   process exits without reading stdin: write stops, no crash\n");
	free(in);

	// Sortie plafonnée : le reste est lu et jeté, le processus n'est pas bloqué.
	studio_proc_opts_t capped = {.max_output = 1 << 20};
	t0                        = now_s();
	done = run((const char *[]){"sh", "-c", "yes 0123456789 | head -c 20000000", NULL}, &capped, NULL, 20000, &r);
	CHECK(done && r.exit_code == 0 && r.out_size == (1u << 20) && r.out_truncated, "cap: done %d size %zu truncated %d", done,
	      r.out_size, r.out_truncated);
	studio_proc_result_free(&r);
	printf("  ok   output capped at 1 MiB out of 20 MB, process not blocked (%.2f s)\n", now_s() - t0);
}

static bool pid_alive(pid_t pid) {
	return kill(pid, 0) == 0 || errno == EPERM;
}

// Un timeout tue le processus et ses descendants.
static void test_timeout(void) {
	studio_proc_opts_t opts = {0};
	char               err[256];
	studio_proc_t     *p = studio_proc_spawn((const char *[]){"sh", "-c", "sleep 30 & echo $!; wait", NULL}, &opts, err, sizeof(err));
	CHECK(p != NULL, "spawn: %s", err);
	if (p == NULL) {
		return;
	}
	char *line  = studio_proc_read_line(p, 5000);
	pid_t child = line != NULL ? (pid_t)atoi(line) : 0;
	free(line);
	CHECK(child > 0 && pid_alive(child), "grandchild pid");
	double t0 = now_s();
	CHECK(!studio_proc_wait(p, 200), "wait should time out");
	studio_proc_kill(p, 500);
	CHECK(studio_proc_exited(p) && studio_proc_signal(p) == SIGTERM, "killed by SIGTERM: signal %d", studio_proc_signal(p));
	double killed = now_s() - t0;
	studio_proc_free(p);
	// Le petit-enfant, orphelin, est récolté par init/launchd peu après.
	bool gone = false;
	for (int i = 0; i < 100 && !gone; i++) {
		gone = !pid_alive(child);
		usleep(10000);
	}
	CHECK(gone, "grandchild %d still alive after kill", (int)child);
	CHECK(killed < 1.5, "kill took %.2f s", killed);

	// studio_proc_run : timeout
	studio_proc_result_t r;
	t0        = now_s();
	bool done = run((const char *[]){"sleep", "30", NULL}, NULL, NULL, 300, &r);
	double dt = now_s() - t0;
	CHECK(!done && r.timed_out && r.exit_code == -1 && dt < 1.5 && strstr(r.error, "timed out") != NULL,
	      "timeout: done %d timed_out %d %.2f s", done, r.timed_out, dt);
	studio_proc_result_free(&r);

	// SIGTERM ignoré : SIGKILL après la grâce.
	t0   = now_s();
	done = run((const char *[]){"sh", "-c", "trap '' TERM; while :; do sleep 0.05; done", NULL}, NULL, NULL, 300, &r);
	dt   = now_s() - t0;
	CHECK(!done && r.timed_out && r.signal == SIGKILL && dt < 2.5, "SIGTERM ignored: signal %d %.2f s", r.signal, dt);
	studio_proc_result_free(&r);

	// Annulation
	volatile int cancel = 1;
	t0                  = now_s();
	done   = studio_proc_run((const char *[]){"sleep", "30", NULL}, NULL, NULL, 0, -1, &cancel, &r);
	dt     = now_s() - t0;
	CHECK(!done && r.cancelled && !r.timed_out && dt < 1.5, "cancel: done %d cancelled %d %.2f s", done, r.cancelled, dt);
	studio_proc_result_free(&r);
	printf("  ok   timeout kills the process group (grandchild included), SIGKILL fallback, cancel\n");
}

// Protocole ligne à ligne, comme le sidecar du lot C2 : un processus qui reste ouvert, requête puis réponse.
static void test_line_protocol(void) {
	char           err[256];
	studio_proc_t *p = studio_proc_spawn(
	    (const char *[]){"sh", "-c", "while IFS= read -r l; do printf 'got:%s\\n' \"$l\"; done; echo bye", NULL}, NULL, err,
	    sizeof(err));
	CHECK(p != NULL, "spawn: %s", err);
	if (p == NULL) {
		return;
	}
	int    n  = 500, ok = 0;
	double t0 = now_s();
	for (int i = 0; i < n; i++) {
		char req[64], exp[64];
		snprintf(req, sizeof(req), "{\"op\":\"ping\",\"i\":%d}\n", i);
		snprintf(exp, sizeof(exp), "got:{\"op\":\"ping\",\"i\":%d}", i);
		if (studio_proc_write(p, req, strlen(req), 2000)) {
			char *line = studio_proc_read_line(p, 2000);
			ok += line != NULL && strcmp(line, exp) == 0;
			free(line);
		}
	}
	double dt = now_s() - t0;
	CHECK(ok == n, "line protocol: %d/%d replies", ok, n);
	CHECK(studio_proc_read_line(p, 50) == NULL, "no reply pending");
	studio_proc_close_stdin(p);
	char *bye = studio_proc_read_line(p, 2000);
	CHECK(bye != NULL && strcmp(bye, "bye") == 0, "EOF handled: \"%s\"", bye ? bye : "(null)");
	free(bye);
	CHECK(studio_proc_wait(p, 2000) && studio_proc_exit_code(p) == 0, "exit after EOF");
	CHECK(studio_proc_read_line(p, 0) == NULL, "read after exit");
	CHECK(!studio_proc_write(p, "x\n", 2, 100), "write after stdin closed");
	studio_proc_free(p);
	printf("  ok   line protocol: %d request/reply round trips, %.3f ms each\n", n, dt * 1000 / n);

	// \r\n et dernière ligne sans \n
	p = studio_proc_spawn((const char *[]){"printf", "a\\r\\nb", NULL}, NULL, err, sizeof(err));
	char *a = studio_proc_read_line(p, 2000);
	char *b = studio_proc_read_line(p, 2000);
	CHECK(a != NULL && strcmp(a, "a") == 0 && b == NULL, "CRLF / partial line");
	size_t size;
	bool   truncated;
	CHECK(studio_proc_wait(p, 2000) && strcmp(studio_proc_stdout(p, &size, &truncated), "b") == 0, "partial line kept");
	free(a);
	free(b);
	studio_proc_free(p);

	// Ligne plus longue que max_output : NULL, pas de boucle infinie.
	studio_proc_opts_t small = {.max_output = 1024};
	p = studio_proc_spawn((const char *[]){"sh", "-c", "head -c 5000 /dev/zero | tr '\\0' x; echo; sleep 5", NULL}, &small, err,
	                      sizeof(err));
	double t1   = now_s();
	char  *long_line = studio_proc_read_line(p, 3000);
	CHECK(long_line == NULL && now_s() - t1 < 1.0, "line longer than max_output");
	free(long_line);
	studio_proc_free(p); // tue sleep 5
	printf("  ok   CRLF, partial last line, over-long line\n");
}

// Processus qui dure (sidecar) : stderr relayé au fil de l'eau, attente annulable.
static void test_long_lived(void) {
	char           err[256];
	studio_proc_t *p = studio_proc_spawn(
	    (const char *[]){"sh", "-c", "while IFS= read -r l; do echo \"log:$l\" >&2; echo \"ok:$l\"; done", NULL}, NULL, err,
	    sizeof(err));
	CHECK(p != NULL, "spawn: %s", err);
	if (p == NULL) {
		return;
	}
	studio_proc_write(p, "a\n", 2, 2000);
	char *line = studio_proc_read_line(p, 2000);
	free(line);
	studio_proc_wait(p, 100); // laisse arriver le stderr de "a"
	size_t size;
	bool   truncated;
	char  *e1 = studio_proc_take_stderr(p, &size, &truncated);
	CHECK(e1 != NULL && strcmp(e1, "log:a\n") == 0 && size == 6 && !truncated, "take_stderr: \"%s\"", e1 ? e1 : "(null)");
	studio_proc_write(p, "b\n", 2, 2000);
	line = studio_proc_read_line(p, 2000);
	free(line);
	studio_proc_wait(p, 100);
	char *e2 = studio_proc_take_stderr(p, NULL, NULL);
	CHECK(e2 != NULL && strcmp(e2, "log:b\n") == 0, "take_stderr consumes: \"%s\"", e2 ? e2 : "(null)");
	char *e3 = studio_proc_take_stderr(p, &size, NULL);
	CHECK(e3 != NULL && size == 0 && e3[0] == 0, "take_stderr empty");
	free(e1);
	free(e2);
	free(e3);

	// Une attente sans réponse rend la main dès que l'annulation est demandée, sans tuer le processus.
	volatile int cancel = 1;
	studio_proc_set_cancel(p, &cancel);
	double t0 = now_s();
	line      = studio_proc_read_line(p, 5000);
	CHECK(line == NULL && now_s() - t0 < 0.5, "read_line cancelled in %.2f s", now_s() - t0);
	CHECK(!studio_proc_wait(p, 5000) && now_s() - t0 < 1.0, "wait cancelled");
	studio_proc_set_cancel(p, NULL);
	studio_proc_write(p, "c\n", 2, 2000);
	line = studio_proc_read_line(p, 2000);
	CHECK(line != NULL && strcmp(line, "ok:c") == 0, "still alive after cancel: \"%s\"", line ? line : "(null)");
	free(line);

	// Plafond : stderr tronqué au-delà de max_output, puis le drapeau repart à zéro.
	studio_proc_free(p);
	studio_proc_opts_t small = {.max_output = 100};
	p = studio_proc_spawn((const char *[]){"sh", "-c", "head -c 5000 /dev/zero | tr '\\0' x >&2; read l; echo y >&2; read l", NULL},
	                      &small, err, sizeof(err));
	studio_proc_wait(p, 300);
	char *big = studio_proc_take_stderr(p, &size, &truncated);
	CHECK(big != NULL && size == 100 && truncated, "take_stderr capped: %zu bytes, truncated %d", size, truncated);
	studio_proc_write(p, "\n", 1, 2000);
	studio_proc_wait(p, 300);
	char *small_err = studio_proc_take_stderr(p, &size, &truncated);
	CHECK(small_err != NULL && strcmp(small_err, "y\n") == 0 && !truncated, "truncated flag reset");
	free(big);
	free(small_err);
	studio_proc_free(p);
	printf("  ok   long-lived process: stderr taken incrementally (capped), cancellable waits\n");
}

// Pas de fuite de fd ni de zombie après de nombreux lancements.
static void test_leaks(void) {
	int before = open_fds();
	for (int i = 0; i < 200; i++) {
		studio_proc_result_t r;
		run((const char *[]){"true", NULL}, NULL, i % 2 ? "x" : NULL, 5000, &r);
		studio_proc_result_free(&r);
		if (i % 50 == 0) {
			char           err[64];
			studio_proc_t *p = studio_proc_spawn((const char *[]){"sleep", "10", NULL}, NULL, err, sizeof(err));
			studio_proc_free(p); // tué sans attendre
		}
	}
	int after = open_fds();
	CHECK(before == after, "fd leak: %d before, %d after", before, after);
	char cmd[128];
	snprintf(cmd, sizeof(cmd), "ps -A -o ppid=,stat= | awk '$1 == %d && $2 ~ /Z/' | wc -l", (int)getpid()); // nos zombies
	FILE *ps = popen(cmd, "r");
	int   zombies = 0;
	if (ps != NULL) {
		if (fscanf(ps, "%d", &zombies) != 1) {
			zombies = 0;
		}
		pclose(ps);
	}
	CHECK(zombies == 0, "%d zombie processes", zombies);
	printf("  ok   200 runs + 4 kills: no fd leak (%d fds), no zombie\n", after);
}

int main(void) {
	signal(SIGPIPE, SIG_DFL); // comme une app qui n'ignore pas SIGPIPE : une écriture ratée doit ne pas nous tuer
	test_basics();
	test_arguments();
	test_env_cwd();
	test_large_io();
	test_timeout();
	test_line_protocol();
	test_long_lived();
	test_leaks();
	printf("%d checks, %d failures\n", checks, failures);
	return failures > 0;
}
