/**
 * @file    uart_link.c
 * @brief   Высокоскоростной канал связи AT32F403 (USART1 PA9/PA10, 921600 бод, круговой DMA RX)
 *          и полный обработчик команд управления от мастера ESP32-S3.
 */

#include "omnia_at32.h"
#include <string.h>

#define UART_RX_DMA_RING_SIZE   1024U

static uint8_t              g_uart_rx_ring[UART_RX_DMA_RING_SIZE];
static uint16_t             g_uart_rx_rd_idx = 0U;
static uint8_t              g_uart_tx_buf[OMNIA_PROTO_MAX_FRAME_SIZE];
static uint8_t              g_tx_seq         = 0U;
static uint32_t             g_last_hb_ms     = 0U;
static uint32_t             g_last_meter_ms  = 0U;

static omnia_proto_parser_t g_parser;
static omnia_dsp_ctx_t     *g_dsp            = NULL;

static void uart1_write_blocking(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        while (usart_flag_get(USART1, USART_TDBE_FLAG) == RESET) {}
        usart_data_transmit(USART1, data[i]);
    }
    while (usart_flag_get(USART1, USART_TDC_FLAG) == RESET) {}
}

static void send_frame(uint8_t flags, uint8_t seq, uint8_t cmd, const void *payload, uint16_t len)
{
    const size_t n = omnia_proto_build_frame(
        g_uart_tx_buf, sizeof(g_uart_tx_buf), flags, seq, cmd, payload, len);
    if (n > 0U) {
        uart1_write_blocking(g_uart_tx_buf, n);
    }
}

static void send_ack(uint8_t req_seq, uint8_t acked_cmd, omnia_err_code_t code)
{
    omnia_ack_payload_t ack = {
        .acked_cmd   = acked_cmd,
        .status_code = (uint8_t)code
    };
    const uint8_t fl = (code == OMNIA_STATUS_OK) ? OMNIA_FLAG_IS_ACK : OMNIA_FLAG_IS_NACK;
    send_frame(fl, req_seq, OMNIA_EVT_ACK_NACK, &ack, sizeof(ack));
}

void omnia_uart_send_status_now(void)
{
    if (!g_dsp) {
        return;
    }
    omnia_usb_trigger_cfg_t trig;
    omnia_usb_trigger_get_cfg(&trig);

    omnia_at32_status_t st;
    memset(&st, 0, sizeof(st));
    st.sample_rate_hz    = omnia_i2s_get_current_fs();
    st.active_dsp_mask   = g_dsp->active_mask;
    st.muted             = g_dsp->muted ? 1U : 0U;
    st.bit_perfect_mode  = g_dsp->bit_perfect_active ? 1U : 0U;
    st.active_i2s_src    = (uint8_t)omnia_i2s_get_source_mode();
    st.usb_vbus_detected = omnia_usb_dac_is_vbus_present() ? 1U : 0U;
    st.usb_active_port   = (omnia_i2s_get_source_mode() == OMNIA_SRC_USB_DAC_UAC1) ? 1U : 0U;
    st.ab_compare_bypass = g_dsp->ab_compare_bypass ? 1U : 0U;
    st.cpu_load_percent  = omnia_i2s_get_cpu_load_pct();
    st.dc_fault_tripped  = g_dsp->dc_fault_tripped ? 1U : 0U;
    st.clip_events       = g_dsp->clip_events;
    st.current_vol_db    = g_dsp->target_vol_db;
    st.usb_streaming     = omnia_usb_dac_is_streaming() ? 1U : 0U;
    st.bt_codec_id       = omnia_bt_receiver_get_codec();
    st.usb_trigger_en    = trig.enable_auto_switch;

    send_frame(0U, g_tx_seq++, OMNIA_EVT_STATUS, &st, sizeof(st));
}

void omnia_uart_send_bt_meta(const omnia_bt_meta_evt_t *meta)
{
    if (meta) {
        send_frame(0U, g_tx_seq++, OMNIA_EVT_BT_META, meta, (uint16_t)sizeof(*meta));
    }
}

