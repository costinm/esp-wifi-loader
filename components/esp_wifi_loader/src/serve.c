/* SPDX-License-Identifier: Apache-2.0 */
/* The part shared by every build: open the state log, serve one connection. The loader and
 * bootstrap builds call this from their accept loop (esp_wifi_loader.c); a host application
 * that links the component in library mode (DMesh's Rust Main) calls it from its own. */
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "internal.h"
#include "wl_crypto.h"

static const char *TAG = "wl";

/* The loader is an auxiliary for upgrades and development, not a function of the device: in library
 * mode nothing of it stays in RAM between uploads. A session (state log view and Main status) is
 * heap-allocated for one operation and freed after it; the state log is read from flash each time. */
typedef struct {
    wl_upload_ctx_t ctx; /* first: wl_ctx_close() frees the session through it */
    wl_state_t state;
} wl_session_t;

static bool main_valid(const esp_partition_t *p)
{
    esp_image_header_t h;
    return esp_partition_read(p, 0, &h, sizeof(h)) == ESP_OK &&
           h.magic == ESP_IMAGE_HEADER_MAGIC && h.chip_id == CONFIG_IDF_FIRMWARE_CHIP_ID;
}

wl_upload_ctx_t *wl_ctx_open(void)
{
    wl_session_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->ctx.state = &s->state;
    s->ctx.main_part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (!s->ctx.main_part || !wl_state_open_flash(&s->state)) {
        ESP_LOGE(TAG, "state sector unavailable");
        free(s);
        return NULL;
    }
    s->ctx.main_present = main_valid(s->ctx.main_part);
    return &s->ctx;
}

void wl_ctx_close(wl_upload_ctx_t *ctx) { free(ctx); }

/* Nothing is kept: the state is opened when an upload or a version query needs it. */
esp_err_t esp_wifi_loader_init(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL)
               ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool esp_wifi_loader_module_minver(unsigned slot, uint32_t *version)
{
    if (slot >= WL_MODULE_SLOTS) return false;
    wl_upload_ctx_t *ctx = wl_ctx_open();
    if (!ctx) return false;
    bool found = ctx->state->view.has_module & (1u << slot);
    if (found) *version = ctx->state->view.minver_module[slot];
    wl_ctx_close(ctx);
    return found;
}

int esp_wifi_loader_set_module_minver(unsigned slot, uint32_t version)
{
    if (slot >= WL_MODULE_SLOTS) return -1;
    wl_upload_ctx_t *ctx = wl_ctx_open();
    if (!ctx) return -1;
    uint8_t rec[5] = {(uint8_t)slot};
    memcpy(rec + 1, &version, 4);
    int rc = wl_state_append(ctx->state, WL_REC_MINVER_MODULE, rec, sizeof(rec));
    wl_ctx_close(ctx);
    return rc;
}

static void restart_with(uint8_t handoff)
{
    WL_RTC_HANDOFF = handoff;
    esp_restart();
}

esp_wifi_loader_result_t wl_serve_ctx(int fd, wl_upload_ctx_t *ctx)
{
    if (!wl_crypto_init()) return WL_SERVE_NONE; /* idempotent */
    wl_upload_result_t r = wl_upload(fd, ctx);
    if (r == WL_UPLOAD_NONE) return WL_SERVE_NONE;
    /* Let the status line drain before the connection (or the chip) goes away. */
    shutdown(fd, SHUT_WR);
    vTaskDelay(pdMS_TO_TICKS(200));
    if (r == WL_UPLOAD_REQUEST_LOADER) restart_with(WL_HANDOFF_LOADER); /* upgrade through the loader */
    if (r == WL_UPLOAD_REBOOT) restart_with(WL_HANDOFF_MAIN);
#if CONFIG_WL_MODE_LIBRARY
    return WL_SERVE_INSTALLED; /* Main keeps running; the new loader/Stage2 is used at next boot */
#else
    restart_with(WL_HANDOFF_MAIN);
    return WL_SERVE_INSTALLED;
#endif
}

esp_wifi_loader_result_t esp_wifi_loader_serve(int fd)
{
    wl_upload_ctx_t *ctx = wl_ctx_open();
    if (!ctx) return WL_SERVE_NONE;
    esp_wifi_loader_result_t r = wl_serve_ctx(fd, ctx);
    wl_ctx_close(ctx);
    return r;
}
