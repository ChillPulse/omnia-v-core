/*
 * Core 0: аудио-движок. ТЗ, Том 2, Гл. 4 + Том 3, Гл. 2 (DSP) + Том 1 (Bit-Perfect).
 *
 * Конвейер блока (512 стерео-кадров):
 *   Ring Buffer (PSRAM) -> декодирование в float32 -> DSP (Room + PEQ + AGC)
 *   -> громкость -> FFT-спектр (в очередь для UI) -> I2S DMA, слот 32 бит.
 *
 * Bit-Perfect: исходные 16/24/32-бит целые переводятся в float32 без апсемплинга;
 * частота I2S перенастраивается под поток. Контейнер I2S фиксирован 32 бит (Padded),
 * поэтому разрядность источника не урезается. См. README, раздел «Bit-Perfect».
 */

#include "omnia.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>
#include <math.h>

static const char* TAG = "OMNIA_AUDIO_CORE0";

static i2s_chan_handle_t tx_channel = NULL;

// ---------- I2S ----------
void init_i2s_dma_bit_perfect(uint32_t sample_rate, uint8_t bits_per_sample)
{
    (void)bits_per_sample;   // контейнер всегда 32-бит Padded (PCM5102A)

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;   // при пустом буфере DMA выдает тишину, без хрипа
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_channel, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,   // PCM5102A формирует MCLK внутри из BCK
            .bclk = (gpio_num_t)PIN_I2S_BCK,
            .ws   = (gpio_num_t)PIN_I2S_LRCK,
            .dout = (gpio_num_t)PIN_I2S_DIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_channel, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_channel));
    ESP_LOGI(TAG, "I2S0 запущен: %u Гц, контейнер 32 бит", (unsigned)sample_rate);
}

static void i2s_set_rate(uint32_t rate)
{
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    esp_err_t e = i2s_channel_disable(tx_channel);
    if (e == ESP_OK) e = i2s_channel_reconfig_std_clock(tx_channel, &clk);
    if (e == ESP_OK) e = i2s_channel_enable(tx_channel);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Не удалось перенастроить I2S на %u Гц: %s", (unsigned)rate, esp_err_to_name(e));
    }
}

