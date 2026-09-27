#include "studio_expr.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPR_MAX_CODE  256
#define EXPR_MAX_DEPTH 32
#define EXPR_MAX_NEST  64

static const char *var_names[STUDIO_VAR_COUNT] = {
    "base_r", "base_g", "base_b", "height", "metal", "nor_r", "nor_g",  "nor_g_directx", "nor_b", "occ",   "opac",
    "rough",  "smooth", "emis",   "subs",   "diff_r", "diff_g", "diff_b", "spec_r",       "spec_g", "spec_b"};

const char *studio_var_name(int var) {
	return var >= 0 && var < STUDIO_VAR_COUNT ? var_names[var] : NULL;
}

int studio_var_find(const char *name) {
	for (int i = 0; i < STUDIO_VAR_COUNT; ++i) {
		if (strcmp(var_names[i], name) == 0) {
			return i;
		}
	}
	return -1;
}

typedef enum {
	OP_CONST,
	OP_VAR,
	OP_NEG,
	OP_ADD,
	OP_SUB,
	OP_MUL,
	OP_DIV,
	OP_ABS,
	OP_SQRT,
	OP_MIN,
	OP_MAX,
	OP_POW,
	OP_STEP,
	OP_CLAMP,
	OP_MIX,
} op_t;

typedef struct {
	op_t  op;
	int   var;
	float value;
} instr_t;

struct studio_expr {
	instr_t  code[EXPR_MAX_CODE];
	int      count;
	int      depth;
	uint32_t vars;
	int      layer_count;
	char     layer_names[STUDIO_EXPR_MAX_LAYERS][STUDIO_EXPR_MAX_LAYER_NAME];
	int      layer_channels[STUDIO_EXPR_MAX_LAYERS];
};

typedef struct {
	const char    *src;
	const char    *p;
	studio_expr_t *e;
	int            sp;
	int            nest;
	char          *err;
	size_t         err_size;
	bool           failed;
} parser_t;

static void fail(parser_t *ps, const char *format, ...) {
	if (ps->failed) {
		return;
	}
	ps->failed = true;
	if (ps->err == NULL || ps->err_size == 0) {
		return;
	}
	char    msg[160];
	va_list args;
	va_start(args, format);
	vsnprintf(msg, sizeof(msg), format, args);
	va_end(args);
	snprintf(ps->err, ps->err_size, "%s (column %d in \"%s\")", msg, (int)(ps->p - ps->src) + 1, ps->src);
}

static int arity(op_t op) {
	switch (op) {
	case OP_CONST:
	case OP_VAR:
		return 0;
	case OP_NEG:
	case OP_ABS:
	case OP_SQRT:
		return 1;
	case OP_CLAMP:
	case OP_MIX:
		return 3;
	default:
		return 2;
	}
}

static float apply(op_t op, float a, float b, float c) {
	switch (op) {
	case OP_NEG:
		return -a;
	case OP_ADD:
		return a + b;
	case OP_SUB:
		return a - b;
	case OP_MUL:
		return a * b;
	case OP_DIV:
		return b == 0.0f ? 0.0f : a / b;
	case OP_ABS:
		return fabsf(a);
	case OP_SQRT:
		return a > 0.0f ? sqrtf(a) : 0.0f;
	case OP_MIN:
		return fminf(a, b);
	case OP_MAX:
		return fmaxf(a, b);
	case OP_POW:
		return powf(a, b);
	case OP_STEP: // step(edge, x)
		return b < a ? 0.0f : 1.0f;
	case OP_CLAMP:
		return fminf(fmaxf(a, b), c);
	case OP_MIX:
		return a + (b - a) * c;
	default:
		return 0.0f;
	}
}

