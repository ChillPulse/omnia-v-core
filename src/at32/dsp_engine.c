/**
 * @file    dsp_engine.c
 * @brief   Реализация 9-звенного студийного DSP-тракта OMNIA (32-bit float + Bit-Perfect Direct)
 */

#include "omnia_dsp.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DSP_NORM_TO_FLOAT       (1.0f / 2147483648.0f)  /* int32 full-scale -> [-1.0, +1.0) */
#define DSP_SCALE_TO_INT32      (2147483647.0f)
#define DSP_ANTI_DENORMAL       (1.0e-24f)

/* --- Вспомогательная математика --- */
static inline float clampf(float v, float lo, float hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static inline float db_to_lin(float db)
{
    if (db <= -95.9f) {
        return 0.0f;
    }
    return powf(10.0f, db * 0.05f);
}

static inline float lin_to_db(float lin)
{
    if (lin <= 1.0e-5f) {
        return -100.0f;
    }
    return 20.0f * log10f(lin);
}

static inline float sanitize_sample(float x)
{
    /* Быстрая проверка конечности числа через побитовое представление IEEE-754 */
    union { float f; uint32_t u; } v = { x };
    if ((v.u & 0x7F800000U) == 0x7F800000U) {
        return 0.0f;
    }
    return x;
}

static inline uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    if (x == 0U) {
        x = 0x9E3779B9U;
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Коэффициент экспоненциального сглаживания 1-го порядка по времени (мс) */
static float calc_slew_coeff(uint32_t fs, float time_ms)
{
    if (time_ms <= 0.1f || fs == 0U) {
        return 1.0f;
    }
    const float samples = ((float)fs * time_ms) * 0.001f;
    return 1.0f - expf(-3.0f / samples); /* ~95% установления за time_ms */
}

/* --- Расчёт коэффициентов биквада (Audio EQ Cookbook Роберта Бристоу-Джонсона) --- */
static void biquad_reset_z(omnia_biquad_state_t *st)
{
    st->z1_l = 0.0f;
    st->z2_l = 0.0f;
    st->z1_r = 0.0f;
    st->z2_r = 0.0f;
}

static void biquad_design(
    omnia_biquad_state_t *st,
    omnia_biquad_type_t   type,
    uint32_t              fs_hz,
    float                 f0_hz,
    float                 q,
    float                 gain_db)
{
    if (type == OMNIA_BIQUAD_OFF || fs_hz < 8000U) {
        st->b0 = 1.0f; st->b1 = 0.0f; st->b2 = 0.0f;
        st->a1 = 0.0f; st->a2 = 0.0f;
        st->bypass = true;
        return;
    }

    /* При пренебрежимо малом усилении Peak/Shelf переводим звено в прозрачность */
    if ((type == OMNIA_BIQUAD_PEAK || type == OMNIA_BIQUAD_LOW_SHELF || type == OMNIA_BIQUAD_HIGH_SHELF) &&
        fabsf(gain_db) < 0.02f) {
        st->b0 = 1.0f; st->b1 = 0.0f; st->b2 = 0.0f;
        st->a1 = 0.0f; st->a2 = 0.0f;
        st->bypass = true;
        return;
    }

    const float fs   = (float)fs_hz;
    const float f0   = clampf(f0_hz, 12.0f, fs * 0.46f);
    const float qval = clampf(q, 0.12f, 20.0f);
    const float gdb  = clampf(gain_db, -24.0f, 24.0f);

    const float w0    = (2.0f * (float)M_PI * f0) / fs;
    const float cosw0 = cosf(w0);
    const float sinw0 = sinf(w0);
    const float alpha = sinw0 / (2.0f * qval);
    const float A     = powf(10.0f, gdb / 40.0f);

    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a0 = 1.0f, a1 = 0.0f, a2 = 0.0f;

    switch (type) {
    case OMNIA_BIQUAD_PEAK:
        b0 =  1.0f + alpha * A;
        b1 = -2.0f * cosw0;
        b2 =  1.0f - alpha * A;
        a0 =  1.0f + alpha / A;
        a1 = -2.0f * cosw0;
        a2 =  1.0f - alpha / A;
        break;

    case OMNIA_BIQUAD_LOW_SHELF: {
        const float two_sqrtA_alpha = 2.0f * sqrtf(A) * alpha;
        b0 =        A * ((A + 1.0f) - (A - 1.0f) * cosw0 + two_sqrtA_alpha);
        b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw0);
        b2 =        A * ((A + 1.0f) - (A - 1.0f) * cosw0 - two_sqrtA_alpha);
        a0 =             (A + 1.0f) + (A - 1.0f) * cosw0 + two_sqrtA_alpha;
        a1 =    -2.0f * ((A - 1.0f) + (A + 1.0f) * cosw0);
        a2 =             (A + 1.0f) + (A - 1.0f) * cosw0 - two_sqrtA_alpha;
        break;
    }

    case OMNIA_BIQUAD_HIGH_SHELF: {
        const float two_sqrtA_alpha = 2.0f * sqrtf(A) * alpha;
        b0 =         A * ((A + 1.0f) + (A - 1.0f) * cosw0 + two_sqrtA_alpha);
        b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0);
        b2 =         A * ((A + 1.0f) + (A - 1.0f) * cosw0 - two_sqrtA_alpha);
        a0 =              (A + 1.0f) - (A - 1.0f) * cosw0 + two_sqrtA_alpha;
        a1 =      2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0);
        a2 =              (A + 1.0f) - (A - 1.0f) * cosw0 - two_sqrtA_alpha;
        break;
    }

    case OMNIA_BIQUAD_LOW_PASS:
        b0 = (1.0f - cosw0) * 0.5f;
        b1 =  1.0f - cosw0;
        b2 = (1.0f - cosw0) * 0.5f;
        a0 =  1.0f + alpha;
        a1 = -2.0f * cosw0;
        a2 =  1.0f - alpha;
        break;

    case OMNIA_BIQUAD_HIGH_PASS:
        b0 =  (1.0f + cosw0) * 0.5f;
        b1 = -(1.0f + cosw0);
        b2 =  (1.0f + cosw0) * 0.5f;
        a0 =   1.0f + alpha;
        a1 =  -2.0f * cosw0;
        a2 =   1.0f - alpha;
        break;

    case OMNIA_BIQUAD_NOTCH:
        b0 =  1.0f;
        b1 = -2.0f * cosw0;
        b2 =  1.0f;
        a0 =  1.0f + alpha;
        a1 = -2.0f * cosw0;
        a2 =  1.0f - alpha;
        break;

    case OMNIA_BIQUAD_ALL_PASS:
        b0 =  1.0f - alpha;
        b1 = -2.0f * cosw0;
        b2 =  1.0f + alpha;
        a0 =  1.0f + alpha;
        a1 = -2.0f * cosw0;
        a2 =  1.0f - alpha;
        break;

    default:
        st->bypass = true;
        return;
    }

    const float inv_a0 = 1.0f / a0;
    st->b0 = b0 * inv_a0;
    st->b1 = b1 * inv_a0;
    st->b2 = b2 * inv_a0;
    st->a1 = a1 * inv_a0;
    st->a2 = a2 * inv_a0;
    st->bypass = false;
}

