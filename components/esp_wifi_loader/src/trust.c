/* The trust anchor: the loader public key written into the Stage2 region at a fixed flash offset. */
#include "esp_flash.h"
#include "internal.h"

bool wl_trust_key(uint8_t key[WL_PUBKEY_SIZE])
{
    uint8_t slot[WL_TRUST_SIZE];
    if (esp_flash_read(NULL, slot, WL_TRUST_OFFSET, sizeof(slot)) != ESP_OK) return false;
    return wl_trust_parse(slot, key);
}