// ---------- Декодирование в float32 ----------
static inline float sample_at(const uint8_t* p, uint8_t bits)
{
    switch (bits) {
        case 16: {
            int16_t v = (int16_t)((uint16_t)(p[0] | (p[1] << 8)));
            return v / 32768.0f;
        }
        case 24: {
            int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24);
            return (v >> 8) / 8388608.0f;
        }
        case 32: {
            int32_t v = (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 |
                                  (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
            return v / 2147483648.0f;
        }
        default:
            return 0.0f;
    }
}

static void decode_to_float(const uint8_t* src, size_t frames, const omnia_audio_format_t& f, float* dst)
{
    size_t bps = f.bits / 8;
    size_t fb  = bps * f.channels;
    for (size_t i = 0; i < frames; i++) {
        const uint8_t* p = src + i * fb;
        float l = sample_at(p, f.bits);
        float r = (f.channels > 1) ? sample_at(p + bps, f.bits) : l;
        dst[2 * i]     = l;
        dst[2 * i + 1] = r;
    }
}

// ---------- Розовый шум для Room Correction Wizard (Kellet) ----------
static uint32_t s_rng = 0x2545F491u;
static float white_noise(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(int32_t)s_rng / 2147483648.0f;
}
static float pink_noise(float w, float* b)
{
    b[0] = 0.99886f * b[0] + w * 0.0555179f;
    b[1] = 0.99332f * b[1] + w * 0.0750759f;
    b[2] = 0.96900f * b[2] + w * 0.1538520f;
    b[3] = 0.86650f * b[3] + w * 0.3104856f;
    b[4] = 0.55000f * b[4] + w * 0.5329522f;
    b[5] = -0.7616f * b[5] - w * 0.0168980f;
    float p = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f;
    b[6] = w * 0.115926f;
    return p * 0.11f;
}

// ---------- Аудио-задача ----------
void vTaskAudioEngine(void* pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "Audio Engine запущен на ядре %d", xPortGetCoreID());

    omnia_audio_format_t fmt = {44100, 16, 2};
    init_i2s_dma_bit_perfect(fmt.sample_rate, 32);
    uint32_t cur_rate = fmt.sample_rate;

    const size_t max_src_bytes = FRAMES_PER_BLOCK * 8;   // 32-бит стерео — максимум
    uint8_t* src   = (uint8_t*)heap_caps_malloc(max_src_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    float*   work  = (float*)heap_caps_malloc(FRAMES_PER_BLOCK * 2 * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int32_t* out   = (int32_t*)heap_caps_malloc(FRAMES_PER_BLOCK * 2 * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!src || !work || !out) {
        ESP_LOGE(TAG, "Нет внутренней DMA-памяти для аудио-буферов");
        vTaskDelete(NULL);
        return;
    }

    omnia_spectrum_t spec;
    float pink_state[7] = {0};
    size_t carry = 0;   // хвост неполного кадра между блоками

    while (1) {
        omnia_audio_format_t nf;
        if (xQueueReceive(audio_format_queue, &nf, 0) == pdTRUE) {
            ESP_LOGI(TAG, "Смена формата: %u Гц / %u бит / %u кан.",
                     (unsigned)nf.sample_rate, nf.bits, nf.channels);
            if (nf.sample_rate != cur_rate) {
                i2s_set_rate(nf.sample_rate);
                cur_rate = nf.sample_rate;
            }
            fmt = nf;
            carry = 0;
            audio_buffer.clear();
        }

        omnia_state_t st;
        omnia_state_snapshot(&st);

        if (st.play_state == OMNIA_PLAY_STANDBY || st.play_state == OMNIA_PLAY_STOPPED ||
            st.play_state == OMNIA_PLAY_PAUSED) {
            if (!st.test_noise) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
        }

        if (st.play_state == OMNIA_PLAY_SEEKING) {
            // Перемотка: глушим поток, очищаем буфер, ждем, затем возобновляем
            audio_buffer.clear();
            carry = 0;
            vTaskDelay(pdMS_TO_TICKS(200));
            omnia_set_play_state(OMNIA_PLAY_PLAYING);
            continue;
        }

        size_t frames = 0;
        bool noise = st.test_noise;

        if (noise) {
            frames = FRAMES_PER_BLOCK;
            for (size_t i = 0; i < frames; i++) {
                float p = pink_noise(white_noise(), pink_state) * 0.25f;   // ~ -12 dBFS
                work[2 * i]     = p;
                work[2 * i + 1] = p;
            }
        } else {
            size_t fb  = (size_t)fmt.channels * (fmt.bits / 8);
            size_t cap = FRAMES_PER_BLOCK * fb;
            size_t got = audio_buffer.read(src + carry, cap - carry);
            size_t total = carry + got;
            frames = total / fb;
            carry = total - frames * fb;
            if (carry) memmove(src, src + frames * fb, carry);
            if (frames == 0) {
                vTaskDelay(pdMS_TO_TICKS(4));
                continue;
            }
            decode_to_float(src, frames, fmt, work);
        }

        // DSP: Room Correction -> User PEQ -> AGC/anti-AD (не применяется к тестовому шуму)
        if (!noise) {
            dsp_processor.process_stereo_samples(work, frames, st.anti_ad);
        }

        // Громкость (квадратичная кривая) и mute
        float gain = st.is_muted ? 0.0f : powf(st.volume / 100.0f, 2.0f);
        for (size_t i = 0; i < frames * 2; i++) {
            work[i] *= gain;
        }

        // Спектр: последние 512 кадров блока -> 64 полосы -> очередь UI (Core 1)
        if (frames >= FFT_SIZE) {
            float mono[FFT_SIZE];
            for (size_t i = 0; i < FFT_SIZE; i++) {
                size_t f = frames - FFT_SIZE + i;
                mono[i] = 0.5f * (work[2 * f] + work[2 * f + 1]);
            }
            compute_spectrum_bands(mono, fmt.sample_rate, spec.bands);
            xQueueOverwrite(spectrum_queue, &spec);
        }

        // float32 -> 32-бит Padded (клиппинг по ±1.0)
        for (size_t i = 0; i < frames * 2; i++) {
            float v = work[i];
            if (v > 1.0f) v = 1.0f;
            else if (v < -1.0f) v = -1.0f;
            out[i] = (int32_t)(v * 2147483647.0f);
        }

        size_t written = 0;
        i2s_channel_write(tx_channel, out, frames * 2 * sizeof(int32_t), &written, portMAX_DELAY);
    }
}