static inline void biquad_step_stereo(omnia_biquad_state_t *st, float *l, float *r)
{
    if (st->bypass) {
        return;
    }
    const float xl = *l;
    const float xr = *r;

    const float yl = st->b0 * xl + st->z1_l;
    st->z1_l = st->b1 * xl - st->a1 * yl + st->z2_l + DSP_ANTI_DENORMAL;
    st->z2_l = st->b2 * xl - st->a2 * yl - DSP_ANTI_DENORMAL;

    const float yr = st->b0 * xr + st->z1_r;
    st->z1_r = st->b1 * xr - st->a1 * yr + st->z2_r + DSP_ANTI_DENORMAL;
    st->z2_r = st->b2 * xr - st->a2 * yr - DSP_ANTI_DENORMAL;

    *l = yl;
    *r = yr;
}

static inline float biquad_step_mono_l(omnia_biquad_state_t *st, float x)
{
    if (st->bypass) {
        return x;
    }
    const float y = st->b0 * x + st->z1_l;
    st->z1_l = st->b1 * x - st->a1 * y + st->z2_l + DSP_ANTI_DENORMAL;
    st->z2_l = st->b2 * x - st->a2 * y - DSP_ANTI_DENORMAL;
    return y;
}

/* --- Авторасчёт запаса по перегрузке (Dynamic Headroom) --- */
static void update_headroom_gain(omnia_dsp_ctx_t *ctx)
{
    float atten_db = clampf(ctx->hpf_cfg.manual_atten_db, -24.0f, 0.0f);

    if (ctx->hpf_cfg.auto_headroom_en) {
        float max_boost_db = 0.0f;
        if ((ctx->active_mask & OMNIA_DSP_BIT_ROOM_PEQ) && ctx->room_cfg.enable) {
            for (uint8_t i = 0; i < ctx->room_cfg.band_count && i < OMNIA_MAX_ROOM_PEQ_BANDS; ++i) {
                if (ctx->room_cfg.bands[i].type != OMNIA_BIQUAD_OFF && ctx->room_cfg.bands[i].gain_db > 0.0f) {
                    max_boost_db += ctx->room_cfg.bands[i].gain_db * 0.65f;
                }
            }
        }
        if ((ctx->active_mask & OMNIA_DSP_BIT_USER_PEQ) && ctx->user_cfg.enable) {
            for (uint8_t i = 0; i < ctx->user_cfg.band_count && i < OMNIA_MAX_USER_PEQ_BANDS; ++i) {
                if (ctx->user_cfg.bands[i].type != OMNIA_BIQUAD_OFF && ctx->user_cfg.bands[i].gain_db > 0.0f) {
                    max_boost_db += ctx->user_cfg.bands[i].gain_db * 0.65f;
                }
            }
        }
        if ((ctx->active_mask & OMNIA_DSP_BIT_VBB_BASS) && ctx->vbb_cfg.enable) {
            max_boost_db += ctx->vbb_cfg.amount * 4.5f;
        }
        if ((ctx->active_mask & OMNIA_DSP_BIT_AETHER) && ctx->aether_cfg.enable && ctx->aether_cfg.air_gain_db > 0.0f) {
            max_boost_db += ctx->aether_cfg.air_gain_db * 0.4f * ctx->aether_cfg.master_blend;
        }
        max_boost_db = clampf(max_boost_db, 0.0f, 15.0f);
        if (-max_boost_db < atten_db) {
            atten_db = -max_boost_db;
        }
    }
    ctx->headroom_gain_lin = db_to_lin(atten_db);
}

static void update_loudness_filters(omnia_dsp_ctx_t *ctx)
{
    if (!ctx->loudness_cfg.enable || !(ctx->active_mask & OMNIA_DSP_BIT_LOUDNESS)) {
        ctx->loudness_low_ls.bypass  = true;
        ctx->loudness_high_hs.bypass = true;
        return;
    }

    /* Насколько текущая громкость тише референсного комфортного уровня */
    const float deficit_db = ctx->loudness_cfg.ref_volume_db - ctx->target_vol_db;
    if (deficit_db <= 0.5f) {
        ctx->loudness_low_ls.bypass  = true;
        ctx->loudness_high_hs.bypass = true;
        return;
    }

    /* Плавная кривая ISO 226: полный подъём достигается при убавлении на 36 дБ ниже референса */
    const float ratio    = clampf(deficit_db / 36.0f, 0.0f, 1.0f);
    const float low_db   = ratio * clampf(ctx->loudness_cfg.max_bass_boost_db, 0.0f, 12.0f);
    const float high_db  = ratio * clampf(ctx->loudness_cfg.max_high_boost_db, 0.0f, 6.0f);

    biquad_design(&ctx->loudness_low_ls,  OMNIA_BIQUAD_LOW_SHELF,  ctx->sample_rate_hz, 75.0f,   0.707f, low_db);
    biquad_design(&ctx->loudness_high_hs, OMNIA_BIQUAD_HIGH_SHELF, ctx->sample_rate_hz, 9500.0f, 0.707f, high_db);
}

static void update_volume_targets(omnia_dsp_ctx_t *ctx, uint16_t ramp_ms)
{
    ctx->gain_slewing_coeff = calc_slew_coeff(ctx->sample_rate_hz, (float)ramp_ms);
    if (ctx->muted || ctx->dc_fault_tripped) {
        ctx->target_gain_l = 0.0f;
        ctx->target_gain_r = 0.0f;
        return;
    }

    const float total_db = clampf(ctx->target_vol_db + ctx->replaygain_db, -96.0f, 6.0f);
    const float base_lin = (fabsf(total_db) < 0.005f) ? 1.0f : db_to_lin(total_db);
    const float bal      = clampf(ctx->balance, -1.0f, 1.0f);

    ctx->target_gain_l = base_lin * ((bal > 0.0f) ? (1.0f - bal) : 1.0f);
    ctx->target_gain_r = base_lin * ((bal < 0.0f) ? (1.0f + bal) : 1.0f);
    update_loudness_filters(ctx);
}

