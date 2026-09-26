#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE // clock_gettime, sigtimedwait, pthread_sigmask, posix_spawn_file_actions_addchdir_np en -std=c11
#endif
#include "studio_proc.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STUDIO_PROC_DEFAULT_MAX_OUTPUT ((size_t)64 << 20)
// Attente maximale entre deux contrôles de l'état du processus et du drapeau d'annulation.
#define STUDIO_PROC_TICK_MS 20

static void set_error(char *buf, size_t size, const char *format, ...) {
	if (buf == NULL || size == 0) {
		return;
	}
	va_list args;
	va_start(args, format);
	vsnprintf(buf, size, format, args);
	va_end(args);
}

// Tampon de sortie : [pos, len) pas encore consommé, au plus cap octets non consommés.
typedef struct proc_buf {
	char  *data;
	size_t len;
	size_t pos;
	size_t alloc;
	size_t cap;
	bool   truncated;
} proc_buf_t;

static void buf_append(proc_buf_t *b, const char *src, size_t n) {
	size_t pending = b->len - b->pos;
	if (n > b->cap - pending) {
		n            = b->cap - pending;
		b->truncated = true;
	}
	if (n == 0) {
		return;
	}
	if (b->pos > 0) { // ce que studio_proc_read_line a consommé
		memmove(b->data, b->data + b->pos, pending);
		b->len = pending;
		b->pos = 0;
	}
	if (b->len + n + 1 > b->alloc) {
		size_t alloc = b->alloc > 0 ? b->alloc : 4096;
		while (alloc < b->len + n + 1) {
			alloc *= 2;
		}
		char *grown = realloc(b->data, alloc);
		if (grown == NULL) {
			b->truncated = true;
			return;
		}
		b->data  = grown;
		b->alloc = alloc;
	}
	memcpy(b->data + b->len, src, n);
	b->len += n;
	b->data[b->len] = 0;
}

static const char *buf_view(const proc_buf_t *b, size_t *size, bool *truncated) {
	if (size != NULL) {
		*size = b->len - b->pos;
	}
	if (truncated != NULL) {
		*truncated = b->truncated;
	}
	return b->data != NULL ? b->data + b->pos : "";
}

static char *buf_take(proc_buf_t *b, size_t *size) {
	size_t n = b->len - b->pos;
	char  *out = malloc(n + 1);
	if (out != NULL) {
		if (n > 0) {
			memcpy(out, b->data + b->pos, n);
		}
		out[n] = 0;
	}
	*size = out != NULL ? n : 0;
	return out;
}

#ifndef _WIN32

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <crt_externs.h>
#define STUDIO_PROC_ENVIRON (*_NSGetEnviron())
#define STUDIO_PROC_HAS_ADDCHDIR 1
#else
extern char **environ;
#define STUDIO_PROC_ENVIRON environ
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29))
#define STUDIO_PROC_HAS_ADDCHDIR 1
#endif
#endif

struct studio_proc {
	pid_t         pid;
	int           in_fd;
	int           out_fd;
	int           err_fd;
	proc_buf_t    out;
	proc_buf_t    err;
	bool          exited;
	int           exit_code;
	int           signal;
	volatile int *cancel;
};

static int64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t deadline_from(int timeout_ms) {
	return timeout_ms < 0 ? -1 : now_ms() + timeout_ms;
}

// Délai restant jusqu'à `deadline`, au format timeout_ms (-1 : sans limite).
static int remaining(int64_t deadline) {
	if (deadline < 0) {
		return -1;
	}
	int64_t left = deadline - now_ms();
	return left > 0 ? (int)left : 0;
}

// Délai de poll jusqu'à `deadline` (-1 : sans limite), plafonné à STUDIO_PROC_TICK_MS. -2 si expiré.
static int poll_timeout(int64_t deadline) {
	if (deadline < 0) {
		return STUDIO_PROC_TICK_MS;
	}
	int64_t left = deadline - now_ms();
	if (left <= 0) {
		return -2;
	}
	return left < STUDIO_PROC_TICK_MS ? (int)left : STUDIO_PROC_TICK_MS;
}

