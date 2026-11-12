/**
 * @file    bt_receiver.c
 * @brief   Цифровой 24-битный Bluetooth-приёмник AT32F403ACGU7 (Zero-Mux Architecture):
 *            1) I2S3 Slave RX (PA15=WS, PB3=CK, PB5=SD) на DMA2_Channel1 -> Farrow Cubic ASRC
 *            2) Автоопределение реальной частоты потока BT (44.1 кГц SBC/AAC vs 48 кГц aptX-HD)
 *            3) Двухпротокольный мост UART4 (PA0=TX, PA1=RX @ 115200):
 *               одновременно понимает кастомную прошивку OMNIA-BTM334 (QCC3034 AVRCP 1.6)
 *               и AT-команды модуля BT401 Master!
 */

#include "at32f403a_407.h"
#include "omnia_at32.h"
#include <string.h>
#include <stdlib.h>

#define BT_DMA_HALF_FRAMES          64U
#define BT_DMA_WORDS_PER_HALF       (BT_DMA_HALF_FRAMES * 4U)   /* L_hi, L_lo, R_hi, R_lo */
#define BT_DMA_TOTAL_WORDS          (BT_DMA_WORDS_PER_HALF * 2U)

static uint16_t             s_bt_i2s_dma_buf[BT_DMA_TOTAL_WORDS] __attribute__((aligned(4)));
static omnia_asrc_t         s_bt_asrc __attribute__((section(".sram_ext"), aligned(4)));

static volatile uint32_t    s_rx_frames_accum = 0U;
static uint32_t             s_rate_check_ms   = 0U;

/* Кольцевой буфер приёма строк метаданных по UART4 */
static volatile uint8_t     s_uart4_ring[256];
static volatile uint8_t     s_uart4_wr = 0U;
static uint8_t              s_uart4_rd = 0U;
static char                 s_line_buf[160];
static uint8_t              s_line_len = 0U;

static omnia_bt_meta_evt_t  s_bt_meta;
static bool                 s_meta_dirty = false;

static void uart4_send_str(const char *s)
{
    if (s == NULL) {
        return;
    }
    while (*s != '\0') {
        while (usart_flag_get(UART4, USART_TDBE_FLAG) == RESET) {}
        usart_data_transmit(UART4, (uint16_t)(uint8_t)(*s++));
    }
}

static void unpack_bt_half_to_asrc(const uint16_t *src_words)
{
    int32_t lr_block[BT_DMA_HALF_FRAMES * 2U];
    for (uint32_t i = 0U; i < BT_DMA_HALF_FRAMES; i++) {
        const uint32_t l32 = ((uint32_t)src_words[i * 4U + 0U] << 16) | (uint32_t)src_words[i * 4U + 1U];
        const uint32_t r32 = ((uint32_t)src_words[i * 4U + 2U] << 16) | (uint32_t)src_words[i * 4U + 3U];
        lr_block[i * 2U + 0U] = (int32_t)l32;
        lr_block[i * 2U + 1U] = (int32_t)r32;
    }
    omnia_asrc_push_interleaved(&s_bt_asrc, lr_block, BT_DMA_HALF_FRAMES);
    s_rx_frames_accum += BT_DMA_HALF_FRAMES;
}

void DMA2_Channel1_IRQHandler(void)
{
    if (dma_flag_get(DMA2_HDT1_FLAG) != RESET) {
        dma_flag_clear(DMA2_HDT1_FLAG);
        unpack_bt_half_to_asrc(&s_bt_i2s_dma_buf[0]);
    }
    if (dma_flag_get(DMA2_FDT1_FLAG) != RESET) {
        dma_flag_clear(DMA2_FDT1_FLAG);
        unpack_bt_half_to_asrc(&s_bt_i2s_dma_buf[BT_DMA_WORDS_PER_HALF]);
    }
}

void UART4_IRQHandler(void)
{
    if (usart_flag_get(UART4, USART_RDBF_FLAG) != RESET) {
        const uint8_t b = (uint8_t)usart_data_receive(UART4);
        s_uart4_ring[s_uart4_wr++] = b;
    }
}

