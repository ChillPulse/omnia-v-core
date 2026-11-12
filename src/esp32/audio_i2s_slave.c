/**
 * @file    audio_i2s_slave.c
 * @brief   Аудио-ядро Core 1 на ESP32-S3-N16R8: ведомый передатчик I2S0 Slave TX (32-bit Phillips)
 *          с нулевым джиттером от мастера AT32F403, DMA-пулом 24 КБ во внутренней SRAM
 *          и кольцевым PCM-буфером 2.5 МБ в Octal PSRAM.
 */

#include "omnia_esp32.h"
#include <math.h>
#include <string.h>
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "omnia_i2s_slave";

#define I2S_FEED_CHUNK_FRAMES   256U

static i2s_chan_handle_t     g_tx_chan       = NULL;
static omnia_static_pools_t *g_pools         = NULL;
static SemaphoreHandle_t     g_ring_lock      = NULL;
static SemaphoreHandle_t     g_i2s_lock       = NULL;

static portMUX_TYPE          g_tone_lock     = portMUX_INITIALIZER_UNLOCKED;
static uint32_t              g_fs_hz         = 48000U;
static bool                  g_test_tone_en  = false;
static float                 g_tone_phase    = 0.0f;
static float                 g_tone_freq_hz  = 1000.0f;
static float                 g_tone_amp      = 0.125f;

static size_t pcm_ring_pop(int32_t *dst_lr, size_t max_frames)
{
    if (!g_pools || !g_pools->pcm_ring) {
        return 0U;
    }
    const size_t frame_bytes = sizeof(int32_t) * 2U;

    xSemaphoreTake(g_ring_lock, portMAX_DELAY);
    const size_t avail_frames = g_pools->pcm_filled / frame_bytes;
    const size_t to_read = (avail_frames < max_frames) ? avail_frames : max_frames;
    size_t bytes_rem = to_read * frame_bytes;
    uint8_t *out_p = (uint8_t *)dst_lr;

    while (bytes_rem > 0U) {
        size_t chunk = g_pools->pcm_size - g_pools->pcm_rd;
        if (chunk > bytes_rem) {
            chunk = bytes_rem;
        }
        memcpy(out_p, &g_pools->pcm_ring[g_pools->pcm_rd], chunk);
        g_pools->pcm_rd = (g_pools->pcm_rd + chunk) % g_pools->pcm_size;
        g_pools->pcm_filled -= chunk;
        out_p     += chunk;
        bytes_rem -= chunk;
    }
    xSemaphoreGive(g_ring_lock);
    return to_read;
}

size_t omnia_audio_pcm_write(const int32_t *interleaved_lr, size_t stereo_frames)
{
    if (!g_pools || !g_pools->pcm_ring || !interleaved_lr) {
        return 0U;
    }
    const size_t frame_bytes = sizeof(int32_t) * 2U;

    xSemaphoreTake(g_ring_lock, portMAX_DELAY);
    const size_t free_frames = (g_pools->pcm_size - g_pools->pcm_filled) / frame_bytes;
    const size_t to_write = (stereo_frames < free_frames) ? stereo_frames : free_frames;
    size_t bytes_rem = to_write * frame_bytes;
    const uint8_t *in_p = (const uint8_t *)interleaved_lr;

    while (bytes_rem > 0U) {
        size_t chunk = g_pools->pcm_size - g_pools->pcm_wr;
        if (chunk > bytes_rem) {
            chunk = bytes_rem;
        }
        memcpy(&g_pools->pcm_ring[g_pools->pcm_wr], in_p, chunk);
        g_pools->pcm_wr = (g_pools->pcm_wr + chunk) % g_pools->pcm_size;
        g_pools->pcm_filled += chunk;
        in_p      += chunk;
        bytes_rem -= chunk;
    }
    xSemaphoreGive(g_ring_lock);
    return to_write;
}