static void close_fd(int *fd) {
	if (*fd >= 0) {
		close(*fd);
		*fd = -1;
	}
}

static void set_cloexec(int fd) {
	fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
}

static void set_nonblock(int fd) {
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

// Lit tout ce qui est disponible sur un flux. À EOF, le fd est fermé.
static void drain(int *fd, proc_buf_t *b) {
	char chunk[65536];
	while (*fd >= 0) {
		ssize_t n = read(*fd, chunk, sizeof(chunk));
		if (n > 0) {
			buf_append(b, chunk, (size_t)n);
			continue;
		}
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			return;
		}
		close_fd(fd); // EOF ou erreur
	}
}

static void reap(studio_proc_t *p, bool block) {
	if (p->exited) {
		return;
	}
	int   status;
	pid_t r;
	do {
		r = waitpid(p->pid, &status, block ? 0 : WNOHANG);
	} while (r < 0 && errno == EINTR);
	if (r == p->pid) {
		p->exited    = true;
		p->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		p->signal    = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
	}
	else if (r < 0 && errno == ECHILD) {
		// Déjà récolté ailleurs (SIGCHLD ignoré par l'app) : terminé, code inconnu.
		p->exited    = true;
		p->exit_code = -1;
		p->signal    = 0;
	}
}

// Signal au groupe du processus (lui et ses descendants), ou à lui seul si le groupe n'existe pas.
static void signal_proc(const studio_proc_t *p, int sig) {
	if (kill(-p->pid, sig) != 0) {
		kill(p->pid, sig);
	}
}

static bool cancelled(const studio_proc_t *p) {
	return p->cancel != NULL && *p->cancel != 0;
}

// Écrit sans jamais lever SIGPIPE : le processus peut fermer son stdin à tout moment, et SIGPIPE tuerait ArmorPaint.
static ssize_t write_nosigpipe(int fd, const void *data, size_t size) {
#ifdef __APPLE__
	ssize_t n;
	do {
		n = write(fd, data, size); // F_SETNOSIGPIPE posé au lancement
	} while (n < 0 && errno == EINTR);
	return n;
#else
	sigset_t pipe_set, old_set;
	sigemptyset(&pipe_set);
	sigaddset(&pipe_set, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &pipe_set, &old_set);
	ssize_t n;
	do {
		n = write(fd, data, size);
	} while (n < 0 && errno == EINTR);
	if (n < 0 && errno == EPIPE) {
		int             saved = errno;
		struct timespec zero  = {0, 0};
		while (sigtimedwait(&pipe_set, NULL, &zero) > 0) { // consomme le SIGPIPE en attente sur ce thread
		}
		errno = saved;
	}
	pthread_sigmask(SIG_SETMASK, &old_set, NULL);
	return n;
#endif
}

// Boucle commune : lit stdout/stderr, écrit `wdata` si fourni, jusqu'à ce que `until` soit vrai, que le délai expire ou
// que l'annulation soit demandée. Retourne true si `until` est devenu vrai.
typedef bool (*until_fn)(studio_proc_t *p, void *ctx);

