cmake_minimum_required(VERSION 3.16)

# Set the target chip — Tanmatsu uses the ESP32-P4
set(IDF_TARGET "esp32p4")

# Pull in the ESP-IDF CMake build system
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(matrix_rain)
