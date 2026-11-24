# gogo firmware for the M5StickS3 (ESP32-S3). Build from the repo root:
#   . ~/esp/esp-idf/export.sh && idf.py -C platform/esp32s3 build
# The VM sources are compiled in place from <repo>/vm (see main/CMakeLists.txt).
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gogo)
