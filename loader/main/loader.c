#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi_loader.h"

void app_main(void)
{
    esp_err_t err = esp_wifi_loader_run();
    ESP_LOGE("loader", "esp_wifi_loader_run failed: %s", esp_err_to_name(err));
    esp_restart();
}
