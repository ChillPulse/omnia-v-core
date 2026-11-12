/**
 * @file    omnia_protocol.h
 * @brief   Единый бинарный протокол связи ESP32-S3 <-> AT32F403 (OMNIA Protocol v2)
 *          Включает структуры всех 9 звеньев студийного DSP, потоковый парсер и упаковщик кадров.
 */

#ifndef OMNIA_PROTOCOL_H
#define OMNIA_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "omnia_crc16.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Константы физического и канального уровня --- */
#define OMNIA_UART_DEFAULT_BAUD         921600U
#define OMNIA_UART_TURBO_OTA_BAUD       2000000U

#define OMNIA_PROTO_SYNC_WORD           0xAA55U     /* На линии: 0x55, затем 0xAA (LE16) */
#define OMNIA_PROTO_VERSION             2U
#define OMNIA_PROTO_MAX_PAYLOAD         544U        /* Вмещает чанк OTA 512 байт + заголовок */
#define OMNIA_PROTO_HEADER_SIZE         7U          /* SYNC(2) + LEN(2) + VER_FL(1) + SEQ(1) + CMD(1) */
#define OMNIA_PROTO_CRC_SIZE            2U
#define OMNIA_PROTO_MAX_FRAME_SIZE      (OMNIA_PROTO_HEADER_SIZE + OMNIA_PROTO_MAX_PAYLOAD + OMNIA_PROTO_CRC_SIZE)

/* Младший полубайт поля ver_flags */
#define OMNIA_FLAG_ACK_REQ              0x01U       /* Отправитель ждёт подтверждения ACK */
#define OMNIA_FLAG_IS_ACK               0x02U       /* Кадр является положительным ответом ACK */
#define OMNIA_FLAG_IS_NACK              0x04U       /* Кадр является отказом / ошибкой NACK */

#define OMNIA_MAKE_VER_FLAGS(flags)     ((uint8_t)(((OMNIA_PROTO_VERSION & 0x0FU) << 4) | ((flags) & 0x0FU)))
#define OMNIA_GET_VERSION(ver_flags)    ((uint8_t)(((ver_flags) >> 4) & 0x0FU))
#define OMNIA_GET_FLAGS(ver_flags)      ((uint8_t)((ver_flags) & 0x0FU))

/* --- Реестр команд (ESP32 -> AT32) и событий (AT32 -> ESP32) --- */
typedef enum {
    /* Управление трактом и транспортом */
    OMNIA_CMD_MUTE                  = 0x01,
    OMNIA_CMD_UNMUTE                = 0x02,
    OMNIA_CMD_SET_VOLUME            = 0x03,
    OMNIA_CMD_SET_FS                = 0x04,
    OMNIA_CMD_SET_I2S_SOURCE        = 0x05,
    OMNIA_CMD_AB_COMPARE            = 0x06,

    /* Загрузка параметров студийного DSP */
    OMNIA_CMD_LOAD_HPF_HEADROOM     = 0x10,
    OMNIA_CMD_LOAD_ROOM_PEQ         = 0x11,
    OMNIA_CMD_LOAD_USER_PEQ         = 0x12,
    OMNIA_CMD_LOAD_ANTI_AD          = 0x13,
    OMNIA_CMD_LOAD_VBB              = 0x14,
    OMNIA_CMD_LOAD_AETHER           = 0x15,
    OMNIA_CMD_LOAD_LOUDNESS         = 0x16,
    OMNIA_CMD_LOAD_LIMITER          = 0x17,
    OMNIA_CMD_SET_BYPASS_MASK       = 0x18,
    OMNIA_CMD_PRESET_OP             = 0x19,

    /* Управление периферией и питанием */
    OMNIA_CMD_USB_MUX_CTRL          = 0x20, /* В Zero-Mux режиме: настройка Триггера USB-DAC (omnia_usb_trigger_cfg_t) */
    OMNIA_CMD_POWER_STATE           = 0x21,
    OMNIA_CMD_MCLK_CFG              = 0x22,
    OMNIA_CMD_BT_CTRL               = 0x23, /* Управление Bluetooth BTM334 / BT401 (Play/Next/Pair/Reset) */

    /* Сервисные команды и Smart Dual-OTA */
    OMNIA_CMD_OTA_BEGIN             = 0x30,
    OMNIA_CMD_OTA_CHUNK             = 0x31,
    OMNIA_CMD_OTA_VERIFY_COMMIT     = 0x32,
    OMNIA_CMD_GET_SYS_INFO          = 0x33,
    OMNIA_CMD_PING                  = 0x34,

    /* Телеметрия и ответы (AT32 -> ESP32) */
    OMNIA_EVT_STATUS                = 0x80,
    OMNIA_EVT_SIGNAL_METERS         = 0x81,
    OMNIA_EVT_ACK_NACK              = 0x82,
    OMNIA_EVT_HEARTBEAT             = 0x83,
    OMNIA_EVT_OTA_PROGRESS          = 0x84,
    OMNIA_EVT_SYS_INFO              = 0x85,
    OMNIA_EVT_BT_META               = 0x86, /* Метаданные трека AVRCP 1.6 и кодек (aptX-HD 24-bit и др.) */
    OMNIA_EVT_USB_TRIGGER           = 0x87  /* Срабатывание/отпускание триггера +5V звуковой карты USB   */
} omnia_cmd_id_t;