static bool loop(studio_proc_t *p, int64_t deadline, const char *wdata, size_t wsize, size_t *written, until_fn until,
                 void *ctx) {
	while (true) {
		if (until(p, ctx)) {
			return true;
		}
		if (cancelled(p)) {
			return false;
		}
		int wait = poll_timeout(deadline);
		if (wait == -2) {
			return false;
		}
		struct pollfd fds[3];
		int           n = 0, out_i = -1, err_i = -1, in_i = -1;
		if (p->out_fd >= 0) {
			out_i = n;
			fds[n++] = (struct pollfd){.fd = p->out_fd, .events = POLLIN};
		}
		if (p->err_fd >= 0) {
			err_i = n;
			fds[n++] = (struct pollfd){.fd = p->err_fd, .events = POLLIN};
		}
		if (wdata != NULL && *written < wsize && p->in_fd >= 0) {
			in_i = n;
			fds[n++] = (struct pollfd){.fd = p->in_fd, .events = POLLOUT};
		}
		if (n == 0) {
			// Plus rien à lire ni à écrire : seul l'état du processus peut encore changer.
			struct timespec ts = {0, (long)wait * 1000000};
			nanosleep(&ts, NULL);
			reap(p, false);
			continue;
		}
		int r = poll(fds, (nfds_t)n, wait);
		if (r < 0 && errno != EINTR) {
			return false;
		}
		if (r > 0) {
			if (out_i >= 0 && fds[out_i].revents != 0) {
				drain(&p->out_fd, &p->out);
			}
			if (err_i >= 0 && fds[err_i].revents != 0) {
				drain(&p->err_fd, &p->err);
			}
			if (in_i >= 0 && fds[in_i].revents != 0) {
				if (fds[in_i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
					close_fd(&p->in_fd); // le processus a fermé son stdin
				}
				else {
					ssize_t w = write_nosigpipe(p->in_fd, wdata + *written, wsize - *written);
					if (w > 0) {
						*written += (size_t)w;
					}
					else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
						close_fd(&p->in_fd);
					}
				}
			}
		}
		reap(p, false);
	}
}

