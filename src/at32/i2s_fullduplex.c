/**
 * @file    i2s_fullduplex.c
 * @brief   Полнодуплексный аудио-мастер I2S2 + I2S2EXT (32-bit Phillips, круговой Ping-Pong DMA)
 *          на AT32F403ACGU7 с бесщёлчковой перестройкой Fs, встроенным тестовым синусом и замером % CPU.
 */

#include "omnia_at32.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * Размер одной половины Ping-Pong буфера: 64 стереопары (~1.33 мс задержки при 48 кГц!).
 * В 32-битном формате I2S регистр SPI2->dt передаёт данные двумя 16-битными полусловами (MSB_16, LSB_16),
 * поэтому 1 стереофрейм = 4 полуслова uint16_t; полный двойной буфер = 128 стереопар = 512 uint16_t.
 */
#define AUDIO_HALF_BLOCK_FRAMES     64U
#define AUDIO_FULL_BLOCK_FRAMES     (AUDIO_HALF_BLOCK_FRAMES * 2U)
#define AUDIO_DMA_HALFWORDS_HALF    (AUDIO_HALF_BLOCK_FRAMES * 4U)
#define AUDIO_DMA_HALFWORDS_TOTAL   (AUDIO_FULL_BLOCK_FRAMES * 4U)

static uint16_t g_dma_rx_u16[AUDIO_DMA_HALFWORDS_TOTAL] __attribute__((aligned(8)));
static uint16_t g_dma_tx_u16[AUDIO_DMA_HALFWORDS_TOTAL] __attribute__((aligned(8)));
static int32_t  g_work_in_i32[AUDIO_HALF_BLOCK_FRAMES * 2U];
static int32_t  g_work_out_i32[AUDIO_HALF_BLOCK_FRAMES * 2U];

static omnia_dsp_ctx_t   *g_dsp_ptr       = NULL;
static uint32_t           g_current_fs    = 48000U;
static bool               g_mclk_enabled  = false;
static omnia_i2s_source_t g_source_mode   = OMNIA_SRC_ESP32_I2S;

/* Параметры встроенного эталонного синус-генератора для автономной проверки ЦАПа (Milestone 1) */
static float              g_tone_phase    = 0.0f;
static float              g_tone_step     = 0.0f;
static float              g_tone_amp      = 0.25f;
static volatile uint8_t   g_cpu_load_pct  = 0U;

static void update_tone_generator_step(float freq_hz, float level_dbfs)
{
    if (freq_hz < 20.0f || freq_hz > 20000.0f) {
        freq_hz = 1000.0f;
    }
    if (level_dbfs > 0.0f) {
        level_dbfs = 0.0f;
    }
    g_tone_step = (2.0f * (float)M_PI * freq_hz) / (float)g_current_fs;
    g_tone_amp  = powf(10.0f, level_dbfs * 0.05f) * 2147483647.0f;
}

