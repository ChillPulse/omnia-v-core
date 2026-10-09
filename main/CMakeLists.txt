idf_component_register(
    SRCS
        "main.cpp"
        "omnia_state.cpp"
        "audio_engine/audio_buffer.cpp"
        "audio_engine/core0_audio.cpp"
        "audio_engine/fft.cpp"
        "audio_engine/network_services.cpp"
        "dsp_processor/dsp_core.cpp"
        "media_indexer/storage_hal.cpp"
        "ui_engine/display_hal.cpp"
        "ui_engine/spectra_visualizers.cpp"
        "ui_engine/ui_main.cpp"
        "ui_engine/input_hal.cpp"
        "ui_engine/ir_learn.cpp"
        "ui_engine/web_server_backend.cpp"
        "system/power_manager.cpp"
        "system/seamless_ota.cpp"
        "system/wifi_sta.cpp"
        "system/sensors.cpp"
    INCLUDE_DIRS
        "include"
        "audio_engine/include"
        "dsp_processor/include"
        "ui_engine/include"
    REQUIRES
        driver esp_lcd esp_http_server json esp_wifi nvs_flash esp_netif esp_event
        esp_timer esp_pm app_update esp_adc fatfs vfs sdmmc esp_system
)

# PWA-бандл (index.html.gz) записывается в раздел littlefs при прошивке (см. tools/build_pwa.py)
littlefs_create_partition_image(littlefs ${CMAKE_CURRENT_SOURCE_DIR}/../littlefs_data FLASH_IN_PROJECT)
