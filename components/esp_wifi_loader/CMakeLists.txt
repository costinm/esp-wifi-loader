set(srcs "src/serve.c" "src/upload.c" "src/state.c" "src/rtc.c" "src/state_core.c" "src/announce.c" "src/crypto_psa.c")
# net.c is also the Wi-Fi of an application in library mode (esp_wifi_loader_net_*); it may bring its own instead.
list(APPEND srcs "src/net.c" "src/wifi.c" "src/server.c")
if(NOT CONFIG_WL_MODE_LIBRARY)
    list(APPEND srcs "src/esp_wifi_loader.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_wifi esp_netif esp_event esp_partition esp_timer lwip mbedtls
             bootloader_support esp_system spi_flash)