static void core1_i2s_feeder_task(void *arg)
{
    (void)arg;
    /* Выделяем рабочий блок в быстрой внутренней SRAM для прямой передачи в DMA */
    int32_t *block = heap_caps_malloc(I2S_FEED_CHUNK_FRAMES * 2U * sizeof(int32_t),
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!block) {
        ESP_LOGE(TAG, "Не удалось выделить рабочий буфер I2S во внутренней SRAM!");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        portENTER_CRITICAL(&g_tone_lock);
        const bool tone_en = g_test_tone_en;
        const float tone_hz = g_tone_freq_hz;
        const float tone_amp = g_tone_amp;
        const uint32_t fs_hz = g_fs_hz;
        portEXIT_CRITICAL(&g_tone_lock);
        if (tone_en) {
            const float step = (2.0f * (float)M_PI * tone_hz) / (float)fs_hz;
            const float scale = tone_amp * 2147483647.0f;
            for (size_t f = 0; f < I2S_FEED_CHUNK_FRAMES; ++f) {
                const int32_t s = (int32_t)(sinf(g_tone_phase) * scale);
                g_tone_phase += step;
                if (g_tone_phase >= 2.0f * (float)M_PI) {
                    g_tone_phase -= 2.0f * (float)M_PI;
                }
                block[f * 2U]      = s;
                block[f * 2U + 1U] = s;
            }
        } else {
            const size_t got = pcm_ring_pop(block, I2S_FEED_CHUNK_FRAMES);
            if (got < I2S_FEED_CHUNK_FRAMES) {
                /* При пустом буфере заполняем хвост идеальной цифровой тишиной (0 щелчков) */
                memset(&block[got * 2U], 0, (I2S_FEED_CHUNK_FRAMES - got) * 2U * sizeof(int32_t));
            }
        }

        size_t bytes_written = 0U;
        xSemaphoreTake(g_i2s_lock, portMAX_DELAY);
        if (g_tx_chan) {
            /* Если AT32 перестраивает клок, таймаут 100 мс предотвратит зависание задачи */
            (void)i2s_channel_write(g_tx_chan,
                                    block,
                                    I2S_FEED_CHUNK_FRAMES * 2U * sizeof(int32_t),
                                    &bytes_written,
                                    pdMS_TO_TICKS(100));
        }
        xSemaphoreGive(g_i2s_lock);

        if (bytes_written == 0U) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

esp_err_t omnia_audio_i2s_init(omnia_static_pools_t *pools, uint32_t initial_fs_hz)
{
    g_pools    = pools;
    g_fs_hz    = initial_fs_hz;
    g_ring_lock = xSemaphoreCreateMutex();
    g_i2s_lock  = xSemaphoreCreateMutex();
    if (!g_ring_lock || !g_i2s_lock) {
        return ESP_ERR_NO_MEM;
    }

    /* Строго 8 дескрипторов по 384 стереофрейма (32-bit L+R) = ровно 24 КБ Internal DMA SRAM */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    chan_cfg.dma_desc_num  = OMNIA_DMA_DESC_COUNT;
    chan_cfg.dma_frame_num = OMNIA_DMA_FRAMES_PER_DESC;
    chan_cfg.auto_clear    = true;

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &g_tx_chan, NULL));

    const i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(initial_fs_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = OMNIA_ESP_I2S_BCK_GPIO,
            .ws   = OMNIA_ESP_I2S_WS_GPIO,
            .dout = OMNIA_ESP_I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(g_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(g_tx_chan));

    /* Жёстко закрепляем задачу отдачи аудио за Core 1 с высоким приоритетом 22 */
    if (xTaskCreatePinnedToCore(core1_i2s_feeder_task, "omnia_i2s_tx", 4096, NULL, 22, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Не удалось создать задачу I2S TX");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "I2S0 Slave TX запущен на Core 1 (BCK=GPIO%d, WS=GPIO%d, DOUT=GPIO%d, DMA=24 КБ)",
             OMNIA_ESP_I2S_BCK_GPIO, OMNIA_ESP_I2S_WS_GPIO, OMNIA_ESP_I2S_DOUT_GPIO);
    return ESP_OK;
}

esp_err_t omnia_audio_i2s_reconfig_clock(uint32_t new_fs_hz)
{
    if (!g_tx_chan) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(g_i2s_lock, portMAX_DELAY);
    esp_err_t err = i2s_channel_disable(g_tx_chan);
    if (err == ESP_OK) {
        const i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(new_fs_hz);
        err = i2s_channel_reconfig_std_clock(g_tx_chan, &clk_cfg);
        if (err == ESP_OK) {
            portENTER_CRITICAL(&g_tone_lock);
            g_fs_hz = new_fs_hz;
            portEXIT_CRITICAL(&g_tone_lock);
            err = i2s_channel_enable(g_tx_chan);
        }
    }
    xSemaphoreGive(g_i2s_lock);
    return err;
}

void omnia_audio_set_test_generator(bool enable, float freq_hz, float level_dbfs)
{
    const bool change_level = level_dbfs <= 0.0f && level_dbfs >= -120.0f;
    const float new_amp = change_level ? powf(10.0f, level_dbfs * 0.05f) : 0.0f;
    portENTER_CRITICAL(&g_tone_lock);
    if (freq_hz >= 20.0f && freq_hz <= 20000.0f) g_tone_freq_hz = freq_hz;
    if (change_level) g_tone_amp = new_amp;
    g_test_tone_en = enable;
    portEXIT_CRITICAL(&g_tone_lock);
}