#define OMNIA_CMD_USB_TRIGGER_CFG       OMNIA_CMD_USB_MUX_CTRL

/* Коды результата в пакетах ACK/NACK */
typedef enum {
    OMNIA_STATUS_OK                 = 0x00,
    OMNIA_STATUS_ERR_CRC            = 0x01,
    OMNIA_STATUS_ERR_INVALID_ARG    = 0x02,
    OMNIA_STATUS_ERR_NOT_MUTED      = 0x03, /* Попытка сменить Fs без предварительного Mute */
    OMNIA_STATUS_ERR_FLASH_IO       = 0x04,
    OMNIA_STATUS_ERR_UNSUPPORTED    = 0x05
} omnia_err_code_t;

/* Цифровые источники сигнала на входах AT32F403 (Zero-Mux 24-Bit Architecture) */
typedef enum {
    OMNIA_SRC_NONE_SILENCE          = 0,
    OMNIA_SRC_ESP32_I2S             = 1,    /* Прямой вход ESP32-S3 GPIO5 -> AT32 PB14 (I2S2EXT_SD) */
    OMNIA_SRC_BT_I2S3_ASRC          = 2,    /* Прямой вход Bluetooth BTM334/BT401 -> I2S3 + Farrow ASRC */
    OMNIA_SRC_INTERNAL_TONE         = 3,    /* Тестовый генератор синуса внутри AT32 */
    OMNIA_SRC_USB_DAC_UAC1          = 4     /* Нативный вход USB-DAC AT32 PA11/PA12 (24-bit UAC1 Async) */
} omnia_i2s_source_t;

#define OMNIA_SRC_EXT_BT_I2S            OMNIA_SRC_BT_I2S3_ASRC

/* Кодек активного соединения Bluetooth (для отображения бейджа на TFT и в Web-UI) */
typedef enum {
    OMNIA_BT_CODEC_NONE             = 0,
    OMNIA_BT_CODEC_SBC              = 1,
    OMNIA_BT_CODEC_AAC              = 2,
    OMNIA_BT_CODEC_APTX             = 3,
    OMNIA_BT_CODEC_APTX_LL          = 4,
    OMNIA_BT_CODEC_APTX_HD          = 5     /* 24-bit / 48 kHz (SJR-BTM334 QCC3034) */
} omnia_bt_codec_t;

/* Действия управления модулем Bluetooth (OMNIA_CMD_BT_CTRL) */
typedef enum {
    OMNIA_BT_ACT_PLAY_PAUSE         = 1,
    OMNIA_BT_ACT_NEXT               = 2,
    OMNIA_BT_ACT_PREV               = 3,
    OMNIA_BT_ACT_PAIRING            = 4,
    OMNIA_BT_ACT_DISCONNECT         = 5,
    OMNIA_BT_ACT_RESET_MODULE       = 6
} omnia_bt_action_t;

/* Режимы коммутатора FSUSB30MUX */
typedef enum {
    OMNIA_USB_MODE_AUTO_VBUS        = 0,    /* Автопереход Host <-> UAC1 по детектору +5V на USB-C */
    OMNIA_USB_MODE_FORCE_HOST_A     = 1,    /* Принудительно гнездо USB-A (флешка) */
    OMNIA_USB_MODE_FORCE_DEVICE_C   = 2     /* Принудительно гнездо USB-C (USB-DAC) */
} omnia_usb_mux_mode_t;

