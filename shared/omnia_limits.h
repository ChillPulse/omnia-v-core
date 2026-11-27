/**
 * @file    omnia_limits.h
 * @brief   Единственный источник правды для допустимых диапазонов параметров DSP/протокола.
 *          Используется: (1) валидацией входящих кадров (dsp_validation.c),
 *          (2) клэмпами внутри движка (dsp_engine.c), (3) комментариями протокола,
 *          (4) тестами. Любое изменение — только здесь.
 *
 * Порядок проверки любого параметра: validation -> policy -> engine clamp -> ТЗ.
 * Validation всегда строже или равна клэмпу движка: запрещённые кадры отбрасываются,
 * а не молча прижимаются.
 */

#ifndef OMNIA_LIMITS_H
#define OMNIA_LIMITS_H

/* --- 1. Subsonic HPF + Headroom --- */
#define OMNIA_HPF_FREQ_MIN_HZ        15.0f
#define OMNIA_HPF_FREQ_MAX_HZ        40.0f
#define OMNIA_HPF_Q_MIN              0.1f
#define OMNIA_HPF_Q_MAX              18.0f
#define OMNIA_HPF_MANUAL_ATTEN_MIN_DB (-18.0f)  /* 0..-18 dB по ТЗ §4.1 */
#define OMNIA_HPF_MANUAL_ATTEN_MAX_DB 0.0f

/* --- Полосы PEQ (Room и User) --- */
#define OMNIA_BAND_F0_MIN_HZ         15.0f
#define OMNIA_BAND_F0_MAX_HZ         22000.0f
#define OMNIA_BAND_Q_MIN             0.1f
#define OMNIA_BAND_Q_MAX             18.0f
#define OMNIA_BAND_GAIN_MIN_DB      (-18.0f)
#define OMNIA_BAND_GAIN_MAX_DB       18.0f

/* --- Room PEQ: задержки/тримы/фаза --- */
#define OMNIA_ROOM_DELAY_MIN_MS      0.0f
#define OMNIA_ROOM_DELAY_MAX_MS      20.0f
#define OMNIA_ROOM_TRIM_MIN_DB       (-12.0f)
#define OMNIA_ROOM_TRIM_MAX_DB        6.0f

/* --- 5. Anti-Ad AGC --- */
#define OMNIA_ANTI_AD_THRESH_MIN     0.25f
#define OMNIA_ANTI_AD_THRESH_MAX     0.90f
#define OMNIA_ANTI_AD_ATTACK_MIN     0.0f
#define OMNIA_ANTI_AD_ATTACK_MAX     1.0f
#define OMNIA_ANTI_AD_RELEASE_MIN    0.0f
#define OMNIA_ANTI_AD_RELEASE_MAX    1.0f
#define OMNIA_ANTI_AD_MAKEUP_MIN_DB  0.0f
#define OMNIA_ANTI_AD_MAKEUP_MAX_DB  6.0f

/* --- 6. VBB / Psychoacoustic Bass --- */
#define OMNIA_VBB_AMOUNT_MIN         0.0f
#define OMNIA_VBB_AMOUNT_MAX         1.0f
#define OMNIA_VBB_FREQ_MIN_HZ        40.0f   /* ТЗ §4.1: 40..120 Гц */
#define OMNIA_VBB_FREQ_MAX_HZ        120.0f
#define OMNIA_VBB_DRIVE_MIN          0.0f
#define OMNIA_VBB_DRIVE_MAX          1.0f
#define OMNIA_VBB_LP_MIN_HZ          90.0f   /* Ограничение спектра гармоник */
#define OMNIA_VBB_LP_MAX_HZ          240.0f
#define OMNIA_VBB_MIX_MIN            0.0f
#define OMNIA_VBB_MIX_MAX            1.0f

