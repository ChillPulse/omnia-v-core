/*
 * Omnia (v-core) — точка входа.
 * ТЗ, Том 1, Гл. 5: инициализация кучи, GPIO безопасности, изоляция ядер FreeRTOS.
 *   Core 0: аудио-движок, DSP, I2S DMA, сетевой стек (Wi-Fi/LWIP задает ESP-IDF).
 *   Core 1: LVGL, ввод, датчики, веб-сервер (esp_http_server), ИК, OTA-приемка.
 */

#include "omnia.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "ui_engine.h"

static const char* TAG = "OMNIA_MAIN";

OmniaAudioBuffer audio_buffer;
OmniaDSPCore     dsp_processor;
OmniaIRLearn     ir_learn;
QueueHandle_t    spectrum_queue     = NULL;
QueueHandle_t    audio_format_queue = NULL;

static void halt_with_error(const char* msg)
{
    ESP_LOGE(TAG, "%s", msg);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ТЗ, Том 1, Гл. 5: силовые и коммутационные линии — в безопасное состояние до старта задач
static void init_safe_outputs(void)
{
    gpio_config_t io_conf = {};
    io_conf.intr_type    = GPIO_INTR_DISABLE;
    io_conf.mode         = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << PIN_I2S_XSMT) | (1ULL << PIN_AMP_MUTE) |
                           (1ULL << PIN_USB_VBUS_EN) | (1ULL << PIN_USB_SEL) |
                           (1ULL << PIN_AMP_POWER_REG);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en   = GPIO_PULLUP_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    gpio_set_level((gpio_num_t)PIN_AMP_POWER_REG, 0);  // реле БП УМЗЧ разомкнуто
    gpio_set_level((gpio_num_t)PIN_I2S_XSMT, 0);       // ЦАП в mute
    gpio_set_level((gpio_num_t)PIN_AMP_MUTE, 1);       // TPA в mute (по умолчанию HIGH = mute)
    gpio_set_level((gpio_num_t)PIN_USB_VBUS_EN, 0);    // питание флешки выключено
    gpio_set_level((gpio_num_t)PIN_USB_SEL, 0);        // MUX в дефолтном положении
}

// Core 1: UI, ввод, датчики, сетевые сервисы
static void vTaskUIEngine(void* pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "UI Engine запущен на ядре %d", xPortGetCoreID());

    if (!init_st7789_vcore_graphics()) {
        halt_with_error("Ошибка инициализации дисплея ST7789");
    }
    ui_init();
    input_init();
    if (!sensors_init()) {
        ESP_LOGW(TAG, "BH1750 не отвечает: датчик освещенности отключен");
    }
    if (!init_sd_card_spi3()) {
        ESP_LOGW(TAG, "SD-карта не смонтирована (проверьте разводку SPI3, см. README)");
    }
    ir_learn.init();
    wifi_sta_start();
    init_omnia_pwa_webserver();

    TickType_t last_periodic = 0;
    while (1) {
        input_poll();

        omnia_spectrum_t sp;
        if (xQueueReceive(spectrum_queue, &sp, 0) == pdTRUE) {
            spectra_ui_update(sp.bands);
        }

        lv_timer_handler();   // LVGL: только из этой задачи

        TickType_t now = xTaskGetTickCount();
        if (now - last_periodic >= pdMS_TO_TICKS(200)) {
            last_periodic = now;
            omnia_update_sensors(wifi_get_rssi(), sensors_read_lux());
            ui_refresh();
            ws_broadcast_state();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Старт Omnia (v-core) %s на чистом ESP-IDF", OMNIA_VERSION);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    omnia_state_init();

    // Проверка Octal PSRAM (ТЗ, Том 1, Гл. 5)
    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_size == 0) {
        halt_with_error("Критический сбой: Octal PSRAM не обнаружена (проверьте N16R8 и sdkconfig)");
    }
    ESP_LOGI(TAG, "Octal PSRAM: %u байт", (unsigned)psram_size);

    if (!audio_buffer.init()) {
        halt_with_error("Не удалось выделить Ring Buffer 4 МБ в PSRAM");
    }
    dsp_processor.init(44100);

    spectrum_queue     = xQueueCreate(1, sizeof(omnia_spectrum_t));
    audio_format_queue = xQueueCreate(1, sizeof(omnia_audio_format_t));
    if (!spectrum_queue || !audio_format_queue) {
        halt_with_error("Не удалось создать очереди FreeRTOS");
    }

    init_safe_outputs();

    // Core 0: аудио-движок с максимальным приоритетом (configMAX_PRIORITIES - 1)
    xTaskCreatePinnedToCore(vTaskAudioEngine, "TaskAudioEngine", 12288, NULL,
                            configMAX_PRIORITIES - 1, NULL, 0);
    // Core 1: UI, веб-сервер, ввод
    xTaskCreatePinnedToCore(vTaskUIEngine, "TaskUIEngine", 16384, NULL, 5, NULL, 1);

    // Прошивка загружена успешно — отменяем откат (если включен rollback)
    esp_ota_mark_app_valid_cancel_rollback();

    ESP_LOGI(TAG, "Планировщик распределил задачи по ядрам LX7.");
}
