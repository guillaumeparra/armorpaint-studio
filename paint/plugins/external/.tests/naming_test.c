// Banc de test de studio_naming.c (lot B3), hors de l'app, sous ASan/UBSan : lancé par sg.sh.
// Usage : naming_test <dossier de travail>
#include "../studio_naming.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

static void expands(const char *pattern, const char *channel, const char *udim, const char *ext, const char *expected) {
	char *got = studio_naming_expand(pattern, channel, udim, ext);
	CHECK(got != NULL && strcmp(got, expected) == 0, "expand(\"%s\"): got \"%s\", expected \"%s\"", pattern, got ? got : "(null)", expected);
	free(got);
}

static void test_expand(void) {
	expands("/p/tex/v003/robot_<channel>.<ext>", "base", "", "png", "/p/tex/v003/robot_base.png");
	expands("/p/tex/v003/robot_<channel>.<UDIM>.<ext>", "nor", "1001", "exr", "/p/tex/v003/robot_nor.1001.exr");
	expands("<channel><channel>", "a", "", "", "aa");
	expands("", "a", "", "", "");
	// Un '<' qui n'ouvre pas de marqueur reste tel quel, y compris en fin de chaîne.
	expands("/p/<chan>/<ext", "base", "", "png", "/p/<chan>/<ext");
	expands("/p/x<", "base", "", "png", "/p/x<");
	// Les valeurs ne créent pas de dossier.
	expands("/p/<channel>.<ext>", "head/../../etc", "", "png", "/p/head_.._.._etc.png");
	expands("/p/<channel>.<ext>", "C:\\x", "", "png", "/p/C__x.png");
	// Un marqueur dans une valeur n'est pas remplacé à nouveau.
	expands("/p/<channel>_<UDIM>", "<UDIM>", "1002", "", "/p/<UDIM>_1002");
	char long_channel[5000];
	memset(long_channel, 'c', sizeof(long_channel) - 1);
	long_channel[sizeof(long_channel) - 1] = 0;
	char *got = studio_naming_expand("<channel><channel>", long_channel, "", "");
	CHECK(got != NULL && strlen(got) == 2 * (sizeof(long_channel) - 1), "long value");
	free(got);
}

static int is_dir(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void test_make_parent(const char *work) {
	char path[4096], dir[4096];
	snprintf(dir, sizeof(dir), "%s/naming/a/b/v003", work);
	snprintf(path, sizeof(path), "%s/robot_base.png", dir);
	CHECK(studio_naming_make_parent(path) && is_dir(dir), "make_parent %s", path);
	CHECK(studio_naming_make_parent(path), "make_parent again");
	snprintf(path, sizeof(path), "%s/naming//c///robot.png", work);
	snprintf(dir, sizeof(dir), "%s/naming/c", work);
	CHECK(studio_naming_make_parent(path) && is_dir(dir), "make_parent with doubled separators");
	CHECK(studio_naming_make_parent("robot.png"), "no parent");
	CHECK(studio_naming_make_parent("/robot.png"), "root parent");
	// Un fichier à la place d'un dossier : échec signalé.
	snprintf(path, sizeof(path), "%s/naming/file", work);
	FILE *f = fopen(path, "w");
	if (f != NULL) {
		fclose(f);
	}
	snprintf(path, sizeof(path), "%s/naming/file/sub/robot.png", work);
	CHECK(!studio_naming_make_parent(path), "parent blocked by a file");
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: naming_test <work dir>\n");
		return 2;
	}
	test_expand();
	test_make_parent(argv[1]);
	printf("  %d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