void omnia_uart_send_usb_trigger_evt(const omnia_usb_trigger_evt_t *evt)
{
    if (evt) {
        send_frame(0U, g_tx_seq++, OMNIA_EVT_USB_TRIGGER, evt, (uint16_t)sizeof(*evt));
    }
}

static void send_sys_info(uint8_t seq)
{
    const omnia_spi_flash_info_t *fl = omnia_ext_flash_get_info();
    omnia_sys_info_t info;
    memset(&info, 0, sizeof(info));
    info.fw_major          = 2U;
    info.fw_minor          = 1U;
    info.fw_patch          = 0U;
    info.ext_flash_present = (fl && fl->present) ? fl->bus_type : 0U;
    info.sram_size_kb      = omnia_get_sram_size_kb();
    info.sysclk_hz         = system_core_clock;
    info.ext_flash_jedec   = fl ? fl->jedec_id : 0U;
    info.ext_flash_bytes   = fl ? fl->capacity_bytes : 0U;

    send_frame(OMNIA_FLAG_IS_ACK, seq, OMNIA_EVT_SYS_INFO, &info, sizeof(info));
}

static void dispatch_rx_frame(const omnia_frame_hdr_t *hdr, const uint8_t *pay)
{
    omnia_err_code_t res = OMNIA_STATUS_OK;
    const uint16_t   len = hdr->payload_len;

    switch (hdr->cmd) {
    case OMNIA_CMD_MUTE:
        if (len >= sizeof(omnia_cmd_mute_t)) {
            const omnia_cmd_mute_t *m = (const omnia_cmd_mute_t *)pay;
            omnia_dsp_set_mute(g_dsp, true, m->ramp_ms);
            if (m->hard_xsmt) {
                omnia_delay_ms(m->ramp_ms > 40U ? 40U : m->ramp_ms);
                omnia_hw_set_dac_xsmt(false);
            }
        } else {
            omnia_dsp_set_mute(g_dsp, true, 25U);
        }
        break;

    case OMNIA_CMD_UNMUTE:
        omnia_hw_set_dac_xsmt(true);
        if (len >= sizeof(omnia_cmd_mute_t)) {
            const omnia_cmd_mute_t *m = (const omnia_cmd_mute_t *)pay;
            omnia_dsp_set_mute(g_dsp, false, m->ramp_ms);
        } else {
            omnia_dsp_set_mute(g_dsp, false, 25U);
        }
        break;

    case OMNIA_CMD_SET_VOLUME:
        if (len == sizeof(omnia_cmd_volume_t)) {
            const omnia_cmd_volume_t *v = (const omnia_cmd_volume_t *)pay;
            omnia_dsp_set_volume(g_dsp, v->volume_db, v->replaygain_db, v->balance, v->ramp_ms);
        } else {
            res = OMNIA_STATUS_ERR_INVALID_ARG;
        }
        break;

    case OMNIA_CMD_SET_FS:
        if (len == sizeof(omnia_cmd_set_fs_t)) {
            /* Строгая защита Policy 1: менять Fs разрешено только при активном Mute! */
            if (!g_dsp->muted) {
                res = OMNIA_STATUS_ERR_NOT_MUTED;
            } else {
                const omnia_cmd_set_fs_t *f = (const omnia_cmd_set_fs_t *)pay;
                if (!omnia_i2s_reconfigure_fs(f->sample_rate_hz, omnia_i2s_is_mclk_enabled())) {
                    res = OMNIA_STATUS_ERR_INVALID_ARG;
                }
            }
        } else {
            res = OMNIA_STATUS_ERR_INVALID_ARG;
        }
        break;

    case OMNIA_CMD_SET_I2S_SOURCE:
        if (len == sizeof(omnia_cmd_source_t)) {
            const omnia_cmd_source_t *s = (const omnia_cmd_source_t *)pay;
            const omnia_i2s_source_t src = (omnia_i2s_source_t)s->source;
            omnia_usb_trigger_notify_manual_source(src);
            if (src == OMNIA_SRC_USB_DAC_UAC1) {
                const uint32_t usb_fs = omnia_usb_dac_get_host_rate();
                if (usb_fs != omnia_i2s_get_current_fs()) {
                    (void)omnia_i2s_reconfigure_fs(usb_fs, omnia_i2s_is_mclk_enabled());
                }
            }
            omnia_i2s_set_source_mode(src, s->tone_freq_hz, s->tone_level_dbfs);
        } else {
            res = OMNIA_STATUS_ERR_INVALID_ARG;
        }
        break;

    case OMNIA_CMD_AB_COMPARE:
        if (len == sizeof(omnia_cmd_ab_compare_t)) {
            const omnia_cmd_ab_compare_t *ab = (const omnia_cmd_ab_compare_t *)pay;
            omnia_dsp_set_ab_compare(g_dsp, ab->compare_bypass_en != 0U);
        } else {
            res = OMNIA_STATUS_ERR_INVALID_ARG;
        }
        break;

    case OMNIA_CMD_LOAD_HPF_HEADROOM:
        if (len == sizeof(omnia_hpf_headroom_t)) {
            omnia_dsp_apply_hpf_headroom(g_dsp, (const omnia_hpf_headroom_t *)pay);
        } else res = OMNIA_STATUS_ERR_INVALID_ARG;
        break;

    case OMNIA_CMD_LOAD_ROOM_PEQ:
        if (len == sizeof(omnia_room_peq_t)) {
            omnia_dsp_apply_room_peq(g_dsp, (const omnia_room_peq_t *)pay);
        } else res = OMNIA_STATUS_ERR_INVALID_ARG;
        break;

    case OMNIA_CMD_LOAD_USER_PEQ:
        if (len == sizeof(omnia_user_peq_t)) {
            omnia_dsp_apply_user_peq(g_dsp, (const omnia_user_peq_t *)pay);
        } else res = OMNIA_STATUS_ERR_INVALID_ARG;
        break;

    case OMNIA_CMD_LOAD_ANTI_AD:
        if (len == sizeof(omnia_anti_ad_t)) {
            omnia_dsp_apply_anti_ad(g_dsp, (const omnia_anti_ad_t *)pay);
        } else res = OMNIA_STATUS_ERR_INVALID_ARG;
        break;

    case OMNIA_CMD_LOAD_VBB:
        if (len == sizeof(omnia_vbb_params_t)) {
            omnia_dsp_apply_vbb(g_dsp, (const omnia_vbb_params_t *)pay);
        } else res = OMNIA_STATUS_ERR_INVALID_ARG;
        break;

    case OMNIA_CMD_LOAD_AETHER:
        if (len == sizeof(omnia_aether_params_t)) {
            omnia_dsp_apply_aether(g_dsp, (const omnia_aether_params_t *)pay);
        } else res = OMNIA_STATUS_ERR_INVALID_ARG;
        break;

    case OMNIA_CMD_LOAD_LOUDNESS:
        if (len == sizeof(omnia_loudness_params_t)) {
            omnia_dsp_apply_loudness(g_dsp, (const omnia_loudness_params_t *)pay);
        } else res = OMNIA_STATUS_ERR_INVALID_ARG;
        break;

    case OMNIA_CMD_LOAD_LIMITER:
        if (len == sizeof(omnia_limiter_params_t)) {
            omnia_dsp_apply_limiter(g_dsp, (const omnia_limiter_params_t *)pay);
        } else res = OMNIA_STATUS_ERR_INVALID_ARG;
        break;

    case OMNIA_CMD_SET_BYPASS_MASK:
        if (len == sizeof(omnia_bypass_mask_t)) {
            const omnia_bypass_mask_t *bm = (const omnia_bypass_mask_t *)pay;
            omnia_dsp_set_bypass_mask(g_dsp, bm->active_mask, bm->ramp_ms);
        } else res = OMNIA_STATUS_ERR_INVALID_ARG;
        break;

    case OMNIA_CMD_PRESET_OP:
        if (len == sizeof(omnia_preset_op_t)) {
            const omnia_preset_op_t *pr = (const omnia_preset_op_t *)pay;
            bool ok = false;
            if (pr->op == 0U) {
                ok = omnia_preset_load(pr->slot_index, g_dsp, NULL);
            } else if (pr->op == 1U) {
                ok = omnia_preset_save(pr->slot_index, pr->name, g_dsp);
            }
            if (!ok) {
                res = OMNIA_STATUS_ERR_FLASH_IO;
            }
        } else res = OMNIA_STATUS_ERR_INVALID_ARG;
        break;

    case OMNIA_CMD_USB_MUX_CTRL:
        if (len == sizeof(omnia_usb_trigger_cfg_t)) {
            omnia_usb_trigger_cfg_t cfg;
            memcpy(&cfg, pay, sizeof(cfg));
            omnia_usb_trigger_set_cfg(&cfg);
        } else if (len == sizeof(omnia_usb_mux_cmd_t)) {
            const omnia_usb_mux_cmd_t *u = (const omnia_usb_mux_cmd_t *)pay;
            omnia_usb_trigger_cfg_t cfg;
            omnia_usb_trigger_get_cfg(&cfg);
            cfg.enable_auto_switch = (u->usb_mux_mode == 0U) ? 1U : 0U;
            omnia_usb_trigger_set_cfg(&cfg);
        } else {
            res = OMNIA_STATUS_ERR_INVALID_ARG;
        }
        break;

    case OMNIA_CMD_BT_CTRL:
        if (len == sizeof(omnia_bt_ctrl_cmd_t)) {
            omnia_bt_ctrl_cmd_t bcmd;
            memcpy(&bcmd, pay, sizeof(bcmd));
            omnia_bt_receiver_exec_cmd(&bcmd);
        } else {
            res = OMNIA_STATUS_ERR_INVALID_ARG;
        }
        break;

    case OMNIA_CMD_POWER_STATE:
        if (len == sizeof(omnia_power_cmd_t)) {
            const omnia_power_cmd_t *p = (const omnia_power_cmd_t *)pay;
            omnia_hw_enter_standby(p->power_mode == OMNIA_PWR_STANDBY);
        } else res = OMNIA_STATUS_ERR_INVALID_ARG;
        break;

    case OMNIA_CMD_MCLK_CFG:
        if (len == sizeof(omnia_mclk_cmd_t)) {
            const omnia_mclk_cmd_t *mc = (const omnia_mclk_cmd_t *)pay;
            (void)omnia_i2s_reconfigure_fs(omnia_i2s_get_current_fs(), mc->mclk_enable != 0U);
        } else res = OMNIA_STATUS_ERR_INVALID_ARG;
        break;

    case OMNIA_CMD_GET_SYS_INFO:
        send_sys_info(hdr->seq);
        return;

    case OMNIA_CMD_PING:
        break;

    default:
        res = OMNIA_STATUS_ERR_UNSUPPORTED;
        break;
    }

    if (OMNIA_GET_FLAGS(hdr->ver_flags) & OMNIA_FLAG_ACK_REQ) {
        send_ack(hdr->seq, hdr->cmd, res);
    }
}

