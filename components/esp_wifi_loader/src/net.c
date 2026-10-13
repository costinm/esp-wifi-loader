/* SPDX-License-Identifier: Apache-2.0 */
/* WPA2 flash networks (public constant passphrase), link-local IPv6 only (docs/boot.md "Network").
 *
 *  - Join the flasher network (DIRECT-xy-flash) when a scan sees it.
 *  - Otherwise host the ESP-RESCUE network and keep scanning for
 *    the flasher network between connections.
 * No DHCPv4 client; the ESP-RESCUE AP runs a bare DHCPv4 server (no router/DNS) only so
 * Android accepts the network. No SLAAC/global address, no DNS: the flash networks
 * provide no internet and an untrusted device must not get any. */
#include <stdio.h>
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
#include "nvs.h"
#include "nvs_flash.h"

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
            ESP_LOGW("wl", "disconnected, reason %d", (int)((wifi_event_sta_disconnected_t *)data)->reason);
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
    if (!cfg) nvs_flash_init();     /* the loader: for the optional configured network (a failure means there is none) */
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
    wifi_init_config_t init;
    esp_wifi_loader_wifi_init_config(&init);
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    /* 11b/g/n, 20 MHz, as DMesh does: with 11ax on (ESP32-C6) an access point that is fine with the other chips did not answer */
    wifi_protocols_t proto = {.ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N, .ghz_5g = 0};
    esp_wifi_set_protocols(WIFI_IF_STA, &proto);
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
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

/* The configured network: optional. NVS namespace "wl", keys "ssid" and "psk" (esp_wifi_loader_network_set). When it is set and
 * a scan sees it, it is joined; otherwise the flasher network is, as before. A device is therefore installable with no network
 * of the user's at all (the upload is signed and checked either way), and uses theirs when it has been told one. */
#define SAVED_NS "wl"
static char saved_ssid[33], saved_psk[64];