static void copy_clean_utf8(char *dst, uint32_t cap, const char *src)
{
    if (dst == NULL || cap == 0U || src == NULL) {
        return;
    }
    uint32_t i = 0U;
    while (src[i] != '\0' && i < (cap - 1U)) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void parse_bt_uart_line(const char *line)
{
    /* 1. Диалект кастомной прошивки OMNIA-BTM334 (QCC3034 ADK) и FSC-BT1026 */
    if (strncmp(line, "+STAT=", 6) == 0) {
        if (strstr(line + 6, "CONN") != NULL) {
            s_bt_meta.connected = 1U;
        } else if (strstr(line + 6, "DISC") != NULL) {
            s_bt_meta.connected = 0U;
            s_bt_meta.playing   = 0U;
            s_bt_meta.codec_id  = OMNIA_BT_CODEC_NONE;
        }
        s_meta_dirty = true;
    } else if (strncmp(line, "+PLAY=", 6) == 0 || strncmp(line, "+PLAYSTAT=", 10) == 0) {
        const char *val = strchr(line, '=');
        if (val != NULL) {
            s_bt_meta.playing = (val[1] == '1') ? 1U : 0U;
            if (s_bt_meta.playing != 0U) {
                s_bt_meta.connected = 1U;
            }
            s_meta_dirty = true;
        }
    } else if (strncmp(line, "+CODEC=", 7) == 0) {
        const char *c = line + 7;
        if (strstr(c, "APTXHD") != NULL || strstr(c, "APTX-HD") != NULL) {
            s_bt_meta.codec_id  = OMNIA_BT_CODEC_APTX_HD;
            s_bt_meta.bit_depth = 24U;
        } else if (strstr(c, "APTXLL") != NULL) {
            s_bt_meta.codec_id  = OMNIA_BT_CODEC_APTX_LL;
            s_bt_meta.bit_depth = 16U;
        } else if (strstr(c, "APTX") != NULL) {
            s_bt_meta.codec_id  = OMNIA_BT_CODEC_APTX;
            s_bt_meta.bit_depth = 16U;
        } else if (strstr(c, "AAC") != NULL) {
            s_bt_meta.codec_id  = OMNIA_BT_CODEC_AAC;
            s_bt_meta.bit_depth = 24U;
        } else {
            s_bt_meta.codec_id  = OMNIA_BT_CODEC_SBC;
            s_bt_meta.bit_depth = 16U;
        }
        s_bt_meta.connected = 1U;
        s_meta_dirty = true;
    } else if (strncmp(line, "+SR=", 4) == 0) {
        const uint32_t sr = (uint32_t)strtoul(line + 4, NULL, 10);
        if (sr == 44100U || sr == 48000U) {
            s_bt_meta.sample_rate_hz = sr;
            omnia_asrc_set_rates(&s_bt_asrc, sr, s_bt_asrc.out_fs_hz);
            s_meta_dirty = true;
        }
    } else if (strncmp(line, "+TITLE=", 7) == 0) {
        copy_clean_utf8(s_bt_meta.title, sizeof(s_bt_meta.title), line + 7);
        s_meta_dirty = true;
    } else if (strncmp(line, "+ARTIST=", 8) == 0) {
        copy_clean_utf8(s_bt_meta.artist, sizeof(s_bt_meta.artist), line + 8);
        s_meta_dirty = true;
    }
    /* 2. Диалект модуля BT401 Master (KT1025A: TS+00/01/02, MF+...) */
    else if (strncmp(line, "TS+", 3) == 0) {
        if (strncmp(line + 3, "00", 2) == 0) {
            s_bt_meta.connected = 0U;
            s_bt_meta.playing   = 0U;
            s_bt_meta.codec_id  = OMNIA_BT_CODEC_NONE;
        } else if (strncmp(line + 3, "01", 2) == 0) {
            s_bt_meta.connected = 1U;
            s_bt_meta.playing   = 0U;
            if (s_bt_meta.codec_id == OMNIA_BT_CODEC_NONE) {
                s_bt_meta.codec_id  = OMNIA_BT_CODEC_SBC;
                s_bt_meta.bit_depth = 16U;
            }
        } else if (strncmp(line + 3, "02", 2) == 0) {
            s_bt_meta.connected = 1U;
            s_bt_meta.playing   = 1U;
            if (s_bt_meta.codec_id == OMNIA_BT_CODEC_NONE) {
                s_bt_meta.codec_id  = OMNIA_BT_CODEC_SBC;
                s_bt_meta.bit_depth = 16U;
            }
        }
        s_meta_dirty = true;
    } else if (strncmp(line, "MF+", 3) == 0) {
        copy_clean_utf8(s_bt_meta.title, sizeof(s_bt_meta.title), line + 3);
        s_meta_dirty = true;
    }
}

void omnia_bt_receiver_init(uint32_t out_fs_hz)
{
    memset(&s_bt_meta, 0, sizeof(s_bt_meta));
    s_bt_meta.sample_rate_hz = 48000U;
    s_bt_meta.bit_depth      = 24U;
    omnia_asrc_init(&s_bt_asrc, 48000U, (out_fs_hz != 0U) ? out_fs_hz : 48000U);

    crm_periph_clock_enable(CRM_IOMUX_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_SPI3_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_UART4_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_DMA2_PERIPH_CLOCK, TRUE);

    /* Ремап I2S3 на PA15(WS), PB3(CK), PB5(SD) + сохранение SWD на PA13/PA14 */
    gpio_pin_remap_config(SWJTAG_GMUX_010, TRUE);
    gpio_pin_remap_config(SPI3_GMUX_0010, TRUE);
    /* Ремап UART4 на PA0(TX), PA1(RX) */
    gpio_pin_remap_config(UART4_GMUX_0010, TRUE);

    gpio_init_type gpio;
    gpio_default_para_init(&gpio);

    /* PA15 = I2S3_WS (Slave In), PB3 = I2S3_CK (Slave In), PB5 = I2S3_SD (Slave In) */
    gpio.gpio_mode = GPIO_MODE_INPUT;
    gpio.gpio_pull = GPIO_PULL_DOWN;
    gpio.gpio_pins = GPIO_PINS_15;
    gpio_init(GPIOA, &gpio);

    gpio.gpio_pins = GPIO_PINS_3 | GPIO_PINS_5;
    gpio_init(GPIOB, &gpio);

    /* PA0 = UART4_TX, PA1 = UART4_RX */
    gpio.gpio_mode           = GPIO_MODE_MUX;
    gpio.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
    gpio.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
    gpio.gpio_pull           = GPIO_PULL_NONE;
    gpio.gpio_pins           = GPIO_PINS_0;
    gpio_init(GPIOA, &gpio);

    gpio.gpio_mode = GPIO_MODE_INPUT;
    gpio.gpio_pull = GPIO_PULL_UP;
    gpio.gpio_pins = GPIO_PINS_1;
    gpio_init(GPIOA, &gpio);

    /* Настройка DMA2_Channel1 с гибким мультиплексором на SPI3_RX (I2S3_RX) */
    dma_reset(DMA2_CHANNEL1);
    dma_flexible_config(DMA2, FLEX_CHANNEL1, DMA_FLEXIBLE_SPI3_RX);

    dma_init_type dma;
    dma_default_para_init(&dma);
    dma.buffer_size           = BT_DMA_TOTAL_WORDS;
    dma.direction             = DMA_DIR_PERIPHERAL_TO_MEMORY;
    dma.memory_base_addr      = (uint32_t)s_bt_i2s_dma_buf;
    dma.memory_data_width     = DMA_MEMORY_DATA_WIDTH_HALFWORD;
    dma.memory_inc_enable     = TRUE;
    dma.peripheral_base_addr  = (uint32_t)&(SPI3->dt);
    dma.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_HALFWORD;
    dma.peripheral_inc_enable = FALSE;
    dma.priority              = DMA_PRIORITY_HIGH;
    dma.loop_mode_enable      = TRUE;
    dma_init(DMA2_CHANNEL1, &dma);
    dma_interrupt_enable(DMA2_CHANNEL1, DMA_HDT_INT | DMA_FDT_INT, TRUE);

    /* Настройка I2S3 в режиме 24-bit Slave RX (принимает и 24-bit BTM334, и 16-bit BT401 в 32-bit слотах) */
    spi_i2s_reset(SPI3);
    i2s_init_type i2s;
    i2s_default_para_init(&i2s);
    i2s.audio_protocol       = I2S_AUDIO_PROTOCOL_PHILLIPS;
    i2s.data_channel_format  = I2S_DATA_24BIT_CHANNEL_32BIT;
    i2s.mclk_output_enable   = FALSE;
    i2s.audio_sampling_freq  = I2S_AUDIO_FREQUENCY_48K;
    i2s.clock_polarity       = I2S_CLOCK_POLARITY_LOW;
    i2s.operation_mode       = I2S_MODE_SLAVE_RX;
    i2s_init(SPI3, &i2s);

    spi_i2s_dma_receiver_enable(SPI3, TRUE);
    nvic_irq_enable(DMA2_Channel1_IRQn, 2, 0);
    dma_channel_enable(DMA2_CHANNEL1, TRUE);
    i2s_enable(SPI3, TRUE);

    /* Настройка UART4 @ 115200 8N1 */
    usart_init(UART4, 115200U, USART_DATA_8BITS, USART_STOP_1_BIT);
    usart_transmitter_enable(UART4, TRUE);
    usart_receiver_enable(UART4, TRUE);
    usart_interrupt_enable(UART4, USART_RDBF_INT, TRUE);
    nvic_irq_enable(UART4_IRQn, 5, 0);
    usart_enable(UART4, TRUE);
}

void omnia_bt_receiver_notify_out_fs(uint32_t out_fs_hz)
{
    omnia_asrc_set_rates(&s_bt_asrc, s_bt_asrc.in_fs_hz, out_fs_hz);
}

void omnia_bt_receiver_pull_audio(int32_t *out_lr_q31, uint32_t frames)
{
    omnia_asrc_pull_interleaved(&s_bt_asrc, out_lr_q31, frames);
}

void omnia_bt_receiver_poll(uint32_t now_ms)
{
    /* 1. Разбор входящих строк метаданных от BTM334 / BT401 */
    while (s_uart4_rd != s_uart4_wr) {
        const char ch = (char)s_uart4_ring[s_uart4_rd++];
        if (ch == '\r' || ch == '\n') {
            if (s_line_len > 0U) {
                s_line_buf[s_line_len] = '\0';
                parse_bt_uart_line(s_line_buf);
                s_line_len = 0U;
            }
        } else if (s_line_len < (sizeof(s_line_buf) - 1U)) {
            s_line_buf[s_line_len++] = ch;
        }
    }

    /* 2. Аппаратный частотомер I2S3 (окно 250 мс): сам различает 44.1 кГц и 48.0 кГц даже без UART! */
    if ((uint32_t)(now_ms - s_rate_check_ms) >= 250U) {
        const uint32_t dt_ms = now_ms - s_rate_check_ms;
        s_rate_check_ms = now_ms;

        const uint32_t frames = s_rx_frames_accum;
        s_rx_frames_accum = 0U;

        if (dt_ms > 0U) {
            const uint32_t est_fs = (frames * 1000U) / dt_ms;
            uint32_t detected_fs = 0U;
            if (est_fs >= 42500U && est_fs <= 46000U) {
                detected_fs = 44100U;
            } else if (est_fs > 46000U && est_fs <= 50500U) {
                detected_fs = 48000U;
            }

            if (detected_fs != 0U && detected_fs != s_bt_asrc.in_fs_hz) {
                omnia_asrc_set_rates(&s_bt_asrc, detected_fs, s_bt_asrc.out_fs_hz);
                s_bt_meta.sample_rate_hz = detected_fs;
                s_meta_dirty = true;
            }
        }
    }

    if (s_meta_dirty) {
        s_meta_dirty = false;
        omnia_uart_send_bt_meta(&s_bt_meta);
    }
}

void omnia_bt_receiver_exec_cmd(const omnia_bt_ctrl_cmd_t *cmd)
{
    if (cmd == NULL) {
        return;
    }
    /* Универсальная отправка: первая команда для OMNIA-BTM334 (QCC3034), вторая для BT401 */
    switch ((omnia_bt_action_t)cmd->action) {
    case OMNIA_BT_ACT_PLAY_PAUSE:
        uart4_send_str("AT+PLAYPAUSE\r\nAT+CB\r\n");
        break;
    case OMNIA_BT_ACT_NEXT:
        uart4_send_str("AT+FWD\r\nAT+CC\r\n");
        break;
    case OMNIA_BT_ACT_PREV:
        uart4_send_str("AT+BACK\r\nAT+CD\r\n");
        break;
    case OMNIA_BT_ACT_PAIRING:
        uart4_send_str("AT+PAIR\r\nAT+CA\r\n");
        break;
    case OMNIA_BT_ACT_DISCONNECT:
        uart4_send_str("AT+DISC\r\nAT+CE\r\n");
        break;
    case OMNIA_BT_ACT_RESET_MODULE:
        omnia_hw_bt_pulse_reset(150U);
        break;
    default:
        break;
    }
}

uint8_t omnia_bt_receiver_get_codec(void)
{
    return s_bt_meta.codec_id;
}

bool omnia_bt_receiver_is_connected(void)
{
    return (s_bt_meta.connected != 0U) || (omnia_asrc_level(&s_bt_asrc) > 16U);
}
