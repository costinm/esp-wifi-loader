#pragma once

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

/* Open the state log and crypto. Call once before esp_wifi_loader_serve(). */
esp_err_t esp_wifi_loader_init(void);

/* Serve ONE accepted TCP connection (docs/protocol.md): verify and install a signed
 * package, or answer a zero-length Main manifest. The caller owns and closes `fd` and
 * should set receive/send timeouts (about 10 s) on it. Returns after the reply was
 * sent; in the loader and bootstrap builds a finished install restarts the chip, and a
 * zero-length manifest restarts it into the loader, from inside this call. */
esp_wifi_loader_result_t esp_wifi_loader_serve(int fd);

/* Run the loader (loader and bootstrap builds only): join ESP-FLASH or host ESP-RESCUE, announce, accept signed
 * uploads. Returns only on error; on success it restarts into Main. */
esp_err_t esp_wifi_loader_run(void);

#ifdef __cplusplus
}
#endif
