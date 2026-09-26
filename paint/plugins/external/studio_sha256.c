#include "studio_sha256.h"
#include "studio_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void transform(studio_sha256_t *ctx, const uint8_t *p) {
	uint32_t w[64];
	for (int i = 0; i < 16; ++i) {
		w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
	}
	for (int i = 16; i < 64; ++i) {
		uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i]        = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
	uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
	for (int i = 0; i < 64; ++i) {
		uint32_t t1 = h + (ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
		uint32_t t2 = (ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		h           = g;
		g           = f;
		f           = e;
		e           = d + t1;
		d           = c;
		c           = b;
		b           = a;
		a           = t1 + t2;
	}
	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
	ctx->state[5] += f;
	ctx->state[6] += g;
	ctx->state[7] += h;
}

void studio_sha256_init(studio_sha256_t *ctx) {
	static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
	                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	memcpy(ctx->state, init, sizeof(init));
	ctx->length = 0;
	ctx->used   = 0;
}

void studio_sha256_update(studio_sha256_t *ctx, const void *data, size_t size) {
	const uint8_t *p = data;
	ctx->length += size;
	if (ctx->used > 0) {
		size_t take = 64 - ctx->used < size ? 64 - ctx->used : size;
		memcpy(ctx->block + ctx->used, p, take);
		ctx->used += take;
		p += take;
		size -= take;
		if (ctx->used < 64) {
			return;
		}
		transform(ctx, ctx->block);
		ctx->used = 0;
	}
	for (; size >= 64; p += 64, size -= 64) {
		transform(ctx, p);
	}
	memcpy(ctx->block, p, size);
	ctx->used = size;
}

void studio_sha256_final(studio_sha256_t *ctx, char hex[65]) {
	uint64_t bits = ctx->length * 8;
	uint8_t  pad  = 0x80;
	uint8_t  zero = 0;
	uint64_t len  = ctx->length;
	studio_sha256_update(ctx, &pad, 1);
	while (ctx->used != 56) {
		studio_sha256_update(ctx, &zero, 1);
	}
	uint8_t tail[8];
	for (int i = 0; i < 8; ++i) {
		tail[i] = (uint8_t)(bits >> (56 - i * 8));
	}
	studio_sha256_update(ctx, tail, 8);
	ctx->length = len;
	static const char digits[] = "0123456789abcdef";
	for (int i = 0; i < 32; ++i) {
		uint8_t byte   = (uint8_t)(ctx->state[i / 4] >> (24 - (i % 4) * 8));
		hex[i * 2]     = digits[byte >> 4];
		hex[i * 2 + 1] = digits[byte & 15];
	}
	hex[64] = 0;
}

void studio_sha256_hex(const void *data, size_t size, char hex[65]) {
	studio_sha256_t ctx;
	studio_sha256_init(&ctx);
	studio_sha256_update(&ctx, data, size);
	studio_sha256_final(&ctx, hex);
}

int studio_sha256_file(const char *path, char hex[65]) {
	FILE *f = studio_fopen(path, "rb");
	if (f == NULL) {
		return 0;
	}
	studio_sha256_t ctx;
	studio_sha256_init(&ctx);
	uint8_t *buf = malloc(1 << 16);
	size_t   n   = 0;
	int      ok  = buf != NULL;
	while (ok && (n = fread(buf, 1, 1 << 16, f)) > 0) {
		studio_sha256_update(&ctx, buf, n);
	}
	ok = ok && !ferror(f);
	fclose(f);
	free(buf);
	if (ok) {
		studio_sha256_final(&ctx, hex);
	}
	return ok;
}