static void process_half_buffer(size_t half_offset_u16)
{
    const uint32_t t0 = omnia_dwt_cycles();
    const uint16_t *rx = &g_dma_rx_u16[half_offset_u16];
    uint16_t       *tx = &g_dma_tx_u16[half_offset_u16];

    if (g_source_mode == OMNIA_SRC_INTERNAL_TONE) {
        for (size_t f = 0; f < AUDIO_HALF_BLOCK_FRAMES; ++f) {
            const int32_t s = (int32_t)(sinf(g_tone_phase) * g_tone_amp);
            g_tone_phase += g_tone_step;
            if (g_tone_phase >= 2.0f * (float)M_PI) {
                g_tone_phase -= 2.0f * (float)M_PI;
            }
            g_work_in_i32[f * 2U]      = s;
            g_work_in_i32[f * 2U + 1U] = s;
        }
    } else if (g_source_mode == OMNIA_SRC_NONE_SILENCE) {
        memset(g_work_in_i32, 0, sizeof(g_work_in_i32));
    } else {
        /* Распаковка 32-битных сэмплов I2S из пары 16-битных регистров DMA (MSB_16, LSB_16) */
        for (size_t f = 0; f < AUDIO_HALF_BLOCK_FRAMES; ++f) {
            const uint32_t l_u32 = ((uint32_t)rx[f * 4U]      << 16) | rx[f * 4U + 1U];
            const uint32_t r_u32 = ((uint32_t)rx[f * 4U + 2U] << 16) | rx[f * 4U + 3U];
            g_work_in_i32[f * 2U]      = (int32_t)l_u32;
            g_work_in_i32[f * 2U + 1U] = (int32_t)r_u32;
        }
    }

    if (g_dsp_ptr) {
        omnia_dsp_process_block_i32(g_dsp_ptr, g_work_in_i32, g_work_out_i32, AUDIO_HALF_BLOCK_FRAMES);
    } else {
        memcpy(g_work_out_i32, g_work_in_i32, sizeof(g_work_out_i32));
    }

    /* Упаковка 32-битных выходных сэмплов обратно в [MSB_16, LSB_16] для ЦАПа PCM5102A */
    for (size_t f = 0; f < AUDIO_HALF_BLOCK_FRAMES; ++f) {
        const uint32_t lu = (uint32_t)g_work_out_i32[f * 2U];
        const uint32_t ru = (uint32_t)g_work_out_i32[f * 2U + 1U];
        tx[f * 4U]      = (uint16_t)(lu >> 16);
        tx[f * 4U + 1U] = (uint16_t)(lu & 0xFFFFU);
        tx[f * 4U + 2U] = (uint16_t)(ru >> 16);
        tx[f * 4U + 3U] = (uint16_t)(ru & 0xFFFFU);
    }

    /* Точный расчёт процента загрузки ядра по времени бюджета блока */
    const uint32_t elapsed_cycles = omnia_dwt_cycles() - t0;
    const uint32_t budget_cycles  = (system_core_clock / g_current_fs) * AUDIO_HALF_BLOCK_FRAMES;
    if (budget_cycles > 0U) {
        const uint32_t pct = (elapsed_cycles * 100U) / budget_cycles;
        g_cpu_load_pct = (pct > 100U) ? 100U : (uint8_t)pct;
    }
}

/* Прерывание DMA1 Channel 1 (I2S2 Master TX синхронно ведёт и приём I2S2EXT RX) */
void DMA1_Channel1_IRQHandler(void)
{
    if (dma_flag_get(DMA1_HDT1_FLAG) != RESET) {
        dma_flag_clear(DMA1_HDT1_FLAG);
        process_half_buffer(0U);
    }
    if (dma_flag_get(DMA1_FDT1_FLAG) != RESET) {
        dma_flag_clear(DMA1_FDT1_FLAG);
        process_half_buffer(AUDIO_DMA_HALFWORDS_HALF);
    }
}