static void recalc_all_coefficients(omnia_dsp_ctx_t *ctx)
{
    /* 1. Subsonic HPF */
    biquad_design(&ctx->subsonic_biquad,
                  ctx->hpf_cfg.hpf_enable ? OMNIA_BIQUAD_HIGH_PASS : OMNIA_BIQUAD_OFF,
                  ctx->sample_rate_hz, ctx->hpf_cfg.hpf_freq_hz, ctx->hpf_cfg.hpf_q, 0.0f);

    /* 3. Room PEQ */
    for (uint8_t i = 0; i < OMNIA_MAX_ROOM_PEQ_BANDS; ++i) {
        if (ctx->room_cfg.enable && i < ctx->room_cfg.band_count) {
            const omnia_biquad_band_t *b = &ctx->room_cfg.bands[i];
            biquad_design(&ctx->room_biquads[i], (omnia_biquad_type_t)b->type,
                          ctx->sample_rate_hz, b->f0_hz, b->q, b->gain_db);
        } else {
            ctx->room_biquads[i].bypass = true;
        }
    }

    /* 4. User PEQ */
    for (uint8_t i = 0; i < OMNIA_MAX_USER_PEQ_BANDS; ++i) {
        if (ctx->user_cfg.enable && i < ctx->user_cfg.band_count) {
            const omnia_biquad_band_t *b = &ctx->user_cfg.bands[i];
            biquad_design(&ctx->user_biquads[i], (omnia_biquad_type_t)b->type,
                          ctx->sample_rate_hz, b->f0_hz, b->q, b->gain_db);
        } else {
            ctx->user_biquads[i].bypass = true;
        }
    }

    /* 5. Anti-Ad */
    ctx->agc_makeup_lin = db_to_lin(clampf(ctx->anti_ad_cfg.makeup_db, 0.0f, 9.0f));

    /* 6. VBB Bass */
    if (ctx->vbb_cfg.enable) {
        biquad_design(&ctx->vbb_extract_lp, OMNIA_BIQUAD_LOW_PASS,
                      ctx->sample_rate_hz, clampf(ctx->vbb_cfg.freq_hz, 35.0f, 130.0f), 0.75f, 0.0f);
        biquad_design(&ctx->vbb_harm_lp, OMNIA_BIQUAD_LOW_PASS,
                      ctx->sample_rate_hz, clampf(ctx->vbb_cfg.lowpass_hz, 80.0f, 260.0f), 0.707f, 0.0f);
        biquad_design(&ctx->vbb_dc_hp, OMNIA_BIQUAD_HIGH_PASS,
                      ctx->sample_rate_hz, 28.0f, 0.707f, 0.0f);
    }

    /* 7. Studio AETHER */
    ctx->aether_center_gain = db_to_lin(clampf(ctx->aether_cfg.center_anchor_db, -6.0f, 6.0f));
    biquad_design(&ctx->aether_bass_lp,
                  ctx->aether_cfg.bass_keep_mono ? OMNIA_BIQUAD_LOW_PASS : OMNIA_BIQUAD_OFF,
                  ctx->sample_rate_hz, clampf(ctx->aether_cfg.bass_xover_hz, 40.0f, 250.0f), 0.707f, 0.0f);
    biquad_design(&ctx->aether_side_hp,
                  ctx->aether_cfg.side_hpf_en ? OMNIA_BIQUAD_HIGH_PASS : OMNIA_BIQUAD_OFF,
                  ctx->sample_rate_hz, clampf(ctx->aether_cfg.side_hpf_hz, 20.0f, 180.0f), 0.707f, 0.0f);
    biquad_design(&ctx->aether_air_hs,
                  (ctx->aether_cfg.air_gain_db > 0.05f) ? OMNIA_BIQUAD_HIGH_SHELF : OMNIA_BIQUAD_OFF,
                  ctx->sample_rate_hz,
                  clampf(ctx->aether_cfg.air_freq_hz, 5500.0f, 16000.0f),
                  clampf(ctx->aether_cfg.air_q, 0.4f, 1.6f),
                  clampf(ctx->aether_cfg.air_gain_db, 0.0f, 8.0f));

    uint32_t d_samp = (uint32_t)(((float)ctx->sample_rate_hz * clampf(ctx->aether_cfg.stage_size_ms, 0.5f, 12.0f)) * 0.001f);
    if (d_samp < 8U) {
        d_samp = 8U;
    }
    if (d_samp >= OMNIA_AETHER_MAX_DELAY_SAMPLES) {
        d_samp = OMNIA_AETHER_MAX_DELAY_SAMPLES - 1U;
    }
    ctx->aether_delay_len = (uint16_t)d_samp;

    /* 9. Safety Limiter */
    ctx->limiter_ceiling_lin = db_to_lin(clampf(ctx->limiter_cfg.ceiling_dbfs, -6.0f, -0.1f));
    ctx->limiter_rel_coeff   = calc_slew_coeff(ctx->sample_rate_hz, clampf(ctx->limiter_cfg.release_ms, 20.0f, 600.0f));
    uint32_t la_samp = (uint32_t)(((float)ctx->sample_rate_hz * clampf(ctx->limiter_cfg.lookahead_ms, 0.2f, 3.0f)) * 0.001f);
    if (la_samp < 4U) {
        la_samp = 4U;
    }
    if (la_samp >= OMNIA_LIMITER_MAX_LOOKAHEAD) {
        la_samp = OMNIA_LIMITER_MAX_LOOKAHEAD - 1U;
    }
    ctx->limiter_delay_len = (uint16_t)la_samp;

    update_headroom_gain(ctx);
    update_loudness_filters(ctx);
}

/* ========================================================================== *
 *  ПУБЛИЧНЫЙ ИНТЕРФЕЙС НАСТРОЙКИ DSP
 * ========================================================================== */

