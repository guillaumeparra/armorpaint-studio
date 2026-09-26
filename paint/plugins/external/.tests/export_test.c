// Banc des lots B4/B5 hors de l'app (studio_expr, studio_preset, studio_naming_format, studio_preflight, studio_sha256,
// studio_fs), sous ASan/UBSan. Lancé par .tests/export.sh.
//
// export_test unit <dossier de travail> <dossier des presets livrés>
// export_test sha <fichier>...     empreintes, comparées à shasum -a 256 par export.sh
// export_test fuzz <n>             expressions aléatoires

#include "../studio_expr.h"
#include "../studio_fs.h"
#include "../studio_json.h"
#include "../studio_naming.h"
#include "../studio_preflight.h"
#include "../studio_preset.h"
#include "../studio_sha256.h"
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int failures = 0;
static int checks   = 0;

#define CHECK(c, ...)                                                                                                  \
	do {                                                                                                               \
		checks++;                                                                                                      \
		if (!(c)) {                                                                                                    \
			printf("  FAIL %s:%d ", __FILE__, __LINE__);                                                               \
			printf(__VA_ARGS__);                                                                                       \
			printf("\n");                                                                                              \
			failures++;                                                                                                \
		}                                                                                                              \
	} while (0)

static uint32_t rng = 42;
static uint32_t rnd(void) {
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

// ---- SHA-256 ---------------------------------------------------------------------------------------------------------

static void test_sha256(void) {
	char hex[65];
	studio_sha256_hex("", 0, hex);
	CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0, "sha256('') %s", hex);
	studio_sha256_hex("abc", 3, hex);
	CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0, "sha256(abc) %s", hex);
	const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	studio_sha256_hex(two, strlen(two), hex);
	CHECK(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0, "sha256(448 bits)");
	// Un million de 'a', par morceaux de tailles variables (chemins de update).
	studio_sha256_t ctx;
	studio_sha256_init(&ctx);
	char block[1000];
	memset(block, 'a', sizeof(block));
	size_t done = 0;
	while (done < 1000000) {
		size_t n = 1 + rnd() % 999;
		n        = n > 1000000 - done ? 1000000 - done : n;
		studio_sha256_update(&ctx, block, n);
		done += n;
	}
	studio_sha256_final(&ctx, hex);
	CHECK(strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0, "sha256(1M a)");
}

// ---- Expressions -----------------------------------------------------------------------------------------------------

static float eval1(const char *src, const float *values) {
	char           err[256];
	studio_expr_t *e = studio_expr_compile(src, err, sizeof(err));
	if (e == NULL) {
		CHECK(0, "compile '%s': %s", src, err);
		return NAN;
	}
	const float *vars[STUDIO_VAR_COUNT];
	for (int i = 0; i < STUDIO_VAR_COUNT; ++i) {
		vars[i] = &values[i];
	}
	float  out     = 0;
	float *scratch = malloc(sizeof(float) * (size_t)(studio_expr_depth(e) + 1));
	studio_expr_eval(e, 1, vars, &out, scratch);
	free(scratch);
	studio_expr_free(e);
	return out;
}