void omnia_uart_link_init(omnia_dsp_ctx_t *dsp_ctx)
{
    gpio_init_type gpio_cfg;
    dma_init_type  dma_cfg;

    g_dsp = dsp_ctx;
    omnia_proto_parser_init(&g_parser);

    crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_DMA1_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_USART1_PERIPH_CLOCK, TRUE);

    /* PA9 = USART1_TX, PA10 = USART1_RX */
    gpio_default_para_init(&gpio_cfg);
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_cfg.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_mode           = GPIO_MODE_MUX;
    gpio_cfg.gpio_pull           = GPIO_PULL_NONE;
    gpio_cfg.gpio_pins           = GPIO_PINS_9;
    gpio_init(GPIOA, &gpio_cfg);

    gpio_cfg.gpio_mode = GPIO_MODE_INPUT;
    gpio_cfg.gpio_pull = GPIO_PULL_UP;
    gpio_cfg.gpio_pins = GPIO_PINS_10;
    gpio_init(GPIOA, &gpio_cfg);

    /* Круговой DMA приём на DMA1 Channel 5 без риска потери байт во время аудио-прерываний */
    dma_reset(DMA1_CHANNEL5);
    dma_default_para_init(&dma_cfg);
    dma_cfg.buffer_size           = UART_RX_DMA_RING_SIZE;
    dma_cfg.direction             = DMA_DIR_PERIPHERAL_TO_MEMORY;
    dma_cfg.memory_base_addr      = (uint32_t)g_uart_rx_ring;
    dma_cfg.memory_data_width     = DMA_MEMORY_DATA_WIDTH_BYTE;
    dma_cfg.memory_inc_enable     = TRUE;
    dma_cfg.peripheral_base_addr  = (uint32_t)&(USART1->dt);
    dma_cfg.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_BYTE;
    dma_cfg.peripheral_inc_enable = FALSE;
    dma_cfg.priority              = DMA_PRIORITY_MEDIUM;
    dma_cfg.loop_mode_enable      = TRUE;
    dma_init(DMA1_CHANNEL5, &dma_cfg);
    dma_flexible_config(DMA1, FLEX_CHANNEL5, DMA_FLEXIBLE_UART1_RX);
    dma_channel_enable(DMA1_CHANNEL5, TRUE);

    usart_init(USART1, OMNIA_UART_DEFAULT_BAUD, USART_DATA_8BITS, USART_STOP_1_BIT);
    usart_parity_selection_config(USART1, USART_PARITY_NONE);
    usart_transmitter_enable(USART1, TRUE);
    usart_receiver_enable(USART1, TRUE);
    usart_dma_receiver_enable(USART1, TRUE);
    usart_enable(USART1, TRUE);

    g_uart_rx_rd_idx = 0U;
}

