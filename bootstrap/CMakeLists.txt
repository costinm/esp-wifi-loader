cmake_minimum_required(VERSION 3.16)

# The loader code built to run as Main (ota_0): same component, same loader.c,
# CONFIG_WL_MODE_BOOTSTRAP=y. It installs the loader and Stage2 over Wi-Fi.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(esp-wifi-bootstrap)
