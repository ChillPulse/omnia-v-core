FILE(GLOB_RECURSE esp32_sources "${CMAKE_CURRENT_SOURCE_DIR}/esp32/*.c")

idf_component_register(
    SRCS ${esp32_sources}
    INCLUDE_DIRS "esp32/include" "../shared"
    REQUIRES driver esp_timer nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -O3)
