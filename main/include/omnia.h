#ifndef OMNIA_H
#define OMNIA_H

/*
 * Omnia (v-core): общие типы, глобальные объекты и прототипы модулей.
 * Все модули подключают только этот заголовок (и config_pins.h через него).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_http_server.h"
#include "config_pins.h"
#include "audio_buffer.h"
#include "dsp_core.h"
#include "ir_learn.h"

#define OMNIA_VERSION        "1.0.0-vcore"
#define SPECTRUM_BANDS       64      // ТЗ Том 3, Гл. 3
#define FFT_SIZE             512     // ТЗ Том 1, Гл. 1 (512-точечный FFT)
#define FRAMES_PER_BLOCK     512     // стерео-кадров на блок аудиодвижка

#define OMNIA_CONFIG_LUX_NONE 0

// ---------- Состояние плеера ----------
typedef enum {
    OMNIA_PLAY_STANDBY = 0,   // УМЗЧ обесточен, экран выключен
    OMNIA_PLAY_STOPPED,
    OMNIA_PLAY_PLAYING,
    OMNIA_PLAY_PAUSED,
    OMNIA_PLAY_SEEKING        // перемотка: звук глушится, поток перезапускается
} omnia_play_state_t;

typedef enum {
    OMNIA_KEY_NONE = 0,
    OMNIA_KEY_POWER,
    OMNIA_KEY_MUTE,
    OMNIA_KEY_SOURCE,
    OMNIA_KEY_FAVORITE,
    OMNIA_KEY_PREV,
    OMNIA_KEY_NEXT,
    OMNIA_KEY_PLAY,
    OMNIA_KEY_VOL_UP,         // шаг ±5
    OMNIA_KEY_VOL_DOWN,
    OMNIA_KEY_ENC_CW,         // шаг энкодера ±1
    OMNIA_KEY_ENC_CCW
} omnia_key_t;

typedef struct {
    uint32_t sample_rate;     // Гц
    uint8_t  bits;            // 16 / 24 / 32
    uint8_t  channels;        // 1 / 2
} omnia_audio_format_t;

typedef struct {
    float bands[SPECTRUM_BANDS];   // нормализованные 0..1
} omnia_spectrum_t;

typedef struct {
    omnia_play_state_t play_state;
    char     source[12];          // SD, USB, DLNA, AIRPLAY, NAS, RADIO, PC_DAC, BT
    uint8_t  volume;              // 0..100
    bool     is_muted;
    bool     test_noise;          // внутренний розовый шум для калибровки комнаты
    char     track[64];
    char     artist[64];
    uint32_t elapsed;             // сек
    uint32_t total;               // сек
    uint32_t bitrate;             // kbps
    char     format[24];
    int8_t   wifi_rssi;           // dBm
    uint8_t  dsp_idx;
    uint16_t lux;
    uint8_t  anti_ad;             // 0 выкл, 1 мягкий, 2 агрессивный
} omnia_state_t;

// ---------- Глобальные объекты и очереди (main.cpp) ----------
extern OmniaAudioBuffer audio_buffer;
extern OmniaDSPCore     dsp_processor;
extern OmniaIRLearn     ir_learn;
extern QueueHandle_t    spectrum_queue;      // длина 1, перезапись
extern QueueHandle_t    audio_format_queue;  // длина 1, перезапись

// ---------- Конфигурация (omnia_state.cpp, меняется через WebSocket SET_CONFIG) ----------
extern bool    g_usb_autowake_enabled;
extern uint8_t g_standby_display_mode;   // 0 Deep Sleep, 1 Chrono, 2 Meteo
extern uint8_t g_standby_ring_mode;      // 0 выкл, 1 пульс, 2 такты, 3 погода
extern uint8_t g_amp_mute_polarity;      // 0: HIGH = mute (по коду ТЗ Тома 1/5), 1: LOW = mute

// ---------- Состояние (omnia_state.cpp) ----------
void    omnia_state_init(void);
void    omnia_state_lock(void);
void    omnia_state_unlock(void);
void    omnia_state_snapshot(omnia_state_t* out);
void    omnia_set_play_state(omnia_play_state_t st);
const char* omnia_play_state_name(omnia_play_state_t st);
void    omnia_set_volume(uint8_t pct);
void    omnia_set_mute(bool mute);
bool    omnia_is_muted(void);
void    omnia_set_test_noise(bool on);
bool    omnia_select_source(const char* name);
void    omnia_transport(const char* action);
void    omnia_seek(uint32_t position_sec);
void    omnia_set_audio_format(const omnia_audio_format_t* f);
void    omnia_dispatch_key(omnia_key_t key);
void    omnia_update_sensors(int8_t wifi_rssi, uint16_t lux);
void    omnia_set_anti_ad(uint8_t mode);

// ---------- Аудио-движок (core0_audio.cpp, fft.cpp) ----------
void vTaskAudioEngine(void* pvParameters);
void init_i2s_dma_bit_perfect(uint32_t sample_rate, uint8_t bits_per_sample);
void compute_spectrum_bands(const float* mono_fft_size, uint32_t sample_rate, float* bands_out);

// ---------- Сеть / сервисы (network_services.cpp, web_server_backend.cpp, ota) ----------
void dlna_register_handlers(httpd_handle_t server);
void register_ota_handlers(httpd_handle_t server);
void init_omnia_pwa_webserver(void);
void ws_broadcast_state(void);
void cp1251_to_utf8(char* out, const char* in, size_t max_out_len);
void wifi_sta_start(void);
int8_t wifi_get_rssi(void);

// ---------- Хранилища (storage_hal.cpp) ----------
bool init_sd_card_spi3(void);
void init_usb_host_msc(void);

// ---------- Дисплей и ввод (display_hal.cpp, input_hal.cpp) ----------
bool init_st7789_vcore_graphics(void);
void display_set_backlight(uint8_t pct);
void input_init(void);
void input_poll(void);

// ---------- Питание (power_manager.cpp) ----------
void omnia_request_power(bool on);
void hw_set_mute(bool mute);
void enter_omnia_standby_sleep(void);
void exit_omnia_standby(void);

// ---------- Датчики (sensors.cpp) ----------
bool     sensors_init(void);
uint16_t sensors_read_lux(void);

#endif // OMNIA_H
