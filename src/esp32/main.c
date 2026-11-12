/**
 * @file    main.c
 * @brief   Точка входа флагманского стримера OMNIA на ESP32-S3-N16R8 (ESP-IDF v5.x, без Arduino):
 *          выделение статических пулов в Octal PSRAM, подъём ведомого I2S0 на Core 1,
 *          связь с DSP-мастером AT32F403 по UART1 (921600 бод) и фоновый мониторинг.
 */

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "omnia_esp32.h"

static const char *TAG = "omnia_main";

static omnia_static_pools_t g_static_pools;

static void on_usb_vbus_hotplug(bool vbus_present, uint8_t active_usb_port)
{
    if (vbus_present && active_usb_port == 1U) {
        ESP_LOGI(TAG, "Автопереход USB «на лету»: активирован вход USB-C DAC (UAC1 Async)");
        omnia_rgb_ring_set_mode(OMNIA_LED_MODE_TUBE_AMBER, 0.30f);
    } else {
        ESP_LOGI(TAG, "Автопереход USB «на лету»: USB-DAC не выбран или VBUS отключён");
        omnia_rgb_ring_set_mode(OMNIA_LED_MODE_OBSIDIAN_CYAN, 0.30f);
    }
}

static void allocate_static_audio_pools(void)
{
    memset(&g_static_pools, 0, sizeof(g_static_pools));

    g_static_pools.compressed_ring = heap_caps_malloc(
        OMNIA_COMPRESSED_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    g_static_pools.pcm_ring = heap_caps_malloc(
        OMNIA_PCM_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (g_static_pools.compressed_ring && g_static_pools.pcm_ring) {
        g_static_pools.compressed_size = OMNIA_COMPRESSED_RING_BYTES;
        g_static_pools.pcm_size        = OMNIA_PCM_RING_BYTES;
        ESP_LOGI(TAG, "Статические пулы Octal PSRAM выделены: Compressed=%u КБ, PCM=%u КБ",
                 (unsigned)(g_static_pools.compressed_size / 1024U),
                 (unsigned)(g_static_pools.pcm_size / 1024U));
    } else {
        /* Безопасный стендовый откат для отладки без активированной PSRAM */
        if (g_static_pools.compressed_ring) free(g_static_pools.compressed_ring);
        if (g_static_pools.pcm_ring)        free(g_static_pools.pcm_ring);
        g_static_pools.compressed_size = 32U * 1024U;
        g_static_pools.pcm_size        = 64U * 1024U;
        g_static_pools.compressed_ring = heap_caps_malloc(g_static_pools.compressed_size, MALLOC_CAP_8BIT);
        g_static_pools.pcm_ring        = heap_caps_malloc(g_static_pools.pcm_size, MALLOC_CAP_8BIT);
        ESP_LOGW(TAG, "Octal PSRAM не обнаружена: активирован стендовый SRAM-пул (%u КБ PCM)",
                 (unsigned)(g_static_pools.pcm_size / 1024U));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "==========================================================");
    ESP_LOGI(TAG, "  OMNIA Flagship Streamer & Studio DSP v2.0 (ESP-IDF 5.x) ");
    ESP_LOGI(TAG, "==========================================================");

    /* 1. Инициализация энергонезависимого хранилища NVS (пресеты, Wi-Fi, обученные пульты ИК) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. Выделение неразрывных статических аудио-колец без фрагментации кучи */
    allocate_static_audio_pools();

    /* 3. Подъём подсветки 12x WS2812 (Obsidian Cyan / Standby lub-dub) на GPIO48 */
    ESP_ERROR_CHECK(omnia_rgb_ring_init());
    omnia_rgb_ring_set_mode(OMNIA_LED_MODE_OBSIDIAN_CYAN, 0.28f);

    /* 4. Подъём ведомого I2S0 Slave TX (32-bit Phillips) на Core 1 */
    ESP_ERROR_CHECK(omnia_audio_i2s_init(&g_static_pools, 48000U));

    /* 5. Подъём протокольного моста с DSP-процессором AT32F403ACGU7 (UART1 @ 921600 бод) */
    ESP_ERROR_CHECK(omnia_at32_bridge_init(on_usb_vbus_hotplug));

    /* 6. Опрос паспорта AT32F403 (проверка 224 КБ SRAM и опциональной SPI NOR Flash) */
    omnia_sys_info_t at32_info;
    if (omnia_at32_query_sys_info(&at32_info, 250U) == ESP_OK) {
        ESP_LOGI(TAG, "AT32F403 на связи: FW %u.%u.%u, SYSCLK=%lu МГц, SRAM=%u КБ",
                 at32_info.fw_major, at32_info.fw_minor, at32_info.fw_patch,
                 (unsigned long)(at32_info.sysclk_hz / 1000000UL),
                 at32_info.sram_size_kb);
        if (at32_info.ext_flash_present) {
            ESP_LOGI(TAG, "Обнаружена внешняя SPI NOR Flash на AT32 (шина #%u): JEDEC=0x%06lX, объём=%lu КБ",
                     at32_info.ext_flash_present,
                     (unsigned long)at32_info.ext_flash_jedec,
                     (unsigned long)(at32_info.ext_flash_bytes / 1024UL));
        } else {
            ESP_LOGI(TAG, "Внешняя SPI Flash на AT32 не распаяна — пресеты хранятся во внутренней Flash AT32");
        }

        /* Синхронизируем сохранённую настройку Триггера звуковой карты USB-DAC из NVS */
        (void)omnia_at32_sync_usb_trigger_to_at32();
        /* Без ранее подтверждённой конфигурации питание усилителя заблокировано. */
        ret = omnia_at32_sync_amp_ctrl_to_at32();
        if (ret != ESP_OK) ESP_LOGW(TAG, "Конфигурация усилителя отсутствует/не принята: %s", esp_err_to_name(ret));

        /* Загружаем стартовый студийный профиль AETHER + ISO 226 и комфортную громкость -24 дБ */
        (void)omnia_at32_load_default_studio_profile();
        (void)omnia_at32_set_volume(-24.0f, 0.0f, 0.0f, 40U);
        const omnia_cmd_mute_t unmute = { .ramp_ms = 35U, .hard_xsmt = 0U };
        (void)omnia_at32_send_cmd_sync(OMNIA_CMD_UNMUTE, &unmute, sizeof(unmute), 60U);
    } else {
        ESP_LOGW(TAG, "Ожидание подключения платы AT32F403 по UART1 (GPIO7/GPIO8)...");
    }

    /* Главный цикл супервизора Core 0 */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        omnia_at32_status_t st;
        omnia_at32_get_latest_status(&st);
        if (st.sample_rate_hz > 0U) {
            ESP_LOGI(TAG, "Статус тракта: Fs=%lu Гц | Vol=%.1f дБ | BitPerfect=%u | CPU AT32=%u%% | Clips=%u",
                     (unsigned long)st.sample_rate_hz,
                     (double)st.current_vol_db,
                     st.bit_perfect_mode,
                     st.cpu_load_percent,
                     st.clip_events);
        }
    }
}