static void saved_load(void)
{
    saved_ssid[0] = saved_psk[0] = 0;
    if (lib && lib->ssid[0]) return;          /* the caller named a network: that one, exactly */
    nvs_handle_t h;
    if (nvs_open(SAVED_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(saved_ssid);
    if (nvs_get_str(h, "ssid", saved_ssid, &n) != ESP_OK) saved_ssid[0] = 0;
    n = sizeof(saved_psk);
    if (saved_ssid[0] && nvs_get_str(h, "psk", saved_psk, &n) != ESP_OK) saved_psk[0] = 0;
    nvs_close(h);
}

int esp_wifi_loader_network_set(const uint8_t *ssid, size_t ssid_len, const uint8_t *psk, size_t psk_len)
{
    if ((ssid_len && !ssid) || (psk_len && !psk) || ssid_len > 32 || psk_len > 63) return -1;
    char s[33], p[64];
    memcpy(s, ssid, ssid_len); s[ssid_len] = 0;
    memcpy(p, psk, psk_len); p[psk_len] = 0;
    nvs_handle_t h;
    if (nvs_open(SAVED_NS, NVS_READWRITE, &h) != ESP_OK) return -3;
    esp_err_t e = ESP_OK;
    if (!ssid_len) {
        nvs_erase_key(h, "ssid");
        nvs_erase_key(h, "psk");
    } else {
        e = nvs_set_str(h, "ssid", s);
        if (e == ESP_OK) e = psk_len ? nvs_set_str(h, "psk", p) : nvs_erase_key(h, "psk");
        if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    }
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? 0 : -3;
}

int esp_wifi_loader_network_get(char ssid[33], int *has_psk)
{
    saved_load();
    strlcpy(ssid, saved_ssid, 33);
    *has_psk = saved_psk[0] != 0;
    return saved_ssid[0] ? 1 : 0;
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
static bool scan_once(wifi_ap_record_t *best)
{
    /* a longer dwell per channel than the default: with it a strong access point was missed in most passes */
    wifi_scan_config_t scan = {.show_hidden = false, .scan_time = {.active = {.min = 120, .max = 360}}};
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

static bool scan_flash(wifi_ap_record_t *best) { return scan_once(best); }

bool wl_net_flash_visible(void)
{
    wifi_ap_record_t best;
    return scan_flash(&best);
}

/* The one place the Wi-Fi driver's buffer counts are decided, for this library and for an application with its own driver init
 * (DMesh): the build's defaults, then the device's NVS settings (namespace "wl": rx_static, rx_dynamic, tx_dynamic, rx_ba, 16-bit),
 * so a device can trade RAM for throughput without a rebuild. The static RX buffers are the fixed cost (about 1.6 KB each, held while
 * the driver is initialised); the dynamic RX and TX counts are limits on what a burst may take. Values are clamped to what the driver
 * accepts (the block-ack window may not exceed twice the static RX buffers, nor the dynamic ones). Changes apply at the next init. */
static uint16_t wifi_nv_u16(nvs_handle_t h, const char *key, uint16_t def, uint16_t lo, uint16_t hi)
{
    uint16_t v = def;
    if (h && nvs_get_u16(h, key, &v) != ESP_OK) v = def;
    return v < lo ? lo : v > hi ? hi : v;
}

void esp_wifi_loader_wifi_init_config(void *config)
{
    wifi_init_config_t *cfg = config;
    wifi_init_config_t def = WIFI_INIT_CONFIG_DEFAULT();
    *cfg = def;
    nvs_handle_t h = 0;
    if (nvs_open(SAVED_NS, NVS_READONLY, &h) != ESP_OK) h = 0;
    cfg->static_rx_buf_num = wifi_nv_u16(h, "rx_static", def.static_rx_buf_num, 2, 64);
    cfg->dynamic_rx_buf_num = wifi_nv_u16(h, "rx_dynamic", def.dynamic_rx_buf_num, cfg->static_rx_buf_num, 128);
    if (cfg->tx_buf_type == 1) cfg->dynamic_tx_buf_num = wifi_nv_u16(h, "tx_dynamic", def.dynamic_tx_buf_num, 4, 128);
    if (cfg->ampdu_rx_enable) {
        int max_ba = 2 * cfg->static_rx_buf_num;
        if (max_ba >= cfg->dynamic_rx_buf_num) max_ba = cfg->dynamic_rx_buf_num - 1;
        cfg->rx_ba_win = wifi_nv_u16(h, "rx_ba", def.rx_ba_win, 2, 64);
        if (cfg->rx_ba_win > max_ba) cfg->rx_ba_win = max_ba;
    }
    if (h) nvs_close(h);
}

int wl_net_scan(esp_wifi_loader_ap_t *aps, size_t max)
{
    wifi_scan_config_t scan = {.show_hidden = true, .scan_time = {.active = {.min = 120, .max = 360}}};
    uint16_t count = 32;
    wifi_ap_record_t *records = malloc(count * sizeof(*records));
    if (!records) return -6;
    int n = -7;
    if (esp_wifi_scan_start(&scan, true) == ESP_OK && esp_wifi_scan_get_ap_records(&count, records) == ESP_OK) {
        n = count < max ? count : (int)max;
        for (int i = 0; i < n; ++i) {
            size_t len = strnlen((const char *)records[i].ssid, sizeof(records[i].ssid));
            memset(&aps[i], 0, sizeof(aps[i]));
            memcpy(aps[i].ssid, records[i].ssid, len);
            aps[i].ssid_len = (uint8_t)len;
            memcpy(aps[i].bssid, records[i].bssid, 6);
            aps[i].rssi = records[i].rssi;
            aps[i].channel = records[i].primary;
            aps[i].auth = (uint8_t)records[i].authmode;
        }
    }
    free(records);
    return n;
}

/* The configured network. Picked from our own scan and joined by BSSID and channel: some access points do not answer the
 * directed probe the driver sends when it is only given a name (measured: "scan specific ssid" found nothing for an access
 * point that the wildcard scan lists at -41 dBm). A pass can miss a network, so up to three are tried (the first active, the others passive). */
static bool join_configured(void)
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        /* an active scan first, then passive ones: some access points answer probes late or not at all but beacon steadily */
        wifi_scan_config_t scan = {.show_hidden = true, .scan_type = attempt ? WIFI_SCAN_TYPE_PASSIVE : WIFI_SCAN_TYPE_ACTIVE,
                                   .scan_time = {.active = {.min = 120, .max = 360}, .passive = 360}};
        uint16_t count = 24;
        wifi_ap_record_t *records = malloc(count * sizeof(*records)), best = {0};
        bool found = false;
        if (!records) return false;
        if (esp_wifi_scan_start(&scan, true) == ESP_OK && esp_wifi_scan_get_ap_records(&count, records) == ESP_OK) {
            for (uint16_t i = 0; i < count; ++i) {
                if (strncmp((const char *)records[i].ssid, saved_ssid, sizeof(records[i].ssid)) != 0) continue;
                if (!found || records[i].rssi > best.rssi) { best = records[i]; found = true; }
            }
        }
        free(records);
        if (!found) continue;
        wifi_config_t cfg = {0};
        memcpy(cfg.sta.ssid, best.ssid, sizeof(cfg.sta.ssid));
        strlcpy((char *)cfg.sta.password, saved_psk, sizeof(cfg.sta.password));
        /* as DMesh's candidate join does (wifi_esp.rs): the access point's own mode as the floor, PMF capable (required for
         * WPA3), both SAE derivations; DMesh is the reference for what works with real access points */
        cfg.sta.threshold.authmode = best.authmode;
        cfg.sta.pmf_cfg.capable = true;
        cfg.sta.pmf_cfg.required = best.authmode == WIFI_AUTH_WPA3_PSK;
        cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        cfg.sta.channel = best.primary;
        memcpy(cfg.sta.bssid, best.bssid, sizeof(cfg.sta.bssid));
        cfg.sta.bssid_set = true;
        ESP_LOGW("wl", "joining the configured network \"%s\" (channel %u)", saved_ssid, (unsigned)best.primary);
        if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK || esp_wifi_connect() != ESP_OK) return false;
        for (int i = 0; i < 150 && !wl_net_link_up(); ++i) vTaskDelay(pdMS_TO_TICKS(100));
        if (wl_net_link_up()) {
            if (lib) strlcpy(lib->joined, saved_ssid, sizeof(lib->joined));
            return true;
        }
        esp_wifi_disconnect();
        return false;     /* seen but not joined (wrong passphrase?): the flasher network is the fallback */
    }
    ESP_LOGW("wl", "configured network \"%s\" not seen", saved_ssid);
    return false;
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
    saved_load();
    if (saved_ssid[0] && join_configured()) return true;     /* optional: else the flasher network, as when none is set */
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

static const struct { const char *name, *key; uint16_t lo, hi; } nv_keys[] = {
    {"wifi.rx_static", "rx_static", 2, 64}, {"wifi.rx_dynamic", "rx_dynamic", 2, 128},
    {"wifi.tx_dynamic", "tx_dynamic", 4, 128}, {"wifi.rx_ba", "rx_ba", 2, 64},
};

int esp_wifi_loader_nv(const char *key, size_t key_len, const uint8_t *value, size_t value_len, uint8_t *out, size_t cap)
{
    for (size_t i = 0; i < sizeof(nv_keys) / sizeof(nv_keys[0]); ++i) {
        if (strlen(nv_keys[i].name) != key_len || memcmp(nv_keys[i].name, key, key_len)) continue;
        nvs_handle_t h;
        if (nvs_open(SAVED_NS, value ? NVS_READWRITE : NVS_READONLY, &h) != ESP_OK) return value ? -3 : -4;
        int rc;
        if (!value) {
            uint16_t v;
            if (nvs_get_u16(h, nv_keys[i].key, &v) != ESP_OK) rc = -4;
            else rc = snprintf((char *)out, cap, "%u", (unsigned)v) >= (int)cap ? -1 : (int)strlen((char *)out);
        } else if (!value_len) {
            nvs_erase_key(h, nv_keys[i].key);
            rc = nvs_commit(h) == ESP_OK ? 0 : -3;
        } else {
            unsigned v = 0;
            bool ok = value_len <= 4;
            for (size_t k = 0; ok && k < value_len; ++k) { ok = value[k] >= '0' && value[k] <= '9'; v = v * 10 + (value[k] - '0'); }
            if (!ok || v < nv_keys[i].lo || v > nv_keys[i].hi) rc = -1;
            else rc = nvs_set_u16(h, nv_keys[i].key, (uint16_t)v) == ESP_OK && nvs_commit(h) == ESP_OK ? 0 : -3;
        }
        nvs_close(h);
        return rc;
    }
    return -4;
}
