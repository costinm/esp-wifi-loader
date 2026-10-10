/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include "psa/crypto.h"
#include "wl_crypto.h"

#if CONFIG_WL_CRYPTO_STUB
/* Size experiment only (docs/size-experiment.md): no crypto, every check fails. */
bool wl_crypto_init(void) { return true; }
bool wl_sha256(const void *d, size_t n, uint8_t o[32]) { (void)d; (void)n; (void)o; return false; }
wl_sha256_ctx_t *wl_sha256_new(void) { return NULL; }
bool wl_sha256_update(wl_sha256_ctx_t *c, const void *d, size_t n) { (void)c; (void)d; (void)n; return false; }
bool wl_sha256_finish(wl_sha256_ctx_t *c, uint8_t o[32]) { (void)c; (void)o; return false; }
void wl_sha256_free(wl_sha256_ctx_t *c) { (void)c; }
bool wl_ecdsa_p256_verify(const uint8_t p[65], const uint8_t s[64], const uint8_t h[32]) { (void)p; (void)s; (void)h; return false; }
#else
struct wl_sha256_ctx { psa_hash_operation_t op; };

bool wl_crypto_init(void) { return psa_crypto_init() == PSA_SUCCESS; }

bool wl_sha256(const void *data, size_t len, uint8_t out[32])
{
    size_t n;
    return psa_hash_compute(PSA_ALG_SHA_256, data, len, out, 32, &n) == PSA_SUCCESS && n == 32;
}

wl_sha256_ctx_t *wl_sha256_new(void)
{
    wl_sha256_ctx_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->op = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&c->op, PSA_ALG_SHA_256) != PSA_SUCCESS) { free(c); return NULL; }
    return c;
}

bool wl_sha256_update(wl_sha256_ctx_t *c, const void *data, size_t len)
{
    return psa_hash_update(&c->op, data, len) == PSA_SUCCESS;
}

bool wl_sha256_finish(wl_sha256_ctx_t *c, uint8_t out[32])
{
    size_t n;
    bool ok = psa_hash_finish(&c->op, out, 32, &n) == PSA_SUCCESS && n == 32;
    free(c);
    return ok;
}

void wl_sha256_free(wl_sha256_ctx_t *c)
{
    if (!c) return;
    psa_hash_abort(&c->op);
    free(c);
}

bool wl_ecdsa_p256_verify(const uint8_t pub[65], const uint8_t sig[64], const uint8_t hash[32])
{
    if (pub[0] != 0x04) return false;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_key_id_t key;
    if (psa_import_key(&attr, pub, 65, &key) != PSA_SUCCESS) return false;
    psa_status_t st = psa_verify_hash(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, 32, sig, 64);
    psa_destroy_key(key);
    return st == PSA_SUCCESS;
}
#endif