studio_proc_t *studio_proc_spawn(const char *const *argv, const studio_proc_opts_t *opts, char *err, size_t err_size) {
	set_error(err, err_size, "");
	studio_proc_opts_t none = {0};
	if (opts == NULL) {
		opts = &none;
	}
	if (argv == NULL || argv[0] == NULL || argv[0][0] == 0) {
		set_error(err, err_size, "empty command");
		return NULL;
	}
#ifndef STUDIO_PROC_HAS_ADDCHDIR
	if (opts->cwd != NULL) {
		set_error(err, err_size, "cwd not supported on this platform (glibc < 2.29)");
		return NULL;
	}
#endif

	studio_proc_t *p = calloc(1, sizeof(studio_proc_t));
	if (p == NULL) {
		set_error(err, err_size, "out of memory");
		return NULL;
	}
	p->in_fd = p->out_fd = p->err_fd = -1;
	size_t cap                       = opts->max_output > 0 ? opts->max_output : STUDIO_PROC_DEFAULT_MAX_OUTPUT;
	p->out.cap = p->err.cap = cap;

	int    in_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1}, err_pipe[2] = {-1, -1};
	char **envp       = NULL;
	bool   ok         = pipe(in_pipe) == 0 && pipe(out_pipe) == 0 && (opts->merge_stderr || pipe(err_pipe) == 0);
	if (!ok) {
		set_error(err, err_size, "pipe: %s", strerror(errno));
	}
	int fds[6] = {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]};
	for (int i = 0; i < 6; i++) {
		if (fds[i] >= 0) {
			set_cloexec(fds[i]); // les dup2 du fils les rendent héritables sur 0, 1, 2 seulement
		}
	}

	// Environnement : l'hérité, avec les entrées de opts->env ajoutées ou remplacées.
	if (ok && opts->env != NULL) {
		char **parent = STUDIO_PROC_ENVIRON;
		size_t np = 0, ne = 0;
		while (parent != NULL && parent[np] != NULL) {
			np++;
		}
		while (opts->env[ne] != NULL) {
			ne++;
		}
		envp = calloc(np + ne + 1, sizeof(char *));
		if (envp == NULL) {
			ok = false;
			set_error(err, err_size, "out of memory");
		}
		else {
			size_t k = 0;
			for (size_t i = 0; i < np; i++) {
				const char *eq  = strchr(parent[i], '=');
				size_t      len = eq != NULL ? (size_t)(eq - parent[i]) : strlen(parent[i]);
				bool        overridden = false;
				for (size_t j = 0; j < ne && !overridden; j++) {
					overridden = strncmp(opts->env[j], parent[i], len) == 0 && opts->env[j][len] == '=';
				}
				if (!overridden) {
					envp[k++] = parent[i];
				}
			}
			for (size_t j = 0; j < ne; j++) {
				envp[k++] = (char *)opts->env[j];
			}
			envp[k] = NULL;
		}
	}

	if (ok) {
		posix_spawn_file_actions_t fa;
		posix_spawnattr_t          attr;
		posix_spawn_file_actions_init(&fa);
		posix_spawnattr_init(&attr);
		posix_spawn_file_actions_adddup2(&fa, in_pipe[0], 0);
		posix_spawn_file_actions_adddup2(&fa, out_pipe[1], 1);
		posix_spawn_file_actions_adddup2(&fa, opts->merge_stderr ? out_pipe[1] : err_pipe[1], 2);
#ifdef STUDIO_PROC_HAS_ADDCHDIR
		if (opts->cwd != NULL) {
			// Déprécié depuis macOS 26 au profit de posix_spawn_file_actions_addchdir, absent des versions antérieures.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
			posix_spawn_file_actions_addchdir_np(&fa, opts->cwd);
#pragma clang diagnostic pop
		}
#endif
		// Propre groupe de processus (pour tuer les descendants), signaux par défaut et non masqués : l'app peut en
		// ignorer ou en bloquer (SIGPIPE…), le fils ne doit pas en hériter.
		short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
#ifdef __APPLE__
		flags |= POSIX_SPAWN_CLOEXEC_DEFAULT; // aucun autre fd d'ArmorPaint ne fuit dans le fils
#endif
		posix_spawnattr_setflags(&attr, flags);
		posix_spawnattr_setpgroup(&attr, 0);
		sigset_t def, mask;
		sigemptyset(&mask);
		sigemptyset(&def);
		int defaults[] = {SIGPIPE, SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGCHLD, SIGUSR1, SIGUSR2};
		for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
			sigaddset(&def, defaults[i]);
		}
		posix_spawnattr_setsigdefault(&attr, &def);
		posix_spawnattr_setsigmask(&attr, &mask);

		int r = posix_spawnp(&p->pid, argv[0], &fa, &attr, (char *const *)argv, envp != NULL ? envp : STUDIO_PROC_ENVIRON);
		posix_spawn_file_actions_destroy(&fa);
		posix_spawnattr_destroy(&attr);
		if (r != 0) {
			ok = false;
			set_error(err, err_size, "%s: %s", argv[0], strerror(r));
		}
	}
	free(envp);

	// Côté fils fermé chez nous ; côté parent gardé si le lancement a réussi.
	for (int i = 0; i < 6; i++) {
		bool parent_end = fds[i] == in_pipe[1] || fds[i] == out_pipe[0] || fds[i] == err_pipe[0];
		if (fds[i] >= 0 && (!ok || !parent_end)) {
			close(fds[i]);
		}
	}
	if (!ok) {
		free(p);
		return NULL;
	}
	p->in_fd  = in_pipe[1];
	p->out_fd = out_pipe[0];
	p->err_fd = err_pipe[0];
	set_nonblock(p->in_fd);
	set_nonblock(p->out_fd);
	if (p->err_fd >= 0) {
		set_nonblock(p->err_fd);
	}
#ifdef __APPLE__
	fcntl(p->in_fd, F_SETNOSIGPIPE, 1);
#endif
	return p;
}

static bool until_written(studio_proc_t *p, void *ctx) {
	size_t *state = ctx; // [0] écrit, [1] total
	return state[0] >= state[1] || p->in_fd < 0;
}

bool studio_proc_write(studio_proc_t *p, const void *data, size_t size, int timeout_ms) {
	if (p->in_fd < 0) {
		return false;
	}
	size_t state[2] = {0, size};
	loop(p, deadline_from(timeout_ms), data, size, &state[0], until_written, state);
	return state[0] == size;
}

void studio_proc_close_stdin(studio_proc_t *p) {
	close_fd(&p->in_fd);
}

static bool until_line(studio_proc_t *p, void *ctx) {
	(void)ctx;
	const proc_buf_t *b = &p->out;
	return (b->len > b->pos && memchr(b->data + b->pos, '\n', b->len - b->pos) != NULL) || p->out_fd < 0 || b->truncated;
}

