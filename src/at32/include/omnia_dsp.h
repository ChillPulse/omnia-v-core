/**
 * @file    omnia_dsp.h
 * @brief   Студийный 9-звенный по-сэмпловый аудиопроцессор OMNIA (32-bit float + Bit-Perfect Direct)
 *          Не зависит от регистров МК: компилируется на Cortex-M4F FPU и в нативных автотестах GCC.
 */

#ifndef OMNIA_DSP_H
#define OMNIA_DSP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "omnia_protocol.h"
#include "omnia_limits.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Размеры линий задержки (размещаются в секции .sram_ext 128 КБ на AT32) */
#define OMNIA_ROOM_DELAY_RING_SIZE      1024U   /* Степень двойки: до 20.0 мс (960 сэмплов @ 48 кГц, маска 1023) */
#define OMNIA_ROOM_MAX_DELAY_MS         20.0f   /* До 6.86 м разницы расстояний до левой/правой АС */
#define OMNIA_AETHER_MAX_DELAY_SAMPLES  640U    /* До ~13.3 мс стерео/сайд сцены при 48 кГц */
#define OMNIA_LIMITER_MAX_LOOKAHEAD     160U    /* До ~3.33 мс упреждения пиков при 48 кГц */

/* Внутреннее состояние биквадного фильтра Direct Form II Transposed.
 * Коэффициенты могут плавно доводиться до цели (ramp) — см. omnia_dsp_ctx_t::coeff_ramp_enable.
 * sb* — старты ramp'а (текущее значение на момент постановки цели), tb* — цели. */
typedef struct {
    float b0, b1, b2;
    float a1, a2;
    float z1_l, z2_l;
    float z1_r, z2_r;
    bool  bypass;
    float sb0, sb1, sb2, sa1, sa2;   /* старты интерполяции */
    float tb0, tb1, tb2, ta1, ta2;   /* цели интерполяции   */
    uint16_t ramp_left;              /* отсчётов до конца рампы; 0 = рампы нет */
    uint16_t ramp_total;             /* длительность текущей рампы в отсчётах */
    uint16_t _reserved;
} omnia_biquad_state_t;

/* Внешняя арена буферов задержки (передаётся из .sram_ext на AT32 или статики в тестах) */
#define OMNIA_XFEED_DELAY_MAX_SAMPLES  (32U)   /* >= 25 отсчётов для 260 мкс @ 96 кГц */

typedef struct {
    float room_delay_l[OMNIA_ROOM_DELAY_RING_SIZE];
    float room_delay_r[OMNIA_ROOM_DELAY_RING_SIZE];
    float aether_delay_line[OMNIA_AETHER_MAX_DELAY_SAMPLES];
    float limiter_buf_l[OMNIA_LIMITER_MAX_LOOKAHEAD];
    float limiter_buf_r[OMNIA_LIMITER_MAX_LOOKAHEAD];
    float xfeed_delay_l[OMNIA_XFEED_DELAY_MAX_SAMPLES];  /* Bauer Crossfeed ~260 мкс */
    float xfeed_delay_r[OMNIA_XFEED_DELAY_MAX_SAMPLES];
} omnia_dsp_delay_arena_t;

