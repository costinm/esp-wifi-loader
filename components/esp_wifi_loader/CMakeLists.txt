set(srcs "src/serve.c" "src/upload.c" "src/state.c" "src/rtc.c" "src/state_core.c" "src/crypto_psa.c" "src/trust.c")
# The library build has no networking of its own: the host application owns Wi-Fi.
if(NOT CONFIG_WL_MODE_LIBRARY)
    list(APPEND srcs "src/esp_wifi_loader.c" "src/net.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_wifi esp_netif esp_event esp_partition esp_timer lwip mbedtls
             bootloader_support esp_system spi_flash)
