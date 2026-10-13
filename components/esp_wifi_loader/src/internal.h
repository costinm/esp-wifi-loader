#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_partition.h"
#include "esp_wifi_loader.h"
#include "esp_wifi_loader_state.h"

#define WL_FORMAT 1
#define WL_MAGIC 0x31484c57u /* "WLH1" */
#define WL_BLOCK 4096u
#define WL_TARGET_MAIN 1u   /* ota_0; loader mode */
#define WL_TARGET_LOADER 2u /* factory partition minus the state sectors; bootstrap mode */
#define WL_TARGET_STAGE2 3u /* bootloader region; bootstrap mode */
#define WL_TARGET_MODULE 4u /* a module of the application; library mode, through the module sink */

typedef struct {
    uint32_t magic;
    uint8_t format, cpu, target, flags;
    uint32_t version;
    uint32_t image_size;
    uint16_t block_size, block_count;
    uint8_t image_sha256[32];
    uint8_t table_sha256[32];
    uint8_t fqdn_len, rsv[3];
    char fqdn[128];
} __attribute__((packed)) wl_header_t;
_Static_assert(sizeof(wl_header_t) == WL_HEADER_SIZE, "signed header size");

/* state.c */
bool wl_state_open_flash(wl_state_t *st);

/* net.c */
typedef enum { WL_ROLE_STA, WL_ROLE_AP } wl_role_t;
/* cfg NULL (the loader): the Kconfig defaults, link-local IPv6 only, the station and the rescue access point. Otherwise
 * (library mode, wifi.c): the named network, a station only, state on the heap until wl_net_stop(). */
typedef struct { const char *flash_name, *ssid, *passphrase; bool dhcp4; } wl_net_config_t;
esp_err_t wl_net_init(const wl_net_config_t *cfg);
void wl_net_stop(void);                /* library mode: leave the network and give the driver and the memory back */
bool wl_net_status(char ssid[33], char ip[16], int *rssi);   /* the joined network; true when the link is usable */
bool wl_net_join_flash(void);          /* scan, join the flasher network, wait for link-local */
bool wl_net_host_rescue(void);         /* start ESP-RESCUE as AP, link-local only */
bool wl_net_flash_visible(void);       /* scan while hosting */
bool wl_net_link_up(void);
int wl_net_scan(esp_wifi_loader_ap_t *aps, size_t max);   /* blocking scan: the count, strongest first; negative: the driver refused */
wl_role_t wl_net_role(void);
unsigned wl_net_ifindex(void);
void wl_net_mac(uint8_t mac[6]);

/* upload.c */
typedef struct {
    const esp_partition_t *main_part;
    wl_state_t *state;
    bool main_present;
} wl_upload_ctx_t;
/* Flash area a target is written to, as absolute flash offsets. */
typedef struct { uint32_t base, size; } wl_region_t;
/* False when this build's mode does not accept `target`. */
bool wl_target_region(uint8_t target, wl_region_t *region);

typedef enum {
    WL_UPLOAD_NONE = 0,
    WL_UPLOAD_INSTALLED,       /* an image was written and committed */
    WL_UPLOAD_REQUEST_LOADER,  /* zero-length Main manifest: the RTC handoff is set, restart */
    WL_UPLOAD_REBOOT,          /* plain reboot request (op 255) */
} wl_upload_result_t;

/* Handles one connection. */
wl_upload_result_t wl_upload(int fd, wl_upload_ctx_t *ctx);

/* serve.c: a heap-allocated session (state log view, Main partition) for one operation; NULL on failure.
 * Nothing is resident between sessions. */
wl_upload_ctx_t *wl_ctx_open(void);
/* announce.c: one multicast line; `want` (NULL: none) names what the device asks the flasher for */
void wl_announce(int udp, unsigned iface, const wl_upload_ctx_t *ctx, const char *mode, const char *state, const char *want);
void wl_ctx_close(wl_upload_ctx_t *ctx);
/* esp_wifi_loader_serve() for a caller that already holds a session (the loader's accept loop). */
esp_wifi_loader_result_t wl_serve_ctx(int fd, wl_upload_ctx_t *ctx);
