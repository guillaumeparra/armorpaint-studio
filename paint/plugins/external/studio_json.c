#include "studio_json.h"
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define STUDIO_JSON_MAX_DEPTH 512

static const char *ws(const char *p) {
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
		p++;
	}
	return p;
}

static int hex(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

// \uXXXX à p (après le "\u"), -1 si mal formé.
static int hex4(const char *p) {
	int v = 0;
	for (int i = 0; i < 4; i++) {
		int h = hex(p[i]); // s'arrête sur le NUL : hex(0) == -1
		if (h < 0) {
			return -1;
		}
		v = v * 16 + h;
	}
	return v;
}

static int utf8(uint32_t c, char *out) {
	if (c < 0x80) {
		out[0] = (char)c;
		return 1;
	}
	if (c < 0x800) {
		out[0] = (char)(0xc0 | (c >> 6));
		out[1] = (char)(0x80 | (c & 0x3f));
		return 2;
	}
	if (c < 0x10000) {
		out[0] = (char)(0xe0 | (c >> 12));
		out[1] = (char)(0x80 | ((c >> 6) & 0x3f));
		out[2] = (char)(0x80 | (c & 0x3f));
		return 3;
	}
	out[0] = (char)(0xf0 | (c >> 18));
	out[1] = (char)(0x80 | ((c >> 12) & 0x3f));
	out[2] = (char)(0x80 | ((c >> 6) & 0x3f));
	out[3] = (char)(0x80 | (c & 0x3f));
	return 4;
}

// Décode le prochain caractère d'une chaîne JSON dont *p est dans le contenu (après le '"' ouvrant). Écrit ses octets
// UTF-8 dans out, en retourne le nombre ; 0 sur le '"' fermant (non consommé), -1 si mal formé.
static int next_char(const char **p, char *out) {
	const char *s = *p;
	unsigned char c = (unsigned char)*s;
	if (c == '"') {
		return 0;
	}
	if (c < 0x20) {
		return -1; // NUL (fin du texte) ou caractère de contrôle brut, interdit par JSON
	}
	if (c != '\\') {
		out[0] = (char)c;
		*p     = s + 1;
		return 1;
	}
	char e = s[1];
	int  n = 1;
	switch (e) {
	case '"':
	case '\\':
	case '/':
		out[0] = e;
		break;
	case 'b':
		out[0] = '\b';
		break;
	case 'f':
		out[0] = '\f';
		break;
	case 'n':
		out[0] = '\n';
		break;
	case 'r':
		out[0] = '\r';
		break;
	case 't':
		out[0] = '\t';
		break;
	case 'u': {
		int u = hex4(s + 2);
		if (u < 0) {
			return -1;
		}
		s += 6;
		uint32_t cp = (uint32_t)u;
		if (u >= 0xd800 && u <= 0xdbff && s[0] == '\\' && s[1] == 'u') {
			int low = hex4(s + 2);
			if (low >= 0xdc00 && low <= 0xdfff) {
				cp = 0x10000 + (((uint32_t)u - 0xd800) << 10) + ((uint32_t)low - 0xdc00);
				s += 6;
			}
		}
		if (cp >= 0xd800 && cp <= 0xdfff) {
			cp = 0xfffd; // moitié de paire isolée
		}
		*p = s;
		return utf8(cp, out);
	}
	default:
		return -1;
	}
	*p = s + 2;
	return n;
}

// Fin de la chaîne dont p pointe le '"' ouvrant, NULL si mal formée.
static const char *skip_string(const char *p) {
	p++;
	char buf[4];
	int  n;
	while ((n = next_char(&p, buf)) > 0) {
	}
	return n == 0 ? p + 1 : NULL;
}

static const char *skip_digits(const char *p) {
	if (*p < '0' || *p > '9') {
		return NULL;
	}
	while (*p >= '0' && *p <= '9') {
		p++;
	}
	return p;
}

static const char *skip_number(const char *p) {
	if (*p == '-') {
		p++;
	}
	if (*p == '0') {
		p++;
	}
	else if ((p = skip_digits(p)) == NULL) {
		return NULL;
	}
	if (*p == '.' && (p = skip_digits(p + 1)) == NULL) {
		return NULL;
	}
	if (*p == 'e' || *p == 'E') {
		p++;
		if (*p == '+' || *p == '-') {
			p++;
		}
		p = skip_digits(p);
	}
	return p;
}

static const char *skip_value(const char *p, studio_json_type_t *type, int depth) {
	p = ws(p);
	studio_json_type_t t = STUDIO_JSON_INVALID;
	const char        *end = NULL;
	switch (*p) {
	case '"':
		t   = STUDIO_JSON_STRING;
		end = skip_string(p);
		break;
	case 't':
		t   = STUDIO_JSON_BOOL;
		end = strncmp(p, "true", 4) == 0 ? p + 4 : NULL;
		break;
	case 'f':
		t   = STUDIO_JSON_BOOL;
		end = strncmp(p, "false", 5) == 0 ? p + 5 : NULL;
		break;
	case 'n':
		t   = STUDIO_JSON_NULL;
		end = strncmp(p, "null", 4) == 0 ? p + 4 : NULL;
		break;
	case '[':
	case '{': {
		bool object = *p == '{';
		char close  = object ? '}' : ']';
		t           = object ? STUDIO_JSON_OBJECT : STUDIO_JSON_ARRAY;
		if (depth >= STUDIO_JSON_MAX_DEPTH) {
			break;
		}
		p = ws(p + 1);
		if (*p == close) {
			end = p + 1;
			break;
		}
		while (p != NULL) {
			if (object) {
				p = ws(p);
				p = *p == '"' ? skip_string(p) : NULL;
				p = p != NULL ? ws(p) : NULL;
				p = p != NULL && *p == ':' ? p + 1 : NULL;
			}
			p = p != NULL ? skip_value(p, NULL, depth + 1) : NULL;
			p = p != NULL ? ws(p) : NULL;
			if (p != NULL && *p == close) {
				end = p + 1;
				break;
			}
			p = p != NULL && *p == ',' ? p + 1 : NULL;
		}
		break;
	}
	default:
		if (*p == '-' || (*p >= '0' && *p <= '9')) {
			t   = STUDIO_JSON_NUMBER;
			end = skip_number(p);
		}
	}
	if (type != NULL) {
		*type = end != NULL ? t : STUDIO_JSON_INVALID;
	}
	return end;
}

const char *studio_json_skip(const char *p, studio_json_type_t *type) {
	if (p == NULL) {
		if (type != NULL) {
			*type = STUDIO_JSON_INVALID;
		}
		return NULL;
	}
	return skip_value(p, type, 0);
}

// Prochain octet du segment de pointeur [*s, end) ("~0" -> '~', "~1" -> '/'), -1 à la fin ou si mal formé.
static int next_segment_byte(const char **s, const char *end) {
	if (*s >= end) {
		return -1;
	}
	char c = **s;
	if (c != '~') {
		(*s)++;
		return (unsigned char)c;
	}
	if (*s + 1 >= end || ((*s)[1] != '0' && (*s)[1] != '1')) {
		return -2;
	}
	c = (*s)[1] == '0' ? '~' : '/';
	*s += 2;
	return (unsigned char)c;
}

// La clé (p pointe son '"' ouvrant) vaut-elle le segment [seg, seg_end) ? *after : après le '"' fermant, NULL si la
// clé est mal formée.
static bool key_matches(const char *p, const char *seg, const char *seg_end, const char **after) {
	p++;
	bool match = true;
	char buf[4];
	int  n;
	while ((n = next_char(&p, buf)) > 0) {
		for (int i = 0; i < n && match; i++) {
			match = next_segment_byte(&seg, seg_end) == (unsigned char)buf[i];
		}
	}
	*after = n == 0 ? p + 1 : NULL;
	return *after != NULL && match && next_segment_byte(&seg, seg_end) == -1;
}

// Index de tableau du segment [seg, end) : chiffres sans zéro en tête, -1 sinon.
static long segment_index(const char *seg, const char *end) {
	if (seg == end || end - seg > 9 || (*seg == '0' && end - seg > 1)) {
		return -1;
	}
	long v = 0;
	for (const char *c = seg; c < end; c++) {
		if (*c < '0' || *c > '9') {
			return -1;
		}
		v = v * 10 + (*c - '0');
	}
	return v;
}

// Membre ou élément `seg` de la valeur composée qui commence à p (espaces sautés), NULL s'il n'existe pas.
static const char *child(const char *p, const char *seg, const char *seg_end) {
	bool object = *p == '{';
	char close  = object ? '}' : ']';
	long index  = object ? 0 : segment_index(seg, seg_end);
	if ((*p != '{' && *p != '[') || index < 0) {
		return NULL;
	}
	p = ws(p + 1);
	if (*p == close) {
		return NULL;
	}
	for (long i = 0;; i++) {
		bool found;
		if (object) {
			p = ws(p);
			if (*p != '"') {
				return NULL;
			}
			found = key_matches(p, seg, seg_end, &p);
			p     = p != NULL ? ws(p) : NULL;
			if (p == NULL || *p != ':') {
				return NULL;
			}
			p++;
		}
		else {
			found = i == index;
		}
		if (found) {
			return ws(p);
		}
		p = skip_value(p, NULL, 1);
		p = p != NULL ? ws(p) : NULL;
		if (p == NULL || *p != ',') {
			return NULL; // fin de la valeur composée sans l'avoir trouvé, ou mal formée
		}
		p++;
	}
}

studio_json_value_t studio_json_find(const char *json, const char *pointer) {
	studio_json_value_t v = {NULL, 0, STUDIO_JSON_INVALID};
	if (json == NULL) {
		return v;
	}
	const char *p = ws(json);
	const char *s = pointer != NULL ? pointer : "";
	if (*s != 0 && *s != '/') {
		return v;
	}
	while (*s == '/' && p != NULL) {
		const char *seg = s + 1;
		const char *end = strchr(seg, '/');
		if (end == NULL) {
			end = seg + strlen(seg);
		}
		p = child(p, seg, end);
		s = end;
	}
	if (p == NULL) {
		return v;
	}
	const char *end = skip_value(p, &v.type, 0);
	if (end != NULL) {
		v.start = p;
		v.len   = (size_t)(end - p);
	}
	return v;
}

char *studio_json_text(studio_json_value_t v) {
	if (v.type == STUDIO_JSON_INVALID || v.start == NULL) {
		return NULL;
	}
	char *out = malloc(v.len + 1); // le décodage ne rallonge jamais : \uXXXX (6) -> 3 octets au plus, une paire (12) -> 4
	if (out == NULL) {
		return NULL;
	}
	if (v.type != STUDIO_JSON_STRING) {
		memcpy(out, v.start, v.len);
		out[v.len] = 0;
		return out;
	}
	const char *p = v.start + 1;
	size_t      k = 0;
	int         n;
	while ((n = next_char(&p, out + k)) > 0) {
		k += (size_t)n;
	}
	out[k] = 0;
	return out;
}

int studio_json_length(studio_json_value_t v) {
	if (v.type != STUDIO_JSON_ARRAY && v.type != STUDIO_JSON_OBJECT) {
		return -1;
	}
	bool        object = v.type == STUDIO_JSON_OBJECT;
	char        close  = object ? '}' : ']';
	const char *p      = ws(v.start + 1);
	if (*p == close) {
		return 0;
	}
	for (int count = 1;; count++) {
		if (object) {
			p = skip_string(ws(p));
			p = p != NULL ? ws(p) : NULL;
			if (p == NULL || *p != ':') {
				return -1;
			}
			p++;
		}
		p = skip_value(p, NULL, 1);
		p = p != NULL ? ws(p) : NULL;
		if (p == NULL) {
			return -1;
		}
		if (*p == close) {
			return count;
		}
		if (*p != ',') {
			return -1;
		}
		p++;
	}
}

long long studio_json_int(studio_json_value_t v, long long fallback) {
	if (v.type != STUDIO_JSON_NUMBER || v.len > 64) {
		return fallback;
	}
	char buf[65];
	memcpy(buf, v.start, v.len);
	buf[v.len] = 0;
	errno      = 0;
	if (strpbrk(buf, ".eE") == NULL) {
		long long i = strtoll(buf, NULL, 10);
		return errno == 0 ? i : fallback;
	}
	double d = strtod(buf, NULL);
	if (errno != 0 || !(d > -9.2e18 && d < 9.2e18)) {
		return fallback;
	}
	return (long long)trunc(d);
}

bool studio_json_truthy(studio_json_value_t v) {
	switch (v.type) {
	case STUDIO_JSON_BOOL:
		return v.start[0] == 't';
	case STUDIO_JSON_NUMBER:
		return strtod(v.start, NULL) != 0;
	case STUDIO_JSON_STRING:
		return v.len > 2;
	case STUDIO_JSON_ARRAY:
	case STUDIO_JSON_OBJECT:
		return true;
	default:
		return false;
	}
}

char *studio_json_quote(const char *s) {
	if (s == NULL) {
		s = "";
	}
	size_t n = 2;
	for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
		n += *c == '"' || *c == '\\' ? 2 : *c < 0x20 ? 6 : 1;
	}
	char *out = malloc(n + 1);
	if (out == NULL) {
		return NULL;
	}
	char *o = out;
	*o++    = '"';
	for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
		if (*c == '"' || *c == '\\') {
			*o++ = '\\';
			*o++ = (char)*c;
		}
		else if (*c == '\n' || *c == '\r' || *c == '\t') {
			*o++ = '\\';
			*o++ = *c == '\n' ? 'n' : *c == '\r' ? 'r' : 't';
		}
		else if (*c < 0x20) {
			static const char digits[] = "0123456789abcdef";
			memcpy(o, "\\u00", 4);
			o[4] = digits[*c >> 4];
			o[5] = digits[*c & 15];
			o += 6;
		}
		else {
			*o++ = (char)*c;
		}
	}
	*o++ = '"';
	*o   = 0;
	return out;
}
