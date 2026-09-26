#pragma once

// SHA-256 (FIPS 180-4), pour les sommes de contrôle du manifeste d'export (lot B5).

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint32_t state[8];
	uint64_t length;
	uint8_t  block[64];
	size_t   used;
} studio_sha256_t;

void studio_sha256_init(studio_sha256_t *ctx);
void studio_sha256_update(studio_sha256_t *ctx, const void *data, size_t size);
// Écrit l'empreinte en hexadécimal minuscule (64 caractères et le NUL).
void studio_sha256_final(studio_sha256_t *ctx, char hex[65]);
void studio_sha256_hex(const void *data, size_t size, char hex[65]);
// Empreinte d'un fichier : 0 si illisible.
int studio_sha256_file(const char *path, char hex[65]);