/* Состояния питания */
typedef enum {
    OMNIA_PWR_ACTIVE                = 0,
    OMNIA_PWR_STANDBY               = 1
} omnia_power_mode_t;

/* Типы биквадных фильтров для Room PEQ (5 полос) и User PEQ (10..15 полос) */
typedef enum {
    OMNIA_BIQUAD_OFF                = 0,
    OMNIA_BIQUAD_PEAK               = 1,
    OMNIA_BIQUAD_LOW_SHELF          = 2,
    OMNIA_BIQUAD_HIGH_SHELF         = 3,
    OMNIA_BIQUAD_LOW_PASS           = 4,
    OMNIA_BIQUAD_HIGH_PASS          = 5,
    OMNIA_BIQUAD_NOTCH              = 6,
    OMNIA_BIQUAD_ALL_PASS           = 7
} omnia_biquad_type_t;

/* Битовая маска включения/обхода 9 звеньев DSP (бит = 1 -> блок АКТИВЕН, бит = 0 -> блок В ОБХОДЕ) */
#define OMNIA_DSP_BIT_SUBSONIC_HPF      (1U << 0)
#define OMNIA_DSP_BIT_HEADROOM          (1U << 1)
#define OMNIA_DSP_BIT_ROOM_PEQ          (1U << 2)
#define OMNIA_DSP_BIT_USER_PEQ          (1U << 3)
#define OMNIA_DSP_BIT_ANTI_AD           (1U << 4)
#define OMNIA_DSP_BIT_VBB_BASS          (1U << 5)
#define OMNIA_DSP_BIT_AETHER            (1U << 6)
#define OMNIA_DSP_BIT_LOUDNESS          (1U << 7)
#define OMNIA_DSP_BIT_LIMITER           (1U << 8)
#define OMNIA_DSP_ALL_BYPASSED          (0U)

#define OMNIA_MAX_ROOM_PEQ_BANDS        5U
#define OMNIA_MAX_USER_PEQ_BANDS        10U
#define OMNIA_MAX_PRESETS               32U

/* ========================================================================== *
 *  УПАКОВАННЫЕ ПОЛЕЗНЫЕ НАГРУЗКИ ПАКЕТОВ (Little-Endian, 1-byte alignment)
 * ========================================================================== */
#pragma pack(push, 1)

typedef struct {
    uint16_t sync;              /* OMNIA_PROTO_SYNC_WORD (0xAA55) */
    uint16_t payload_len;       /* Длина поля payload (0 .. OMNIA_PROTO_MAX_PAYLOAD) */
    uint8_t  ver_flags;         /* Версия протокола (7:4) и флаги ACK/NACK (3:0) */
    uint8_t  seq;               /* Порядковый номер кадра */
    uint8_t  cmd;               /* Идентификатор команды / события (omnia_cmd_id_t) */
} omnia_frame_hdr_t;

/* 0x01 / 0x02: MUTE / UNMUTE */
typedef struct {
    uint16_t ramp_ms;           /* Длительность плавной рампы (по умолч. 25 мс) */
    uint8_t  hard_xsmt;         /* 1 = также опустить аппаратную линию XSMT после затухания */
} omnia_cmd_mute_t;

/* 0x03: CMD_SET_VOLUME */
typedef struct {
    float    volume_db;         /* Основная громкость: -96.0 .. 0.0 dB (0.0 dB = 100% Bit-Perfect) */
    float    replaygain_db;     /* Поправка ReplayGain из тега трека: -15.0 .. +6.0 dB */
    float    balance;           /* Баланс L/R: -1.0 (влево) .. 0.0 (центр) .. +1.0 (вправо) */
    uint16_t ramp_ms;           /* Время плавного перехода (15 .. 100 мс) */
} omnia_cmd_volume_t;

/* 0x04: CMD_SET_FS */
typedef struct {
    uint32_t sample_rate_hz;    /* 44100 или 48000 (резерв 88200 / 96000) */
    uint8_t  word_bits;         /* Полезная разрядность источника (16 / 24 / 32) */
} omnia_cmd_set_fs_t;