static void emit(parser_t *ps, instr_t in) {
	if (ps->failed) {
		return;
	}
	studio_expr_t *e = ps->e;
	int            n = arity(in.op);
	// Repli : un opérateur dont tous les opérandes sont constants devient une constante.
	if (n > 0 && e->count >= n) {
		bool constant = true;
		for (int i = 0; i < n; ++i) {
			constant = constant && e->code[e->count - n + i].op == OP_CONST;
		}
		if (constant) {
			float v[3] = {0};
			for (int i = 0; i < n; ++i) {
				v[i] = e->code[e->count - n + i].value;
			}
			e->count -= n;
			ps->sp -= n;
			in = (instr_t){.op = OP_CONST, .value = apply(in.op, v[0], v[1], v[2])};
			n  = 0;
		}
	}
	if (e->count >= EXPR_MAX_CODE) {
		fail(ps, "expression too long");
		return;
	}
	e->code[e->count++] = in;
	ps->sp += 1 - n;
	if (ps->sp > EXPR_MAX_DEPTH) {
		fail(ps, "expression too deeply nested");
		return;
	}
	if (ps->sp > e->depth) {
		e->depth = ps->sp;
	}
}

static void skip_space(parser_t *ps) {
	while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r') {
		ps->p++;
	}
}

static bool is_digit(char c) {
	return c >= '0' && c <= '9';
}

static bool is_ident(char c, bool first) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (!first && is_digit(c));
}

// Nombre décimal, indépendant de la locale : chiffres, point, exposant.
static bool parse_number(parser_t *ps, float *out) {
	const char *p        = ps->p;
	double      value    = 0.0;
	int         digits   = 0;
	double      fraction = 0.1;
	while (is_digit(*p)) {
		value = value * 10.0 + (*p++ - '0');
		digits++;
	}
	if (*p == '.') {
		p++;
		while (is_digit(*p)) {
			value += (*p++ - '0') * fraction;
			fraction *= 0.1;
			digits++;
		}
	}
	if (digits == 0) {
		return false;
	}
	if (*p == 'e' || *p == 'E') {
		const char *q    = p + 1;
		int         sign = 1;
		if (*q == '+' || *q == '-') {
			sign = *q++ == '-' ? -1 : 1;
		}
		if (is_digit(*q)) {
			int exponent = 0;
			while (is_digit(*q)) {
				exponent = exponent < 1000 ? exponent * 10 + (*q - '0') : exponent;
				q++;
			}
			value *= pow(10.0, sign * exponent);
			p = q;
		}
	}
	ps->p = p;
	*out  = (float)value;
	return true;
}

static void parse_expr(parser_t *ps);

static void expect(parser_t *ps, char c) {
	skip_space(ps);
	if (*ps->p != c) {
		fail(ps, "'%c' expected", c);
		return;
	}
	ps->p++;
}

// layer('<nom>'), layer_r... : ps->p sur '(' ; channel 0-3.
static void parse_layer(parser_t *ps, const char *fn, int channel) {
	ps->p++; // '('
	skip_space(ps);
	char quote = *ps->p;
	if (quote != '\'' && quote != '"') {
		fail(ps, "%s() takes a layer name in quotes", fn);
		return;
	}
	const char *start = ++ps->p;
	while (*ps->p != 0 && *ps->p != quote) {
		ps->p++;
	}
	size_t len = (size_t)(ps->p - start);
	if (*ps->p == 0) {
		ps->p = start - 1;
		fail(ps, "unterminated layer name");
		return;
	}
	if (len == 0 || len >= STUDIO_EXPR_MAX_LAYER_NAME) {
		ps->p = start - 1;
		fail(ps, len == 0 ? "empty layer name" : "layer name too long");
		return;
	}
	ps->p++; // guillemet fermant
	expect(ps, ')');
	if (ps->failed) {
		return;
	}
	studio_expr_t *e = ps->e;
	int            i = 0;
	while (i < e->layer_count &&
	       !(e->layer_channels[i] == channel && strlen(e->layer_names[i]) == len && memcmp(e->layer_names[i], start, len) == 0)) {
		i++;
	}
	if (i == e->layer_count) {
		if (i == STUDIO_EXPR_MAX_LAYERS) {
			fail(ps, "more than %d layer channels", STUDIO_EXPR_MAX_LAYERS);
			return;
		}
		memcpy(e->layer_names[i], start, len);
		e->layer_names[i][len] = 0;
		e->layer_channels[i]   = channel;
		e->layer_count++;
	}
	emit(ps, (instr_t){.op = OP_VAR, .var = STUDIO_VAR_COUNT + i});
}

