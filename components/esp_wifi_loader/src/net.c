/* SPDX-License-Identifier: Apache-2.0 */
/* WPA2 flash networks (public constant passphrase), link-local IPv6 only (docs/boot.md "Network").
 *
 *  - Join the flasher network (DIRECT-xy-flash) when a scan sees it.
 *  - Otherwise host the ESP-RESCUE network and keep scanning for
 *    the flasher network between connections.
 * No DHCPv4 client; the ESP-RESCUE AP runs a bare DHCPv4 server (no router/DNS) only so
 * Android accepts the network. No SLAAC/global address, no DNS: the flash networks
 * provide no internet and an untrusted device must not get any. */
#include <string.h>
#include <strings.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "internal.h"

_Static_assert(sizeof(CONFIG_WL_PASSPHRASE) >= 9 && sizeof(CONFIG_WL_PASSPHRASE) <= 64,
               "CONFIG_WL_PASSPHRASE must be 8..63 characters");

static esp_netif_t *sta_netif, *ap_netif;
static volatile bool sta_connected, ip6_ready;
static volatile wl_role_t role = WL_ROLE_STA;
static bool ap_active;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_CONNECTED:
            sta_connected = true;
            esp_netif_create_ip6_linklocal(sta_netif);
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            sta_connected = false;
            if (role == WL_ROLE_STA) ip6_ready = false;
            break;
        case WIFI_EVENT_AP_START:
            /* Android only reports a joined network once it has an IP configuration, and
             * a link-local-only network never gets one. Offer a bare DHCPv4 lease (no
             * router, no DNS) so it does; the protocol itself stays on IPv6 link-local. */
#if CONFIG_LWIP_DHCPS
            {
                uint8_t no_router = 0;
                esp_netif_dhcps_stop(ap_netif);
                esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS,
                                       &no_router, sizeof(no_router));
                esp_netif_dhcps_start(ap_netif);
            }
#endif
            esp_netif_create_ip6_linklocal(ap_netif);
            break;
        case WIFI_EVENT_AP_STOP:
            if (role == WL_ROLE_AP) ip6_ready = false;
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_GOT_IP6) {
        ip_event_got_ip6_t *e = data;
        if ((e->esp_netif == sta_netif && role == WL_ROLE_STA) ||
            (e->esp_netif == ap_netif && role == WL_ROLE_AP)) {
            ip6_ready = true;
        }
    }
}

esp_err_t wl_net_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_dhcpc_stop(sta_netif); /* no DHCPv4 request when joining */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, on_event, NULL));
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    return ESP_OK;
}

/* The flasher network is named DIRECT-xy-<CONFIG_WL_FLASH_NAME>: xy is any two characters (Android
 * chooses them for a Wi-Fi Direct group). */
static bool is_flash_ssid(const char *name)
{
    static const char prefix[] = "DIRECT-";
    const size_t p = sizeof(prefix) - 1, n = strlen(CONFIG_WL_FLASH_NAME);
    return strlen(name) == p + 2 + 1 + n && strncasecmp(name, prefix, p) == 0 && name[p + 2] == '-' &&
           strncasecmp(name + p + 3, CONFIG_WL_FLASH_NAME, n) == 0;
}

/* Blocking scan for the flasher network; fills the best record. */
static bool scan_flash(wifi_ap_record_t *best)
{
    static wifi_ap_record_t records[24];
    wifi_scan_config_t scan = {.show_hidden = false};
    uint16_t count = sizeof(records) / sizeof(records[0]);
    if (esp_wifi_scan_start(&scan, true) != ESP_OK ||
        esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) return false;
    bool found = false;
    for (uint16_t i = 0; i < count; ++i) {
        char name[sizeof(records[i].ssid) + 1];
        memcpy(name, records[i].ssid, sizeof(records[i].ssid));
        name[sizeof(records[i].ssid)] = 0;
        if (!is_flash_ssid(name)) continue;
        if (!found || records[i].rssi > best->rssi) { *best = records[i]; found = true; }
    }
    ESP_LOGW("wl", "scan: %u networks, flash network %s", (unsigned)count, found ? "found" : "not seen");
    return found;
}

bool wl_net_flash_visible(void)
{
    wifi_ap_record_t best;
    return scan_flash(&best);
}

bool wl_net_join_flash(void)
{
    wifi_ap_record_t best;
    role = WL_ROLE_STA;
    ip6_ready = false;
    if (ap_active) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        ap_active = false;
    }
    if (!scan_flash(&best)) return false;
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, best.ssid, sizeof(cfg.sta.ssid)); /* whichever name was found */
    strlcpy((char *)cfg.sta.password, CONFIG_WL_PASSPHRASE, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.sta.channel = best.primary;
    memcpy(cfg.sta.bssid, best.bssid, sizeof(cfg.sta.bssid));
    cfg.sta.bssid_set = true;
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK || esp_wifi_connect() != ESP_OK) return false;
    for (int i = 0; i < 150 && !ip6_ready; ++i) vTaskDelay(pdMS_TO_TICKS(100));
    if (!ip6_ready) esp_wifi_disconnect();
    return ip6_ready;
}

bool wl_net_host_rescue(void)
{
    esp_wifi_disconnect();
    role = WL_ROLE_AP;
    ip6_ready = false;
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.ap.ssid, CONFIG_WL_RESCUE_SSID, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = strlen(CONFIG_WL_RESCUE_SSID);
    cfg.ap.channel = CONFIG_WL_RESCUE_CHANNEL;
    strlcpy((char *)cfg.ap.password, CONFIG_WL_PASSPHRASE, sizeof(cfg.ap.password));
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.ap.max_connection = 4;
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_AP, &cfg) != ESP_OK) return false;
    ap_active = true;
    for (int i = 0; i < 50 && !ip6_ready; ++i) vTaskDelay(pdMS_TO_TICKS(100));
    return ip6_ready;
}

bool wl_net_link_up(void) { return ip6_ready; }
wl_role_t wl_net_role(void) { return role; }

unsigned wl_net_ifindex(void)
{
    return esp_netif_get_netif_impl_index(role == WL_ROLE_AP ? ap_netif : sta_netif);
}

void wl_net_mac(uint8_t mac[6])
{
    esp_wifi_get_mac(WIFI_IF_STA, mac);
}