/* 0x05: CMD_SET_I2S_SOURCE */
typedef struct {
    uint8_t  source;            /* omnia_i2s_source_t */
    float    tone_freq_hz;      /* Для OMNIA_SRC_INTERNAL_TONE (например, 1000.0f Гц) */
    float    tone_level_dbfs;   /* Для OMNIA_SRC_INTERNAL_TONE (например, -12.0f dBFS) */
} omnia_cmd_source_t;

/* 0x06: CMD_AB_COMPARE */
typedef struct {
    uint8_t  compare_bypass_en; /* 0 = полный обработанный тракт (A), 1 = чистый вход с выравниванием по RMS (B) */
} omnia_cmd_ab_compare_t;

/* Описание одной полосы параметрического эквалайзера */
typedef struct {
    uint8_t  type;              /* omnia_biquad_type_t (0 = выкл) */
    uint8_t  reserved;
    float    f0_hz;             /* Центральная частота / частота среза (15 .. 22000 Гц) */
    float    q;                 /* Добротность Q (0.1 .. 18.0) */
    float    gain_db;           /* Усиление полосы (-18.0 .. +18.0 dB) */
} omnia_biquad_band_t;

/* 0x10: CMD_LOAD_HPF_HEADROOM */
typedef struct {
    uint8_t  hpf_enable;        /* Включение инфразвукового ФВЧ */
    uint8_t  auto_headroom_en;  /* Автоматический расчёт запаса по плюсовым пикам эквалайзера */
    float    hpf_freq_hz;       /* Срез Subsonic (15.0 .. 40.0 Гц, по умолч. 20.0 Гц) */
    float    hpf_q;             /* Добротность (по умолч. 0.7071 — Баттерворт) */
    float    manual_atten_db;   /* Ручной пред-аттенюатор (0.0 .. -18.0 dB) */
} omnia_hpf_headroom_t;

/* 0x11: CMD_LOAD_ROOM_PEQ (Акустическая рум-коррекция: 5 полос PEQ + задержки АС + фаза + трим) */
typedef struct {
    uint8_t             enable;
    uint8_t             band_count;     /* <= OMNIA_MAX_ROOM_PEQ_BANDS (5) */
    uint8_t             invert_phase_l; /* 0 = 0°, 1 = 180° инверсия фазы левой АС */
    uint8_t             invert_phase_r; /* 0 = 0°, 1 = 180° инверсия фазы правой АС */
    float               delay_l_ms;     /* Задержка выравнивания левой АС: 0.00 .. 20.00 мс (шаг 0.01 мс ~ 3.4 мм) */
    float               delay_r_ms;     /* Задержка выравнивания правой АС: 0.00 .. 20.00 мс (шаг 0.01 мс ~ 3.4 мм) */
    float               trim_l_db;      /* Калибровочный трим левой АС: -12.0 .. +6.0 dB */
    float               trim_r_db;      /* Калибровочный трим правой АС: -12.0 .. +6.0 dB */
    omnia_biquad_band_t bands[OMNIA_MAX_ROOM_PEQ_BANDS];
} omnia_room_peq_t;

/* 0x12: CMD_LOAD_USER_PEQ */
typedef struct {
    uint8_t             enable;
    uint8_t             band_count; /* <= OMNIA_MAX_USER_PEQ_BANDS (10) */
    omnia_biquad_band_t bands[OMNIA_MAX_USER_PEQ_BANDS];
} omnia_user_peq_t;

/* 0x13: CMD_LOAD_ANTI_AD */
typedef struct {
    uint8_t  mode;              /* 0 = OFF, 1 = Soft (мягкий), 2 = Aggro (жёсткое удержание рекламы) */
    float    threshold_lin;     /* Порог срабатывания (0.25 .. 0.90) */
    float    attack_coeff;      /* Коэффициент атаки (по умолч. 0.992f) */
    float    release_coeff;     /* Коэффициент восстановления (по умолч. 0.0002f) */
    float    makeup_db;         /* Компенсация уровня (0.0 .. +6.0 dB) */
} omnia_anti_ad_t;

/* 0x14: CMD_LOAD_VBB (Виртуальный психоакустический бас) */
typedef struct {
    uint8_t  enable;
    uint8_t  mode;              /* 0 = Clean Shelf, 1 = Warm Harmonics, 2 = Missing Fundamental */
    float    amount;            /* Макро-уровень эффекта (0.0 .. 1.0) */
    float    freq_hz;           /* Целевая частота баса (40.0 .. 120.0 Гц) */
    float    drive;             /* Насыщение обертонов (0.0 .. 1.0) */
    float    lowpass_hz;        /* Ограничение спектра гармоник (90.0 .. 240.0 Гц) */
    float    mix;               /* Баланс Dry/Wet (0.0 .. 1.0) */
} omnia_vbb_params_t;

