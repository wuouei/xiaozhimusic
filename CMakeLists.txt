# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.8.6")

# Include ESP-ADF if available
if(DEFINED ENV{ADF_PATH})
    include($ENV{ADF_PATH}/CMakeLists.txt)
    message(STATUS "ESP-ADF found at: $ENV{ADF_PATH}")
else()
    message(WARNING "ESP-ADF not found. Set ADF_PATH environment variable to use ESP-ADF features.")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi)

# Disable specific warnings (ESP-ADF v2.7 vs ESP-IDF 5.4 API compatibility)
add_compile_options(-Wno-missing-field-initializers -Wno-error=enum-compare -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=return-type)