static void parse_primary(parser_t *ps) {
	skip_space(ps);
	if (ps->failed) {
		return;
	}
	float value = 0.0f;
	if (*ps->p == '(') {
		ps->p++;
		parse_expr(ps);
		expect(ps, ')');
		return;
	}
	if (is_digit(*ps->p) || *ps->p == '.') {
		if (!parse_number(ps, &value)) {
			fail(ps, "invalid number");
			return;
		}
		emit(ps, (instr_t){.op = OP_CONST, .value = value});
		return;
	}
	if (!is_ident(*ps->p, true)) {
		fail(ps, *ps->p == 0 ? "unexpected end of expression" : "unexpected '%c'", *ps->p);
		return;
	}
	const char *start = ps->p;
	while (is_ident(*ps->p, false)) {
		ps->p++;
	}
	char   name[32];
	size_t len = (size_t)(ps->p - start);
	if (len >= sizeof(name)) {
		ps->p = start;
		fail(ps, "unknown name");
		return;
	}
	memcpy(name, start, len);
	name[len] = 0;
	skip_space(ps);
	if (*ps->p != '(') {
		int var = studio_var_find(name);
		if (var < 0) {
			ps->p = start;
			fail(ps, "unknown channel '%s'", name);
			return;
		}
		ps->e->vars |= 1u << var;
		emit(ps, (instr_t){.op = OP_VAR, .var = var});
		return;
	}
	static const char *layer_fns[] = {"layer", "layer_r", "layer_g", "layer_b", "layer_a"};
	for (int i = 0; i < (int)(sizeof(layer_fns) / sizeof(layer_fns[0])); ++i) {
		if (strcmp(layer_fns[i], name) == 0) {
			parse_layer(ps, name, i > 0 ? i - 1 : 0);
			return;
		}
	}
	static const struct {
		const char *name;
		op_t        op;
	} functions[] = {{"abs", OP_ABS}, {"sqrt", OP_SQRT}, {"min", OP_MIN},     {"max", OP_MAX},
	                 {"pow", OP_POW}, {"step", OP_STEP}, {"clamp", OP_CLAMP}, {"mix", OP_MIX}};
	int fn = -1;
	for (int i = 0; i < (int)(sizeof(functions) / sizeof(functions[0])); ++i) {
		if (strcmp(functions[i].name, name) == 0) {
			fn = i;
		}
	}
	if (fn < 0) {
		ps->p = start;
		fail(ps, "unknown function '%s'", name);
		return;
	}
	ps->p++; // '('
	int want = arity(functions[fn].op);
	for (int i = 0; i < want && !ps->failed; ++i) {
		if (i > 0) {
			expect(ps, ',');
		}
		parse_expr(ps);
	}
	skip_space(ps);
	if (!ps->failed && *ps->p == ',') {
		fail(ps, "%s() takes %d argument%s", name, want, want > 1 ? "s" : "");
		return;
	}
	expect(ps, ')');
	emit(ps, (instr_t){.op = functions[fn].op});
}

static void parse_unary(parser_t *ps) {
	skip_space(ps);
	if (*ps->p == '-' || *ps->p == '+') {
		bool negate = *ps->p == '-';
		ps->p++;
		if (++ps->nest > EXPR_MAX_NEST) {
			fail(ps, "expression too deeply nested");
			return;
		}
		parse_unary(ps);
		ps->nest--;
		if (negate) {
			emit(ps, (instr_t){.op = OP_NEG});
		}
		return;
	}
	parse_primary(ps);
}

static void parse_term(parser_t *ps) {
	parse_unary(ps);
	for (;;) {
		skip_space(ps);
		if (ps->failed || (*ps->p != '*' && *ps->p != '/')) {
			return;
		}
		op_t op = *ps->p == '*' ? OP_MUL : OP_DIV;
		ps->p++;
		parse_unary(ps);
		emit(ps, (instr_t){.op = op});
	}
}