/* 0x15: CMD_LOAD_AETHER (Полный 12-параметровый студийный блок «Эфир») */
typedef struct {
    uint8_t  enable;
    uint8_t  bass_keep_mono;    /* Удержание НЧ строго в центре (моно ниже bass_xover_hz) */
    uint8_t  side_hpf_en;       /* Очистка бокового канала S от низкочастотного гула */
    uint8_t  crossfeed_en;      /* Бинауральный режим для наушников */

    float    master_blend;      /* Общая макро-ручка вовлечения Эфира (0.0 .. 1.0) */
    float    width;             /* Ширина стереобазы (0.0 = моно, 1.0 = норма, до 2.0) */
    float    center_anchor_db;  /* Плотность вокала в центре M (-6.0 .. +6.0 dB) */
    float    bass_xover_hz;     /* Частота моно-замка баса (40.0 .. 250.0 Гц) */
    float    side_hpf_hz;       /* Частота среза S-канала (20.0 .. 180.0 Гц) */

    float    air_gain_db;       /* Уровень «Шёлкового воздуха» (0.0 .. +8.0 dB) */
    float    air_freq_hz;       /* Частота полки воздуха (5500.0 .. 16000.0 Гц) */
    float    air_q;             /* Добротность полки воздуха (0.4 .. 1.6) */
    float    de_harsh;          /* Динамическое подавление сибилянтов «с-ц-ш» (0.0 .. 1.0) */

    float    depth_amount;      /* Глубина 3D-эшелонирования за линию АС (0.0 .. 0.45) */
    float    stage_size_ms;     /* Размер акустического пространства в мс (0.5 .. 12.0 мс) */
    float    triode_warmth;     /* Чётная 2-я ламповая гармоника (0.0 .. 0.30) */
    float    crossfeed_level;   /* Глубина наушникового Crossfeed (0.0 .. 1.0) */
    float    crossfeed_fc_hz;   /* Частота тени головы Crossfeed (500.0 .. 900.0 Гц) */
} omnia_aether_params_t;

/* 0x16: CMD_LOAD_LOUDNESS (Динамическая тонкомпенсация ISO 226) */
typedef struct {
    uint8_t  enable;
    float    ref_volume_db;     /* Уровень громкости, выше которого коррекция равна 0 (напр. -18.0 dB) */
    float    max_bass_boost_db; /* Максимальная компенсация НЧ на шёпоте (0.0 .. +12.0 dB) */
    float    max_high_boost_db; /* Максимальная компенсация ВЧ на шёпоте (0.0 .. +6.0 dB) */
} omnia_loudness_params_t;

/* 0x17: CMD_LOAD_LIMITER (Финальный защитный Look-Ahead Limiter) */
typedef struct {
    uint8_t  enable;
    uint8_t  tpdf_dither_en;    /* Мягкий 24-битный TPDF дизер при активном DSP */
    float    ceiling_dbfs;      /* Потолок ограничения (-3.0 .. -0.2 dBFS, по умолч. -0.8 dBFS) */
    float    lookahead_ms;      /* Время упреждения пиков (0.5 .. 3.0 мс) */
    float    release_ms;        /* Время плавного отпускания (30.0 .. 500.0 мс) */
} omnia_limiter_params_t;

/* 0x18: CMD_SET_BYPASS_MASK */
typedef struct {
    uint16_t active_mask;       /* Комбинация флагов OMNIA_DSP_BIT_* (0 = чистый обход) */
    uint16_t ramp_ms;           /* Плавность переключения (20 .. 50 мс) */
} omnia_bypass_mask_t;

/* 0x19: CMD_PRESET_OP */
typedef struct {
    uint8_t  op;                /* 0 = Load, 1 = Save Current, 2 = Delete / Reset to Factory */
    uint8_t  slot_index;        /* Номер слота 0 .. 31 */
    char     name[24];          /* Имя пресета (UTF-8, null-terminated) */
} omnia_preset_op_t;

/* 0x20 / 0x21 / 0x22: Периферия */
typedef struct {
    uint8_t  usb_mux_mode;      /* Устаревший 1-байтный формат обратной совместимости */
} omnia_usb_mux_cmd_t;

