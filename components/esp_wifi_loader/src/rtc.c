#include "esp_wifi_loader.h"

void esp_wifi_loader_request_on_next_boot(void)
{
    WL_RTC_HANDOFF = WL_HANDOFF_LOADER;
}
