/* SPDX-License-Identifier: Apache-2.0 */
/* Library mode: Wi-Fi on request. esp_wifi_loader_wifi_start() joins the network (the flash network, or the one the
 * caller names) in a task that rejoins when the link drops, until esp_wifi_loader_wifi_stop(); then the radio, the driver
 * and this file's memory are released. A device that is mostly off holds nothing: no statics but one pointer, the rest is
 * on the heap while it runs. The caller passes the name and passphrase each time; nothing is stored. */
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "internal.h"

enum { WIFI_OFF, WIFI_JOINING, WIFI_UP };
struct wifi { volatile int state; volatile bool stop; char ssid[33], psk[64]; };
static struct wifi *w;      /* non-NULL from start until the task has released everything */

static void wifi_task(void *arg)
{
    struct wifi *me = arg;
    const wl_net_config_t cfg = {.ssid = me->ssid[0] ? me->ssid : NULL, .passphrase = me->psk[0] ? me->psk : NULL, .dhcp4 = true};
    if (wl_net_init(&cfg) == ESP_OK) {
        while (!me->stop) {
            if (!wl_net_link_up()) {
                me->state = WIFI_JOINING;
                if (!wl_net_join_flash()) { vTaskDelay(pdMS_TO_TICKS(3000)); continue; }
            }
            me->state = WIFI_UP;
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        wl_net_stop();
    }
    w = NULL;
    free(me);
    vTaskDelete(NULL);
}

int esp_wifi_loader_wifi_start(const uint8_t *ssid, size_t ssid_len, const uint8_t *psk, size_t psk_len)
{
    if (w) return -5;     /* busy: also while a stop is still finishing */
    if ((ssid_len && !ssid) || (psk_len && !psk) || ssid_len >= sizeof(w->ssid) || psk_len >= sizeof(w->psk)) return -1;
    struct wifi *n = calloc(1, sizeof(*n));
    if (!n) return -6;
    if (ssid_len) memcpy(n->ssid, ssid, ssid_len);
    if (psk_len) memcpy(n->psk, psk, psk_len);
    n->state = WIFI_JOINING;
    w = n;
    if (xTaskCreate(wifi_task, "wl_wifi", 4096, n, 5, NULL) != pdPASS) { w = NULL; free(n); return -6; }
    return 0;
}

void esp_wifi_loader_wifi_stop(void) { if (w) w->stop = true; }

int esp_wifi_loader_wifi_status(int *state, char ssid[33], char ip[16], int *rssi)
{
    ssid[0] = ip[0] = 0;
    *rssi = 0;
    struct wifi *s = w;
    *state = s ? s->state : WIFI_OFF;
    if (*state == WIFI_UP) wl_net_status(ssid, ip, rssi);
    return 0;
}
