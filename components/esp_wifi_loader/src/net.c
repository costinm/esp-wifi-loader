/* SPDX-License-Identifier: Apache-2.0 */
/* WPA2 flash networks (public constant passphrase), link-local IPv6 only (docs/boot.md "Network").
 *
 *  - Join the flasher network (DIRECT-xy-flash) when a scan sees it.
 *  - Otherwise host the ESP-RESCUE network and keep scanning for
 *    the flasher network between connections.
 * No DHCPv4 client; the ESP-RESCUE AP runs a bare DHCPv4 server (no router/DNS) only so
 * Android accepts the network. No SLAAC/global address, no DNS: the flash networks
 * provide no internet and an untrusted device must not get any. */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi_loader.h"
#include "internal.h"

_Static_assert(sizeof(CONFIG_WL_PASSPHRASE) >= 9 && sizeof(CONFIG_WL_PASSPHRASE) <= 64,
               "CONFIG_WL_PASSPHRASE must be 8..63 characters");

static esp_netif_t *sta_netif, *ap_netif;
static volatile bool sta_connected, ip6_ready, ip4_ready;
/* Library mode only: what the caller named and what was joined, on the heap from wl_net_init() to wl_net_stop(). The
 * loader (cfg NULL) has none of it and uses the Kconfig defaults. */
static struct net_lib { char flash_name[33], ssid[33], pass[64], joined[33], ip4[16]; bool dhcp4; } *lib;
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
            ip4_ready = false;
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
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        if (lib) esp_ip4addr_ntoa(&e->ip_info.ip, lib->ip4, sizeof(lib->ip4));
        ip4_ready = lib != NULL;
    } else if (base == IP_EVENT && id == IP_EVENT_GOT_IP6) {
        ip_event_got_ip6_t *e = data;
        if ((e->esp_netif == sta_netif && role == WL_ROLE_STA) ||
            (e->esp_netif == ap_netif && role == WL_ROLE_AP)) {
            ip6_ready = true;
        }
    }
}

esp_err_t wl_net_init(const wl_net_config_t *cfg)
{
    if (cfg) {
        lib = calloc(1, sizeof(*lib));
        if (!lib) return ESP_ERR_NO_MEM;
        strlcpy(lib->flash_name, cfg->flash_name ? cfg->flash_name : CONFIG_WL_FLASH_NAME, sizeof(lib->flash_name));
        strlcpy(lib->ssid, cfg->ssid ? cfg->ssid : "", sizeof(lib->ssid));
        strlcpy(lib->pass, cfg->passphrase ? cfg->passphrase : CONFIG_WL_PASSPHRASE, sizeof(lib->pass));
        lib->dhcp4 = cfg->dhcp4;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;     /* another user of the loop is fine */
    sta_netif = esp_netif_create_default_wifi_sta();
    if (!cfg) ap_netif = esp_netif_create_default_wifi_ap();
    if (lib && lib->dhcp4) esp_netif_dhcpc_start(sta_netif);
    else esp_netif_dhcpc_stop(sta_netif); /* no DHCPv4 request when joining */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    return ESP_OK;
}

/* Library mode: everything wl_net_init() took, back (the driver is the big one), so a device that is mostly off holds nothing. */
void wl_net_stop(void)
{
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_GOT_IP6, on_event);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event);
    if (sta_netif) esp_netif_destroy_default_wifi(sta_netif);
    if (ap_netif) esp_netif_destroy_default_wifi(ap_netif);
    sta_netif = ap_netif = NULL;
    sta_connected = ip4_ready = ip6_ready = false;
    free(lib);
    lib = NULL;
}

/* The flasher network is named DIRECT-xy-<flash_name>: xy is any two characters (Android chooses them for a
 * Wi-Fi Direct group). A configured ssid is matched exactly instead. */
static bool is_flash_ssid(const char *name)
{
    if (lib && lib->ssid[0]) return strcmp(name, lib->ssid) == 0;
    static const char prefix[] = "DIRECT-";
    const char *flash_name = lib ? lib->flash_name : CONFIG_WL_FLASH_NAME;
    const size_t p = sizeof(prefix) - 1, n = strlen(flash_name);
    return strlen(name) == p + 2 + 1 + n && strncasecmp(name, prefix, p) == 0 && name[p + 2] == '-' &&
           strncasecmp(name + p + 3, flash_name, n) == 0;
}

/* Blocking scan for the flasher network; fills the best record. */
static bool scan_flash(wifi_ap_record_t *best)
{
    wifi_scan_config_t scan = {.show_hidden = false};
    uint16_t count = 24;
    wifi_ap_record_t *records = malloc(count * sizeof(*records)); /* heap, only during a scan */
    if (!records) return false;
    if (esp_wifi_scan_start(&scan, true) != ESP_OK ||
        esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) { free(records); return false; }
    bool found = false;
    for (uint16_t i = 0; i < count; ++i) {
        char name[sizeof(records[i].ssid) + 1];
        memcpy(name, records[i].ssid, sizeof(records[i].ssid));
        name[sizeof(records[i].ssid)] = 0;
        if (!is_flash_ssid(name)) continue;
        if (!found || records[i].rssi > best->rssi) { *best = records[i]; found = true; }
    }
    free(records);
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
    strlcpy((char *)cfg.sta.password, lib ? lib->pass : CONFIG_WL_PASSPHRASE, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.sta.channel = best.primary;
    memcpy(cfg.sta.bssid, best.bssid, sizeof(cfg.sta.bssid));
    cfg.sta.bssid_set = true;
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK || esp_wifi_connect() != ESP_OK) return false;
    if (lib) memcpy(lib->joined, best.ssid, sizeof(lib->joined) - 1);
    for (int i = 0; i < 150 && !wl_net_link_up(); ++i) vTaskDelay(pdMS_TO_TICKS(100));
    if (!wl_net_link_up()) esp_wifi_disconnect();
    return wl_net_link_up();
}

bool wl_net_host_rescue(void)
{
    esp_wifi_disconnect();
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
    role = WL_ROLE_AP;
    ap_active = true;
    for (int i = 0; i < 50 && !ip6_ready; ++i) vTaskDelay(pdMS_TO_TICKS(100));
    return ip6_ready;
}

bool wl_net_link_up(void) { return lib && lib->dhcp4 ? ip4_ready : ip6_ready; }

bool wl_net_status(char ssid[33], char ip[16], int *rssi)
{
    wifi_ap_record_t ap;
    bool up = wl_net_link_up();
    *rssi = (up && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
    strlcpy(ssid, lib ? lib->joined : "", 33);
    strlcpy(ip, lib && ip4_ready ? lib->ip4 : "", 16);
    return up;
}

wl_role_t wl_net_role(void) { return role; }

unsigned wl_net_ifindex(void)
{
    return esp_netif_get_netif_impl_index(role == WL_ROLE_AP ? ap_netif : sta_netif);
}

void wl_net_mac(uint8_t mac[6])
{
    /* The address of the interface the announce is sent on, which the flasher sees. */
    esp_wifi_get_mac(role == WL_ROLE_AP ? WIFI_IF_AP : WIFI_IF_STA, mac);
}