/* --- 7. AETHER («Эфир») --- */
#define OMNIA_AETHER_BLEND_MIN       0.0f
#define OMNIA_AETHER_BLEND_MAX       1.0f
#define OMNIA_AETHER_WIDTH_MIN       0.0f    /* 0 = моно, 1 = оригинал, 2 = максимум */
#define OMNIA_AETHER_WIDTH_MAX       2.0f
#define OMNIA_AETHER_CENTER_MIN_DB   (-6.0f)
#define OMNIA_AETHER_CENTER_MAX_DB    6.0f
#define OMNIA_AETHER_BASS_XOVER_MIN  40.0f
#define OMNIA_AETHER_BASS_XOVER_MAX  250.0f
#define OMNIA_AETHER_SIDE_HPF_MIN    20.0f
#define OMNIA_AETHER_SIDE_HPF_MAX    180.0f
#define OMNIA_AETHER_AIR_GAIN_MIN_DB 0.0f
#define OMNIA_AETHER_AIR_GAIN_MAX_DB 8.0f
#define OMNIA_AETHER_AIR_FREQ_MIN    5500.0f
#define OMNIA_AETHER_AIR_FREQ_MAX    16000.0f
#define OMNIA_AETHER_AIR_Q_MIN       0.4f
#define OMNIA_AETHER_AIR_Q_MAX       1.6f
#define OMNIA_AETHER_DEHARSH_MIN     0.0f
#define OMNIA_AETHER_DEHARSH_MAX     1.0f
#define OMNIA_AETHER_DEPTH_MIN       0.0f
#define OMNIA_AETHER_DEPTH_MAX       0.45f
#define OMNIA_AETHER_STAGE_MIN_MS    0.4f    /* ТЗ §4.2: 0.4..12 мс */
#define OMNIA_AETHER_STAGE_MAX_MS    12.0f
#define OMNIA_AETHER_TRIODE_MIN      0.0f
#define OMNIA_AETHER_TRIODE_MAX      0.30f
#define OMNIA_AETHER_XFEED_MIN       0.0f
#define OMNIA_AETHER_XFEED_MAX       1.0f
#define OMNIA_AETHER_XFEED_FC_MIN    500.0f
#define OMNIA_AETHER_XFEED_FC_MAX    900.0f
#define OMNIA_AETHER_XFEED_DELAY_US  260U    /* Наушниковая межушная задержка Bauer, ~260 мкс */

/* --- 8a. Dynamic Loudness (ISO 226) --- */
#define OMNIA_LOUD_REF_MIN_DB        (-96.0f)
#define OMNIA_LOUD_REF_MAX_DB         0.0f
#define OMNIA_LOUD_MAX_BASS_MIN_DB    0.0f
#define OMNIA_LOUD_MAX_BASS_MAX_DB    12.0f
#define OMNIA_LOUD_MAX_HIGH_MIN_DB    0.0f
#define OMNIA_LOUD_MAX_HIGH_MAX_DB    6.0f

/* --- 9. Limiter --- */
#define OMNIA_LIM_CEILING_MIN_DBFS  (-3.0f)
#define OMNIA_LIM_CEILING_MAX_DBFS  (-0.2f)
#define OMNIA_LIM_LOOKAHEAD_MIN_MS   0.5f
#define OMNIA_LIM_LOOKAHEAD_MAX_MS   3.0f
#define OMNIA_LIM_RELEASE_MIN_MS     30.0f
#define OMNIA_LIM_RELEASE_MAX_MS     500.0f

/* --- Громкость / ReplayGain / баланс --- */
#define OMNIA_VOL_MIN_DB            (-96.0f)
#define OMNIA_VOL_MAX_DB             0.0f
#define OMNIA_REGAIN_MIN_DB         (-18.0f)
#define OMNIA_REGAIN_MAX_DB          6.0f
#define OMNIA_BALANCE_MIN           (-1.0f)
#define OMNIA_BALANCE_MAX            1.0f

/* Заметка: движок может применять дополнительные жёсткие потолки,
 * не зависящие от пользовательской настройки (например, частота фильтра
 * относительно Fs/2). Они не являются взаимозаменяемыми с пользовательскими
 * диапазонами и должны оставаться строже. */

#endif /* OMNIA_LIMITS_H */
