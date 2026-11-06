cmake_minimum_required(VERSION 3.16)

# General defaults, the XIAO ESP32S3 + Wio-WM6180 pin map, and the radio settings that match
# an OpenMANET mesh (README.md). Plain `idf.py build` uses all three.
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.openmanet")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(openmanet-32)
