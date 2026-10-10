/* SPDX-License-Identifier: Apache-2.0 */
/* The part shared by every build: open the state log, serve one connection. The loader and
 * bootstrap builds call this from their accept loop (esp_wifi_loader.c); a host application
 * that links the component in library mode (DMesh's Rust Main) calls it from its own. */
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
static wl_state_t state;
static wl_upload_ctx_t ctx = {.state = &state};
static bool inited;

wl_upload_ctx_t *wl_ctx(void) { return &ctx; }

static bool main_valid(const esp_partition_t *p)
{
    esp_image_header_t h;
    return esp_partition_read(p, 0, &h, sizeof(h)) == ESP_OK &&
           h.magic == ESP_IMAGE_HEADER_MAGIC && h.chip_id == CONFIG_IDF_FIRMWARE_CHIP_ID;
}

esp_err_t esp_wifi_loader_init(void)
{
    if (inited) return ESP_OK;
    ctx.main_part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (!ctx.main_part) return ESP_ERR_NOT_FOUND;
    if (!wl_state_open_flash(&state)) {
        ESP_LOGE(TAG, "state sector unavailable");
        return ESP_FAIL;
    }
    if (!wl_crypto_init()) return ESP_FAIL;
    ctx.main_present = main_valid(ctx.main_part);
    inited = true;
    return ESP_OK;
}

static void restart_with(uint8_t handoff)
{
    WL_RTC_HANDOFF = handoff;
    esp_restart();
}

esp_wifi_loader_result_t esp_wifi_loader_serve(int fd)
{
    if (!inited) return WL_SERVE_NONE;
    wl_upload_result_t r = wl_upload(fd, &ctx);
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