void omnia_dsp_reset_states(omnia_dsp_ctx_t *ctx)
{
    biquad_reset_z(&ctx->subsonic_biquad);
    for (uint8_t i = 0; i < OMNIA_MAX_ROOM_PEQ_BANDS; ++i) {
        biquad_reset_z(&ctx->room_biquads[i]);
    }
    for (uint8_t i = 0; i < OMNIA_MAX_USER_PEQ_BANDS; ++i) {
        biquad_reset_z(&ctx->user_biquads[i]);
    }
    biquad_reset_z(&ctx->vbb_extract_lp);
    biquad_reset_z(&ctx->vbb_harm_lp);
    biquad_reset_z(&ctx->vbb_dc_hp);
    biquad_reset_z(&ctx->aether_bass_lp);
    biquad_reset_z(&ctx->aether_side_hp);
    biquad_reset_z(&ctx->aether_air_hs);
    biquad_reset_z(&ctx->loudness_low_ls);
    biquad_reset_z(&ctx->loudness_high_hs);

    ctx->agc_env       = 0.0f;
    ctx->deharsh_env   = 0.0f;
    ctx->ap_z1         = 0.0f;
    ctx->ap_z2         = 0.0f;
    ctx->triode_dc_l   = 0.0f;
    ctx->triode_dc_r   = 0.0f;
    ctx->xfeed_lp_l    = 0.0f;
    ctx->xfeed_lp_r    = 0.0f;
    ctx->limiter_env       = 1.0f;
    ctx->limiter_hold_gain = 1.0f;
    ctx->limiter_hold_cnt  = 0U;
    ctx->aether_wr_idx     = 0U;
    ctx->limiter_wr_idx    = 0U;
    ctx->dc_track_l    = 0.0f;
    ctx->dc_track_r    = 0.0f;

    if (ctx->arena) {
        memset(ctx->arena, 0, sizeof(*ctx->arena));
    }
}

void omnia_dsp_init(omnia_dsp_ctx_t *ctx, omnia_dsp_delay_arena_t *arena, uint32_t sample_rate_hz)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->arena              = arena;
    ctx->sample_rate_hz     = (sample_rate_hz >= 8000U) ? sample_rate_hz : 48000U;
    ctx->active_mask        = OMNIA_DSP_ALL_BYPASSED; /* По умолчанию из коробки: Bit-Perfect Direct! */
    ctx->muted              = true;
    ctx->target_vol_db      = 0.0f;
    ctx->current_gain_l     = 0.0f;
    ctx->current_gain_r     = 0.0f;
    ctx->wet_mix_current    = 1.0f;
    ctx->wet_mix_target     = 1.0f;
    ctx->wet_mix_slew       = calc_slew_coeff(ctx->sample_rate_hz, 30.0f);
    ctx->rms_in_env         = 1.0e-4f;
    ctx->rms_out_env        = 1.0e-4f;
    ctx->limiter_env        = 1.0f;
    ctx->min_limiter_gain   = 1.0f;
    ctx->prng_state         = 0xA55A1234U;

    /* Безопасные студийные дефолты каждого звена */
    ctx->hpf_cfg.hpf_enable       = 1U;
    ctx->hpf_cfg.auto_headroom_en = 1U;
    ctx->hpf_cfg.hpf_freq_hz      = 20.0f;
    ctx->hpf_cfg.hpf_q            = 0.7071f;
    ctx->hpf_cfg.manual_atten_db  = 0.0f;

    ctx->anti_ad_cfg.mode          = 0U;
    ctx->anti_ad_cfg.threshold_lin = 0.65f;
    ctx->anti_ad_cfg.attack_coeff  = 0.992f;
    ctx->anti_ad_cfg.release_coeff = 0.0002f;
    ctx->anti_ad_cfg.makeup_db     = 0.0f;

    ctx->vbb_cfg.enable     = 0U;
    ctx->vbb_cfg.mode       = 1U;
    ctx->vbb_cfg.amount     = 0.35f;
    ctx->vbb_cfg.freq_hz    = 65.0f;
    ctx->vbb_cfg.drive      = 0.30f;
    ctx->vbb_cfg.lowpass_hz = 140.0f;
    ctx->vbb_cfg.mix        = 0.50f;

    ctx->aether_cfg.enable           = 0U;
    ctx->aether_cfg.bass_keep_mono   = 1U;
    ctx->aether_cfg.side_hpf_en      = 1U;
    ctx->aether_cfg.crossfeed_en     = 0U;
    ctx->aether_cfg.master_blend     = 0.75f;
    ctx->aether_cfg.width            = 1.25f;
    ctx->aether_cfg.center_anchor_db = 0.5f;
    ctx->aether_cfg.bass_xover_hz    = 110.0f;
    ctx->aether_cfg.side_hpf_hz      = 45.0f;
    ctx->aether_cfg.air_gain_db      = 2.5f;
    ctx->aether_cfg.air_freq_hz      = 10500.0f;
    ctx->aether_cfg.air_q            = 0.75f;
    ctx->aether_cfg.de_harsh         = 0.40f;
    ctx->aether_cfg.depth_amount     = 0.18f;
    ctx->aether_cfg.stage_size_ms    = 4.2f;
    ctx->aether_cfg.triode_warmth    = 0.08f;
    ctx->aether_cfg.crossfeed_level  = 0.30f;
    ctx->aether_cfg.crossfeed_fc_hz  = 700.0f;

    ctx->aether_blend_cur = ctx->aether_cfg.master_blend;
    ctx->aether_width_cur = ctx->aether_cfg.width;

    ctx->loudness_cfg.enable            = 0U;
    ctx->loudness_cfg.ref_volume_db     = -18.0f;
    ctx->loudness_cfg.max_bass_boost_db = 7.5f;
    ctx->loudness_cfg.max_high_boost_db = 3.5f;

    ctx->limiter_cfg.enable        = 1U;
    ctx->limiter_cfg.tpdf_dither_en = 1U;
    ctx->limiter_cfg.ceiling_dbfs  = -0.8f;
    ctx->limiter_cfg.lookahead_ms  = 1.8f;
    ctx->limiter_cfg.release_ms    = 120.0f;

    omnia_dsp_reset_states(ctx);
    recalc_all_coefficients(ctx);
    update_volume_targets(ctx, 25U);
}

void omnia_dsp_set_sample_rate(omnia_dsp_ctx_t *ctx, uint32_t sample_rate_hz)
{
    if (sample_rate_hz >= 8000U && sample_rate_hz != ctx->sample_rate_hz) {
        ctx->sample_rate_hz = sample_rate_hz;
        ctx->wet_mix_slew   = calc_slew_coeff(sample_rate_hz, 30.0f);
        omnia_dsp_reset_states(ctx);
        recalc_all_coefficients(ctx);
        update_volume_targets(ctx, 25U);
    }
}