/* 0x20: OMNIA_CMD_USB_TRIGGER_CFG — настройка включаемой Триггер-функции звуковой карты USB-DAC */
typedef struct {
    uint8_t  enable_auto_switch;    /* 1 = ВКЛ: автопереключение на USB-DAC при появлении +5V USB; 0 = ВЫКЛ */
    uint8_t  wake_from_standby;     /* 1 = будить стример и реле из Standby при включении ТВ-бокса / ПК     */
    uint8_t  auto_return_prev_src;  /* 1 = при выключении ТВ-бокса/ПК возвращать прежний вход (Радио/USB/BT)*/
    uint8_t  prefer_vbus_pin;       /* 1 = детектор PB9 VBUS_DET (+ резерв SOF); 0 = только пакеты USB SOF  */
    uint16_t debounce_on_ms;        /* Защита от дребезга при подаче +5V (по умолчанию 150 мс)              */
    uint16_t debounce_off_ms;       /* Пауза отпускания при выключении/перезагрузке хоста (по умолч. 800 мс)*/
} omnia_usb_trigger_cfg_t;

typedef struct {
    uint8_t  power_mode;        /* omnia_power_mode_t */
    uint8_t  amp_relay_en;      /* 1 = включить выходное реле УМЗЧ */
} omnia_power_cmd_t;

typedef struct {
    uint8_t  mclk_enable;       /* 0 = 3-wire I2S (PCM5102 PLL), 1 = выдавать MCLK 256fs на PA3 */
} omnia_mclk_cmd_t;

/* 0x23: OMNIA_CMD_BT_CTRL — управление Bluetooth SJR-BTM334 / BT401 */
typedef struct {
    uint8_t  action;            /* omnia_bt_action_t */
    uint8_t  param;             /* Резерв / громкость AVRCP 0..127 */
} omnia_bt_ctrl_cmd_t;

/* 0x80: EVT_STATUS (Периодический и событийный отчёт AT32 -> ESP32) */
typedef struct {
    uint32_t sample_rate_hz;    /* Текущая тактовая частота I2S2 мастера */
    uint16_t active_dsp_mask;   /* Активные звенья DSP */
    uint8_t  muted;             /* 1 = тракт в Mute */
    uint8_t  bit_perfect_mode;  /* 1 = математический 1:1 24-bit Bit-Perfect активен! */
    uint8_t  active_i2s_src;    /* Текущий открытый вход (omnia_i2s_source_t) */
    uint8_t  usb_vbus_detected; /* 1 = обнаружено +5V / активный хост от ТВ-бокса/ПК/смартфона */
    uint8_t  usb_active_port;   /* 1 = в данный момент выбран вход USB-DAC UAC1 */
    uint8_t  ab_compare_bypass; /* 1 = сейчас нажато сравнение «Чистый исходник» */
    uint8_t  cpu_load_percent;  /* Реальная загрузка ядра Cortex-M4F по счётчику тактов DWT */
    uint8_t  dc_fault_tripped;  /* 1 = сработала аварийная защита от постоянки на выходе */
    uint16_t clip_events;       /* Счётчик перехваченных лимитером пиков */
    float    current_vol_db;    /* Текущее сглаженное положение громкости */
    uint8_t  usb_streaming;     /* 1 = идёт изохронный 24-битный поток UAC1 */
    uint8_t  bt_codec_id;       /* Активный кодек Bluetooth (omnia_bt_codec_t) */
    uint8_t  usb_trigger_en;    /* 1 = авто-триггер USB-DAC включён пользователем */
} omnia_at32_status_t;

/* 0x86: EVT_BT_META — Метаданные трека AVRCP 1.6 и кодек Bluetooth */
typedef struct {
    uint8_t  connected;         /* 1 = смартфон подключён */
    uint8_t  playing;           /* 1 = идёт воспроизведение A2DP */
    uint8_t  codec_id;          /* omnia_bt_codec_t (вкл. aptX-HD 24-bit) */
    uint8_t  bit_depth;         /* 16 или 24 бит */
    uint32_t sample_rate_hz;    /* 44100 или 48000 Гц */
    char     title[64];         /* UTF-8 название трека */
    char     artist[48];        /* UTF-8 исполнитель */
} omnia_bt_meta_evt_t;

