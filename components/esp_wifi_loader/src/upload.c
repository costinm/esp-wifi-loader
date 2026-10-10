/* SPDX-License-Identifier: Apache-2.0 */
/* TCP upload of a signed Main image. Wire format: docs/protocol.md.
 *
 *   transport header (16) | public key (65) | signature (64) |
 *   signed header (216) | block table (8 * block_count) | image
 *
 * Nothing is erased before the signature, key pin, version, chip, size and
 * block table are checked. The first 4 KiB sector of the image is held back
 * and written last, so an interrupted install leaves Main unbootable and
 * Stage2 falls back to the loader. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include "esp_app_format.h"
#include "esp_flash.h"
#include "esp_timer.h"
#include "wl_crypto.h"
#include "internal.h"

#define SECTOR WL_BLOCK
#define ERASE_BLOCK 65536u
#define TABLE_ENTRY 8u

static uint8_t head[SECTOR], body[SECTOR];

static esp_err_t fl_erase(const wl_region_t *r, uint32_t off, size_t len)
{
    return esp_flash_erase_region(NULL, r->base + off, len);
}
static esp_err_t fl_write(const wl_region_t *r, uint32_t off, const void *buf, size_t len)
{
    return esp_flash_write(NULL, buf, r->base + off, len);
}

bool wl_target_region(uint8_t target, wl_region_t *r)
{
#if CONFIG_WL_MODE_LOADER
    if (target == WL_TARGET_MAIN) {
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                            ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
        if (!p) return false;
        r->base = p->address; r->size = p->size;
        return true;
    }
#else
    if (target == WL_TARGET_LOADER) {
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                            ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
        if (!p || p->size <= WL_STATE_SIZE) return false;
        r->base = p->address; r->size = p->size - WL_STATE_SIZE; /* keep the state log */
        return true;
    }
    if (target == WL_TARGET_STAGE2) {
        r->base = CONFIG_BOOTLOADER_OFFSET_IN_FLASH;
        r->size = 0x8000 - CONFIG_BOOTLOADER_OFFSET_IN_FLASH; /* up to the partition table */
        return true;
    }
#endif
    return false;
}

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static bool send_all(int fd, const char *s)
{
    size_t left = strlen(s);
    while (left) {
        ssize_t n = send(fd, s, left, 0);
        if (n <= 0) return false;
        s += n; left -= (size_t)n;
    }
    return true;
}

static bool read_exact(int fd, void *out, size_t left)
{
    uint8_t *p = out;
    while (left) {
        ssize_t n = recv(fd, p, left, 0);
        if (n <= 0) return false;
        p += n; left -= (size_t)n;
    }
    return true;
}

static bool sha256(const void *data, size_t len, uint8_t out[32]) { return wl_sha256(data, len, out); }

static bool verify_signature(const uint8_t pub[WL_PUBKEY_SIZE], const uint8_t sig[64], const void *header)
{
    uint8_t hash[32];
    return sha256(header, WL_HEADER_SIZE, hash) && wl_ecdsa_p256_verify(pub, sig, hash);
}

