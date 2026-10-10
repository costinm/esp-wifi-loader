/* State sector access from the loader app (esp_partition backend). */
#include "esp_partition.h"
#include "internal.h"

static const esp_partition_t *part;
static uint32_t base;

static int rd(void *c, uint32_t off, void *buf, size_t n)
{
    return esp_partition_read(part, base + off, buf, n) == ESP_OK ? 0 : -1;
}
static int wr(void *c, uint32_t off, const void *buf, size_t n)
{
    return esp_partition_write(part, base + off, buf, n) == ESP_OK ? 0 : -1;
}
static int er(void *c, uint32_t off)
{
    return esp_partition_erase_range(part, base + off, WL_STATE_SECTOR) == ESP_OK ? 0 : -1;
}

bool wl_state_open_flash(wl_state_t *st)
{
    static const wl_flash_ops_t ops = {NULL, rd, wr, er};
    part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (!part || part->size < 2 * WL_STATE_SIZE) return false;
    base = part->size - WL_STATE_SIZE;
    return wl_state_open(st, &ops) == 0;
}
