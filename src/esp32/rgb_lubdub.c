/**
 * @file    rgb_lubdub.c
 * @brief   Аппаратный драйвер кольца 12x WS2812 на RMT TX (GPIO38, 0% нагрузки на CPU при передаче)
 *          с биометрическим кардиоритмом Standby «lub-dub» (S1 -> 140 мс -> S2 -> диастола).
 */

#include "omnia_esp32.h"
#include <math.h>
#include <string.h>
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "omnia_rgb";

#define RMT_WS2812_RES_HZ       10000000U   /* 10 МГц -> 1 тик = 100 нс */

static rmt_channel_handle_t     g_rmt_chan     = NULL;
static rmt_encoder_handle_t     g_bytes_enc    = NULL;
static volatile omnia_led_mode_t g_led_mode    = OMNIA_LED_MODE_OBSIDIAN_CYAN;
static volatile float           g_brightness   = 0.35f;
static uint8_t                  g_grb_buf[OMNIA_RGB_RING_LED_COUNT * 3U];

/**
 * @brief  Реалистичная двухфазная огибающая живого сердца «lub-dub» (~52 уд/мин = период 1150 мс):
 *         - Первая систола S1 («lub») на t = 110 мс
 *         - Пауза 150 мс и более короткая систола S2 («dub») на t = 275 мс
 *         - Мягкое затухание и спокойная диастола до конца цикла
 */
static float compute_lubdub_envelope(uint32_t phase_ms)
{
    const float t = (float)(phase_ms % 1150U);
    const float d1 = (t - 110.0f) / 48.0f;
    const float d2 = (t - 275.0f) / 36.0f;
    const float s1 = expf(-0.5f * d1 * d1);
    const float s2 = 0.68f * expf(-0.5f * d2 * d2);
    float env = 0.04f + s1 + s2;
    if (env > 1.0f) {
        env = 1.0f;
    }
    return env;
}

static void set_all_pixels_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    for (size_t i = 0; i < OMNIA_RGB_RING_LED_COUNT; ++i) {
        /* Порядок байт WS2812B — строго GRB */
        g_grb_buf[i * 3U + 0U] = g;
        g_grb_buf[i * 3U + 1U] = r;
        g_grb_buf[i * 3U + 2U] = b;
    }
}

static void rgb_animator_task(void *arg)
{
    (void)arg;
    const rmt_transmit_config_t tx_cfg = { .loop_count = 0 };

    while (1) {
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        const omnia_led_mode_t mode = g_led_mode;
        const float br = g_brightness;

        if (mode == OMNIA_LED_MODE_STANDBY_HEART) {
            /* В режиме Standby яркость строго ограничена <= 8% (не слепит ночью в спальне) */
            const float pulse = compute_lubdub_envelope(now_ms) * 0.08f;
            const uint8_t r = (uint8_t)(255.0f * pulse);
            const uint8_t g = (uint8_t)(95.0f  * pulse);
            const uint8_t b = (uint8_t)(18.0f  * pulse);
            set_all_pixels_rgb(r, g, b);
        } else if (mode == OMNIA_LED_MODE_OBSIDIAN_CYAN) {
            /* Фирменный оттенок Obsidian Cyan (#00D4FF) */
            set_all_pixels_rgb(0U, (uint8_t)(212.0f * br), (uint8_t)(255.0f * br));
        } else if (mode == OMNIA_LED_MODE_TUBE_AMBER) {
            /* Тёплый ламповый янтарь (#FFB347) */
            set_all_pixels_rgb((uint8_t)(255.0f * br), (uint8_t)(150.0f * br), (uint8_t)(38.0f * br));
        } else {
            set_all_pixels_rgb(0U, 0U, 0U);
        }

        if (g_rmt_chan && g_bytes_enc) {
            (void)rmt_transmit(g_rmt_chan, g_bytes_enc, g_grb_buf, sizeof(g_grb_buf), &tx_cfg);
            (void)rmt_tx_wait_all_done(g_rmt_chan, pdMS_TO_TICKS(20));
        }

        vTaskDelay(pdMS_TO_TICKS(25)); /* Плавные 40 Гц обновления */
    }
}

esp_err_t omnia_rgb_ring_init(void)
{
    const rmt_tx_channel_config_t tx_chan_cfg = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .gpio_num          = OMNIA_ESP_RGB_RING_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz     = RMT_WS2812_RES_HZ,
        .trans_queue_depth = 2,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_cfg, &g_rmt_chan));

    /* Тайминги WS2812B при разрешении 100 нс: T0H=400нс, T0L=850нс; T1H=800нс, T1L=450нс */
    const rmt_bytes_encoder_config_t bytes_enc_cfg = {
        .bit0 = {
            .level0    = 1,
            .duration0 = 4,
            .level1    = 0,
            .duration1 = 8,
        },
        .bit1 = {
            .level0    = 1,
            .duration0 = 8,
            .level1    = 0,
            .duration1 = 5,
        },
        .flags.msb_first = 1,
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&bytes_enc_cfg, &g_bytes_enc));
    ESP_ERROR_CHECK(rmt_enable(g_rmt_chan));

    xTaskCreatePinnedToCore(rgb_animator_task, "omnia_rgb", 3072, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "Кольцо WS2812 (%d LED) с режимом «lub-dub» запущено на GPIO%d",
             OMNIA_RGB_RING_LED_COUNT, OMNIA_ESP_RGB_RING_GPIO);
    return ESP_OK;
}

void omnia_rgb_ring_set_mode(omnia_led_mode_t mode, float master_brightness_0_1)
{
    if (master_brightness_0_1 < 0.0f) master_brightness_0_1 = 0.0f;
    if (master_brightness_0_1 > 1.0f) master_brightness_0_1 = 1.0f;
    g_brightness = master_brightness_0_1;
    g_led_mode   = mode;
}
