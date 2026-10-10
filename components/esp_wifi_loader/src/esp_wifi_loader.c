/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "wl_crypto.h"
#include "internal.h"

#define WL_VERSION 1
#if CONFIG_WL_MODE_BOOTSTRAP
#define ANNOUNCE_BOOTSTRAP 1
#else
#define ANNOUNCE_BOOTSTRAP 0
#endif
#define ANNOUNCE_PERIOD_MS 5000

static const char *TAG = "wl";

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

#if CONFIG_WL_MODE_BOOTSTRAP
#define MODE_NAME "bootstrap"
#else
#define MODE_NAME "loader"
#endif

static void restart_main(void)
{
    WL_RTC_HANDOFF = WL_HANDOFF_MAIN;
    esp_restart();
}

/* The bootstrap build is Main itself: it never leaves on an idle timeout. */
static bool idle_exit_allowed(const wl_upload_ctx_t *ctx)
{
#if CONFIG_WL_MODE_BOOTSTRAP
    (void)ctx;
    return false;
#else
    return ctx->main_present;
#endif
}

static void announce(int udp, unsigned iface, const wl_upload_ctx_t *ctx, bool blank)
{
    struct sockaddr_in6 to = {.sin6_family = AF_INET6, .sin6_port = htons(CONFIG_WL_PORT),
                              .sin6_scope_id = iface};
    inet_pton(AF_INET6, CONFIG_WL_ANNOUNCE_GROUP, &to.sin6_addr);
    setsockopt(udp, IPPROTO_IPV6, IPV6_MULTICAST_IF, &iface, sizeof(iface));

    const wl_state_view_t *v = &ctx->state->view;
    const char *state = blank ? "blank" : (ANNOUNCE_BOOTSTRAP || WL_RTC_REASON == WL_REASON_UPDATE) ? "update" : "crash";
    uint32_t version = 0;
    char key[9] = "none", fqdn[129] = "";
    if (v->has_header) {
        const wl_header_t *h = (const wl_header_t *)v->header;
        version = h->version;
        size_t n = h->fqdn_len < sizeof(h->fqdn) ? h->fqdn_len : sizeof(h->fqdn);
        memcpy(fqdn, h->fqdn, n);
        fqdn[n] = 0;
    }
    if (v->has_pin) {
        uint8_t hash[32];
        if (wl_sha256(v->pin, sizeof(v->pin), hash)) {
            snprintf(key, sizeof(key), "%02x%02x%02x%02x", hash[0], hash[1], hash[2], hash[3]);
        }
    }
    uint8_t mac[6];
    wl_net_mac(mac);
    char line[400];
    int n = snprintf(line, sizeof(line),
                     "esp-wifi-loader v=1 mode=" MODE_NAME " state=%s version=%lu key=%s fqdn=%s chip=%s "
                     "mac=%02x:%02x:%02x:%02x:%02x:%02x port=%d role=%s loader=%d up=%lld\n",
                     state, (unsigned long)version, key, fqdn, CONFIG_IDF_TARGET,
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], CONFIG_WL_PORT,
                     wl_net_role() == WL_ROLE_AP ? "ap" : "sta", WL_VERSION,
                     (long long)(now_ms() / 1000));
    sendto(udp, line, n, 0, (struct sockaddr *)&to, sizeof(to));
}

esp_err_t esp_wifi_loader_run(void)
{
    ESP_ERROR_CHECK(esp_wifi_loader_init());
    wl_upload_ctx_t *ctx = wl_ctx();
    ESP_LOGW(TAG, MODE_NAME " v%d main=%s reason=%u ", WL_VERSION, ctx->main_present ? "valid" : "blank",
             (unsigned)WL_RTC_REASON);

    ESP_ERROR_CHECK(wl_net_init());

    int listener = socket(AF_INET6, SOCK_STREAM, 0), udp = socket(AF_INET6, SOCK_DGRAM, 0);
    if (listener < 0 || udp < 0) return ESP_FAIL;
    int on = 1;
    setsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on)); /* IPv6 only */
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(udp, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &on, sizeof(on));
    struct sockaddr_in6 any = {.sin6_family = AF_INET6, .sin6_port = htons(CONFIG_WL_PORT)};
    if (bind(listener, (struct sockaddr *)&any, sizeof(any)) || listen(listener, 1)) return ESP_FAIL;

    int64_t idle = now_ms() + CONFIG_WL_IDLE_TIMEOUT_S * 1000LL;
    for (;;) {
        /* Join ESP-FLASH if it is there, otherwise host ESP-RESCUE. */
        bool linked = wl_net_join_flash() || wl_net_host_rescue();
        if (!linked) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (idle_exit_allowed(ctx) && now_ms() > idle) restart_main();
            continue;
        }
        int64_t next_announce = 0, next_scan = now_ms() + CONFIG_WL_SCAN_INTERVAL_S * 1000LL;
        while (wl_net_link_up()) {
            if (now_ms() >= next_announce) {
                announce(udp, wl_net_ifindex(), ctx, !ANNOUNCE_BOOTSTRAP && !ctx->main_present);
                next_announce = now_ms() + ANNOUNCE_PERIOD_MS;
            }
            /* While hosting, look for ESP-FLASH between connections. */
            if (wl_net_role() == WL_ROLE_AP && now_ms() >= next_scan) {
                if (wl_net_flash_visible()) break; /* re-enter: join it */
                next_scan = now_ms() + CONFIG_WL_SCAN_INTERVAL_S * 1000LL;
            }
            if (idle_exit_allowed(ctx) && now_ms() > idle) restart_main();

            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(listener, &rd);
            struct timeval wait = {.tv_sec = 1};
            if (select(listener + 1, &rd, NULL, NULL, &wait) <= 0) continue;
            int fd = accept(listener, NULL, NULL);
            if (fd < 0) continue;
            struct timeval timeout = {.tv_sec = 10};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            /* A finished install or a loader request restarts the chip from inside. */
            esp_wifi_loader_serve(fd);
            close(fd);
            /* No commit: keep serving; the scan timer resumes as before. */
            idle = now_ms() + CONFIG_WL_IDLE_TIMEOUT_S * 1000LL;
            next_announce = 0;
        }
        ESP_LOGW(TAG, "link lost or ESP-FLASH appeared; reselecting network");
    }
}