wl_upload_result_t wl_upload(int fd, wl_upload_ctx_t *ctx)
{
    uint8_t t[16], pub[WL_PUBKEY_SIZE], sig[64];
    wl_header_t hdr;
    uint8_t *table = NULL;
    char line[160];
    int code = 6;
    const char *reason = "short-read-or-timeout";
    int64_t start = now_ms(), erase_ms = 0, write_ms = 0;
    uint32_t offset = 0;
    bool dry = false;
    wl_upload_result_t result = WL_UPLOAD_NONE;
    wl_sha256_ctx_t *whole = NULL;
    wl_region_t region = {0};

    if (!read_exact(fd, t, sizeof(t))) goto fail;
    code = 1; reason = "bad-header";
    if (memcmp(t, "WLU1", 4) || t[4] != WL_FORMAT) goto fail;
    code = 2; reason = "unsupported-header";
    if ((t[5] & ~2) || t[7] || (t[6] != 0 && t[6] != 255)) goto fail;
    dry = t[5] & 2;
    if (t[6] == 255) { /* reboot request: expects the client FIN */
        code = 6; reason = "expected-fin";
        if (recv(fd, body, 1, 0) != 0) goto fail;
        return send_all(fd, "OK 0\n") ? WL_UPLOAD_REBOOT : WL_UPLOAD_NONE;
    }

    if (!read_exact(fd, pub, sizeof(pub)) || !read_exact(fd, sig, sizeof(sig)) ||
        !read_exact(fd, &hdr, sizeof(hdr))) goto fail;

    /* ---- checks before any erase ---- */
    code = 7; reason = "bad-signature";
    if (hdr.magic != WL_MAGIC || !verify_signature(pub, sig, &hdr)) goto fail;
    const wl_state_view_t *v = &ctx->state->view;
    code = 8; reason = "key-mismatch";
    if (v->has_pin && memcmp(v->pin, pub, sizeof(pub))) goto fail;
    code = 9; reason = "rollback";
    {
        bool has = hdr.target == WL_TARGET_MAIN ? v->has_minver
                 : hdr.target == WL_TARGET_LOADER ? v->has_minver_loader : v->has_minver_stage2;
        uint32_t min = hdr.target == WL_TARGET_MAIN ? v->minver
                     : hdr.target == WL_TARGET_LOADER ? v->minver_loader : v->minver_stage2;
        if (has && hdr.version < min) goto fail;
    }
    /* A signed Main header with no image is a request to reboot into the loader. Only
     * where Main itself is running (bootstrap/library): the loader has nothing to reboot to. */
    bool manifest = false;
#if !CONFIG_WL_MODE_LOADER
    manifest = hdr.image_size == 0 && hdr.target == WL_TARGET_MAIN;
#endif
    code = 10; reason = "wrong-chip-or-target";
    if (hdr.format != WL_FORMAT || hdr.cpu != CONFIG_IDF_FIRMWARE_CHIP_ID ||
        (!manifest && !wl_target_region(hdr.target, &region))) goto fail;
    if (manifest) {
        code = 3; reason = "bad-length";
        if (hdr.block_count != 0) goto fail;
        code = 6; reason = "expected-fin";
        if (recv(fd, body, 1, 0) != 0) goto fail;
        snprintf(line, sizeof(line), "OK 0 request-loader%s\n", dry ? " dry" : "");
        printf("wl: %s", line);
        if (send_all(fd, line) && !dry) {
            WL_RTC_HANDOFF = WL_HANDOFF_LOADER; /* the caller restarts after the reply drained */
            result = WL_UPLOAD_REQUEST_LOADER;
        }
        goto cleanup;
    }
    code = 3; reason = "bad-length";
    if (hdr.image_size < SECTOR || hdr.image_size > region.size ||
        hdr.block_size != WL_BLOCK ||
        hdr.block_count != (hdr.image_size + WL_BLOCK - 1) / WL_BLOCK ||
        hdr.fqdn_len > sizeof(hdr.fqdn)) goto fail;

    size_t table_len = (size_t)hdr.block_count * TABLE_ENTRY;
    table = malloc(table_len);
    code = 11; reason = "no-memory";
    if (!table) goto fail;
    code = 6; reason = "short-read-or-timeout";
    if (!read_exact(fd, table, table_len)) goto fail;
    uint8_t hash[32];
    code = 12; reason = "bad-block-table";
    if (!sha256(table, table_len, hash) || memcmp(hash, hdr.table_sha256, 32)) goto fail;

    /* ---- stream the image ---- */
    code = 6; reason = "short-read-or-timeout";
    if (!read_exact(fd, head, SECTOR)) goto fail;
    const esp_image_header_t *image = (const esp_image_header_t *)head;
    code = 4; reason = "bad-image-header";
    if (image->magic != ESP_IMAGE_HEADER_MAGIC || image->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) goto fail;
    code = 13; reason = "bad-block";
    if (!sha256(head, SECTOR, hash) || memcmp(hash, table, TABLE_ENTRY)) goto fail;
    code = 11; reason = "hash-init";
    whole = wl_sha256_new();
    if (!whole || !wl_sha256_update(whole, head, SECTOR)) goto fail;

    code = 5; reason = "flash-erase";
    if (!dry) {
        int64_t tm = now_ms();
        /* At most one block, and never past the image: the Stage2 region is
         * only 28 KiB and ends at the partition table. */
        size_t first = hdr.image_size < ERASE_BLOCK
                           ? (hdr.image_size + SECTOR - 1) / SECTOR * SECTOR : ERASE_BLOCK;
        if (fl_erase(&region, 0, first) != ESP_OK) goto fail;
        erase_ms += now_ms() - tm;
        if (hdr.target == WL_TARGET_MAIN) ctx->main_present = false;
    }
    offset = SECTOR;
    while (offset < hdr.image_size) {
        size_t n = hdr.image_size - offset < SECTOR ? hdr.image_size - offset : SECTOR;
        code = 6; reason = "short-read-or-timeout";
        if (!read_exact(fd, body, n)) goto fail;
        /* A short final block is hashed over its real length; the table entry
         * covers the same bytes. */
        code = 13; reason = "bad-block";
        if (!sha256(body, n, hash) ||
            memcmp(hash, table + (size_t)(offset / SECTOR) * TABLE_ENTRY, TABLE_ENTRY)) goto fail;
        if (!wl_sha256_update(whole, body, n)) goto fail;
        if (!dry) {
            if (offset % ERASE_BLOCK == 0) {
                size_t extent = hdr.image_size - offset < ERASE_BLOCK
                                    ? (hdr.image_size - offset + SECTOR - 1) / SECTOR * SECTOR
                                    : ERASE_BLOCK;
                int64_t tm = now_ms();
                code = 5; reason = "flash-erase";
                if (fl_erase(&region, offset, extent) != ESP_OK) goto fail;
                erase_ms += now_ms() - tm;
            }
            int64_t tm = now_ms();
            code = 5; reason = "flash-write";
            if (fl_write(&region, offset, body, n) != ESP_OK) goto fail;
            write_ms += now_ms() - tm;
        }
        offset += n;
        if (offset % ERASE_BLOCK == 0 || offset == hdr.image_size) {
            snprintf(line, sizeof(line), "P %lu %lu\n", (unsigned long)offset,
                     (unsigned long)(dry ? 0 : offset - SECTOR));
            if (!send_all(fd, line)) goto cleanup;
        }
    }
    code = 14; reason = "bad-image-hash";
    bool finished = wl_sha256_finish(whole, hash);
    whole = NULL; /* finish frees it */
    if (!finished) goto fail;
    if (memcmp(hash, hdr.image_sha256, 32)) goto fail;

    code = 6; reason = "expected-fin";
    if (recv(fd, body, 1, 0) != 0) goto fail;

    if (!dry) {
        /* Commit: head sector last, then the state records. */
        int64_t tm = now_ms();
        code = 5; reason = "head-commit";
        if (fl_write(&region, 0, head, SECTOR) != ESP_OK) goto fail;
        write_ms += now_ms() - tm;
        bool first = !ctx->state->view.has_pin;
        uint32_t ver = hdr.version;
        if (first) wl_state_append(ctx->state, WL_REC_PIN, pub, sizeof(pub));
        if (hdr.target == WL_TARGET_MAIN) {
            ctx->main_present = true;
            wl_state_append(ctx->state, WL_REC_MINVER, &ver, sizeof(ver));
            wl_state_append(ctx->state, WL_REC_HEADER, &hdr, sizeof(hdr));
        } else {
            wl_state_append(ctx->state, hdr.target == WL_TARGET_LOADER ? WL_REC_MINVER_LOADER
                                                                       : WL_REC_MINVER_STAGE2,
                            &ver, sizeof(ver));
        }
    }
    snprintf(line, sizeof(line), "OK %lu ms=%lld erase_ms=%lld write_ms=%lld%s\n",
             (unsigned long)hdr.image_size, (long long)(now_ms() - start), (long long)erase_ms,
             (long long)write_ms, dry ? " dry" : "");
    printf("wl: %s", line);
    if (send_all(fd, line) && !dry) result = WL_UPLOAD_INSTALLED;
    goto cleanup;
fail:
    snprintf(line, sizeof(line), "ERR %d %s\n", code, reason);
    printf("wl: %s", line);
    send_all(fd, line);
cleanup:
    wl_sha256_free(whole);
    free(table);
    return result;
}
