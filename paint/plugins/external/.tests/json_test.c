// Banc de test de studio_json.c (lot C2), hors de l'app, sous ASan/UBSan : lancé par sg.sh.
#include "../studio_json.h"
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

// Texte de la valeur à `pointer`, "(invalid)" si absente. Libéré au prochain appel.
static const char *text(const char *json, const char *pointer) {
	static char *last = NULL;
	free(last);
	last = studio_json_text(studio_json_find(json, pointer));
	return last != NULL ? last : "(invalid)";
}

#define TEXT_IS(json, pointer, expected)                                                                               \
	do {                                                                                                               \
		const char *got_ = text(json, pointer);                                                                        \
		CHECK(strcmp(got_, expected) == 0, "%s at \"%s\": got \"%s\", expected \"%s\"", #json, pointer, got_, expected); \
	} while (0)

static const char *reply =
    "{\"id\":7,\"ok\":true,\"data\":[{\"id\":1,\"content\":\"texturing\",\"entity.Asset.code\":\"robot\",\"a/b\":1,\"t~\":2,"
    "\"entity\":{\"type\":\"Asset\",\"id\":1,\"name\":\"robot\"},\"due\":null,\"tags\":[],\"x\":{}},"
    "{\"id\":2,\"content\":\"\\u00c9lodie \\u00ab\\u65e5\\u672c\\u00bb \\ud83c\\udfa8 \\\"q\\\" \\\\ \\/ \\n\\t\",\"est\":-1.5e2}]}";

static void test_find(void) {
	TEXT_IS(reply, "/id", "7");
	TEXT_IS(reply, "/ok", "true");
	TEXT_IS(reply, "/data/0/content", "texturing");
	TEXT_IS(reply, "/data/0/entity.Asset.code", "robot");
	TEXT_IS(reply, "/data/0/a~1b", "1");
	TEXT_IS(reply, "/data/0/t~0", "2");
	TEXT_IS(reply, "/data/0/entity/name", "robot");
	TEXT_IS(reply, "/data/0/entity", "{\"type\":\"Asset\",\"id\":1,\"name\":\"robot\"}");
	TEXT_IS(reply, "/data/0/due", "null");
	TEXT_IS(reply, "/data/0/tags", "[]");
	TEXT_IS(reply, "/data/1/content", "Élodie «日本» 🎨 \"q\" \\ / \n\t");
	TEXT_IS(reply, "/data/1/est", "-1.5e2");
	TEXT_IS(reply, "", reply);
	TEXT_IS("  \n 42 ", "", "42");
	TEXT_IS("{\"\":{\"\":3}}", "//", "3"); // clés vides
	TEXT_IS("{\"a\":1,\"a\":2}", "/a", "1");
	// \u00e9 dans une clé : comparée décodée.
	TEXT_IS("{\"caf\\u00e9\":\"ok\"}", "/café", "ok");

	const char *missing[] = {"/nope", "/data/2", "/data/-1", "/data/01", "/data/x", "/data/0/content/0", "/id/0", "data",
	                         "/data/0/t~2", "/data/0/a~"};
	for (size_t i = 0; i < sizeof(missing) / sizeof(missing[0]); i++) {
		studio_json_value_t v = studio_json_find(reply, missing[i]);
		CHECK(v.type == STUDIO_JSON_INVALID && v.start == NULL, "missing \"%s\"", missing[i]);
	}
	CHECK(studio_json_find(NULL, "").type == STUDIO_JSON_INVALID, "NULL json");
	CHECK(studio_json_find("{\"a\":1}", NULL).type == STUDIO_JSON_OBJECT, "NULL pointer is root");

	struct {
		const char        *json;
		studio_json_type_t type;
	} types[] = {{"null", STUDIO_JSON_NULL},       {"false", STUDIO_JSON_BOOL}, {"-0.5", STUDIO_JSON_NUMBER},
	             {"\"s\"", STUDIO_JSON_STRING},     {"[1]", STUDIO_JSON_ARRAY},  {"{}", STUDIO_JSON_OBJECT}};
	for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		CHECK(studio_json_find(types[i].json, "").type == types[i].type, "type of %s", types[i].json);
	}
	printf("  ok   JSON Pointer lookup (escapes, dotted keys, UTF-8, surrogate pairs, missing paths)\n");
}

