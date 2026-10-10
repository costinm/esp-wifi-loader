/* Writes the 8 KiB state area the C loader would produce, for the Rust crate's interop test:
 *   gcc -I components/esp_wifi_loader/include tests/make_state_fixture.c \
 *       components/esp_wifi_loader/src/state_core.c -o /tmp/mk && /tmp/mk tests/data/state.bin */
#include <stdio.h>
#include <string.h>
#include "esp_wifi_loader_state.h"

static unsigned char flash[WL_STATE_SIZE];
static int f_read(void *c, uint32_t o, void *b, size_t n) { (void)c; memcpy(b, flash + o, n); return 0; }
static int f_write(void *c, uint32_t o, const void *b, size_t n)
{
    (void)c;
    for (size_t i = 0; i < n; ++i) flash[o + i] &= ((const unsigned char *)b)[i];
    return 0;
}
static int f_erase(void *c, uint32_t o) { (void)c; memset(flash + o, 0xff, WL_STATE_SECTOR); return 0; }

int main(int argc, char **argv)
{
    wl_flash_ops_t ops = {NULL, f_read, f_write, f_erase};
    wl_state_t st;
    unsigned char key[WL_PUBKEY_SIZE], hdr[WL_HEADER_SIZE];
    memset(flash, 0xff, sizeof(flash));
    wl_state_open(&st, &ops);
    for (int i = 0; i < WL_PUBKEY_SIZE; ++i) key[i] = (unsigned char)(i ? i : 4);
    for (int i = 0; i < WL_HEADER_SIZE; ++i) hdr[i] = (unsigned char)(0x80 + i);
    uint32_t minver = 7, loader = 33, stage2 = 44;
    unsigned char qio = 1;
    wl_state_append(&st, WL_REC_PIN, key, sizeof(key));
    wl_state_append(&st, WL_REC_MINVER, &minver, 4);
    wl_state_append(&st, WL_REC_HEADER, hdr, sizeof(hdr));
    wl_state_append(&st, WL_REC_QIO, &qio, 1);
    wl_state_append(&st, WL_REC_MINVER_LOADER, &loader, 4);
    wl_state_append(&st, WL_REC_MINVER_STAGE2, &stage2, 4);
    FILE *f = fopen(argc > 1 ? argv[1] : "state.bin", "wb");
    fwrite(flash, 1, sizeof(flash), f);
    fclose(f);
    return 0;
}