static void test_expr(void) {
	float v[STUDIO_VAR_COUNT];
	for (int i = 0; i < STUDIO_VAR_COUNT; ++i) {
		v[i] = 0.1f * (float)(i + 1);
	}
	float occ = v[STUDIO_VAR_OCC], rough = v[STUDIO_VAR_ROUGH], metal = v[STUDIO_VAR_METAL];
	struct {
		const char *src;
		float       want;
	} cases[] = {
	    {"occ*rough", occ * rough},
	    {"occ * rough * metal", occ * rough * metal},
	    {"1 - 2 * 3", -5},
	    {"-(2+3)*2", -10},
	    {"2*-3", -6},
	    {"--2", 2},
	    {"+2", 2},
	    {"8/2/2", 2},
	    {"1-1-1", -1},
	    {"mix(0, 10, 0.25)", 2.5f},
	    {"clamp(5, 0, 1)", 1},
	    {"clamp(-5, 0, 1)", 0},
	    {"step(0.5, 0.7)", 1},
	    {"step(0.5, 0.2)", 0},
	    {"pow(2, 3)", 8},
	    {"sqrt(16)", 4},
	    {"sqrt(-1)", 0},
	    {"1/0", 0},
	    {"rough/0", 0},
	    {"min(occ, rough)", fminf(occ, rough)},
	    {"max(occ, rough)", fmaxf(occ, rough)},
	    {"abs(-rough)", rough},
	    {"1e2", 100},
	    {"2.5E-1", 0.25f},
	    {".5", 0.5f},
	    {"0.0", 0},
	    {"1.0", 1},
	    {"  ( rough )  ", rough},
	    {"1 - metal", 1 - metal},
	    {"mix(rough, 0.5, metal)", rough + (0.5f - rough) * metal},
	    {"pow(-1, 0.5)", 0}, // NaN -> 0
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		float got = eval1(cases[i].src, v);
		CHECK(fabsf(got - cases[i].want) < 1e-5f, "'%s' = %g, want %g", cases[i].src, got, cases[i].want);
	}

	char           err[256];
	studio_expr_t *e = studio_expr_compile("occ*rough", err, sizeof(err));
	CHECK(e != NULL && studio_expr_vars(e) == ((1u << STUDIO_VAR_OCC) | (1u << STUDIO_VAR_ROUGH)), "vars mask");
	CHECK(e != NULL && studio_expr_single_var(e) == -1, "not single");
	studio_expr_free(e);
	e = studio_expr_compile("nor_g_directx", err, sizeof(err));
	CHECK(e != NULL && studio_expr_single_var(e) == STUDIO_VAR_NOR_G_DIRECTX, "single var");
	studio_expr_free(e);
	float value = -1;
	e           = studio_expr_compile("(1 + 2) * 0.25 - min(1, 0.5)", err, sizeof(err));
	CHECK(e != NULL && studio_expr_constant(e, &value) && fabsf(value - 0.25f) < 1e-6f, "constant fold %g", value);
	studio_expr_free(e);

	const char *bad[] = {"", "occ*", "foo", "min(1)", "min(1,2,3)", "(((", "rough rough", "1..2", "rough)", "é",
	                     "abs", "abs()", "sqrt(1,)", ",", "1 +* 2", "base_rr", "BASE_R", "clamp(1,2)"};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
		err[0] = 0;
		e      = studio_expr_compile(bad[i], err, sizeof(err));
		CHECK(e == NULL && err[0] != 0, "'%s' should fail", bad[i]);
		studio_expr_free(e);
	}
	// Imbrication et longueur démesurées : erreur propre, pas de débordement de pile.
	char *deep = malloc(200001);
	for (int i = 0; i < 100000; ++i) {
		deep[i] = '(';
	}
	strcpy(deep + 100000, "1");
	CHECK(studio_expr_compile(deep, err, sizeof(err)) == NULL, "deep parens");
	memset(deep, '-', 100000);
	CHECK(studio_expr_compile(deep, err, sizeof(err)) == NULL, "deep unary");
	char *longx = malloc(20000);
	longx[0]    = 0;
	for (int i = 0; i < 2000; ++i) {
		strcat(longx, i > 0 ? "+rough" : "rough");
	}
	CHECK(studio_expr_compile(longx, err, sizeof(err)) == NULL && strstr(err, "long") != NULL, "long: %s", err);
	free(longx);
	// Chaîne à droite : a+(b+(c+...)) pousse la pile d'évaluation.
	deep[0] = 0;
	for (int i = 0; i < 40; ++i) {
		strcat(deep, "rough+(");
	}
	strcat(deep, "1");
	for (int i = 0; i < 40; ++i) {
		strcat(deep, ")");
	}
	CHECK(studio_expr_compile(deep, err, sizeof(err)) == NULL && strstr(err, "nested") != NULL, "stack depth: %s", err);
	free(deep);

	// Évaluation par ligne : n valeurs, out confondu avec une entrée.
	int    n = 1000;
	float *rows[STUDIO_VAR_COUNT];
	for (int i = 0; i < STUDIO_VAR_COUNT; ++i) {
		rows[i] = malloc(sizeof(float) * (size_t)n);
		for (int k = 0; k < n; ++k) {
			rows[i][k] = (float)(rnd() % 1000) / 999.0f;
		}
	}
	e              = studio_expr_compile("clamp(occ * 2 - rough, 0, 1)", err, sizeof(err));
	float *scratch = malloc(sizeof(float) * (size_t)(studio_expr_depth(e) * n));
	float *expect  = malloc(sizeof(float) * (size_t)n);
	for (int k = 0; k < n; ++k) {
		expect[k] = fminf(fmaxf(rows[STUDIO_VAR_OCC][k] * 2 - rows[STUDIO_VAR_ROUGH][k], 0), 1);
	}
	studio_expr_eval(e, n, (const float *const *)rows, rows[STUDIO_VAR_OCC], scratch);
	int diff = 0;
	for (int k = 0; k < n; ++k) {
		diff += rows[STUDIO_VAR_OCC][k] != expect[k];
	}
	CHECK(diff == 0, "row eval: %d differences", diff);
	studio_expr_free(e);
	free(scratch);
	free(expect);
	for (int i = 0; i < STUDIO_VAR_COUNT; ++i) {
		free(rows[i]);
	}
	CHECK(studio_var_find("spec_b") == STUDIO_VAR_SPEC_B && studio_var_find("x") == -1, "var names");
}

