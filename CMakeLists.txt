cmake_minimum_required(VERSION 3.16.0)

# Совместимость одновременно с VS Code PlatformIO и чистым ESP-IDF v5.x (idf.py build)
if(NOT "${CMAKE_CURRENT_SOURCE_DIR}/src" IN_LIST EXTRA_COMPONENT_DIRS AND NOT "src" IN_LIST EXTRA_COMPONENT_DIRS)
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(omnia-v-core)
# LTO приложения; bootloader намеренно собирается со штатными флагами ESP-IDF.
target_link_options(${CMAKE_PROJECT_NAME}.elf PRIVATE -flto)
