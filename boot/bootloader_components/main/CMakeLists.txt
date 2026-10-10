set(WL_COMPONENT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../components/esp_wifi_loader")

idf_component_register(
    SRCS "bootloader_start.c" "${WL_COMPONENT_DIR}/src/state_core.c"
    INCLUDE_DIRS "." "${WL_COMPONENT_DIR}/include"
    PRIV_REQUIRES bootloader_support nvs_flash hal)

if(NOT DEFINED ENV{WL_STAGE2_VERSION} OR "$ENV{WL_STAGE2_VERSION}" STREQUAL "")
    string(TIMESTAMP WL_STAGE2_DATE "%Y%m%d" UTC)
    set(WL_STAGE2_VERSION "${WL_STAGE2_DATE}000")
else()
    set(WL_STAGE2_VERSION "$ENV{WL_STAGE2_VERSION}")
endif()
if(NOT WL_STAGE2_VERSION MATCHES "^[0-9]+$")
    message(FATAL_ERROR "WL_STAGE2_VERSION must be numeric, got '${WL_STAGE2_VERSION}'")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE WL_STAGE2_VERSION=${WL_STAGE2_VERSION})