/* Полный контекст студийного DSP-тракта */
typedef struct {
    uint32_t sample_rate_hz;
    uint16_t active_mask;
    bool     muted;
    bool     ab_compare_bypass;
    bool     bit_perfect_active;
    /* false внутри omnia_dsp_reset_states/omnia_dsp_init (мгновенный пересчёт без рампы);
     * true после завершения init/set_sample_rate: последующие пересчёты коэффициентов
     * (recalc_all_coefficients) доводят биквады рампой ~5 мс вместо мгновенной подстановки
     * (anti-click при drag-настройке PEQ). Bypass→enable звена рампой НЕ идёт (identity snap). */
    bool     coeff_ramp_enable;

    /* Плавный регулятор громкости и анти-поп рампы */
    float    target_vol_db;
    float    current_vol_db;    /* Сглаженное отображаемое значение (телеметрия) */
    float    replaygain_db;
    float    balance;
    float    current_gain_l;
    float    current_gain_r;
    float    target_gain_l;
    float    target_gain_r;
    float    gain_slewing_coeff;

    /* Сглаживание обхода / A-B сравнения с компенсацией RMS */
    float    wet_mix_current;
    float    wet_mix_target;
    float    wet_mix_slew;
    float    rms_in_env;
    float    rms_out_env;

    /* 1. Subsonic HPF + 2. Dynamic Headroom */
    omnia_hpf_headroom_t hpf_cfg;
    omnia_biquad_state_t subsonic_biquad;
    float                headroom_gain_lin;

    /* 3. Room PEQ (5 полос + суб-сэмпловые задержки АС + фаза + трим) + 4. User PEQ (10 полос) */
    omnia_room_peq_t     room_cfg;
    omnia_biquad_state_t room_biquads[OMNIA_MAX_ROOM_PEQ_BANDS];
    float                room_delay_target_l;
    float                room_delay_target_r;
    float                room_delay_cur_l;
    float                room_delay_cur_r;
    float                room_gain_target_l;
    float                room_gain_target_r;
    float                room_gain_cur_l;
    float                room_gain_cur_r;
    uint16_t             room_delay_wr_idx;
    omnia_user_peq_t     user_cfg;
    omnia_biquad_state_t user_biquads[OMNIA_MAX_USER_PEQ_BANDS];

    /* 5. Anti-Ad AGC */
    omnia_anti_ad_t      anti_ad_cfg;
    float                agc_env;
    float                agc_makeup_lin;

    /* 6. VBB / Psychoacoustic Bass */
    omnia_vbb_params_t   vbb_cfg;
    omnia_biquad_state_t vbb_extract_lp;
    omnia_biquad_state_t vbb_harm_lp;
    omnia_biquad_state_t vbb_dc_hp;
    omnia_biquad_state_t vbb_shelf;     /* Режим 0 "Clean Shelf": настоящая НЧ-полка */

    /* 7. Studio AETHER Processor («Эфир») */
    omnia_aether_params_t aether_cfg;
    float                 aether_blend_cur;
    float                 aether_width_cur;
    float                 aether_center_gain;
    omnia_biquad_state_t  aether_bass_hp;   /* Моно-замок НЧ в S-канале: честный ФВЧ 2-го порядка (side - LP(side) давал бы фазовую ошибку ~ fj/fc -> -19 дБ вместо -32 дБ) */
    omnia_biquad_state_t  aether_side_hp;   /* Очистка S-канала */
    omnia_biquad_state_t  aether_air_hs;    /* Полка шёлкового воздуха */
    float                 deharsh_env;      /* Детектор сибилянтов ВЧ */
    float                 ap_z1, ap_z2;     /* Каскад фазовых All-Pass декорреляторов */
    float                 triode_dc_l, triode_dc_r;
    float                 xfeed_lp_l, xfeed_lp_r;
    uint16_t              xfeed_wr_idx;     /* Кольцевой индекс линии задержки кроссфида */
    uint16_t              aether_wr_idx;
    uint16_t              aether_delay_len;

    /* 8. Dynamic Loudness (ISO 226) */
    omnia_loudness_params_t loudness_cfg;
    omnia_biquad_state_t    loudness_low_ls;
    omnia_biquad_state_t    loudness_high_hs;

    /* 9. Final Safety Look-Ahead Limiter + DC Guard + TPDF Dither */
    omnia_limiter_params_t limiter_cfg;
    float                  limiter_ceiling_lin;
    float                  limiter_env;
    float                  limiter_hold_gain;
    float                  limiter_rel_coeff;
    uint16_t               limiter_hold_cnt;
    uint16_t               limiter_wr_idx;
    uint16_t               limiter_delay_len;
    uint32_t               prng_state;
    /* Оценка суб-сэмплового (true) пика: хвост последних 3 отсчётов на канал
     * (+ текущий отсчёт как правый сосед). Интерполяция Catmull-Rom оценивает пик
     * между отсчётами; ограничение: кубическая оценка, не polyphase 4x. */
    float                  tp_hist_l[4];   /* [0..2] используются, [3] зарезервирован */
    float                  tp_hist_r[4];

    /* Детектор постоянки (DC Fault Guard) и пикметры */
    float                  dc_track_l;
    float                  dc_track_r;
    bool                   dc_fault_tripped;
    uint16_t               clip_events;
    float                  peak_in_l, peak_in_r;
    float                  peak_out_l, peak_out_r;
    float                  min_limiter_gain;

    omnia_dsp_delay_arena_t *arena;
} omnia_dsp_ctx_t;