static void start_i2s2_hardware(uint32_t fs_hz, bool mclk_out)
{
    gpio_init_type gpio_cfg;
    dma_init_type  dma_cfg;
    i2s_init_type  i2s_cfg;

    crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_IOMUX_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_DMA1_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_SPI2_PERIPH_CLOCK, TRUE);

    /* Вывод опционального синхронного MCLK (256*Fs) на пин PA3 через ремап SPI2_GMUX_0001 */
    gpio_pin_remap_config(SPI2_GMUX_0001, mclk_out ? TRUE : FALSE);
    gpio_default_para_init(&gpio_cfg);
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_cfg.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;

    if (mclk_out) {
        gpio_cfg.gpio_mode = GPIO_MODE_MUX;
        gpio_cfg.gpio_pull = GPIO_PULL_NONE;
        gpio_cfg.gpio_pins = GPIO_PINS_3;
        gpio_init(GPIOA, &gpio_cfg);
    } else {
        gpio_cfg.gpio_mode = GPIO_MODE_INPUT;
        gpio_cfg.gpio_pull = GPIO_PULL_DOWN;
        gpio_cfg.gpio_pins = GPIO_PINS_3;
        gpio_init(GPIOA, &gpio_cfg);
    }

    /* PB12 = WS (Master Out), PB13 = CK (Master Out), PB15 = SD (Master TX -> PCM5102A DIN) */
    gpio_cfg.gpio_mode = GPIO_MODE_MUX;
    gpio_cfg.gpio_pull = GPIO_PULL_NONE;
    gpio_cfg.gpio_pins = GPIO_PINS_12 | GPIO_PINS_13 | GPIO_PINS_15;
    gpio_init(GPIOB, &gpio_cfg);

    /* PB14 = I2S2EXT_SD (Full-Duplex RX <- выход 74HC125): по спецификации Artery строго GPIO_MODE_INPUT! */
    gpio_cfg.gpio_mode = GPIO_MODE_INPUT;
    gpio_cfg.gpio_pull = GPIO_PULL_DOWN;
    gpio_cfg.gpio_pins = GPIO_PINS_14;
    gpio_init(GPIOB, &gpio_cfg);

    memset(g_dma_rx_u16, 0, sizeof(g_dma_rx_u16));
    memset(g_dma_tx_u16, 0, sizeof(g_dma_tx_u16));

    /* DMA1 Channel 2: I2S2EXT RX (круговой приём от 74HC125) */
    dma_reset(DMA1_CHANNEL2);
    dma_default_para_init(&dma_cfg);
    dma_cfg.buffer_size           = AUDIO_DMA_HALFWORDS_TOTAL;
    dma_cfg.direction             = DMA_DIR_PERIPHERAL_TO_MEMORY;
    dma_cfg.memory_base_addr      = (uint32_t)g_dma_rx_u16;
    dma_cfg.memory_data_width     = DMA_MEMORY_DATA_WIDTH_HALFWORD;
    dma_cfg.memory_inc_enable     = TRUE;
    dma_cfg.peripheral_base_addr  = (uint32_t)&(I2S2EXT->dt);
    dma_cfg.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_HALFWORD;
    dma_cfg.peripheral_inc_enable = FALSE;
    dma_cfg.priority              = DMA_PRIORITY_VERY_HIGH;
    dma_cfg.loop_mode_enable      = TRUE;
    dma_init(DMA1_CHANNEL2, &dma_cfg);
    dma_flexible_config(DMA1, FLEX_CHANNEL2, DMA_FLEXIBLE_I2S2EXT_RX);

    /* DMA1 Channel 1: SPI2/I2S2 TX (круговая выдача в ЦАП + генерация прерываний HDT/FDT) */
    dma_reset(DMA1_CHANNEL1);
    dma_cfg.direction             = DMA_DIR_MEMORY_TO_PERIPHERAL;
    dma_cfg.memory_base_addr      = (uint32_t)g_dma_tx_u16;
    dma_cfg.peripheral_base_addr  = (uint32_t)&(SPI2->dt);
    dma_init(DMA1_CHANNEL1, &dma_cfg);
    dma_flexible_config(DMA1, FLEX_CHANNEL1, DMA_FLEXIBLE_SPI2_TX);

    dma_interrupt_enable(DMA1_CHANNEL1, DMA_HDT_INT | DMA_FDT_INT, TRUE);
    nvic_irq_enable(DMA1_Channel1_IRQn, 0, 0); /* Высший приоритет прерывания в системе = 0 джиттера! */

    /* Конфигурация I2S2 Master TX + I2S2EXT Slave RX (32-bit Phillips) */
    spi_i2s_reset(SPI2);
    i2s_default_para_init(&i2s_cfg);
    i2s_cfg.audio_protocol      = I2S_AUDIO_PROTOCOL_PHILLIPS;
    i2s_cfg.data_channel_format = I2S_DATA_32BIT_CHANNEL_32BIT;
    i2s_cfg.mclk_output_enable  = mclk_out ? TRUE : FALSE;
    i2s_cfg.audio_sampling_freq = (i2s_audio_sampling_freq_type)fs_hz;
    i2s_cfg.clock_polarity      = I2S_CLOCK_POLARITY_LOW;
    i2s_cfg.operation_mode      = I2S_MODE_MASTER_TX;
    i2s_init(SPI2, &i2s_cfg);

    i2s_cfg.operation_mode      = I2S_MODE_SLAVE_RX;
    i2s_init(I2S2EXT, &i2s_cfg);

    spi_i2s_dma_receiver_enable(I2S2EXT, TRUE);
    spi_i2s_dma_transmitter_enable(SPI2, TRUE);

    dma_channel_enable(DMA1_CHANNEL2, TRUE);
    dma_channel_enable(DMA1_CHANNEL1, TRUE);

    i2s_enable(I2S2EXT, TRUE);
    i2s_enable(SPI2, TRUE);
}