static void gen_expr(char *out, size_t size, int depth, int constant) {
	static const char *vars[]  = {"occ", "rough", "metal", "base_r", "nor_g", "height", "spec_b"};
	static const char *nums[]  = {"0", "1", "0.5", "2", "1e-3", "3.25", "0.0"};
	static const char *ops[]   = {" + ", " - ", " * ", " / "};
	static const char *fn1[]   = {"abs", "sqrt"};
	static const char *fn2[]   = {"min", "max", "pow", "step"};
	static const char *fn3[]   = {"clamp", "mix"};
	size_t             len     = strlen(out);
	int                kind    = depth > 6 ? (int)(rnd() % 2) : (int)(rnd() % 7);
	if (len + 64 > size) {
		kind = 1;
	}
	switch (kind) {
	case 0:
		snprintf(out + len, size - len, "%s", constant ? nums[rnd() % 7] : vars[rnd() % 7]);
		return;
	case 1:
		snprintf(out + len, size - len, "%s", nums[rnd() % 7]);
		return;
	case 2:
		gen_expr(out, size, depth + 1, constant);
		strncat(out, ops[rnd() % 4], size - strlen(out) - 1);
		gen_expr(out, size, depth + 1, constant);
		return;
	case 3:
		strncat(out, "-(", size - len - 1);
		gen_expr(out, size, depth + 1, constant);
		strncat(out, ")", size - strlen(out) - 1);
		return;
	case 4:
		snprintf(out + len, size - len, "%s(", fn1[rnd() % 2]);
		gen_expr(out, size, depth + 1, constant);
		strncat(out, ")", size - strlen(out) - 1);
		return;
	case 5:
		snprintf(out + len, size - len, "%s(", fn2[rnd() % 4]);
		gen_expr(out, size, depth + 1, constant);
		strncat(out, ", ", size - strlen(out) - 1);
		gen_expr(out, size, depth + 1, constant);
		strncat(out, ")", size - strlen(out) - 1);
		return;
	default:
		snprintf(out + len, size - len, "%s(", fn3[rnd() % 2]);
		gen_expr(out, size, depth + 1, constant);
		strncat(out, ", ", size - strlen(out) - 1);
		gen_expr(out, size, depth + 1, constant);
		strncat(out, ", ", size - strlen(out) - 1);
		gen_expr(out, size, depth + 1, constant);
		strncat(out, ")", size - strlen(out) - 1);
	}
}

static void fuzz_expr(int count) {
	static const char *tokens[] = {"occ", "rough", "metal", "base_r", "1", "0.5", "2e3", "-", "+", "*", "/", "(", ")",
	                               ",", "min(", "max(", "clamp(", "mix(", "pow(", "abs(", "sqrt(", "step(", " ", "x", ".",
	                               "e", "é", "\t"};
	int ntok = (int)(sizeof(tokens) / sizeof(tokens[0]));
	float  vals[STUDIO_VAR_COUNT * 4];
	float *rows[STUDIO_VAR_COUNT];
	for (int i = 0; i < STUDIO_VAR_COUNT; ++i) {
		rows[i] = &vals[i * 4];
		for (int k = 0; k < 4; ++k) {
			rows[i][k] = (float)(rnd() % 2001) / 1000.0f - 1.0f;
		}
	}
	int compiled = 0;
	for (int i = 0; i < count; ++i) {
		char src[512] = "";
		int  len      = (int)(rnd() % 30);
		for (int k = 0; k < len; ++k) {
			strcat(src, tokens[rnd() % (uint32_t)ntok]);
		}
		char           err[256];
		studio_expr_t *e = studio_expr_compile(src, err, sizeof(err));
		if (e == NULL) {
			continue;
		}
		compiled++;
		float *scratch = malloc(sizeof(float) * (size_t)(studio_expr_depth(e) * 4 + 1));
		float  out[4];
		studio_expr_eval(e, 4, (const float *const *)rows, out, scratch);
		for (int k = 0; k < 4; ++k) {
			CHECK(!isnan(out[k]), "NaN from '%s'", src);
		}
		free(scratch);
		studio_expr_free(e);
	}
	printf("  fuzz: %d random strings, %d valid\n", count, compiled);

	// Expressions bien formées tirées de la grammaire : toutes doivent compiler (sauf trop longues ou trop profondes)
	// et s'évaluer sans NaN ; une expression sans variable doit être repliée en constante de même valeur.
	int deep = 0, folded = 0;
	for (int i = 0; i < count / 4; ++i) {
		char src[4096] = "";
		gen_expr(src, sizeof(src), 0, (int)(rnd() % 2));
		char           err[256];
		studio_expr_t *e = studio_expr_compile(src, err, sizeof(err));
		if (e == NULL) {
			CHECK(strstr(err, "nested") != NULL || strstr(err, "long") != NULL, "'%s': %s", src, err);
			deep++;
			continue;
		}
		float *scratch = malloc(sizeof(float) * (size_t)(studio_expr_depth(e) * 4 + 1));
		float  out[4];
		studio_expr_eval(e, 4, (const float *const *)rows, out, scratch);
		float c = 0;
		if (studio_expr_vars(e) == 0) {
			CHECK(studio_expr_constant(e, &c), "'%s' not folded", src);
			CHECK(c == out[0] || (isnan(c) && out[0] == 0) || (isinf(c) && c == out[0]), "fold '%s': %g vs %g", src, c,
			      out[0]);
			folded++;
		}
		for (int k = 0; k < 4; ++k) {
			CHECK(!isnan(out[k]), "NaN from '%s'", src);
		}
		free(scratch);
		studio_expr_free(e);
	}
	printf("  fuzz: %d generated expressions (%d too deep, %d constant)\n", count / 4, deep, folded);
}

// ---- Nommage ---------------------------------------------------------------------------------------------------------

static void check_format(const char *pattern, const char *file, const char *variant, const char *channel,
                         const char *udim, const char *ext, const char *want) {
	const char *names[]  = {"file", "variant", "channel", "UDIM", "ext"};
	const char *values[] = {file, variant, channel, udim, ext};
	char       *got      = studio_naming_format(pattern, names, values, 5);
	CHECK(got != NULL && strcmp(got, want) == 0, "format '%s' = '%s', want '%s'", pattern, got, want);
	free(got);
}

