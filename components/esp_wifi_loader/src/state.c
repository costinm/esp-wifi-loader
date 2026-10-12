/* State sector access from the loader app (esp_partition backend). */
#include "esp_partition.h"
#include "internal.h"


#define BASE(p) ((p)->size - WL_STATE_SIZE)

static int rd(void *c, uint32_t off, void *buf, size_t n)
{
    const esp_partition_t *part = c;
    return esp_partition_read(part, BASE(part) + off, buf, n) == ESP_OK ? 0 : -1;
}
static int wr(void *c, uint32_t off, const void *buf, size_t n)
{
    const esp_partition_t *part = c;
    return esp_partition_write(part, BASE(part) + off, buf, n) == ESP_OK ? 0 : -1;
}
static int er(void *c, uint32_t off)
{
    const esp_partition_t *part = c;
    return esp_partition_erase_range(part, BASE(part) + off, WL_STATE_SECTOR) == ESP_OK ? 0 : -1;
}

bool wl_state_open_flash(wl_state_t *st)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (!part || part->size < 2 * WL_STATE_SIZE) return false;
    const wl_flash_ops_t ops = {(void *)part, rd, wr, er};
    return wl_state_open(st, &ops) == 0;
}