static void test_malformed(void) {
	const char *bad[] = {"",       "{",          "[1,]",         "[1 2]",       "{\"a\" 1}",     "{\"a\":}",   "{a:1}",
	                     "\"abc",  "\"a\\x\"",   "\"a\\u12\"",   "\"a\tb\"",    "[01]",          "1.",         "1e",
	                     "-",      "tru",        "nul",          "[}",          "{\"a\":1]",     "{\"a\":1,}", "+1",
	                     ".5",     "[\"\\u\"]",  "\"\\uzzzz\""};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		CHECK(studio_json_find(bad[i], "").type == STUDIO_JSON_INVALID, "malformed accepted: %s", bad[i]);
	}
	// Troncature à chaque position d'une réponse réelle : jamais de lecture hors du texte (ASan), jamais valide.
	size_t n = strlen(reply);
	for (size_t i = 0; i < n; i++) {
		char *cut = malloc(i + 1);
		memcpy(cut, reply, i);
		cut[i] = 0;
		studio_json_value_t v = studio_json_find(cut, "");
		CHECK(v.type == STUDIO_JSON_INVALID, "truncated at %zu accepted", i);
		studio_json_find(cut, "/data/1/content");
		free(cut);
	}
	// Profondeur : 512 niveaux passent, au-delà c'est refusé sans déborder la pile.
	char *deep = malloc(2 * 5000 + 1);
	for (int depth = 510; depth <= 5000; depth += 4490) {
		memset(deep, '[', (size_t)depth);
		memset(deep + depth, ']', (size_t)depth);
		deep[2 * depth] = 0;
		CHECK((studio_json_find(deep, "").type == STUDIO_JSON_ARRAY) == (depth < 512), "depth %d", depth);
	}
	free(deep);
	// Ce qui suit la valeur désignée n'est pas lu.
	TEXT_IS("{\"a\":1,\"b\":", "/a", "1");
	// Une moitié de paire isolée devient U+FFFD.
	TEXT_IS("\"\\ud83c.\\udfa8\"", "", "\xef\xbf\xbd.\xef\xbf\xbd");
	printf("  ok   malformed input rejected, truncation at every offset, depth limit, lone surrogates\n");
}

static void test_helpers(void) {
	CHECK(studio_json_length(studio_json_find(reply, "/data")) == 2, "length array");
	CHECK(studio_json_length(studio_json_find(reply, "/data/0")) == 9, "length object: %d",
	      studio_json_length(studio_json_find(reply, "/data/0")));
	CHECK(studio_json_length(studio_json_find(reply, "/data/0/tags")) == 0, "length empty array");
	CHECK(studio_json_length(studio_json_find(reply, "/data/0/x")) == 0, "length empty object");
	CHECK(studio_json_length(studio_json_find(reply, "/id")) == -1, "length number");
	CHECK(studio_json_length(studio_json_find(reply, "/nope")) == -1, "length missing");

	CHECK(studio_json_int(studio_json_find(reply, "/id"), -9) == 7, "int");
	CHECK(studio_json_int(studio_json_find(reply, "/data/1/est"), -9) == -150, "int from exponent");
	CHECK(studio_json_int(studio_json_find("2.9", ""), -9) == 2, "int truncates");
	CHECK(studio_json_int(studio_json_find("9007199254740993", ""), -9) == 9007199254740993LL, "int 64-bit exact");
	CHECK(studio_json_int(studio_json_find("99999999999999999999", ""), -9) == -9, "int overflow");
	CHECK(studio_json_int(studio_json_find("1e300", ""), -9) == -9, "int overflow (double)");
	CHECK(studio_json_int(studio_json_find("\"7\"", ""), -9) == -9, "int of a string");

	const char *truthy[] = {"true", "1", "-0.1", "\"x\"", "[]", "{}"};
	const char *falsy[]  = {"false", "0", "0.0", "\"\"", "null", "nope"};
	for (int i = 0; i < 6; i++) {
		CHECK(studio_json_truthy(studio_json_find(truthy[i], "")), "truthy %s", truthy[i]);
		CHECK(!studio_json_truthy(studio_json_find(falsy[i], "")), "falsy %s", falsy[i]);
	}

	// quote puis relecture : aller-retour exact.
	const char *strings[] = {"", "plain", "a \"quoted\" \\ path\\to", "tab\tnl\ncr\r\x01\x1f", "Élodie 🎨 /x/", NULL};
	for (int i = 0; i < 6; i++) {
		char *q    = studio_json_quote(strings[i]);
		char *back = studio_json_text(studio_json_find(q, ""));
		CHECK(back != NULL && strcmp(back, strings[i] != NULL ? strings[i] : "") == 0, "quote round trip %d: %s", i, q);
		free(q);
		free(back);
	}
	char *q = studio_json_quote("a\x01\"");
	CHECK(strcmp(q, "\"a\\u0001\\\"\"") == 0, "quote escapes: %s", q);
	free(q);
	printf("  ok   length, int, truthy, quote round trip\n");
}

int main(void) {
	test_find();
	test_malformed();
	test_helpers();
	printf("%d checks, %d failures\n", checks, failures);
	return failures > 0;
}