static void test_naming(void) {
	const char *d = STUDIO_PRESET_DEFAULT_NAMING;
	check_format(d, "robot", "", "baseColor", "", "exr", "robot_baseColor.exr");
	check_format(d, "robot", "body", "baseColor", "", "exr", "robot_body_baseColor.exr");
	check_format(d, "robot", "", "baseColor", "1001", "exr", "robot_baseColor.1001.exr");
	check_format(d, "robot", "body", "normal", "1002", "tif", "robot_body_normal.1002.tif");
	check_format("<file>_<channel>.<ext>", "a/b", "", "c:d", "", "png", "a_b_c_d.png");
	check_format("tex/<file>/<channel>.<ext>", "robot", "", "rough", "", "png", "tex/robot/rough.png");
	check_format("[<nope>]<file>", "robot", "", "", "", "", "<nope>robot");
	check_format("<file>[_v<variant>_]x", "f", "", "", "", "", "fx");
	check_format("<file>[plain]", "f", "", "", "", "", "fplain");
	check_format("<file>[_<variant>", "f", "v", "", "", "", "f[_v");
	check_format("<file>]_[<channel>]", "f", "", "c", "", "", "f]_c");
	check_format("<<file>>", "f", "", "", "", "", "<f>");
	check_format("", "f", "", "", "", "", "");
	check_format("[<UDIM>][<UDIM>]", "f", "", "", "1001", "", "10011001");
}

// ---- Presets ---------------------------------------------------------------------------------------------------------

static studio_preset_t *parse(const char *json, char *err, size_t size) {
	return studio_preset_parse(json, "test", err, size);
}

static void expect_error(const char *json, const char *fragment) {
	char             err[512] = "";
	studio_preset_t *p        = parse(json, err, sizeof(err));
	CHECK(p == NULL && strstr(err, fragment) != NULL, "preset error '%s' expected, got '%s' for %s", fragment, err, json);
	studio_preset_free(p);
}

// is_data de studio_ocio pour les tests : "Raw" est le seul espace de donnée.
static int fake_is_data(const char *cs) {
	return cs != NULL && strcmp(cs, "Raw") == 0;
}