void omnia_dsp_set_mute(omnia_dsp_ctx_t *ctx, bool mute, uint16_t ramp_ms)
{
    ctx->muted = mute;
    update_volume_targets(ctx, ramp_ms);
}

void omnia_dsp_set_volume(omnia_dsp_ctx_t *ctx, float vol_db, float replaygain_db, float balance, uint16_t ramp_ms)
{
    ctx->target_vol_db = clampf(vol_db, -96.0f, 0.0f);
    ctx->replaygain_db = clampf(replaygain_db, -18.0f, 6.0f);
    ctx->balance       = clampf(balance, -1.0f, 1.0f);
    update_volume_targets(ctx, ramp_ms);
}

void omnia_dsp_set_bypass_mask(omnia_dsp_ctx_t *ctx, uint16_t active_mask, uint16_t ramp_ms)
{
    ctx->active_mask  = active_mask;
    ctx->wet_mix_slew = calc_slew_coeff(ctx->sample_rate_hz, (float)ramp_ms);
    ctx->wet_mix_target = (active_mask != OMNIA_DSP_ALL_BYPASSED && !ctx->ab_compare_bypass) ? 1.0f : 0.0f;
    update_headroom_gain(ctx);
    update_loudness_filters(ctx);
}

void omnia_dsp_set_ab_compare(omnia_dsp_ctx_t *ctx, bool bypass_compare)
{
    ctx->ab_compare_bypass = bypass_compare;
    ctx->wet_mix_slew      = calc_slew_coeff(ctx->sample_rate_hz, 25.0f);
    ctx->wet_mix_target    = (ctx->active_mask != OMNIA_DSP_ALL_BYPASSED && !bypass_compare) ? 1.0f : 0.0f;
}

void omnia_dsp_apply_hpf_headroom(omnia_dsp_ctx_t *ctx, const omnia_hpf_headroom_t *cfg)
{
    ctx->hpf_cfg = *cfg;
    recalc_all_coefficients(ctx);
}

void omnia_dsp_apply_room_peq(omnia_dsp_ctx_t *ctx, const omnia_room_peq_t *cfg)
{
    ctx->room_cfg = *cfg;
    recalc_all_coefficients(ctx);
}

void omnia_dsp_apply_user_peq(omnia_dsp_ctx_t *ctx, const omnia_user_peq_t *cfg)
{
    ctx->user_cfg = *cfg;
    recalc_all_coefficients(ctx);
}

void omnia_dsp_apply_anti_ad(omnia_dsp_ctx_t *ctx, const omnia_anti_ad_t *cfg)
{
    ctx->anti_ad_cfg = *cfg;
    recalc_all_coefficients(ctx);
}

void omnia_dsp_apply_vbb(omnia_dsp_ctx_t *ctx, const omnia_vbb_params_t *cfg)
{
    ctx->vbb_cfg = *cfg;
    recalc_all_coefficients(ctx);
}

void omnia_dsp_apply_aether(omnia_dsp_ctx_t *ctx, const omnia_aether_params_t *cfg)
{
    ctx->aether_cfg = *cfg;
    recalc_all_coefficients(ctx);
}

void omnia_dsp_apply_loudness(omnia_dsp_ctx_t *ctx, const omnia_loudness_params_t *cfg)
{
    ctx->loudness_cfg = *cfg;
    update_loudness_filters(ctx);
}

void omnia_dsp_apply_limiter(omnia_dsp_ctx_t *ctx, const omnia_limiter_params_t *cfg)
{
    ctx->limiter_cfg = *cfg;
    recalc_all_coefficients(ctx);
}

/* ========================================================================== *
 *  ГОРЯЧИЙ ЦИКЛ ОБРАБОТКИ ЗВУКА (Вызывается в прерывании половины/конца DMA)
 * ========================================================================== */

