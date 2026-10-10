#pragma once
/* The only crypto the loader itself needs: SHA-256 and ECDSA P-256 verify.
 * Isolated so the backend (PSA today) can be swapped for a small verifier. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool wl_crypto_init(void);
bool wl_sha256(const void *data, size_t len, uint8_t out[32]);

typedef struct wl_sha256_ctx wl_sha256_ctx_t;
wl_sha256_ctx_t *wl_sha256_new(void);
bool wl_sha256_update(wl_sha256_ctx_t *c, const void *data, size_t len);
bool wl_sha256_finish(wl_sha256_ctx_t *c, uint8_t out[32]); /* frees c */
void wl_sha256_free(wl_sha256_ctx_t *c);

/* pub: uncompressed point (0x04 || X || Y); sig: r || s; hash: SHA-256 digest. */
bool wl_ecdsa_p256_verify(const uint8_t pub[65], const uint8_t sig[64], const uint8_t hash[32]);