static void test_presets(const char *shipped_dir) {
	char             err[512] = "";
	studio_preset_t *p        = parse("{\"version\": 1, \"description\": \"t\", \"naming\": \"<file>_<channel>.<ext>\","
	                                  " \"defaults\": {\"format\": \"exr\", \"bits\": 16, \"color_space\": \"raw\","
	                                  " \"compression\": {\"exr\": \"piz\", \"tiff\": \"lzw\"}},"
	                                  " \"resolutions\": [4096, \"2048x1024\"], \"overwrite\": \"block\","
	                                  " \"manifest\": false, \"textures\": ["
	                                  "  {\"name\": \"base\", \"channels\": [\"base_r\", \"base_g\", \"base_b\"],"
	                                  "   \"color_space\": \"linear\"},"
	                                  "  {\"name\": \"orm\", \"suffix\": \"ORM\", \"channels\": [\"occ\", \"rough\","
	                                  "   \"metal\"], \"format\": \"png\", \"bits\": 16, \"compression\": \"best\"},"
	                                  "  {\"name\": \"mask\", \"channels\": [\"occ*rough\"], \"format\": \"tiff\"},"
	                                  "  {\"name\": \"h\", \"channels\": [\"height\"], \"bits\": 32},"
	                                  "  {\"name\": \"prev\", \"channels\": [\"base_r\", \"base_g\", \"base_b\"],"
	                                  "   \"format\": \"jpg\", \"quality\": 75, \"color_space\": \"srgb\"},"
	                                  "  {\"name\": \"k\", \"channels\": [0.5, 1, \"1.0\", \"0.0\"], \"format\": \"png\"}"
	                                  " ]}",
	                                  err, sizeof(err));
	CHECK(p != NULL, "full preset: %s", err);
	if (p != NULL) {
		CHECK(p->texture_count == 6 && p->overwrite == STUDIO_OVERWRITE_BLOCK && !p->manifest, "preset fields");
		CHECK(p->resolution_count == 2 && p->resolutions[0].width == 4096 && p->resolutions[0].height == 4096 &&
		          p->resolutions[1].width == 2048 && p->resolutions[1].height == 1024,
		      "resolutions");
		CHECK(strcmp(p->naming, "<file>_<channel>.<ext>") == 0, "naming");
		studio_preset_texture_t *t = p->textures;
		CHECK(t[0].format == STUDIO_FORMAT_EXR && t[0].bits == 16 && strcmp(t[0].compression, "piz") == 0 &&
		          t[0].color == STUDIO_COLOR_LINEAR && t[0].channels == 3 && strcmp(t[0].suffix, "base") == 0,
		      "texture base");
		CHECK(t[1].format == STUDIO_FORMAT_PNG && t[1].bits == 16 && strcmp(t[1].compression, "best") == 0 &&
		          strcmp(t[1].suffix, "ORM") == 0 && t[1].color == STUDIO_COLOR_RAW,
		      "texture orm");
		CHECK(t[2].format == STUDIO_FORMAT_TIFF && t[2].bits == 16 && strcmp(t[2].compression, "lzw") == 0 &&
		          t[2].channels == 1,
		      "texture mask (defaults.bits applies to tiff, compression from the per-format object)");
		CHECK(t[3].format == STUDIO_FORMAT_EXR && t[3].bits == 32, "texture h");
		CHECK(t[4].format == STUDIO_FORMAT_JPG && t[4].bits == 8 && t[4].quality == 75 && t[4].compression == NULL,
		      "texture prev (defaults.bits 16 does not apply to jpg)");
		CHECK(t[5].channels == 4 && t[5].bits == 16, "texture k");
		char *stub = studio_preset_stub(p);
		CHECK(stub != NULL && studio_json_skip(stub, NULL) != NULL, "stub is JSON");
		CHECK(stub != NULL && strstr(stub, "\"name\": \"mask\", \"channels\": [\"0.0\", \"0.0\", \"0.0\", \"1.0\"]") != NULL,
		      "stub expression -> 0.0: %s", stub);
		CHECK(stub != NULL && strstr(stub, "[\"1.0\", \"1.0\", \"1.0\", \"0.0\"]") != NULL, "stub constants: %s", stub);
		CHECK(stub != NULL && strstr(stub, "\"name\": \"prev\", \"channels\": [\"base_r\", \"base_g\", \"base_b\", "
		                                   "\"1.0\"], \"color_space\": \"linear\"") != NULL,
		      "stub srgb colour -> linear: %s", stub);
		studio_json_value_t v = studio_json_find(stub, "/textures/2/channels");
		CHECK(studio_json_length(v) == 4, "stub 4 channels");
		CHECK(studio_json_length(studio_json_find(stub, "/textures/0")) == 3, "stub: only upstream keys");
		free(stub);
	}
	studio_preset_free(p);

	p = parse("{\"textures\": [{\"name\": \"a\", \"channels\": [\"rough\"], \"format\": \"png\"}]}", err, sizeof(err));
	CHECK(p != NULL && p->textures[0].bits == 8 && strcmp(p->textures[0].compression, "default") == 0 &&
	          strcmp(p->naming, STUDIO_PRESET_DEFAULT_NAMING) == 0 && p->manifest && p->overwrite == STUDIO_OVERWRITE_WARN,
	      "minimal preset: %s", err);
	studio_preset_free(p);

	// Lot A3 : noms OCIO, hérités des défauts, remplacés par un mot-clé ; le stub upstream les écrit en "linear".
	p = parse("{\"defaults\": {\"format\": \"exr\", \"color_space\": \"ACEScg\"}, \"textures\": ["
	          " {\"name\": \"a\", \"channels\": [\"base_r\", \"base_g\", \"base_b\"]},"
	          " {\"name\": \"b\", \"channels\": [\"rough\"], \"color_space\": \"raw\"},"
	          " {\"name\": \"c\", \"channels\": [\"base_r\", \"base_g\", \"base_b\"], \"format\": \"png\","
	          "  \"color_space\": \"sRGB - \\\"Texture\\\"\"},"
	          " {\"name\": \"d\", \"channels\": [\"base_r\", \"base_g\", \"base_b\"], \"color_space\": \"Raw\"}]}",
	          err, sizeof(err));
	CHECK(p != NULL, "OCIO preset: %s", err);
	if (p != NULL) {
		studio_preset_texture_t *t = p->textures;
		CHECK(t[0].color == STUDIO_COLOR_OCIO && strcmp(t[0].color_space, "ACEScg") == 0 &&
		          strcmp(studio_preset_color(&t[0]), "ACEScg") == 0,
		      "OCIO name from the defaults");
		CHECK(t[1].color == STUDIO_COLOR_RAW && t[1].color_space == NULL && strcmp(studio_preset_color(&t[1]), "raw") == 0,
		      "keyword over an OCIO default");
		CHECK(t[2].color == STUDIO_COLOR_OCIO && strcmp(t[2].color_space, "sRGB - \"Texture\"") == 0, "OCIO name kept as is");
		CHECK(t[3].color == STUDIO_COLOR_OCIO && strcmp(t[3].color_space, "Raw") == 0, "keywords are lower case only");
		char *stub = studio_preset_stub(p);
		CHECK(stub != NULL && studio_json_skip(stub, NULL) != NULL && strstr(stub, "srgb") == NULL, "stub OCIO -> linear: %s",
		      stub);
		free(stub);
	}
	studio_preset_free(p);
	// Espace imposé à l'export : les textures de couleur seulement (linear, srgb, OCIO hors donnée).
	p = parse("{\"defaults\": {\"format\": \"exr\"}, \"textures\": ["
	          " {\"name\": \"lin\", \"channels\": [\"base_r\", \"base_g\", \"base_b\"], \"color_space\": \"linear\"},"
	          " {\"name\": \"rough\", \"channels\": [\"rough\"], \"color_space\": \"raw\"},"
	          " {\"name\": \"srgb\", \"channels\": [\"base_r\", \"base_g\", \"base_b\"], \"format\": \"png\","
	          "  \"color_space\": \"srgb\"},"
	          " {\"name\": \"cg\", \"channels\": [\"base_r\", \"base_g\", \"base_b\"], \"color_space\": \"ACEScg\"},"
	          " {\"name\": \"data\", \"channels\": [\"nor_r\", \"nor_g\", \"nor_b\"], \"color_space\": \"Raw\"}]}",
	          err, sizeof(err));
	CHECK(p != NULL, "override preset: %s", err);
	if (p != NULL) {
		studio_preset_texture_t *t = p->textures;
		CHECK(studio_preset_texture_is_color(&t[4], NULL) && !studio_preset_texture_is_color(&t[4], fake_is_data) &&
		          !studio_preset_texture_is_color(&t[1], NULL) && studio_preset_texture_is_color(&t[3], fake_is_data),
		      "colour textures: linear, srgb and OCIO colour spaces, not raw nor OCIO data");
		int n = studio_preset_override_color(p, "Linear P3-D65", fake_is_data);
		CHECK(n == 3, "override: 3 colour textures changed, got %d", n);
		CHECK(t[0].color == STUDIO_COLOR_OCIO && strcmp(t[0].color_space, "Linear P3-D65") == 0 &&
		          t[2].color == STUDIO_COLOR_OCIO && strcmp(t[2].color_space, "Linear P3-D65") == 0 &&
		          strcmp(t[3].color_space, "Linear P3-D65") == 0,
		      "override: linear, srgb and ACEScg -> Linear P3-D65");
		CHECK(t[1].color == STUDIO_COLOR_RAW && t[1].color_space == NULL && strcmp(t[4].color_space, "Raw") == 0,
		      "override: raw and data colorspace unchanged");
		CHECK(t[2].format == STUDIO_FORMAT_PNG && t[2].bits == 8, "override: format and depth unchanged");
	}
	studio_preset_free(p);
	expect_error("{\"textures\": [{\"name\": \"a\", \"channels\": [\"occ\"], \"format\": \"png\", "
	             "\"color_space\": \"\"}]}",
	             "OCIO colorspace name");
	expect_error("{\"textures\": [{\"name\": \"a\", \"channels\": [\"occ\"], \"format\": \"png\", "
	             "\"color_space\": \"ocio\"}]}",
	             "color_space 'ocio'");
	expect_error("{\"defaults\": {\"color_space\": 3}, \"textures\": [{\"name\": \"a\", \"channels\": [\"occ\"], "
	             "\"format\": \"png\"}]}",
	             "defaults.color_space");
	expect_error("", "valid JSON");
	expect_error("[]", "valid JSON");
	expect_error("{\"textures\": []}", "non-empty");
	expect_error("{\"textures\": [{\"name\": \"a\", \"channels\": [\"rough\"], \"format\": \"png\"}]} x", "valid JSON");
	expect_error("{\"texture\": []}", "unknown key 'texture'");
	expect_error("{\"version\": 2, \"textures\": []}", "version");
	expect_error("{\"textures\": [{\"name\": \"a\", \"channels\": [\"rough\"], \"format\": \"png\", "
	             "\"colour_space\": \"raw\"}]}",
	             "unknown key 'colour_space'");
	expect_error("{\"textures\": [{\"name\": \"a\", \"channels\": [\"rough\"]}]}", "format missing");
	expect_error("{\"textures\": [{\"name\": \"a\", \"channels\": [\"rough\"], \"format\": \"bmp\"}]}", "'bmp' unknown");
	expect_error("{\"textures\": [{\"name\": \"a\", \"channels\": [\"rough\", \"occ\"], \"format\": \"png\"}]}",
	             "1, 3 or 4");
	expect_error("{\"textures\": [{\"name\": \"a\", \"channels\": [\"a\", \"b\", \"c\", \"d\"], \"format\": \"jpg\"}]}",
	             "unknown channel 'a'");
	expect_error("{\"textures\": [{\"name\": \"a\", \"channels\": [\"occ\", \"occ\", \"occ\", \"occ\"], \"format\": "
	             "\"jpg\"}]}",
	             "jpg has no alpha");
	expect_error("{\"textures\": [{\"name\": \"a\", \"channels\": [\"occ\"], \"format\": \"png\", \"bits\": 32}]}",
	             "32 bits not supported in png");
	expect_error("{\"textures\": [{\"name\": \"a\", \"channels\": [\"occ\"], \"format\": \"exr\", \"bits\": 8}]}",
	             "8 bits not supported in exr");
	expect_error("{\"textures\": [{\"name\": \"a\", \"channels\": [\"occ\"], \"format\": \"exr\", \"compression\": "
	             "\"dwaa\"}]}",
	             "compression 'dwaa'");
	expect_error("{\"textures\": [{\"name\": \"a\", \"channels\": [\"occ\"], \"format\": \"tiff\", \"compression\": "
	             "\"zip\"}]}",
	             "none, lzw, deflate");
	expect_error("{\"textures\": [{\"name\": \"a\", \"channels\": [\"occ\"], \"format\": \"jpg\", \"compression\": "
	             "\"best\"}]}",
	             "use quality");
	expect_error("{\"textures\": [{\"name\": \"a\", \"channels\": [\"occ\"], \"format\": \"png\", \"quality\": 50}]}",
	             "only applies to jpg");
	expect_error("{\"textures\": [{\"name\": \"a\", \"channels\": [\"occ\"], \"format\": \"png\"}, {\"name\": \"a\", "
	             "\"channels\": [\"occ\"], \"format\": \"png\"}]}",
	             "already used");
	expect_error("{\"textures\": [{\"channels\": [\"occ\"], \"format\": \"png\"}]}", "name missing");
	expect_error("{\"textures\": [{\"name\": \"a\", \"format\": \"png\"}]}", "channels missing");
	expect_error("{\"textures\": [{\"name\": \"a\", \"channels\": [\"occ*\"], \"format\": \"png\"}]}", "channels[0]");
	expect_error("{\"textures\": [{\"name\": \"a\", \"channels\": [true], \"format\": \"png\"}]}", "channels[0]");
	expect_error("{\"resolutions\": [\"4k\"], \"textures\": [{\"name\": \"a\", \"channels\": [\"occ\"], \"format\": "
	             "\"png\"}]}",
	             "resolutions[0]");
	expect_error("{\"overwrite\": \"never\", \"textures\": []}", "overwrite");
	expect_error("{\"manifest\": 1, \"textures\": []}", "manifest");
	expect_error("{\"defaults\": {\"format\": \"exr\", \"quality\": 0}, \"textures\": []}", "quality");
	expect_error("{\"defaults\": {\"fmt\": \"exr\"}, \"textures\": []}", "unknown key 'fmt'");
	expect_error("{\"defaults\": {\"compression\": {\"jpg\": \"x\"}}, \"textures\": []}", "not png, tiff or exr");
	expect_error("{\"defaults\": {\"format\": \"exr\", \"compression\": \"lzw\"}, \"textures\": [{\"name\": \"a\", "
	             "\"channels\": [\"occ\"]}]}",
	             "compression 'lzw' not supported in exr");

	// Presets livrés.
	DIR *dir = opendir(shipped_dir);
	CHECK(dir != NULL, "shipped presets dir %s", shipped_dir);
	int            shipped = 0;
	struct dirent *ent;
	while (dir != NULL && (ent = readdir(dir)) != NULL) {
		if (strncmp(ent->d_name, "studio_preset_", 14) != 0) {
			continue;
		}
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", shipped_dir, ent->d_name);
		p = studio_preset_load(path, ent->d_name, err, sizeof(err));
		CHECK(p != NULL, "shipped %s: %s", ent->d_name, err);
		if (p != NULL) {
			char *stub = studio_preset_stub(p);
			CHECK(stub != NULL && studio_json_skip(stub, NULL) != NULL, "stub of %s", ent->d_name);
			free(stub);
			CHECK(p->description != NULL && p->description[0] != 0, "%s: description", ent->d_name);
			shipped++;
		}
		studio_preset_free(p);
	}
	if (dir != NULL) {
		closedir(dir);
	}
	CHECK(shipped == 5, "5 shipped presets, found %d", shipped);
	printf("  presets: %d shipped presets parsed\n", shipped);
}

