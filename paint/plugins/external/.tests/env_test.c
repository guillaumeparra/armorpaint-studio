// Banc de test de studio_env.c (lot E5), hors de l'app, sous ASan/UBSan : lancé par env.sh.
#include "../studio_env.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define ENV_IS(name, expected)                                                                                         \
	do {                                                                                                               \
		const char *got_ = getenv(name);                                                                               \
		CHECK(got_ != NULL && strcmp(got_, expected) == 0, "%s: got \"%s\", expected \"%s\"", name,                    \
		      got_ != NULL ? got_ : "(unset)", expected);                                                              \
	} while (0)

#define UNSET(name) CHECK(getenv(name) == NULL, "%s: got \"%s\", expected unset", name, getenv(name))

// ---- Stubs d'iron ---------------------------------------------------------------------------------------------------

static char  logs[16384];
static char  save_path[1024];
static char *args[4] = {"ArmorPaint"};
static int   arg_count = 1;

void iron_log(const char *format, ...) {
	char    line[1024];
	va_list ap;
	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	strncat(logs, line, sizeof(logs) - strlen(logs) - 2);
	strcat(logs, "\n");
}

const char *iron_internal_save_path(void) {
	return save_path;
}

int iron_get_arg_count(void) {
	return arg_count;
}

char *iron_get_arg(int index) {
	return args[index];
}

static bool logged(const char *text) {
	return strstr(logs, text) != NULL;
}

static void write_file(const char *path, const char *text) {
	FILE *f = fopen(path, "wb");
	fputs(text, f);
	fclose(f);
}

// ---- Tests ----------------------------------------------------------------------------------------------------------

static void test_syntax(void) {
	setenv("ENVT_HOME_LIKE", "/base", 1);
	setenv("ENVT_KEEP", "from the environment", 1);
	logs[0]   = 0;
	int taken = studio_env_load_text("# comment\n"
	                                 "\n"
	                                 "ENVT_PLAIN=value\n"
	                                 "  export   ENVT_EXPORT = spaced  \n"
	                                 "ENVT_SINGLE='$ENVT_HOME_LIKE # not a comment'\n"
	                                 "ENVT_DOUBLE=\"${ENVT_HOME_LIKE}/x \\\"q\\\" \\\\ \\$HOME\" # comment\n"
	                                 "ENVT_BARE=$ENVT_HOME_LIKE/sub#tag # comment\n"
	                                 "ENVT_UNDEF=a${ENVT_NOPE}b$ENVT_NOPE\n"
	                                 "ENVT_DOLLAR=5$ and ${ and $1\n"
	                                 "ENVT_TILDE=~/lib\n"
	                                 "ENVT_TILDE_MID=a~/lib\n"
	                                 "ENVT_EMPTY=\n"
	                                 "ENVT_CRLF=dos\r\n"
	                                 "ENVT_KEEP=from the file\n"
	                                 "ENVT_CHAIN=$ENVT_PLAIN-2\n"
	                                 "ENVT_PLAIN=second\n"
	                                 "not a line\n"
	                                 "ENVT_BAD_QUOTE=\"open\n"
	                                 "ENVT_AFTER='x' y\n"
	                                 "1ENVT=x\n"
	                                 "ENVT_LAST=no newline",
	                                 "test.env");
	CHECK(taken == 13, "taken: got %d, expected 13", taken);
	ENV_IS("ENVT_PLAIN", "value");
	ENV_IS("ENVT_EXPORT", "spaced");
	ENV_IS("ENVT_SINGLE", "$ENVT_HOME_LIKE # not a comment");
	ENV_IS("ENVT_DOUBLE", "/base/x \"q\" \\ $HOME");
	ENV_IS("ENVT_BARE", "/base/sub#tag");
	ENV_IS("ENVT_UNDEF", "ab");
	ENV_IS("ENVT_DOLLAR", "5$ and ${ and $1");
	char tilde[1024];
	snprintf(tilde, sizeof(tilde), "%s/lib", getenv("HOME"));
	ENV_IS("ENVT_TILDE", tilde);
	ENV_IS("ENVT_TILDE_MID", "a~/lib");
	ENV_IS("ENVT_EMPTY", "");
	ENV_IS("ENVT_CRLF", "dos");
	ENV_IS("ENVT_KEEP", "from the environment");
	ENV_IS("ENVT_CHAIN", "value-2");
	ENV_IS("ENVT_LAST", "no newline");
	UNSET("ENVT_BAD_QUOTE");
	UNSET("ENVT_AFTER");

	const char *origin = studio_env_origin("ENVT_PLAIN");
	CHECK(origin != NULL && strcmp(origin, "test.env") == 0, "origin of ENVT_PLAIN: %s", origin != NULL ? origin : "NULL");
	origin = studio_env_origin("ENVT_KEEP");
	CHECK(origin != NULL && strcmp(origin, "environment") == 0, "origin of ENVT_KEEP: %s", origin != NULL ? origin : "NULL");
	CHECK(studio_env_origin("ENVT_NOPE") == NULL, "origin of an unset variable");

	CHECK(logged("studio: env test.env:17: not a NAME=value line, line ignored"), "warning for line 17:\n%s", logs);
	CHECK(logged("studio: env test.env:18: unterminated quote, line ignored"), "warning for line 18:\n%s", logs);
	CHECK(logged("studio: env test.env:19: text after the closing quote, line ignored"), "warning for line 19:\n%s", logs);
	CHECK(logged("studio: env test.env:20: not a NAME=value line, line ignored"), "warning for line 20:\n%s", logs);
	CHECK(logged("studio: env test.env: 13 variables (ENVT_PLAIN, ENVT_EXPORT,"), "summary:\n%s", logs);
	CHECK(logged("; already set, kept: ENVT_KEEP, ENVT_PLAIN\n"), "kept variables:\n%s", logs);
}

