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
/* The trust anchor from the Stage2 region (false: none, pin the first key instead). */
bool wl_trust_key(uint8_t key[WL_PUBKEY_SIZE]);

/* net.c */
typedef enum { WL_ROLE_STA, WL_ROLE_AP } wl_role_t;
esp_err_t wl_net_init(void);
bool wl_net_join_flash(void);          /* scan, join the flasher network, wait for link-local */
bool wl_net_host_rescue(void);         /* start ESP-RESCUE as AP, link-local only */
bool wl_net_flash_visible(void);       /* scan while hosting */
bool wl_net_link_up(void);
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

/* serve.c: the shared context (state log, Main partition). */
wl_upload_ctx_t *wl_ctx(void);
