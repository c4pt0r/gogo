# gogo firmware for the M5StickS3 and the Cardputer ADV (ESP32-S3; board:
# main/CMakeLists.txt GOGO_BOARD). Build from the repo root:
#   . ~/esp/esp-idf/export.sh && idf.py -C platform/esp32s3 build
# The VM sources are compiled in place from <repo>/vm (see main/CMakeLists.txt).
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gogo)

# No malloc in LittleFS (main/fs_lfs.c passes every buffer in): lfs.c alone
# (not the component's VFS wrapper, which needs malloc and is not linked)
# gets LFS_NO_MALLOC and malloc / free renamed to fs_lfs.c's NULL-returning
# stubs, so it cannot reach the heap. Checked: lfs.c.obj imports
# gogo_lfs_malloc / gogo_lfs_free and no malloc / free.
idf_component_get_property(lfs_dir joltwallet__littlefs COMPONENT_DIR)
idf_component_get_property(lfs_lib joltwallet__littlefs COMPONENT_LIB)
set_property(SOURCE "${lfs_dir}/src/littlefs/lfs.c" TARGET_DIRECTORY ${lfs_lib}
    APPEND PROPERTY COMPILE_DEFINITIONS LFS_NO_MALLOC malloc=gogo_lfs_malloc free=gogo_lfs_free)