// studio_env_init : fichier par défaut, --background, $STUDIO_ENV, résumé trié et secrets masqués.
static void test_init(const char *dir) {
	char path[1024], a[1024], b[1024], list[4096];
	snprintf(save_path, sizeof(save_path), "%s/", dir);
	snprintf(path, sizeof(path), "%sstudio.env", save_path);

	// Pas de fichier.
	remove(path);
	unsetenv("STUDIO_ENV");
	logs[0] = 0;
	studio_env_init();
	CHECK(logged("studio: env: no studio.env ("), "no file:\n%s", logs);

	// Fichier par défaut, ignoré en --background.
	write_file(path, "SUBZERO_INIT_DEFAULT=1\n");
	args[1]   = "--background";
	arg_count = 2;
	logs[0]   = 0;
	studio_env_init();
	UNSET("SUBZERO_INIT_DEFAULT");
	CHECK(logged("ignored in --background"), "--background:\n%s", logs);

	// Fichier par défaut, avec interface.
	arg_count = 1;
	logs[0]   = 0;
	studio_env_init();
	ENV_IS("SUBZERO_INIT_DEFAULT", "1");
	CHECK(logged("studio: env SUBZERO_INIT_DEFAULT=1 ("), "summary line:\n%s", logs);

	// $STUDIO_ENV : plusieurs fichiers, le premier gagne, ~ étendu, un fichier illisible est signalé ; le fichier par
	// défaut n'est pas lu.
	snprintf(a, sizeof(a), "%s/a.env", dir);
	snprintf(b, sizeof(b), "%s/b.env", dir);
	write_file(a, "STUDIO_INIT_A=a\nSTUDIO_INIT_BOTH=from a\nSTUDIO_SG_SCRIPT_KEY=hunter2\n");
	write_file(b, "STUDIO_INIT_B=b\nSTUDIO_INIT_BOTH=from b\n");
	write_file(path, "STUDIO_INIT_NOT_READ=1\n");
	snprintf(list, sizeof(list), "%s::%s:%s/missing.env", a, b, dir);
	setenv("STUDIO_ENV", list, 1);
	logs[0] = 0;
	studio_env_init();
	ENV_IS("STUDIO_INIT_A", "a");
	ENV_IS("STUDIO_INIT_B", "b");
	ENV_IS("STUDIO_INIT_BOTH", "from a");
	UNSET("STUDIO_INIT_NOT_READ");
	CHECK(logged("missing.env: cannot read the file"), "unreadable file:\n%s", logs);
	CHECK(logged("STUDIO_SG_SCRIPT_KEY=*** ("), "secret masked:\n%s", logs);
	CHECK(!logged("hunter2"), "secret in the log:\n%s", logs);
	char *first = strstr(logs, "studio: env STUDIO_INIT_A=");
	char *second = strstr(logs, "studio: env STUDIO_INIT_B=");
	CHECK(first != NULL && second != NULL && first < second, "summary sorted:\n%s", logs);
	char expected[2048];
	snprintf(expected, sizeof(expected), "studio: env STUDIO_INIT_BOTH=from a (%s)", a);
	CHECK(logged(expected), "origin in the summary:\n%s", logs);

	// ~ dans $STUDIO_ENV.
	const char *home = getenv("HOME");
	if (home != NULL && strncmp(a, home, strlen(home)) == 0) {
		snprintf(list, sizeof(list), "~%s", a + strlen(home));
		setenv("STUDIO_ENV", list, 1);
		logs[0] = 0;
		studio_env_init();
		CHECK(!logged("cannot read"), "~ in STUDIO_ENV:\n%s", logs);
	}

	// $STUDIO_ENV vide : aucun fichier.
	setenv("STUDIO_ENV", "", 1);
	logs[0] = 0;
	studio_env_init();
	UNSET("STUDIO_INIT_NOT_READ");
	CHECK(logged("STUDIO_ENV is empty"), "empty STUDIO_ENV:\n%s", logs);
}

int main(int argc, char **argv) {
	// env_test --print <fichier> NOM… : valeurs que donne studio_env_load, une par ligne (NOM=valeur, ou NOM seul s'il
	// n'est pas défini), comparées à studio_env.py par subzero_color_check.py (lot E11).
	if (argc >= 3 && strcmp(argv[1], "--print") == 0) {
		for (int i = 3; i < argc; i++) {
			unsetenv(argv[i]);
		}
		if (studio_env_load(argv[2]) < 0) {
			return 2;
		}
		for (int i = 3; i < argc; i++) {
			const char *value = getenv(argv[i]);
			printf(value != NULL ? "%s=%s\n" : "%s\n", argv[i], value);
		}
		return 0;
	}
	if (argc < 2) {
		fprintf(stderr, "usage: env_test <work dir> | env_test --print <file> NAME...\n");
		return 2;
	}
	test_syntax();
	test_init(argv[1]);
	printf("  %d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
