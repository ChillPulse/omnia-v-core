/**
 * @file    omnia_at32.h
 * @brief   Внутренние аппаратные интерфейсы прошивки AT32F403ACGU7
 */

#ifndef OMNIA_AT32_H
#define OMNIA_AT32_H

#include <stdint.h>
#include <stdbool.h>
#include "at32f403a_407.h"
#include "omnia_pins.h"
#include "omnia_protocol.h"
#include "omnia_dsp.h"

/* --- system_sram224.c: 240 МГц, раскрытие 224 КБ SRAM (EOPB0), системный таймер и DWT --- */
void     omnia_system_boot_init(void);
uint16_t omnia_get_sram_size_kb(void);
uint32_t omnia_millis(void);
void     omnia_delay_ms(uint32_t ms);
void     omnia_delay_us(uint32_t us);
uint32_t omnia_dwt_cycles(void);

/* --- anti_pop_hw.c: XSMT, AMP_MUTE, RELAY, 74HC125 I2S Mux и FSUSB30MUX VBUS Auto --- */
void     omnia_hw_gpio_init(void);
void     omnia_hw_set_dac_xsmt(bool unmute);
void     omnia_hw_set_amp_mute(bool mute_active);
void     omnia_hw_set_relay(bool closed);
void     omnia_hw_select_i2s_mux(omnia_i2s_source_t src);
void     omnia_hw_set_usb_mux_mode(omnia_usb_mux_mode_t mode);
void     omnia_hw_poll_usb_vbus(uint32_t now_ms, bool *changed_out);
bool     omnia_hw_is_vbus_detected(void);
uint8_t  omnia_hw_get_active_usb_port(void);
void     omnia_hw_led_set(bool on);
void     omnia_hw_led_toggle(void);
void     omnia_hw_power_sequence_boot(void);
void     omnia_hw_enter_standby(bool standby);

/* --- spi_flash_sfdp.c: Универсальный драйвер ЛЮБОЙ SPI NOR Flash (JEDEC + JESD216 SFDP) --- */
typedef struct {
    bool     present;
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

/* --- i2s_fullduplex.c: I2S2 Full-Duplex Master DMA + генератор синуса + Mute-Handshake --- */
void     omnia_i2s_audio_init(omnia_dsp_ctx_t *dsp_ctx);
bool     omnia_i2s_reconfigure_fs(uint32_t new_fs_hz, bool enable_mclk);
void     omnia_i2s_set_source_mode(omnia_i2s_source_t src, float tone_hz, float tone_dbfs);
omnia_i2s_source_t omnia_i2s_get_source_mode(void);
uint32_t omnia_i2s_get_current_fs(void);
bool     omnia_i2s_is_mclk_enabled(void);
uint8_t  omnia_i2s_get_cpu_load_pct(void);

/* --- uart_link.c: Канал USART1 921600 бод DMA RX + диспетчер команд ESP32 --- */
void     omnia_uart_link_init(omnia_dsp_ctx_t *dsp_ctx);
void     omnia_uart_link_poll(uint32_t now_ms);
void     omnia_uart_send_status_now(void);

#endif /* OMNIA_AT32_H */