/* 0x87: EVT_USB_TRIGGER — Событие срабатывания/отпускания триггера +5V USB-DAC */
typedef struct {
    uint8_t  vbus_active;           /* 1 = питание/хост USB обнаружены, 0 = хост отключён/уснул */
    uint8_t  active_source;         /* Новый активный вход omnia_i2s_source_t                   */
    uint8_t  previous_source;       /* Предыдущий вход (для возврата и отображения на HUD)      */
    uint8_t  woke_from_standby;     /* 1 = стример автоматически пробуждён из режима Standby    */
    uint32_t usb_host_sample_rate;  /* Запрошенная хостом частота UAC1 (48000 или 44100 Гц)     */
} omnia_usb_trigger_evt_t;

/* 0x81: EVT_SIGNAL_METERS (Для стрелочных VU-индикаторов на TFT-экране) */
typedef struct {
    float    in_peak_l_dbfs;
    float    in_peak_r_dbfs;
    float    out_peak_l_dbfs;
    float    out_peak_r_dbfs;
    float    limiter_gr_db;     /* Глубина подавления пика лимитером (0.0 .. -12.0 dB) */
} omnia_signal_meters_t;

/* 0x82: EVT_ACK_NACK */
typedef struct {
    uint8_t  acked_cmd;         /* На какую команду отвечаем */
    uint8_t  status_code;       /* omnia_err_code_t */
} omnia_ack_payload_t;

/* 0x85: EVT_SYS_INFO (Ответ AT32 при старте: диагностика памяти и внешней SPI-флешки) */
typedef struct {
    uint8_t  fw_major;
    uint8_t  fw_minor;
    uint8_t  fw_patch;
    uint8_t  ext_flash_present; /* 1 = обнаружена внешняя микросхема SPI NOR Flash на PA4..PA7 */
    uint16_t sram_size_kb;      /* 224 (после автораскрытия EOPB0) или 96 */
    uint32_t sysclk_hz;         /* 240000000 */
    uint32_t ext_flash_jedec;   /* Например 0xEF4016 (Winbond), 0xC84016 (GigaDevice), 0x856016 (Puya) и т.д. */
    uint32_t ext_flash_bytes;   /* Автоопределённый по SFDP/JEDEC объём внешней флешки в байтах */
} omnia_sys_info_t;

#pragma pack(pop)

/* Проверка выравнивания на этапе компиляции */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(omnia_frame_hdr_t) == OMNIA_PROTO_HEADER_SIZE, "Header packing mismatch");
#endif

/* ========================================================================== *
 *  УПАКОВЩИК И ПОТОКОВЫЙ ПАРСЕР КАДРОВ (Header-Only, без аллокаций в куче)
 * ========================================================================== */

/**
 * @brief  Собирает готовый к отправке по UART бинарный кадр с заголовком и CRC16.
 * @return Полная длина кадра в байтах (или 0, если буфер мал).
 */
static inline size_t omnia_proto_build_frame(
    uint8_t       *out_buf,
    size_t         out_buf_cap,
    uint8_t        flags,
    uint8_t        seq,
    uint8_t        cmd,
    const void    *payload,
    uint16_t       payload_len)
{
    const size_t total = (size_t)OMNIA_PROTO_HEADER_SIZE + payload_len + OMNIA_PROTO_CRC_SIZE;
    if (!out_buf || payload_len > OMNIA_PROTO_MAX_PAYLOAD || out_buf_cap < total) {
        return 0;
    }

    out_buf[0] = (uint8_t)(OMNIA_PROTO_SYNC_WORD & 0xFFU);          /* 0x55 */
    out_buf[1] = (uint8_t)((OMNIA_PROTO_SYNC_WORD >> 8) & 0xFFU);   /* 0xAA */
    out_buf[2] = (uint8_t)(payload_len & 0xFFU);
    out_buf[3] = (uint8_t)((payload_len >> 8) & 0xFFU);
    out_buf[4] = OMNIA_MAKE_VER_FLAGS(flags);
    out_buf[5] = seq;
    out_buf[6] = cmd;

    if (payload_len > 0 && payload != NULL) {
        memcpy(&out_buf[OMNIA_PROTO_HEADER_SIZE], payload, payload_len);
    }

    /* CRC16 считается по полям LEN + VER_FL + SEQ + CMD + PAYLOAD */
    const uint16_t crc = omnia_crc16_ccitt(&out_buf[2], (size_t)(OMNIA_PROTO_HEADER_SIZE - 2U) + payload_len);
    out_buf[OMNIA_PROTO_HEADER_SIZE + payload_len]     = (uint8_t)(crc & 0xFFU);
    out_buf[OMNIA_PROTO_HEADER_SIZE + payload_len + 1] = (uint8_t)((crc >> 8) & 0xFFU);
    return total;
}