static void parse_expr(parser_t *ps) {
	if (++ps->nest > EXPR_MAX_NEST) {
		fail(ps, "expression too deeply nested");
		return;
	}
	parse_term(ps);
	for (;;) {
		skip_space(ps);
		if (ps->failed || (*ps->p != '+' && *ps->p != '-')) {
			break;
		}
		op_t op = *ps->p == '+' ? OP_ADD : OP_SUB;
		ps->p++;
		parse_term(ps);
		emit(ps, (instr_t){.op = op});
	}
	ps->nest--;
}

studio_expr_t *studio_expr_compile(const char *source, char *err, size_t err_size) {
	if (err != NULL && err_size > 0) {
		err[0] = 0;
	}
	studio_expr_t *e = calloc(1, sizeof(studio_expr_t));
	if (e == NULL) {
		if (err != NULL && err_size > 0) {
			snprintf(err, err_size, "out of memory");
		}
		return NULL;
	}
	parser_t ps = {.src = source, .p = source, .e = e, .err = err, .err_size = err_size};
	parse_expr(&ps);
	skip_space(&ps);
	if (!ps.failed && *ps.p != 0) {
		fail(&ps, "unexpected '%c'", *ps.p);
	}
	if (ps.failed) {
		free(e);
		return NULL;
	}
	return e;
}

void studio_expr_free(studio_expr_t *e) {
	free(e);
}

uint32_t studio_expr_vars(const studio_expr_t *e) {
	return e->vars;
}

int studio_expr_layer_count(const studio_expr_t *e) {
	return e->layer_count;
}

const char *studio_expr_layer(const studio_expr_t *e, int i, int *channel) {
	if (i < 0 || i >= e->layer_count) {
		return NULL;
	}
	if (channel != NULL) {
		*channel = e->layer_channels[i];
	}
	return e->layer_names[i];
}

bool studio_expr_constant(const studio_expr_t *e, float *value) {
	if (e->count == 1 && e->code[0].op == OP_CONST) {
		*value = e->code[0].value;
		return true;
	}
	return false;
}

int studio_expr_single_var(const studio_expr_t *e) {
	return e->count == 1 && e->code[0].op == OP_VAR ? e->code[0].var : -1;
}

int studio_expr_depth(const studio_expr_t *e) {
	return e->depth;
}

void studio_expr_eval(const studio_expr_t *e, int n, const float *const *vars, float *out, float *scratch) {
	int sp = 0; // lignes occupées dans scratch
	for (int k = 0; k < e->count; ++k) {
		const instr_t *in = &e->code[k];
		int            a  = arity(in->op);
		float         *r  = scratch + (size_t)(sp - a) * (size_t)n; // résultat : à la place du premier opérande
		if (in->op == OP_CONST) {
			r = scratch + (size_t)sp * (size_t)n;
			for (int i = 0; i < n; ++i) {
				r[i] = in->value;
			}
		}
		else if (in->op == OP_VAR) {
			r = scratch + (size_t)sp * (size_t)n;
			memcpy(r, vars[in->var], sizeof(float) * (size_t)n);
		}
		else if (a == 1) {
			for (int i = 0; i < n; ++i) {
				r[i] = apply(in->op, r[i], 0.0f, 0.0f);
			}
		}
		else if (a == 2) {
			const float *b = r + n;
			switch (in->op) { // chemins courants sans switch par pixel
			case OP_ADD:
				for (int i = 0; i < n; ++i) {
					r[i] += b[i];
				}
				break;
			case OP_SUB:
				for (int i = 0; i < n; ++i) {
					r[i] -= b[i];
				}
				break;
			case OP_MUL:
				for (int i = 0; i < n; ++i) {
					r[i] *= b[i];
				}
				break;
			default:
				for (int i = 0; i < n; ++i) {
					r[i] = apply(in->op, r[i], b[i], 0.0f);
				}
			}
		}
		else {
			const float *b = r + n;
			const float *c = r + 2 * (size_t)n;
			for (int i = 0; i < n; ++i) {
				r[i] = apply(in->op, r[i], b[i], c[i]);
			}
		}
		sp += 1 - a;
	}
	for (int i = 0; i < n; ++i) {
		float v = scratch[i];
		out[i]  = isnan(v) ? 0.0f : v;
	}
}
