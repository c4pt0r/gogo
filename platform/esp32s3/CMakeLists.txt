# gogo firmware for the M5StickS3 and the Cardputer ADV (ESP32-S3; board:
# main/CMakeLists.txt GOGO_BOARD). Build from the repo root:
#   . ~/esp/esp-idf/export.sh && idf.py -C platform/esp32s3 build
# The VM sources are compiled in place from <repo>/runtime/vm (see main/CMakeLists.txt).
cmake_minimum_required(VERSION 3.16)
# The default board (StickS3) has PSRAM: sdkconfig.sticks3 on top of the
# defaults. The Cardputer build gives its own SDKCONFIG_DEFAULTS (see
# main/CMakeLists.txt), so this applies only when none is given.
if(NOT DEFINED SDKCONFIG_DEFAULTS AND NOT DEFINED ENV{SDKCONFIG_DEFAULTS})
    if(DEFINED GOGO_BOARD AND NOT GOGO_BOARD STREQUAL "sticks3")
        message(FATAL_ERROR "GOGO_BOARD=${GOGO_BOARD} needs its own -DSDKCONFIG_DEFAULTS (the default would add the StickS3's PSRAM); see README.md")
    endif()
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.sticks3")
endif()
# The firmware version: `git describe` of the repo at configure time
# ("dev" without git). It is the app's PROJECT_VER, so it is in gogo.bin's
# app descriptor too, where `gogo setup` reads the version it is about to
# flash, and main/CMakeLists.txt gives it to the C code as
# GOGO_FIRMWARE_VERSION (the boot line, `gogo device info`).
execute_process(
    COMMAND git describe --always --dirty
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    OUTPUT_VARIABLE PROJECT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT PROJECT_VER)
    set(PROJECT_VER "dev")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gogo)

# No malloc in LittleFS (runtime/vfs/lfs/lfs_be.c passes every buffer in): lfs.c alone
# (not the component's VFS wrapper, which needs malloc and is not linked)
# gets LFS_NO_MALLOC and malloc / free renamed to lfs_be.c's NULL-returning
# stubs, so it cannot reach the heap. Checked: lfs.c.obj imports
# gogo_lfs_malloc / gogo_lfs_free and no malloc / free.
idf_component_get_property(lfs_dir joltwallet__littlefs COMPONENT_DIR)
idf_component_get_property(lfs_lib joltwallet__littlefs COMPONENT_LIB)
set_property(SOURCE "${lfs_dir}/src/littlefs/lfs.c" TARGET_DIRECTORY ${lfs_lib}
    APPEND PROPERTY COMPILE_DEFINITIONS LFS_NO_MALLOC malloc=gogo_lfs_malloc free=gogo_lfs_free)