void omnia_uart_link_poll(uint32_t now_ms)
{
    /* Вычитываем все новые байты из кругового DMA-кольца USART1 */
    const uint16_t wr_idx = (uint16_t)(UART_RX_DMA_RING_SIZE - dma_data_number_get(DMA1_CHANNEL5));
    while (g_uart_rx_rd_idx != wr_idx) {
        const uint8_t b = g_uart_rx_ring[g_uart_rx_rd_idx];
        g_uart_rx_rd_idx = (uint16_t)((g_uart_rx_rd_idx + 1U) % UART_RX_DMA_RING_SIZE);
        if (omnia_proto_parser_feed(&g_parser, b)) {
            omnia_hw_led_toggle();
            dispatch_rx_frame(&g_parser.hdr, g_parser.payload);
        }
    }

    /* Признак неисправности CRC: возвращаем NACK для ускоренного повтора с ESP32 */
    if (g_parser.crc_errors > 0U) {
        g_parser.crc_errors = 0U;
        send_ack(g_parser.hdr.seq, g_parser.hdr.cmd, OMNIA_STATUS_ERR_CRC);
    }

    /* Пиковые уровни для аналоговых стрелочных индикаторов VU (25 Гц = каждые 40 мс) */
    if ((now_ms - g_last_meter_ms) >= 40U && g_dsp && !g_dsp->muted) {
        g_last_meter_ms = now_ms;
        omnia_signal_meters_t meters;
        omnia_dsp_read_and_reset_meters(g_dsp, &meters);
        send_frame(0U, g_tx_seq++, OMNIA_EVT_SIGNAL_METERS, &meters, sizeof(meters));
    }

    /* Периодический Heartbeat + статус раз в 1000 мс */
    if ((now_ms - g_last_hb_ms) >= 1000U) {
        g_last_hb_ms = now_ms;
        omnia_uart_send_status_now();
    }
}