/* --- API управления и обработки --- */
/* Внутреннее: экспортировано для хост-тестов (стабильность полюсов). Не использовать вне тестов/движка. */
void omnia_dsp_biquad_design(
    omnia_biquad_state_t *st,
    omnia_biquad_type_t   type,
    uint32_t              fs_hz,
    float                 f0_hz,
    float                 q,
    float                 gain_db);

/**
 * @brief  Заводской пресет Эфира по индексу 0..6 (ТЗ §4.2.8). Включение только в RAM, без Flash.
 * @return false при slot >= 7 (ctx не изменяется).
 */
bool omnia_dsp_load_aether_factory(omnia_dsp_ctx_t *ctx, uint8_t slot);

/**
 * @brief  Сервисный сброс аварийной защиты от постоянки (DC Guard) и счётчика клипов.
 *         Вызывается по команде RESET_DC_FAULT; громкость не поднимается, пока
 *         gain- цели не восстановлены и muted актуален.
 */
void omnia_dsp_reset_dc_fault(omnia_dsp_ctx_t *ctx);

void omnia_dsp_init(omnia_dsp_ctx_t *ctx, omnia_dsp_delay_arena_t *arena, uint32_t sample_rate_hz);
void omnia_dsp_set_sample_rate(omnia_dsp_ctx_t *ctx, uint32_t sample_rate_hz);
void omnia_dsp_reset_states(omnia_dsp_ctx_t *ctx);

void omnia_dsp_set_mute(omnia_dsp_ctx_t *ctx, bool mute, uint16_t ramp_ms);
void omnia_dsp_set_volume(omnia_dsp_ctx_t *ctx, float vol_db, float replaygain_db, float balance, uint16_t ramp_ms);
void omnia_dsp_set_bypass_mask(omnia_dsp_ctx_t *ctx, uint16_t active_mask, uint16_t ramp_ms);
void omnia_dsp_set_ab_compare(omnia_dsp_ctx_t *ctx, bool bypass_compare);

void omnia_dsp_apply_hpf_headroom(omnia_dsp_ctx_t *ctx, const omnia_hpf_headroom_t *cfg);
void omnia_dsp_apply_room_peq(omnia_dsp_ctx_t *ctx, const omnia_room_peq_t *cfg);
void omnia_dsp_apply_user_peq(omnia_dsp_ctx_t *ctx, const omnia_user_peq_t *cfg);
void omnia_dsp_apply_anti_ad(omnia_dsp_ctx_t *ctx, const omnia_anti_ad_t *cfg);
void omnia_dsp_apply_vbb(omnia_dsp_ctx_t *ctx, const omnia_vbb_params_t *cfg);
void omnia_dsp_apply_aether(omnia_dsp_ctx_t *ctx, const omnia_aether_params_t *cfg);
void omnia_dsp_apply_loudness(omnia_dsp_ctx_t *ctx, const omnia_loudness_params_t *cfg);
void omnia_dsp_apply_limiter(omnia_dsp_ctx_t *ctx, const omnia_limiter_params_t *cfg);

/**
 * @brief  Обрабатывает блок стерео-сэмплов (32-bit signed PCM, выровненный по старшему биту I2S).
 *         Bit-Perfect Direct (active_mask == 0, !muted, vol == 0 dB, gain осели) копирует int32 1:1.
 *         ВНИМАНИЕ: в Bit-Perfect лимитер/DC-guard НЕ работают — это строгая трактовка bit-perfect.
 *         Дефолтная маска из коробки = SUBSONIC_HPF | LIMITER (безопасность). Bit-perfect — явный выбор.
 */
void omnia_dsp_process_block_i32(
    omnia_dsp_ctx_t *ctx,
    const int32_t   *in_lr_interleaved,
    int32_t         *out_lr_interleaved,
    size_t           stereo_frames);

void omnia_dsp_read_and_reset_meters(omnia_dsp_ctx_t *ctx, omnia_signal_meters_t *out_meters);

#ifdef __cplusplus
}
#endif

#endif /* OMNIA_DSP_H */
