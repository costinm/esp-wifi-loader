/* Host test for the state sector log: gcc -I components/esp_wifi_loader/include ... */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "esp_wifi_loader_state.h"

static uint8_t flash[WL_STATE_SIZE];
static int fail_after = -1; /* writes remaining before simulated power loss */

static int f_read(void *c, uint32_t o, void *b, size_t n) { (void)c; memcpy(b, flash + o, n); return 0; }
static int f_write(void *c, uint32_t o, const void *b, size_t n)
{
    (void)c;
    assert(o % 4 == 0 && n % 4 == 0);
    if (fail_after == 0) { /* torn: program half the words */
        for (size_t i = 0; i < n / 2; ++i) flash[o + i] &= ((const uint8_t *)b)[i];
        return -1;
    }
    if (fail_after > 0) --fail_after;
    for (size_t i = 0; i < n; ++i) flash[o + i] &= ((const uint8_t *)b)[i]; /* NOR: 1->0 only */
    return 0;
}
static int f_erase(void *c, uint32_t o) { (void)c; (void)o; memset(flash + o, 0xff, WL_STATE_SECTOR); return 0; }
static const wl_flash_ops_t ops = {NULL, f_read, f_write, f_erase};

int main(void)
{
    wl_state_t st;
    memset(flash, 0xff, sizeof(flash));
    wl_state_open(&st, &ops);
    assert(st.active < 0 && !st.view.has_pin);

    uint8_t key[WL_PUBKEY_SIZE], hdr[WL_HEADER_SIZE];
    memset(key, 0x04, sizeof(key));
    memset(hdr, 0x11, sizeof(hdr));
    assert(!wl_state_append(&st, WL_REC_PIN, key, sizeof(key)));
    uint32_t v = 7;
    assert(!wl_state_append(&st, WL_REC_MINVER, &v, 4));
    assert(!wl_state_append(&st, WL_REC_HEADER, hdr, sizeof(hdr)));
    uint8_t q = WL_QIO_OK;
    assert(!wl_state_append(&st, WL_REC_QIO, &q, 1));

    uint32_t lv = 33, sv = 44;
    assert(!wl_state_append(&st, WL_REC_MINVER_LOADER, &lv, 4));
    assert(!wl_state_append(&st, WL_REC_MINVER_STAGE2, &sv, 4));
    wl_state_open(&st, &ops);
    assert(st.view.minver_loader == 33 && st.view.minver_stage2 == 44);
    assert(st.view.has_pin && !memcmp(st.view.pin, key, sizeof(key)));
    assert(st.view.minver == 7 && st.view.qio == WL_QIO_OK && st.view.has_header);

    /* Many installs force several compactions; state must survive each. */
    int first_active = st.active;
    for (v = 8; v < 200; ++v) {
        hdr[0] = (uint8_t)v;
        assert(!wl_state_append(&st, WL_REC_MINVER, &v, 4));
        assert(!wl_state_append(&st, WL_REC_HEADER, hdr, sizeof(hdr)));
        wl_state_open(&st, &ops);
        assert(st.view.minver == v && st.view.header[0] == (uint8_t)v);
        assert(st.view.has_pin && st.view.qio == WL_QIO_OK);
        assert(st.view.minver_loader == 33 && st.view.minver_stage2 == 44);
    }
    assert(st.active != first_active || st.seq > 1);

    /* Power loss at every write position of an append leaves a readable log. */
    for (int cut = 0; cut < 8; ++cut) {
        wl_state_open(&st, &ops);
        uint32_t old = st.view.minver, nv = old + 1;
        fail_after = cut;
        (void)wl_state_append(&st, WL_REC_MINVER, &nv, 4);
        fail_after = -1;
        wl_state_open(&st, &ops);
        assert(st.view.has_pin && st.view.qio == WL_QIO_OK);
        assert(st.view.minver == old || st.view.minver == nv);
        assert(!wl_state_append(&st, WL_REC_MINVER, &nv, 4)); /* heals if dirty */
        wl_state_open(&st, &ops);
        assert(st.view.minver == nv);
    }
    /* Module slots (written by the Rust crate) survive; power cut at every write of a COMPACTING append. */
    {
        uint8_t rec[5] = {2, 9, 0, 0, 0};
        assert(!wl_state_append(&st, WL_REC_MINVER_MODULE, rec, 5));
        wl_state_open(&st, &ops);
        assert((st.view.has_module & 4) && st.view.minver_module[2] == 9);
        for (int cut = 0; cut < 14; ++cut) {
            wl_state_open(&st, &ops);
            /* fill the active sector until the next record no longer fits */
            uint8_t filler[WL_HEADER_SIZE];
            memset(filler, 0x5a, sizeof(filler));
            for (int i = 0; i < 40; ++i) {
                uint32_t used_before = st.used;
                assert(!wl_state_append(&st, WL_REC_HEADER, filler, sizeof(filler)));
                if (st.used < used_before) break; /* it compacted */
            }
            while (st.used + 4 + WL_HEADER_SIZE + 4 <= WL_STATE_SECTOR) {
                assert(!wl_state_append(&st, WL_REC_HEADER, filler, sizeof(filler)));
            }
            uint32_t old = st.view.minver, nv = old + 1;
            uint8_t before_pin = st.view.has_pin;
            memset(filler, 0x6b, sizeof(filler));
            fail_after = cut;
            (void)wl_state_append(&st, WL_REC_HEADER, filler, sizeof(filler)); /* compacts */
            fail_after = -1;
            wl_state_open(&st, &ops);
            assert(st.view.has_pin == before_pin && st.view.has_pin);
            assert(st.view.minver == old && (st.view.has_module & 4) && st.view.minver_module[2] == 9);
            assert(st.view.qio == WL_QIO_OK);
            assert(!wl_state_append(&st, WL_REC_MINVER, &nv, 4)); /* heals a torn sector */
            wl_state_open(&st, &ops);
            assert(st.view.minver == nv);
        }
    }
    wl_state_erase_all(&st);
    wl_state_open(&st, &ops);
    assert(!st.view.has_pin);
    puts("state ok");
    return 0;
}
