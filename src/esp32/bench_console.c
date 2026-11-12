/* Временная стендовая консоль, не интерфейс готового изделия.
 * Только для проверки без подключённого усилителя, K2 и динамиков. */
#include "omnia_esp32.h"
#include "omnia_bench_cmd.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <string.h>

static const char *TAG = "omnia_bench";
static bool tone_running;
static int64_t tone_deadline_us;

static void stop_tone(void)
{
    omnia_audio_set_test_generator(false, 1000.0f, -12.0f);
    const omnia_cmd_mute_t mute = { .ramp_ms = 25U, .hard_xsmt = 1U };
    esp_err_t err = omnia_at32_send_cmd_sync(OMNIA_CMD_MUTE, &mute, sizeof(mute), 80U);
    tone_running = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MUTE не подтверждён (%s): отключите питание, XSMT мог остаться включённым!", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Тон остановлен; AT32 подтвердил MUTE и XSMT LOW.");
    }
}

static void start_tone(unsigned seconds)
{
    omnia_at32_status_t st;
    if (tone_running) {
        ESP_LOGW(TAG, "Тест уже идёт; сначала STOP.");
        return;
    }
    if (!omnia_at32_get_recent_status(&st, 2500U) || st.amp_pwr_enabled || st.dc_fault_tripped) {
        ESP_LOGE(TAG, "Нет свежего статуса AT32, включён K2 или есть DC-авария; тест запрещён.");
        return;
    }
    /* Проверка статуса не заменяет физическое отключение K2 и динамиков. */
    const omnia_cmd_mute_t mute = { .ramp_ms = 25U, .hard_xsmt = 1U };
    const omnia_cmd_mute_t unmute = { .ramp_ms = 35U, .hard_xsmt = 0U };
    esp_err_t err = omnia_at32_send_cmd_sync(OMNIA_CMD_MUTE, &mute, sizeof(mute), 80U);
    if (err == ESP_OK) err = omnia_at32_set_volume(-24.0f, 0.0f, 0.0f, 40U);
    const omnia_bypass_mask_t bypass = { .active_mask = OMNIA_DSP_ALL_BYPASSED, .ramp_ms = 35U };
    if (err == ESP_OK) err = omnia_at32_send_cmd_sync(OMNIA_CMD_SET_BYPASS_MASK, &bypass, sizeof(bypass), 80U);
    if (err == ESP_OK) err = omnia_at32_set_input_source(OMNIA_SRC_ESP32_I2S);
    if (err == ESP_OK) {
        omnia_audio_set_test_generator(true, 1000.0f, -12.0f);
        err = omnia_at32_send_cmd_sync(OMNIA_CMD_UNMUTE, &unmute, sizeof(unmute), 80U);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Старт не подтверждён (%s); пытаемся заглушить тракт.", esp_err_to_name(err));
        stop_tone();
        return;
    }
    tone_running = true;
    tone_deadline_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    ESP_LOGW(TAG, "ТОН 1 кГц, %u с. Только измерительный стенд без усилителя!", seconds);
}

static void execute_line(const char *line)
{
    const omnia_bench_command_t cmd = omnia_bench_parse(line);
    switch (cmd.action) {
    case OMNIA_BENCH_HELP:
        ESP_LOGI(TAG, "Команды: HELP | STATUS | TONE NOAMP 1..10 | STOP. Перед TONE физически отключите УМЗЧ, K2 и динамики.");
        break;
    case OMNIA_BENCH_STATUS: {
        omnia_at32_status_t st;
        if (!omnia_at32_get_recent_status(&st, 2500U)) {
            ESP_LOGW(TAG, "Нет свежей телеметрии AT32.");
        } else {
            ESP_LOGI(TAG, "Fs=%lu mute=%u source=%u K2=%u DC=%u mode=%u tone=%u",
                     (unsigned long)st.sample_rate_hz, st.muted, st.active_i2s_src,
                     st.amp_pwr_enabled, st.dc_fault_tripped, st.amp_mute_mode,
                     tone_running ? 1U : 0U);
        }
        break;
    }
    case OMNIA_BENCH_STOP:
        stop_tone();
        break;
    case OMNIA_BENCH_TONE:
        start_tone(cmd.seconds);
        break;
    default:
        ESP_LOGW(TAG, "Неизвестная команда. Введите HELP заглавными буквами.");
        break;
    }
}

static void bench_task(void *arg)
{
    (void)arg;
    char line[48];
    size_t len = 0U;
    bool overflow = false;
    ESP_LOGI(TAG, "Стендовая консоль UART0 115200: HELP (Bluetooth и УМЗЧ не подключать).");
    while (1) {
        uint8_t ch;
        int n = uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(50));
        if (n == 1) {
            if (ch == '\r' || ch == '\n') {
                if (overflow) ESP_LOGW(TAG, "Команда слишком длинная.");
                else if (len > 0U) {
                    line[len] = '\0';
                    execute_line(line);
                }
                len = 0U;
                overflow = false;
            } else if (!overflow && len + 1U < sizeof(line)) {
                line[len++] = (char)ch;
            } else {
                overflow = true;
            }
        }
        if (tone_running) {
            omnia_at32_status_t st;
            if (esp_timer_get_time() >= tone_deadline_us ||
                !omnia_at32_get_recent_status(&st, 2500U) || st.amp_pwr_enabled || st.dc_fault_tripped) {
                stop_tone();
            }
        }
    }
}

esp_err_t omnia_bench_console_init(void)
{
#if CONFIG_ESP_CONSOLE_UART_DEFAULT
    /* Параметры UART0 уже настроены загрузчиком / ESP-IDF. Не меняем скорость логов. */
    if (!uart_is_driver_installed(UART_NUM_0)) {
        esp_err_t err = uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0);
        if (err != ESP_OK) return err;
    }
    return xTaskCreatePinnedToCore(bench_task, "omnia_bench", 4096, NULL, 5, NULL, 0) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
#else
    ESP_LOGW(TAG, "Стендовая консоль требует стандартный UART0; текущая консоль не поддержана.");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
