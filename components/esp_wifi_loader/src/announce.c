/* SPDX-License-Identifier: Apache-2.0 */
/* The announce line (docs/protocol.md "Announce"): one UDP multicast text line saying who this device is and what it wants.
 * The loader sends it from its accept loop; an application in library mode sends it with esp_wifi_loader_announce(). */
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "esp_timer.h"
#include "lwip/inet.h"
#include "wl_crypto.h"
#include "internal.h"

void wl_announce(int udp, unsigned iface, const wl_upload_ctx_t *ctx, const char *mode, const char *state, const char *want)
{
    struct sockaddr_in6 to = {.sin6_family = AF_INET6, .sin6_port = htons(CONFIG_WL_PORT),
                              .sin6_scope_id = iface};
    inet_pton(AF_INET6, CONFIG_WL_ANNOUNCE_GROUP, &to.sin6_addr);
    setsockopt(udp, IPPROTO_IPV6, IPV6_MULTICAST_IF, &iface, sizeof(iface));

    const wl_state_view_t *v = &ctx->state->view;
    uint32_t version = 0;
    char key[9] = "none", fqdn[129] = "";
    if (v->has_header) {
        const wl_header_t *h = (const wl_header_t *)v->header;
        version = h->version;
        size_t n = h->fqdn_len < sizeof(h->fqdn) ? h->fqdn_len : sizeof(h->fqdn);
        memcpy(fqdn, h->fqdn, n);
        fqdn[n] = 0;
    }
    /* key= is the pinned key: it signs everything this loader updates (trust on first use). */
    uint8_t hash[32];
    if (v->has_pin && wl_sha256(v->pin, WL_PUBKEY_SIZE, hash)) {
        snprintf(key, sizeof(key), "%02x%02x%02x%02x", hash[0], hash[1], hash[2], hash[3]);
    }
    uint8_t mac[6];
    wl_net_mac(mac);
    char line[460], wanted[80] = "";
    if (want && *want) snprintf(wanted, sizeof(wanted), " want=%.64s", want);
    int n = snprintf(line, sizeof(line),
                     "esp-wifi-loader v=1 mode=%s state=%s version=%lu key=%s fqdn=%s chip=%s "
                     "mac=%02x:%02x:%02x:%02x:%02x:%02x port=%d role=%s loader=1 up=%lld%s\n",
                     mode, state, (unsigned long)version, key, fqdn, CONFIG_IDF_TARGET,
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], CONFIG_WL_PORT,
                     wl_net_role() == WL_ROLE_AP ? "ap" : "sta",
                     (long long)(esp_timer_get_time() / 1000000), wanted);
    sendto(udp, line, n, 0, (struct sockaddr *)&to, sizeof(to));
}

int esp_wifi_loader_announce(const char *want)
{
    wl_upload_ctx_t *ctx = wl_ctx_open();
    if (!ctx) return -1;
    int udp = socket(AF_INET6, SOCK_DGRAM, 0);      /* one every few seconds: nothing is kept between them */
    int one = 1;
    if (udp >= 0) {
        setsockopt(udp, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &one, sizeof(one));
        wl_announce(udp, wl_net_ifindex(), ctx, "main", "update", want);
        close(udp);
    }
    wl_ctx_close(ctx);
    return udp >= 0 ? 0 : -1;
}