char *studio_proc_read_line(studio_proc_t *p, int timeout_ms) {
	size_t none = 0;
	loop(p, deadline_from(timeout_ms), NULL, 0, &none, until_line, NULL);
	proc_buf_t *b = &p->out;
	if (b->len == b->pos) {
		return NULL;
	}
	char *start = b->data + b->pos;
	char *nl    = memchr(start, '\n', b->len - b->pos);
	if (nl == NULL) {
		return NULL; // délai expiré, EOF sur une ligne incomplète, ou ligne plus longue que max_output
	}
	size_t n = (size_t)(nl - start);
	char  *line = malloc(n + 1);
	if (line == NULL) {
		return NULL;
	}
	memcpy(line, start, n);
	line[n] = 0;
	if (n > 0 && line[n - 1] == '\r') {
		line[n - 1] = 0;
	}
	b->pos += n + 1;
	return line;
}

static bool until_exited(studio_proc_t *p, void *ctx) {
	(void)ctx;
	reap(p, false);
	return p->exited;
}

bool studio_proc_wait(studio_proc_t *p, int timeout_ms) {
	size_t none = 0;
	bool   done = loop(p, deadline_from(timeout_ms), NULL, 0, &none, until_exited, NULL);
	if (done) {
		// Tout ce que le processus a écrit est dans les pipes : on le lit sans attendre EOF, qu'un descendant resté
		// vivant (et qui aurait hérité des pipes) peut retarder indéfiniment.
		drain(&p->out_fd, &p->out);
		drain(&p->err_fd, &p->err);
	}
	return done;
}

void studio_proc_kill(studio_proc_t *p, int grace_ms) {
	reap(p, false);
	if (p->exited) {
		return; // récolté : le pid peut avoir été réattribué, ne plus rien lui envoyer
	}
	signal_proc(p, SIGTERM);
	volatile int *cancel = p->cancel;
	p->cancel            = NULL;
	if (!studio_proc_wait(p, grace_ms)) {
		signal_proc(p, SIGKILL);
		reap(p, true);
		drain(&p->out_fd, &p->out);
		drain(&p->err_fd, &p->err);
	}
	p->cancel = cancel;
}

void studio_proc_free(studio_proc_t *p) {
	if (p == NULL) {
		return;
	}
	close_fd(&p->in_fd);
	studio_proc_kill(p, 200);
	close_fd(&p->out_fd);
	close_fd(&p->err_fd);
	free(p->out.data);
	free(p->err.data);
	free(p);
}

bool studio_proc_exited(const studio_proc_t *p) {
	return p->exited;
}

int studio_proc_exit_code(const studio_proc_t *p) {
	return p->exited ? p->exit_code : -1;
}

int studio_proc_signal(const studio_proc_t *p) {
	return p->exited ? p->signal : 0;
}

const char *studio_proc_stdout(const studio_proc_t *p, size_t *size, bool *truncated) {
	return buf_view(&p->out, size, truncated);
}

const char *studio_proc_stderr(const studio_proc_t *p, size_t *size, bool *truncated) {
	return buf_view(&p->err, size, truncated);
}

void studio_proc_set_cancel(studio_proc_t *p, volatile int *cancel) {
	p->cancel = cancel;
}

char *studio_proc_take_stderr(studio_proc_t *p, size_t *size, bool *truncated) {
	drain(&p->err_fd, &p->err);
	size_t n;
	char  *out = buf_take(&p->err, &n);
	if (truncated != NULL) {
		*truncated = p->err.truncated;
	}
	if (out != NULL) {
		p->err.pos       = p->err.len;
		p->err.truncated = false;
	}
	if (size != NULL) {
		*size = n;
	}
	return out;
}