static void stop_i2s2_hardware(void)
{
    dma_interrupt_enable(DMA1_CHANNEL1, DMA_HDT_INT | DMA_FDT_INT, FALSE);
    dma_channel_enable(DMA1_CHANNEL1, FALSE);
    dma_channel_enable(DMA1_CHANNEL2, FALSE);
    i2s_enable(SPI2, FALSE);
    i2s_enable(I2S2EXT, FALSE);
    spi_i2s_reset(SPI2);
}

void omnia_i2s_audio_init(omnia_dsp_ctx_t *dsp_ctx)
{
    g_dsp_ptr      = dsp_ctx;
    g_current_fs   = 48000U;
    g_mclk_enabled = false;
    update_tone_generator_step(1000.0f, -12.0f);
    start_i2s2_hardware(g_current_fs, g_mclk_enabled);
}

/**
 * @brief  Строгая бесщёлчковая перестройка частоты Fs (Policy 1):
 *         XSMT Mute -> стоп DMA -> пересчёт DSP под новый Fs -> рестарт I2S2 -> пауза стабилизации -> XSMT Unmute.
 */
bool omnia_i2s_reconfigure_fs(uint32_t new_fs_hz, bool enable_mclk)
{
    if (new_fs_hz != 44100U && new_fs_hz != 48000U && new_fs_hz != 96000U) {
        return false;
    }
    if (new_fs_hz == g_current_fs && enable_mclk == g_mclk_enabled) {
        return true;
    }

    omnia_hw_set_dac_xsmt(false);
    omnia_delay_ms(8U);

    stop_i2s2_hardware();
    g_current_fs   = new_fs_hz;
    g_mclk_enabled = enable_mclk;

    if (g_dsp_ptr) {
        omnia_dsp_set_sample_rate(g_dsp_ptr, new_fs_hz);
    }
    update_tone_generator_step(1000.0f, -12.0f);

    start_i2s2_hardware(g_current_fs, g_mclk_enabled);
    omnia_delay_ms(20U);

    if (g_dsp_ptr && !g_dsp_ptr->muted) {
        omnia_hw_set_dac_xsmt(true);
    }
    return true;
}

void omnia_i2s_set_source_mode(omnia_i2s_source_t src, float tone_hz, float tone_dbfs)
{
    g_source_mode = src;
    if (src == OMNIA_SRC_INTERNAL_TONE) {
        omnia_hw_select_i2s_mux(OMNIA_SRC_NONE_SILENCE);
        update_tone_generator_step(tone_hz, tone_dbfs);
    } else {
        omnia_hw_select_i2s_mux(src);
    }
}

omnia_i2s_source_t omnia_i2s_get_source_mode(void)
{
    return g_source_mode;
}

uint32_t omnia_i2s_get_current_fs(void)
{
    return g_current_fs;
}

bool omnia_i2s_is_mclk_enabled(void)
{
    return g_mclk_enabled;
}

uint8_t omnia_i2s_get_cpu_load_pct(void)
{
    return g_cpu_load_pct;
}
