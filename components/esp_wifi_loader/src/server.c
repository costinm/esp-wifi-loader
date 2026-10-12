/* SPDX-License-Identifier: Apache-2.0 */
/* Library mode: the upload server on request. esp_wifi_loader_server_start(name) listens on CONFIG_WL_PORT (IPv6 and
 * IPv4 on one socket) once the Wi-Fi is up, hands each connection to esp_wifi_loader_serve(), and announces by UDP
 * multicast every few seconds that the device asks for the package `name`. It runs until
 * esp_wifi_loader_server_stop(); nothing stops it by itself. */
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "internal.h"

#define ANNOUNCE_MS 5000

struct server { volatile bool stop; char name[33]; };
static struct server *sv;     /* non-NULL from start until the task has released everything */

static int listen_on(int port)
{
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1, off = 0;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
    struct timeval tv = {.tv_sec = 1};      /* accept() wakes every second to look at the stop flag */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in6 a = {.sin6_family = AF_INET6, .sin6_port = htons(port), .sin6_addr = IN6ADDR_ANY_INIT};
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 1) < 0) { close(fd); return -1; }
    return fd;
}

static void serve_one(int fd)
{
    struct timeval tv = {.tv_sec = 10};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    esp_wifi_loader_serve(fd);
    shutdown(fd, SHUT_WR);
    vTaskDelay(pdMS_TO_TICKS(100)); /* let the reply drain */
    close(fd);
}

static void server_task(void *arg)
{
    struct server *me = arg;
    int ls = -1;
    int64_t next_announce = 0;
    while (!me->stop) {
        if (!wl_net_link_up()) {
            if (ls >= 0) { close(ls); ls = -1; }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (ls < 0 && (ls = listen_on(CONFIG_WL_PORT)) < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        int64_t now = esp_timer_get_time() / 1000;
        if (now >= next_announce) {
            esp_wifi_loader_announce(me->name);
            next_announce = now + ANNOUNCE_MS;
        }
        int fd = accept(ls, NULL, NULL);
        if (fd >= 0) { serve_one(fd); next_announce = 0; }
    }
    if (ls >= 0) close(ls);
    sv = NULL;
    free(me);
    vTaskDelete(NULL);
}

int esp_wifi_loader_server_start(const uint8_t *name, size_t name_len)
{
    if (sv) return -5;
    if (!name || !name_len || name_len >= sizeof(sv->name)) return -1;
    struct server *n = calloc(1, sizeof(*n));
    if (!n) return -6;
    memcpy(n->name, name, name_len);
    sv = n;
    if (xTaskCreate(server_task, "wl_server", 8192, n, 5, NULL) != pdPASS) { sv = NULL; free(n); return -6; }
    return 0;
}

void esp_wifi_loader_server_stop(void) { if (sv) sv->stop = true; }