void omnia_dsp_process_block_i32(
    omnia_dsp_ctx_t *ctx,
    const int32_t   *in_lr_interleaved,
    int32_t         *out_lr_interleaved,
    size_t           stereo_frames)
{
    /* Проверка строгого режима BIT-PERFECT DIRECT (0 вмешательств в биты!) */
    const bool gain_unity_settled =
        (!ctx->muted) &&
        (!ctx->dc_fault_tripped) &&
        (fabsf(ctx->target_gain_l - 1.0f) < 1.0e-6f) &&
        (fabsf(ctx->target_gain_r - 1.0f) < 1.0e-6f) &&
        (fabsf(ctx->current_gain_l - 1.0f) < 1.0e-5f) &&
        (fabsf(ctx->current_gain_r - 1.0f) < 1.0e-5f);

    if (ctx->active_mask == OMNIA_DSP_ALL_BYPASSED &&
        !ctx->ab_compare_bypass &&
        gain_unity_settled &&
        ctx->wet_mix_current < 1.0e-4f)
    {
        ctx->bit_perfect_active = true;
        ctx->current_gain_l     = 1.0f;
        ctx->current_gain_r     = 1.0f;

        /* Прямое побитовое копирование + лёгкий мониторинг пиков входа */
        for (size_t i = 0; i < stereo_frames * 2U; i += 2U) {
            const int32_t sl = in_lr_interleaved[i];
            const int32_t sr = in_lr_interleaved[i + 1U];
            out_lr_interleaved[i]      = sl;
            out_lr_interleaved[i + 1U] = sr;

            const float al = fabsf((float)sl * DSP_NORM_TO_FLOAT);
            const float ar = fabsf((float)sr * DSP_NORM_TO_FLOAT);
            if (al > ctx->peak_in_l)  ctx->peak_in_l  = al;
            if (ar > ctx->peak_in_r)  ctx->peak_in_r  = ar;
            if (al > ctx->peak_out_l) ctx->peak_out_l = al;
            if (ar > ctx->peak_out_r) ctx->peak_out_r = ar;
        }
        return;
    }

    ctx->bit_perfect_active = false;
    const uint16_t mask = ctx->active_mask;
    const float param_slew = 0.0025f;

    for (size_t frame = 0; frame < stereo_frames; ++frame) {
        const float dry_l = sanitize_sample((float)in_lr_interleaved[frame * 2U]      * DSP_NORM_TO_FLOAT);
        const float dry_r = sanitize_sample((float)in_lr_interleaved[frame * 2U + 1U] * DSP_NORM_TO_FLOAT);

        const float abs_in_l = fabsf(dry_l);
        const float abs_in_r = fabsf(dry_r);
        if (abs_in_l > ctx->peak_in_l) ctx->peak_in_l = abs_in_l;
        if (abs_in_r > ctx->peak_in_r) ctx->peak_in_r = abs_in_r;

        float l = dry_l;
        float r = dry_r;

        /* --- ЗВЕНО 1: Subsonic HPF --- */
        if ((mask & OMNIA_DSP_BIT_SUBSONIC_HPF) && ctx->hpf_cfg.hpf_enable) {
            biquad_step_stereo(&ctx->subsonic_biquad, &l, &r);
        }

        /* --- ЗВЕНО 2: Dynamic Headroom --- */
        if (mask & OMNIA_DSP_BIT_HEADROOM) {
            l *= ctx->headroom_gain_lin;
            r *= ctx->headroom_gain_lin;
        }

        /* --- ЗВЕНО 3: Room Acoustic PEQ (5 полос) --- */
        if ((mask & OMNIA_DSP_BIT_ROOM_PEQ) && ctx->room_cfg.enable) {
            for (uint8_t b = 0; b < ctx->room_cfg.band_count && b < OMNIA_MAX_ROOM_PEQ_BANDS; ++b) {
                biquad_step_stereo(&ctx->room_biquads[b], &l, &r);
            }
        }

        /* --- ЗВЕНО 4: User Parametric EQ (10 полос) --- */
        if ((mask & OMNIA_DSP_BIT_USER_PEQ) && ctx->user_cfg.enable) {
            for (uint8_t b = 0; b < ctx->user_cfg.band_count && b < OMNIA_MAX_USER_PEQ_BANDS; ++b) {
                biquad_step_stereo(&ctx->user_biquads[b], &l, &r);
            }
        }

        /* --- ЗВЕНО 5: Anti-Ad AGC (сглаживание громкой рекламы на потоках) --- */
        if ((mask & OMNIA_DSP_BIT_ANTI_AD) && ctx->anti_ad_cfg.mode != 0U) {
            const float peak_lr = (fabsf(l) > fabsf(r)) ? fabsf(l) : fabsf(r);
            if (peak_lr > ctx->agc_env) {
                const float att = clampf(1.0f - ctx->anti_ad_cfg.attack_coeff, 0.0005f, 0.2f);
                ctx->agc_env += att * (peak_lr - ctx->agc_env);
            } else {
                const float rel = clampf(ctx->anti_ad_cfg.release_coeff, 0.00005f, 0.02f);
                ctx->agc_env += rel * (peak_lr - ctx->agc_env);
            }
            const float thresh = (ctx->anti_ad_cfg.mode == 2U) ? 0.40f : clampf(ctx->anti_ad_cfg.threshold_lin, 0.25f, 0.90f);
            float agc_gain = ctx->agc_makeup_lin;
            if (ctx->agc_env > thresh && ctx->agc_env > 1.0e-5f) {
                const float ratio_comp = (ctx->anti_ad_cfg.mode == 2U) ? 0.25f : 0.50f;
                agc_gain *= powf(thresh / ctx->agc_env, 1.0f - ratio_comp);
            }
            l *= agc_gain;
            r *= agc_gain;
        }

        /* --- ЗВЕНО 6: VBB / Psychoacoustic Virtual Bass --- */
        if ((mask & OMNIA_DSP_BIT_VBB_BASS) && ctx->vbb_cfg.enable && ctx->vbb_cfg.amount > 0.005f) {
            const float mono_in  = 0.5f * (l + r);
            const float low_band = biquad_step_mono_l(&ctx->vbb_extract_lp, mono_in);
            float vbb_sig = 0.0f;

            if (ctx->vbb_cfg.mode == 0U) {
                /* Режим 0: плотный упругий подъём суб-нч */
                vbb_sig = low_band * (ctx->vbb_cfg.amount * 1.4f);
            } else {
                /* Режимы 1 и 2: генерация чётных и нечётных гармоник («Missing Fundamental») */
                const float drv = 1.0f + clampf(ctx->vbb_cfg.drive, 0.0f, 1.0f) * 5.0f;
                const float x   = low_band * drv;
                const float sat = x / (1.0f + fabsf(x));
                const float even_h = 0.35f * (x * fabsf(x)) / (1.0f + x * x);
                float harm = biquad_step_mono_l(&ctx->vbb_harm_lp, sat + even_h);
                harm = biquad_step_mono_l(&ctx->vbb_dc_hp, harm);
                const float keep_fund = (ctx->vbb_cfg.mode == 2U) ? 0.25f : 0.80f;
                vbb_sig = (low_band * keep_fund + harm) * ctx->vbb_cfg.amount * clampf(ctx->vbb_cfg.mix, 0.0f, 1.0f) * 1.5f;
            }
            l += vbb_sig;
            r += vbb_sig;
        }

        /* --- ЗВЕНО 7: Студийный M/S процессор AETHER («Эфир») --- */
        if ((mask & OMNIA_DSP_BIT_AETHER) && ctx->aether_cfg.enable) {
            ctx->aether_blend_cur += param_slew * (clampf(ctx->aether_cfg.master_blend, 0.0f, 1.0f) - ctx->aether_blend_cur);
            ctx->aether_width_cur += param_slew * (clampf(ctx->aether_cfg.width, 0.0f, 2.0f) - ctx->aether_width_cur);

            float mid  = 0.5f * (l + r) * ctx->aether_center_gain;
            float side = 0.5f * (l - r);

            /* 7a. Sub-Bass Mono Lock: удерживает бочку и бас собранными по центру */
            if (ctx->aether_cfg.bass_keep_mono) {
                const float side_bass = biquad_step_mono_l(&ctx->aether_bass_lp, side);
                side -= side_bass;
            }
            /* 7b. Side HPF */
            if (ctx->aether_cfg.side_hpf_en) {
                side = biquad_step_mono_l(&ctx->aether_side_hp, side);
            }

            /* 7c. Silk Air + динамический подавитель сибилянтов De-Harsh */
            if (!ctx->aether_air_hs.bypass) {
                float air_m = mid;
                float air_s = side;
                biquad_step_stereo(&ctx->aether_air_hs, &air_m, &air_s);
                const float hf_delta_s = air_s - side;
                const float hf_delta_m = (air_m - mid) * 0.35f;

                const float hf_energy = fabsf(hf_delta_s) + fabsf(hf_delta_m);
                ctx->deharsh_env += (hf_energy > ctx->deharsh_env ? 0.08f : 0.002f) * (hf_energy - ctx->deharsh_env);
                float smooth_factor = 1.0f;
                if (ctx->aether_cfg.de_harsh > 0.01f && ctx->deharsh_env > 0.06f) {
                    smooth_factor = 1.0f / (1.0f + ctx->aether_cfg.de_harsh * (ctx->deharsh_env - 0.06f) * 14.0f);
                }
                mid  += hf_delta_m * smooth_factor;
                side += hf_delta_s * smooth_factor;
            }

            /* 7d. 3D Stage Depth (фазовый декоррелятор All-Pass + линия задержки в .sram_ext) */
            float depth_sig = 0.0f;
            if (ctx->arena && ctx->aether_cfg.depth_amount > 0.005f) {
                const uint16_t rd_idx = (uint16_t)((ctx->aether_wr_idx + OMNIA_AETHER_MAX_DELAY_SAMPLES - ctx->aether_delay_len) % OMNIA_AETHER_MAX_DELAY_SAMPLES);
                const float delayed = ctx->arena->aether_delay_line[rd_idx];
                ctx->arena->aether_delay_line[ctx->aether_wr_idx] = mid * 0.35f + side * 0.65f;
                ctx->aether_wr_idx = (uint16_t)((ctx->aether_wr_idx + 1U) % OMNIA_AETHER_MAX_DELAY_SAMPLES);

                /* Двухзвенный фазовый All-Pass (g = 0.618 — золотое сечение без гребёнки) */
                const float g  = 0.618f;
                const float u1 = delayed - g * ctx->ap_z1;
                const float y1 = ctx->ap_z1 + g * u1;
                ctx->ap_z1 = u1 + DSP_ANTI_DENORMAL;

                const float u2 = y1 - (-g) * ctx->ap_z2;
                const float y2 = ctx->ap_z2 + (-g) * u2;
                ctx->ap_z2 = u2 - DSP_ANTI_DENORMAL;

                depth_sig = y2 * clampf(ctx->aether_cfg.depth_amount, 0.0f, 0.45f);
            }

            /* 7e. Расширение стереобазы и обратная матрица M/S -> L/R */
            side = side * ctx->aether_width_cur + depth_sig;
            float wet_l = mid + side;
            float wet_r = mid - side;

            /* 7f. Triode Warmth (мягкая ламповая 2-я гармоника с отсечкой постоянки) */
            if (ctx->aether_cfg.triode_warmth > 0.005f) {
                const float k = clampf(ctx->aether_cfg.triode_warmth, 0.0f, 0.30f);
                const float tl = wet_l + k * (wet_l * wet_l * 0.65f - 0.15f * wet_l * wet_l * wet_l);
                const float tr = wet_r + k * (wet_r * wet_r * 0.65f - 0.15f * wet_r * wet_r * wet_r);
                ctx->triode_dc_l += 0.0005f * (tl - ctx->triode_dc_l);
                ctx->triode_dc_r += 0.0005f * (tr - ctx->triode_dc_r);
                wet_l = tl - ctx->triode_dc_l;
                wet_r = tr - ctx->triode_dc_r;
            }

            /* 7g. Наушниковый режим Bauer Crossfeed */
            if (ctx->aether_cfg.crossfeed_en && ctx->aether_cfg.crossfeed_level > 0.01f) {
                const float fc = clampf(ctx->aether_cfg.crossfeed_fc_hz, 500.0f, 900.0f);
                const float a  = clampf((2.0f * (float)M_PI * fc) / (float)ctx->sample_rate_hz, 0.01f, 0.35f);
                ctx->xfeed_lp_l += a * (wet_l - ctx->xfeed_lp_l);
                ctx->xfeed_lp_r += a * (wet_r - ctx->xfeed_lp_r);
                const float xf = clampf(ctx->aether_cfg.crossfeed_level, 0.0f, 1.0f) * 0.32f;
                const float nl = (wet_l + xf * ctx->xfeed_lp_r) * (1.0f - xf * 0.35f);
                const float nr = (wet_r + xf * ctx->xfeed_lp_l) * (1.0f - xf * 0.35f);
                wet_l = nl;
                wet_r = nr;
            }

            l = l + ctx->aether_blend_cur * (wet_l - l);
            r = r + ctx->aether_blend_cur * (wet_r - r);
        }

        /* --- ЗВЕНО 8a: Dynamic Loudness (ISO 226) --- */
        if ((mask & OMNIA_DSP_BIT_LOUDNESS) && ctx->loudness_cfg.enable) {
            biquad_step_stereo(&ctx->loudness_low_ls,  &l, &r);
            biquad_step_stereo(&ctx->loudness_high_hs, &l, &r);
        }

        /* --- Плавный обход и честное A/B сравнение с выравниванием громкости по RMS --- */
        ctx->wet_mix_target = (mask != OMNIA_DSP_ALL_BYPASSED && !ctx->ab_compare_bypass) ? 1.0f : 0.0f;
        ctx->wet_mix_current += ctx->wet_mix_slew * (ctx->wet_mix_target - ctx->wet_mix_current);

        const float p_in  = 0.5f * (dry_l * dry_l + dry_r * dry_r);
        const float p_out = 0.5f * (l * l + r * r);
        ctx->rms_in_env  += 0.0002f * (p_in  - ctx->rms_in_env);
        ctx->rms_out_env += 0.0002f * (p_out - ctx->rms_out_env);

        if (ctx->wet_mix_current < 0.9995f) {
            float dry_match_gain = 1.0f;
            if (ctx->ab_compare_bypass && ctx->rms_in_env > 1.0e-6f && ctx->rms_out_env > 1.0e-6f) {
                dry_match_gain = clampf(sqrtf(ctx->rms_out_env / ctx->rms_in_env), 0.35f, 2.0f);
            }
            const float ref_l = dry_l * dry_match_gain;
            const float ref_r = dry_r * dry_match_gain;
            l = ref_l + ctx->wet_mix_current * (l - ref_l);
            r = ref_r + ctx->wet_mix_current * (r - ref_r);
        }

        /* --- ЗВЕНО 8b: По-сэмпловый Smoother громкости и Mute --- */
        ctx->current_gain_l += ctx->gain_slewing_coeff * (ctx->target_gain_l - ctx->current_gain_l);
        ctx->current_gain_r += ctx->gain_slewing_coeff * (ctx->target_gain_r - ctx->current_gain_r);
        if (fabsf(ctx->target_gain_l - ctx->current_gain_l) < 1.0e-5f) {
            ctx->current_gain_l = ctx->target_gain_l;
        }
        if (fabsf(ctx->target_gain_r - ctx->current_gain_r) < 1.0e-5f) {
            ctx->current_gain_r = ctx->target_gain_r;
        }
        l *= ctx->current_gain_l;
        r *= ctx->current_gain_r;

        /* --- ЗВЕНО 9: Look-Ahead Safety Limiter + Защита акустики от постоянки (DC Guard) --- */
        if ((mask & OMNIA_DSP_BIT_LIMITER) || ctx->limiter_cfg.enable) {
            const float ahead_peak = (fabsf(l) > fabsf(r)) ? fabsf(l) : fabsf(r);
            float needed_gain = 1.0f;
            if (ahead_peak > ctx->limiter_ceiling_lin && ahead_peak > 1.0e-6f) {
                needed_gain = ctx->limiter_ceiling_lin / ahead_peak;
                ctx->clip_events++;
            }

            /* Удержание пикового минимума на всё время прохождения пика по линии предпросмотра */
            if (needed_gain <= ctx->limiter_hold_gain) {
                ctx->limiter_hold_gain = needed_gain;
                ctx->limiter_hold_cnt  = ctx->limiter_delay_len;
            } else if (ctx->limiter_hold_cnt > 0U) {
                ctx->limiter_hold_cnt--;
            } else {
                ctx->limiter_hold_gain = 1.0f;
            }

            if (ctx->limiter_hold_gain < ctx->limiter_env) {
                const float att_step = 4.0f / (float)(ctx->limiter_delay_len > 0U ? ctx->limiter_delay_len : 1U);
                ctx->limiter_env += clampf(att_step, 0.03f, 1.0f) * (ctx->limiter_hold_gain - ctx->limiter_env);
            } else {
                ctx->limiter_env += ctx->limiter_rel_coeff * (1.0f - ctx->limiter_env);
            }

            if (ctx->arena && ctx->limiter_delay_len > 0U) {
                const uint16_t rd = (uint16_t)((ctx->limiter_wr_idx + OMNIA_LIMITER_MAX_LOOKAHEAD - ctx->limiter_delay_len) % OMNIA_LIMITER_MAX_LOOKAHEAD);
                const float dl = ctx->arena->limiter_buf_l[rd];
                const float dr = ctx->arena->limiter_buf_r[rd];
                ctx->arena->limiter_buf_l[ctx->limiter_wr_idx] = l;
                ctx->arena->limiter_buf_r[ctx->limiter_wr_idx] = r;
                ctx->limiter_wr_idx = (uint16_t)((ctx->limiter_wr_idx + 1U) % OMNIA_LIMITER_MAX_LOOKAHEAD);
                l = dl;
                r = dr;
            }

            l = clampf(l * ctx->limiter_env, -ctx->limiter_ceiling_lin, ctx->limiter_ceiling_lin);
            r = clampf(r * ctx->limiter_env, -ctx->limiter_ceiling_lin, ctx->limiter_ceiling_lin);
            if (ctx->limiter_env < ctx->min_limiter_gain) {
                ctx->min_limiter_gain = ctx->limiter_env;
            }
        }

        /* Жёсткий предохранитель потолка и аварийный детектор постоянки (DC Offset > 0.35 FS) */
        l = clampf(sanitize_sample(l), -0.999999f, 0.999999f);
        r = clampf(sanitize_sample(r), -0.999999f, 0.999999f);

        ctx->dc_track_l += 0.00005f * (l - ctx->dc_track_l);
        ctx->dc_track_r += 0.00005f * (r - ctx->dc_track_r);
        if (fabsf(ctx->dc_track_l) > 0.35f || fabsf(ctx->dc_track_r) > 0.35f) {
            ctx->dc_fault_tripped = true;
            ctx->target_gain_l    = 0.0f;
            ctx->target_gain_r    = 0.0f;
            l = 0.0f;
            r = 0.0f;
        }

        const float abs_out_l = fabsf(l);
        const float abs_out_r = fabsf(r);
        if (abs_out_l > ctx->peak_out_l) ctx->peak_out_l = abs_out_l;
        if (abs_out_r > ctx->peak_out_r) ctx->peak_out_r = abs_out_r;

        /* Мягкий треугольный 24-bit TPDF Dither при активной обработке */
        if (ctx->limiter_cfg.tpdf_dither_en && (abs_out_l > 1.0e-7f || abs_out_r > 1.0e-7f)) {
            const float r1 = (float)(xorshift32(&ctx->prng_state) & 0xFFFFU) * (1.0f / 65536.0f);
            const float r2 = (float)(xorshift32(&ctx->prng_state) & 0xFFFFU) * (1.0f / 65536.0f);
            const float dither = (r1 - r2) * (1.0f / 8388608.0f); /* 1 LSB на уровне 24 бит */
            l = clampf(l + dither, -0.999999f, 0.999999f);
            r = clampf(r - dither, -0.999999f, 0.999999f);
        }

        out_lr_interleaved[frame * 2U]      = (int32_t)(l * DSP_SCALE_TO_INT32);
        out_lr_interleaved[frame * 2U + 1U] = (int32_t)(r * DSP_SCALE_TO_INT32);
    }
}

void omnia_dsp_read_and_reset_meters(omnia_dsp_ctx_t *ctx, omnia_signal_meters_t *out_meters)
{
    if (!out_meters) {
        return;
    }
    out_meters->in_peak_l_dbfs  = lin_to_db(ctx->peak_in_l);
    out_meters->in_peak_r_dbfs  = lin_to_db(ctx->peak_in_r);
    out_meters->out_peak_l_dbfs = lin_to_db(ctx->peak_out_l);
    out_meters->out_peak_r_dbfs = lin_to_db(ctx->peak_out_r);
    out_meters->limiter_gr_db   = lin_to_db(ctx->min_limiter_gain);

    ctx->peak_in_l        = 0.0f;
    ctx->peak_in_r        = 0.0f;
    ctx->peak_out_l       = 0.0f;
    ctx->peak_out_r       = 0.0f;
    ctx->min_limiter_gain = 1.0f;
}
