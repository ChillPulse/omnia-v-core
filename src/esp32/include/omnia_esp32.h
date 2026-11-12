/**
 * @file    omnia_esp32.h
 * @brief   Внутренние интерфейсы хост-ядра ESP32-S3-N16R8 (ESP-IDF v5.x, без Arduino)
 */

#ifndef OMNIA_ESP32_H
#define OMNIA_ESP32_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "omnia_pins.h"
#include "omnia_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Жёстко закреплённые пулы памяти без динамической фрагментации (ТЗ v2.0 Раздел 6.1) */
#define OMNIA_COMPRESSED_RING_BYTES     (1536U * 1024U) /* 1.5 МБ Octal PSRAM под сжатый поток */
#define OMNIA_PCM_RING_BYTES            (2560U * 1024U) /* 2.5 МБ Octal PSRAM под 32-bit PCM */
#define OMNIA_DMA_DESC_COUNT            8U
#define OMNIA_DMA_FRAMES_PER_DESC       384U            /* 8 * 384 * 8 байт = ровно 24 КБ Internal SRAM */

typedef struct {
    uint8_t *compressed_ring;
    size_t   compressed_size;
    uint8_t *pcm_ring;
    size_t   pcm_size;
    size_t   pcm_wr;
    size_t   pcm_rd;
    size_t   pcm_filled;
} omnia_static_pools_t;

/* Callback горячего события от AT32 (например, подключение кабеля ПК/Android в USB-C) */
typedef void (*omnia_vbus_event_cb_t)(bool vbus_present, uint8_t active_usb_port);

/* ── Мост управления ESP32-S3 <-> AT32F403 (UART1 @ 921600 + CRC16 + Retry) ── */
esp_err_t omnia_at32_bridge_init(omnia_vbus_event_cb_t vbus_cb);
esp_err_t omnia_at32_send_cmd_sync(uint8_t cmd, const void *payload, uint16_t len, uint32_t timeout_ms);
esp_err_t omnia_at32_query_sys_info(omnia_sys_info_t *out_info, uint32_t timeout_ms);
esp_err_t omnia_at32_set_volume(float vol_db, float replaygain_db, float balance, uint16_t ramp_ms);
esp_err_t omnia_at32_switch_sample_rate_atomic(uint32_t new_fs_hz);
esp_err_t omnia_at32_set_bit_perfect(bool bit_perfect_direct);
esp_err_t omnia_at32_set_ab_compare(bool bypass_compare);
esp_err_t omnia_at32_load_default_studio_profile(void);
esp_err_t omnia_at32_set_input_source(omnia_i2s_source_t src);
esp_err_t omnia_at32_set_usb_trigger_cfg(const omnia_usb_trigger_cfg_t *cfg, bool save_nvs);
void      omnia_at32_get_usb_trigger_cfg(omnia_usb_trigger_cfg_t *out_cfg);
esp_err_t omnia_at32_sync_usb_trigger_to_at32(void);
esp_err_t omnia_at32_send_bt_ctrl(omnia_bt_action_t action, uint8_t param);
void      omnia_at32_get_latest_status(omnia_at32_status_t *out_status);
void      omnia_at32_get_latest_meters(omnia_signal_meters_t *out_meters);
void      omnia_at32_get_latest_bt_meta(omnia_bt_meta_evt_t *out_meta);

/* ── Ведомый аудио-движок I2S Slave TX на Core 1 (32-bit Phillips Stereo) ─── */
esp_err_t omnia_audio_i2s_init(omnia_static_pools_t *pools, uint32_t initial_fs_hz);
esp_err_t omnia_audio_i2s_reconfig_clock(uint32_t new_fs_hz);
void      omnia_audio_set_test_generator(bool enable, float freq_hz, float level_dbfs);
size_t    omnia_audio_pcm_write(const int32_t *interleaved_lr, size_t stereo_frames);

/* ── Адресная подсветка 12x WS2812 и кардиоритм Standby «lub-dub» (RMT TX) ── */
typedef enum {
    OMNIA_LED_MODE_OBSIDIAN_CYAN = 0,
    OMNIA_LED_MODE_TUBE_AMBER    = 1,
    OMNIA_LED_MODE_STANDBY_HEART = 2,
    OMNIA_LED_MODE_OFF           = 3
} omnia_led_mode_t;

esp_err_t omnia_rgb_ring_init(void);
void      omnia_rgb_ring_set_mode(omnia_led_mode_t mode, float master_brightness_0_1);

#ifdef __cplusplus
}
#endif

#endif /* OMNIA_ESP32_H */