// ---- Préflight -------------------------------------------------------------------------------------------------------

static bool has_issue(const studio_report_t *r, const char *code, bool blocking) {
	for (int i = 0; i < r->count; ++i) {
		if (strcmp(r->issues[i].code, code) == 0 && r->issues[i].blocking == blocking) {
			return true;
		}
	}
	return false;
}

static void test_preflight(const char *work) {
	char err[512];
	studio_preset_t *p = parse("{\"resolutions\": [4096, 2048], \"textures\": ["
	                           "{\"name\": \"base\", \"channels\": [\"base_r\", \"base_g\", \"base_b\"], \"format\": "
	                           "\"exr\", \"color_space\": \"srgb\"},"
	                           "{\"name\": \"h\", \"channels\": [\"height\"], \"format\": \"png\"},"
	                           "{\"name\": \"m\", \"channels\": [\"metal\"], \"format\": \"jpg\"},"
	                           "{\"name\": \"big\", \"channels\": [\"occ\", \"occ\", \"occ\", \"occ\"], \"format\": "
	                           "\"png\", \"bits\": 16}]}",
	                           err, sizeof(err));
	CHECK(p != NULL, "preflight preset: %s", err);
	if (p == NULL) {
		return;
	}
	char dir[1024], ro[1024], existing[1024];
	snprintf(dir, sizeof(dir), "%s/preflight/out/new/sub", work);
	snprintf(ro, sizeof(ro), "%s/preflight/ro", work);
	snprintf(existing, sizeof(existing), "%s/preflight/existing.exr", work);
	mkdir(work, 0777);
	char tmp[1100];
	snprintf(tmp, sizeof(tmp), "%s/preflight", work);
	mkdir(tmp, 0777);
	mkdir(ro, 0777);
	chmod(ro, 0555);
	studio_fs_write(existing, "x", 1);

	studio_plan_file_t files[6];
	char               paths[6][1200];
	snprintf(paths[0], sizeof(paths[0]), "%s/robot_base.1001.exr", dir);
	snprintf(paths[1], sizeof(paths[1]), "%s/robot_h.1001.png", dir);
	snprintf(paths[2], sizeof(paths[2]), "%s", existing);
	snprintf(paths[3], sizeof(paths[3]), "%s/x.png", ro);
	snprintf(paths[4], sizeof(paths[4]), "%s/robot_h.1001.png", dir); // doublon
	snprintf(paths[5], sizeof(paths[5]), "%s/robot_big.png", dir);
	const studio_preset_texture_t *tex[6] = {&p->textures[0], &p->textures[1], &p->textures[0],
	                                         &p->textures[2], &p->textures[1], &p->textures[3]};
	for (int i = 0; i < 6; ++i) {
		files[i] = (studio_plan_file_t){.path = paths[i], .texture = tex[i], .variant = "", .udim = "1001",
		                                .width = 1000, .height = 1000};
		files[i].raw_size = (uint64_t)1000 * 1000 * (uint64_t)tex[i]->channels * (uint64_t)(tex[i]->bits / 8);
	}
	files[5].width = files[5].height = 16384;
	files[5].raw_size                = (uint64_t)16384 * 16384 * 8;
	const char     *tiles[]          = {"1001", "1002", "1005", "1011"};
	studio_plan_t   plan             = {.preset = p, .files = files, .file_count = 6, .width = 1000, .height = 1000,
	                                    .source_bits = 8, .mode = "per UDIM tile", .udim_mesh = true, .per_tile = true,
	                                    .tiles = tiles, .tile_count = 4, .layer_count = 2, .hidden_layers = 1,
	                                    .destination = dir};
	studio_report_t r                = {0};
	studio_preflight_run(&plan, &r);
	CHECK(has_issue(&r, "resolution", true), "resolution blocking");
	CHECK(has_issue(&r, "duplicate_path", true), "duplicate blocking");
	CHECK(has_issue(&r, "not_writable", true), "read-only dir blocking");
	CHECK(has_issue(&r, "png_too_large", true), "png too large blocking");
	CHECK(has_issue(&r, "exists", false), "existing warning");
	CHECK(has_issue(&r, "udim_gaps", false), "udim gaps");
	CHECK(has_issue(&r, "hidden_layers", false), "hidden layers");
	CHECK(has_issue(&r, "height_8bit", false), "8-bit height");
	CHECK(has_issue(&r, "jpg_data", false), "jpg data");
	CHECK(has_issue(&r, "exr_srgb", false), "exr srgb");
	CHECK(has_issue(&r, "bits_above_source", false), "16-bit from 8-bit layers");
	CHECK(!has_issue(&r, "disk_full", true), "enough space");
	CHECK(files[2].exists && !files[0].exists, "exists flags");
	char *text = studio_report_text(&plan, &r);
	char *json = studio_report_json(&plan, &r);
	CHECK(text != NULL && strstr(text, "NOT EXPORTED") != NULL && strstr(text, "1003, 1004, 1006") != NULL,
	      "report text:\n%s", text);
	CHECK(json != NULL && studio_json_skip(json, NULL) != NULL, "report json valid");
	CHECK(json != NULL && strcmp(studio_json_text(studio_json_find(json, "/ok")), "false") == 0, "json ok=false");
	CHECK(json != NULL && studio_json_length(studio_json_find(json, "/files")) == 6, "json files");
	if (getenv("SHOW_REPORT") != NULL) {
		printf("%s\n%s\n", text, json);
	}
	free(text);
	free(json);
	studio_report_free(&r);

	// Politique block, et un plan sans problème.
	p->overwrite    = STUDIO_OVERWRITE_BLOCK;
	plan.files      = &files[2];
	plan.file_count = 1;
	plan.width = plan.height = 2048;
	plan.hidden_layers       = 0;
	plan.udim_mesh = plan.per_tile = false;
	studio_preflight_run(&plan, &r);
	CHECK(has_issue(&r, "exists", true), "overwrite block");
	studio_report_free(&r);
	plan.files = &files[0];
	studio_preflight_run(&plan, &r);
	CHECK(r.blocking == 0, "clean plan: %d blocking (%s)", r.blocking, r.count > 0 ? r.issues[0].message : "");
	studio_report_free(&r);
	plan.layer_count = 0;
	studio_preflight_run(&plan, &r);
	CHECK(has_issue(&r, "no_layers", true), "no layers");
	studio_report_free(&r);
	plan.layer_count = 1;
	plan.file_count  = 0;
	plan.naming_error = "template texture_path: missing field Asset";
	studio_preflight_run(&plan, &r);
	CHECK(has_issue(&r, "no_files", true) && has_issue(&r, "naming", true), "no files, naming");
	studio_report_free(&r);

	chmod(ro, 0755);
	studio_preset_free(p);

	char *found = studio_fs_existing_ancestor(dir);
	CHECK(found != NULL && strcmp(found, tmp) == 0, "existing ancestor %s", found);
	free(found);
	CHECK(studio_fs_free_space(dir) > 0, "free space");
	char *dn = studio_fs_dirname("a/b/c.png");
	CHECK(strcmp(dn, "a/b") == 0, "dirname");
	free(dn);
	dn = studio_fs_dirname("c.png");
	CHECK(strcmp(dn, "") == 0, "dirname bare");
	free(dn);
	dn = studio_fs_dirname("/c.png");
	CHECK(strcmp(dn, "/") == 0, "dirname root");
	free(dn);
}

int main(int argc, char **argv) {
	if (argc >= 3 && strcmp(argv[1], "sha") == 0) {
		for (int i = 2; i < argc; ++i) {
			char hex[65];
			if (!studio_sha256_file(argv[i], hex)) {
				return 1;
			}
			printf("%s  %s\n", hex, argv[i]);
		}
		return 0;
	}
	if (argc >= 3 && strcmp(argv[1], "fuzz") == 0) {
		fuzz_expr(atoi(argv[2]));
		return failures != 0;
	}
	if (argc < 4 || strcmp(argv[1], "unit") != 0) {
		fprintf(stderr, "usage: export_test unit <work> <presets dir> | sha <file>... | fuzz <n>\n");
		return 2;
	}
	test_sha256();
	test_expr();
	test_naming();
	test_presets(argv[3]);
	test_preflight(argv[2]);
	printf("  %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