bool studio_proc_run(const char *const *argv, const studio_proc_opts_t *opts, const void *in, size_t in_size, int timeout_ms,
                     volatile int *cancel, studio_proc_result_t *res) {
	memset(res, 0, sizeof(*res));
	res->exit_code   = -1;
	studio_proc_t *p = studio_proc_spawn(argv, opts, res->error, sizeof(res->error));
	if (p == NULL) {
		res->out = calloc(1, 1);
		res->err = calloc(1, 1);
		return false;
	}
	res->started     = true;
	p->cancel        = cancel;
	int64_t deadline = deadline_from(timeout_ms);
	if (in != NULL && in_size > 0) {
		if (!studio_proc_write(p, in, in_size, remaining(deadline)) && p->in_fd < 0 && !p->exited) {
			set_error(res->error, sizeof(res->error), "stdin closed by the process before all input was written");
		}
	}
	studio_proc_close_stdin(p);
	bool done = studio_proc_wait(p, remaining(deadline));
	if (!done) {
		res->cancelled = cancelled(p);
		res->timed_out = !res->cancelled;
		studio_proc_kill(p, 500);
	}
	res->exit_code     = studio_proc_exit_code(p);
	res->signal        = studio_proc_signal(p);
	res->out_truncated = p->out.truncated;
	res->err_truncated = p->err.truncated;
	res->out           = buf_take(&p->out, &res->out_size);
	res->err           = buf_take(&p->err, &res->err_size);
	if (res->timed_out) {
		set_error(res->error, sizeof(res->error), "timed out after %d ms", timeout_ms);
	}
	else if (res->cancelled) {
		set_error(res->error, sizeof(res->error), "cancelled");
	}
	studio_proc_free(p);
	return done;
}

#else // _WIN32

struct studio_proc {
	int unused;
};

studio_proc_t *studio_proc_spawn(const char *const *argv, const studio_proc_opts_t *opts, char *err, size_t err_size) {
	(void)argv;
	(void)opts;
	set_error(err, err_size, "studio_proc: not implemented on Windows yet");
	return NULL;
}

bool studio_proc_write(studio_proc_t *p, const void *data, size_t size, int timeout_ms) {
	(void)p, (void)data, (void)size, (void)timeout_ms;
	return false;
}

void studio_proc_close_stdin(studio_proc_t *p) {
	(void)p;
}

char *studio_proc_read_line(studio_proc_t *p, int timeout_ms) {
	(void)p, (void)timeout_ms;
	return NULL;
}

bool studio_proc_wait(studio_proc_t *p, int timeout_ms) {
	(void)p, (void)timeout_ms;
	return true;
}

void studio_proc_kill(studio_proc_t *p, int grace_ms) {
	(void)p, (void)grace_ms;
}

void studio_proc_free(studio_proc_t *p) {
	(void)p;
}

bool studio_proc_exited(const studio_proc_t *p) {
	(void)p;
	return true;
}

int studio_proc_exit_code(const studio_proc_t *p) {
	(void)p;
	return -1;
}

int studio_proc_signal(const studio_proc_t *p) {
	(void)p;
	return 0;
}

const char *studio_proc_stdout(const studio_proc_t *p, size_t *size, bool *truncated) {
	(void)p;
	*size      = 0;
	*truncated = false;
	return "";
}

const char *studio_proc_stderr(const studio_proc_t *p, size_t *size, bool *truncated) {
	return studio_proc_stdout(p, size, truncated);
}

void studio_proc_set_cancel(studio_proc_t *p, volatile int *cancel) {
	(void)p, (void)cancel;
}

char *studio_proc_take_stderr(studio_proc_t *p, size_t *size, bool *truncated) {
	(void)p;
	if (size != NULL) {
		*size = 0;
	}
	if (truncated != NULL) {
		*truncated = false;
	}
	return calloc(1, 1);
}

bool studio_proc_run(const char *const *argv, const studio_proc_opts_t *opts, const void *in, size_t in_size, int timeout_ms,
                     volatile int *cancel, studio_proc_result_t *res) {
	(void)in, (void)in_size, (void)timeout_ms, (void)cancel;
	memset(res, 0, sizeof(*res));
	res->exit_code = -1;
	studio_proc_spawn(argv, opts, res->error, sizeof(res->error));
	res->out = calloc(1, 1);
	res->err = calloc(1, 1);
	return false;
}

#endif

void studio_proc_result_free(studio_proc_result_t *res) {
	free(res->out);
	free(res->err);
	res->out = res->err = NULL;
}
