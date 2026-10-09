# Omnia (v-core) — корневой CMake-файл проекта ESP-IDF (v5.3+)
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# ТЗ, Том 1, п. 1: агрессивная оптимизация. LTO подключается отдельно (см. README).
idf_build_set_property(COMPILE_OPTIONS "-O3" APPEND)

project(omnia_v_core)
