#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_loader_rtc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ask Stage2 to run the loader on the next boot (a smooth upgrade request).
 * Call esp_restart() afterwards. A Main in another language writes the same
 * RTC byte; see docs/app-integration.md. */
void esp_wifi_loader_request_on_next_boot(void);

/* Result of serving one connection (esp_wifi_loader_serve). */
typedef enum {
    WL_SERVE_NONE = 0,         /* nothing installed (error, dry run, or already handled) */
    WL_SERVE_INSTALLED = 1,    /* library mode: a loader/Stage2 image was written */
} esp_wifi_loader_result_t;

/* Codes a module sink returns (0 = accepted); they are the numbers of "ERR <code> <reason>" in docs/protocol.md. */
enum {
    WL_ERR_BAD_LENGTH = 3, WL_ERR_BAD_IMAGE_HEADER = 4, WL_ERR_FLASH = 5, WL_ERR_ROLLBACK = 9,
    WL_ERR_WRONG_TARGET = 10, WL_ERR_NO_MEMORY = 11, WL_ERR_UNKNOWN_MODULE = 15, WL_ERR_BUSY = 16,
};

/* Where signed module packages (header target 4) go. The loader verifies everything about the package - the
 * pinned key, the signature, the block table, every block hash and the whole-image hash - and streams the
 * image to the sink; the sink knows only what the module host knows: which module names exist, where they
 * live, their versions and the module image header. Nothing is erased before `head` has accepted the first
 * block. Calls come from the serving task, in this order:
 *   open (name, version, size known; may refuse: unknown name, too big, older version)
 *   head (first block, up to 4 KiB; check the module's own header)
 *   begin (not on dry runs: stop the module, invalidate the slot)
 *   write (every later block, in order, `offset` in the image)
 *   commit (everything verified: save the version, write the head block last)
 *   done (always after open, after the reply was sent: restart what begin stopped) */
typedef struct {
    void *arg;
    int (*open)(void *arg, const char *name, uint32_t version, uint32_t image_size, bool dry);
    int (*head)(void *arg, const uint8_t *data, size_t len);
    int (*begin)(void *arg);
    int (*write)(void *arg, uint32_t offset, const uint8_t *data, size_t len);
    int (*commit)(void *arg, const uint8_t *head, size_t len);
    void (*done)(void *arg, bool installed, bool begun);
} esp_wifi_loader_module_sink_t;

/* The lowest version a module slot (0..7, the sink's own numbering) may be installed at: kept in the loader's
 * state log with the key pin and the Main version, so an old package cannot roll a module back. */
bool esp_wifi_loader_module_minver(unsigned slot, uint32_t *version);   /* false: none recorded */
int esp_wifi_loader_set_module_minver(unsigned slot, uint32_t version); /* 0 on success */

/* Accept module packages (library mode; the sink must stay valid). NULL: modules are refused. */
void esp_wifi_loader_set_module_sink(const esp_wifi_loader_module_sink_t *sink);

/* Checks that the loader's partitions exist. Allocates nothing and keeps nothing: the state log is opened from
 * flash for each upload or version query. */
esp_err_t esp_wifi_loader_init(void);

/* Serve ONE accepted TCP connection (docs/protocol.md): verify and install a signed
 * package, or answer a zero-length Main manifest. The caller owns and closes `fd` and
 * should set receive/send timeouts (about 10 s) on it. Returns after the reply was
 * sent; in the loader and bootstrap builds a finished install restarts the chip, and a
 * zero-length manifest restarts it into the loader, from inside this call.
 *
 * Not reentrant (one upload at a time). The upload buffers (8 KiB) are heap-allocated for the duration of the
 * call only and nothing stays in RAM afterwards. In library mode
 * (an application with its own network) there is no trust on first use and no unsigned reboot request:
 * a key must already be pinned in the state log or every upload is refused. */
esp_wifi_loader_result_t esp_wifi_loader_serve(int fd);

/* Library mode, Wi-Fi on request: join the network in a task (the flash network when `ssid` is empty; `psk` empty: its
 * public passphrase), rejoining until esp_wifi_loader_wifi_stop(). Returns at once: 0, -1 bad argument, -5 already
 * started, -6 no memory. The caller keeps the name; nothing is stored here. The radio is off after stop. */
int esp_wifi_loader_wifi_start(const uint8_t *ssid, size_t ssid_len, const uint8_t *psk, size_t psk_len);
void esp_wifi_loader_wifi_stop(void);
/* State: 0 off, 1 joining, 2 up; the name, address and signal are filled once up. Returns 0. */
int esp_wifi_loader_wifi_status(int *state, char ssid[33], char ip[16], int *rssi);
/* Library mode, upload server on request: accept uploads (esp_wifi_loader_serve) on CONFIG_WL_PORT, IPv6 and IPv4, and
 * announce `want=NAME` (esp_wifi_loader_announce) every 5 s, once the Wi-Fi is up, until esp_wifi_loader_server_stop().
 * Returns at once: 0, -1 bad name, -5 already started, -6 no memory. */
int esp_wifi_loader_server_start(const uint8_t *name, size_t name_len);
void esp_wifi_loader_server_stop(void);

/* Library mode: send one announce line (docs/protocol.md) on the station interface, `mode=main state=update`, with
 * `want=<want>` when given: the name of the package this device asks the flasher for (a module name, or "main").
 * Call it every few seconds while the upload server is open. 0, or -1 when it could not be sent. */
int esp_wifi_loader_announce(const char *want);

/* Run the loader (loader and bootstrap builds only): join the flasher network or host ESP-RESCUE, announce, accept signed
 * uploads. Returns only on error; on success it restarts into Main. */
esp_err_t esp_wifi_loader_run(void);

#ifdef __cplusplus
}
#endif