/* Состояния конечного автомата приёмника */
typedef enum {
    OMNIA_RX_SYNC_0 = 0,
    OMNIA_RX_SYNC_1,
    OMNIA_RX_HEADER,
    OMNIA_RX_PAYLOAD,
    OMNIA_RX_CRC_0,
    OMNIA_RX_CRC_1
} omnia_rx_state_t;

typedef struct {
    omnia_rx_state_t state;
    uint16_t         hdr_idx;
    uint16_t         pay_idx;
    uint16_t         rx_crc;
    uint16_t         calc_crc;
    omnia_frame_hdr_t hdr;
    uint8_t          payload[OMNIA_PROTO_MAX_PAYLOAD];
    uint32_t         frames_ok;
    uint32_t         crc_errors;
} omnia_proto_parser_t;

static inline void omnia_proto_parser_init(omnia_proto_parser_t *p)
{
    if (p) {
        memset(p, 0, sizeof(*p));
        p->state = OMNIA_RX_SYNC_0;
    }
}

/**
 * @brief  Подаёт 1 принятый байт в парсер.
 * @return true, если только что успешно принят и проверен по CRC16 целый кадр
 *         (данные лежат в p->hdr и p->payload).
 */
static inline bool omnia_proto_parser_feed(omnia_proto_parser_t *p, uint8_t byte)
{
    switch (p->state) {
    case OMNIA_RX_SYNC_0:
        if (byte == (uint8_t)(OMNIA_PROTO_SYNC_WORD & 0xFFU)) {
            p->state = OMNIA_RX_SYNC_1;
        }
        break;

    case OMNIA_RX_SYNC_1:
        if (byte == (uint8_t)((OMNIA_PROTO_SYNC_WORD >> 8) & 0xFFU)) {
            p->hdr.sync = OMNIA_PROTO_SYNC_WORD;
            p->hdr_idx  = 2U;
            p->calc_crc = 0xFFFFU;
            p->state    = OMNIA_RX_HEADER;
        } else if (byte != (uint8_t)(OMNIA_PROTO_SYNC_WORD & 0xFFU)) {
            p->state = OMNIA_RX_SYNC_0;
        }
        break;

    case OMNIA_RX_HEADER: {
        uint8_t *raw_hdr = (uint8_t *)&p->hdr;
        raw_hdr[p->hdr_idx++] = byte;
        p->calc_crc = omnia_crc16_update(p->calc_crc, byte);

        if (p->hdr_idx >= OMNIA_PROTO_HEADER_SIZE) {
            if (p->hdr.payload_len > OMNIA_PROTO_MAX_PAYLOAD ||
                OMNIA_GET_VERSION(p->hdr.ver_flags) != OMNIA_PROTO_VERSION) {
                p->state = OMNIA_RX_SYNC_0;
            } else if (p->hdr.payload_len == 0U) {
                p->state = OMNIA_RX_CRC_0;
            } else {
                p->pay_idx = 0U;
                p->state   = OMNIA_RX_PAYLOAD;
            }
        }
        break;
    }

    case OMNIA_RX_PAYLOAD:
        p->payload[p->pay_idx++] = byte;
        p->calc_crc = omnia_crc16_update(p->calc_crc, byte);
        if (p->pay_idx >= p->hdr.payload_len) {
            p->state = OMNIA_RX_CRC_0;
        }
        break;

    case OMNIA_RX_CRC_0:
        p->rx_crc = byte;
        p->state  = OMNIA_RX_CRC_1;
        break;

    case OMNIA_RX_CRC_1:
        p->rx_crc |= (uint16_t)((uint16_t)byte << 8);
        p->state   = OMNIA_RX_SYNC_0;
        if (p->rx_crc == p->calc_crc) {
            p->frames_ok++;
            return true;
        }
        p->crc_errors++;
        break;
    }
    return false;
}

#ifdef __cplusplus
}
#endif

#endif /* OMNIA_PROTOCOL_H */
