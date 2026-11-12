/**
 * @file    omnia_at32.h
 * @brief   Внутренние аппаратные интерфейсы прошивки AT32F403ACGU7 (24-Bit Direct Studio DSP)
 */

#ifndef OMNIA_AT32_H
#define OMNIA_AT32_H

#include <stdint.h>
#include <stdbool.h>
#include "at32f403a_407.h"
#include "omnia_pins.h"
#include "omnia_protocol.h"
#include "omnia_dsp.h"
#include "omnia_asrc.h"

/* --- system_sram224.c: 240 МГц, раскрытие 224 КБ SRAM (EOPB0), системный таймер и DWT --- */
void     omnia_system_boot_init(void);
uint16_t omnia_get_sram_size_kb(void);
uint32_t omnia_millis(void);
void     omnia_delay_ms(uint32_t ms);
void     omnia_delay_us(uint32_t us);
uint32_t omnia_dwt_cycles(void);

/* --- anti_pop_hw.c: Секвенсор ЦАПа и УМЗЧ (PB0 XSMT, PB2 AMP_PWR, PB4 AMP_MUTE, PB8 RELAY, PB9 VBUS) --- */
void     omnia_hw_gpio_init(void);
void     omnia_hw_set_dac_xsmt(bool unmute);
void     omnia_hw_set_amp_pwr(bool power_on);
bool     omnia_hw_is_amp_pwr_on(void);
void     omnia_hw_set_amp_mute(bool mute_active);
void     omnia_hw_set_relay(bool closed);
bool     omnia_hw_amp_cfg_ready(void);
void     omnia_hw_latch_fault(void);
void     omnia_hw_apply_amp_ctrl_cfg(const omnia_amp_ctrl_cfg_t *cfg);
void     omnia_hw_get_amp_ctrl_cfg(omnia_amp_ctrl_cfg_t *out_cfg);
bool     omnia_hw_vbus_pin_raw(void);
void     omnia_hw_bt_pulse_reset(uint32_t low_ms);
void     omnia_hw_poll(uint32_t now_ms);
void     omnia_hw_led_set(bool on);
void     omnia_hw_led_toggle(void);
void     omnia_hw_power_sequence_boot(void);
void     omnia_hw_enter_standby(bool standby);
bool     omnia_hw_is_standby(void);

/* --- spi_flash_sfdp.c: Автоопрос U3 (брюшко BlackPill) + SPI1 (PA4..PA7) по JEDEC + JESD216 SFDP --- */
typedef struct {
    bool     present;
    uint8_t  bus_type;          /* 0 = нет, 1 = SPI1 (PA4..7), 2 = Belly U3 (PA8/PB1/PB10/PB11) */
    uint32_t jedec_id;
    uint32_t capacity_bytes;
    uint16_t sector_size;
    uint16_t page_size;
    bool     addr_4byte;
} omnia_spi_flash_info_t;

void     omnia_ext_flash_init(omnia_spi_flash_info_t *out_info);
const omnia_spi_flash_info_t *omnia_ext_flash_get_info(void);
bool     omnia_preset_save(uint8_t slot, const char *name, const omnia_dsp_ctx_t *dsp);
bool     omnia_preset_load(uint8_t slot, omnia_dsp_ctx_t *dsp, char *out_name24);

/* --- i2s_fullduplex.c: I2S2 Full-Duplex Master DMA (24-bit in 32-bit) + роутер 4 входов --- */
void     omnia_i2s_audio_init(omnia_dsp_ctx_t *dsp_ctx);
bool     omnia_i2s_reconfigure_fs(uint32_t new_fs_hz, bool enable_mclk);
void     omnia_i2s_set_source_mode(omnia_i2s_source_t src, float tone_hz, float tone_dbfs);
omnia_i2s_source_t omnia_i2s_get_source_mode(void);
uint32_t omnia_i2s_get_current_fs(void);
bool     omnia_i2s_is_mclk_enabled(void);
uint8_t  omnia_i2s_get_cpu_load_pct(void);

/* --- bt_receiver.c: I2S3 Slave RX 24-bit + Farrow ASRC + UART4 (OMNIA-BTM334 / BT401) --- */
void     omnia_bt_receiver_init(uint32_t out_fs_hz);
void     omnia_bt_receiver_notify_out_fs(uint32_t out_fs_hz);
void     omnia_bt_receiver_pull_audio(int32_t *out_lr_q31, uint32_t frames);
void     omnia_bt_receiver_poll(uint32_t now_ms);
void     omnia_bt_receiver_exec_cmd(const omnia_bt_ctrl_cmd_t *cmd);
uint8_t  omnia_bt_receiver_get_codec(void);

/* --- usb_dac_uac1.c: Нативный AT32 USBFS UAC1 Async 24-bit 44.1/48 кГц + Триггер +5V --- */
void     omnia_usb_dac_init(void);
void     omnia_usb_dac_pull_audio(int32_t *out_lr_q31, uint32_t frames);
void     omnia_usb_dac_poll(uint32_t now_ms);
void     omnia_usb_trigger_set_cfg(const omnia_usb_trigger_cfg_t *cfg);
void     omnia_usb_trigger_get_cfg(omnia_usb_trigger_cfg_t *out_cfg);
void     omnia_usb_trigger_notify_manual_source(omnia_i2s_source_t manual_src);
bool     omnia_usb_dac_is_vbus_present(void);
bool     omnia_usb_dac_is_streaming(void);
uint32_t omnia_usb_dac_get_host_rate(void);

/* --- uart_link.c: Канал USART1 921600 бод DMA RX + диспетчер команд ESP32 --- */
void     omnia_uart_link_init(omnia_dsp_ctx_t *dsp_ctx);
void     omnia_uart_link_poll(uint32_t now_ms);
void     omnia_uart_send_status_now(void);
void     omnia_uart_send_bt_meta(const omnia_bt_meta_evt_t *meta);
void     omnia_uart_send_usb_trigger_evt(const omnia_usb_trigger_evt_t *evt);

#endif /* OMNIA_AT32_H */
