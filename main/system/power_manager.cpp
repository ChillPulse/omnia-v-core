/*
 * Питание и безопасная коммутация УМЗЧ (ТЗ, Том 5, Гл. 2–3).
 *
 * Включение:  реле БП (GPIO9) -> 1500 мс -> XSMT ЦАП (GPIO7) -> 200 мс -> снятие mute TPA (GPIO39)
 * Выключение: mute TPA -> XSMT = 0 -> 100 мс -> реле БП выключено
 *
 * Полярность mute (g_amp_mute_polarity): 0 = HIGH означает mute (код ТЗ Тома 1 и 5).
 * Последовательности включения выполняются в отдельной задаче, чтобы не блокировать UI.
 *
 * Standby: вход в deep sleep НЕ выполняется, потому что ТЗ требует сохранить сессию PWA
 * (Wi-Fi modem sleep + light sleep). Настройка пробуждения по VBUS записана для будущего режима.
 */

#include "omnia.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "esp_pm.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "OMNIA_POWER";

static bool s_powered = false;

static void set_tpa_mute_pin(bool mute)
{
    bool level = (g_amp_mute_polarity == 0) ? mute : !mute;
    gpio_set_level((gpio_num_t)PIN_AMP_MUTE, level ? 1 : 0);
}

void hw_set_mute(bool mute)
{
    // Пока усилитель обесточен, держим и ЦАП, и TPA в mute
    if (s_powered) {
        gpio_set_level((gpio_num_t)PIN_I2S_XSMT, mute ? 0 : 1);
    }
    set_tpa_mute_pin(mute || !s_powered);
}

static void amp_on_now(void)
{
    ESP_LOGI(TAG, "Включение УМЗЧ: реле -> пауза 1500 мс");
    gpio_set_level((gpio_num_t)PIN_AMP_POWER_REG, 1);
    vTaskDelay(pdMS_TO_TICKS(1500));
    s_powered = true;
    gpio_set_level((gpio_num_t)PIN_I2S_XSMT, omnia_is_muted() ? 0 : 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    set_tpa_mute_pin(omnia_is_muted());   // снимаем mute каскадов (Fade-In на стороне DSP)
    ESP_LOGI(TAG, "УМЗЧ включен");
}

static void amp_off_now(void)
{
    ESP_LOGI(TAG, "Выключение УМЗЧ");
    s_powered = false;
    set_tpa_mute_pin(true);
    gpio_set_level((gpio_num_t)PIN_I2S_XSMT, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level((gpio_num_t)PIN_AMP_POWER_REG, 0);
}

static void power_seq_task(void* arg)
{
    if ((intptr_t)arg) amp_on_now();
    else               amp_off_now();
    vTaskDelete(NULL);
}

void enter_omnia_standby_sleep(void)
{
    ESP_LOGI(TAG, "Переход в Standby");
    omnia_set_play_state(OMNIA_PLAY_STANDBY);
    amp_off_now();                      // синхронно: УМЗЧ безопасно обесточен
    display_set_backlight(0);

    // g_usb_autowake_enabled: пробуждение по VBUS через deep sleep НЕ настраивается здесь.
    // Противоречие ТЗ (deep sleep vs. сохранение сессии PWA) не решено — см. README, раздел «Открытые решения».
    (void)g_usb_autowake_enabled;

    esp_pm_config_t pm = {};
    pm.max_freq_mhz = 240;
    pm.min_freq_mhz = 40;          // ТЗ: CPU снижается до 40 МГц
    pm.light_sleep_enable = true;
    esp_err_t e = esp_pm_configure(&pm);
    if (e != ESP_OK) ESP_LOGW(TAG, "esp_pm_configure: %s", esp_err_to_name(e));
}

void exit_omnia_standby(void)
{
    ESP_LOGI(TAG, "Выход из Standby");
    esp_pm_config_t pm = {};
    pm.max_freq_mhz = 240;
    pm.min_freq_mhz = 240;
    pm.light_sleep_enable = false;
    esp_pm_configure(&pm);

    display_set_backlight(80);
    omnia_set_play_state(OMNIA_PLAY_STOPPED);
    xTaskCreatePinnedToCore(power_seq_task, "omnia_pwr", 3072, (void*)1, 4, NULL, 1);
}

void omnia_request_power(bool on)
{
    if (on) exit_omnia_standby();
    else    enter_omnia_standby_sleep();
}
