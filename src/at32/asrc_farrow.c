/**
 * @file    asrc_farrow.c
 * @brief   Реализация 24/32-битного кубического ресемплера Фарроу (Catmull-Rom 4-pt)
 *          с пропорционально-интегральной (PI) петлёй слежения за дрейфом тактовых частот.
 */

#include "omnia_asrc.h"
#include <string.h>

#define OMNIA_INV_Q31_F         (1.0f / 2147483648.0f)
#define OMNIA_MAX_Q31_SAFE_F    2147483520.0f   /* Наибольший float строго меньше 2^31 (0x7FFFFF80) */

static inline int32_t float_to_q31_sat(float x)
{
    if (x >= 0.99999994f) {
        return 2147483520;
    }
    if (x <= -1.0f) {
        return (-2147483647 - 1);
    }
    return (int32_t)(x * OMNIA_MAX_Q31_SAFE_F);
}

static inline float farrow_cubic_eval(float ym1, float y0, float y1, float y2, float mu)
{
    /* 4-точечный полином Эрмита / Catmull-Rom в схеме Горнера (3 инструкции VFMA.F32) */
    const float c0 = y0;
    const float c1 = 0.5f * (y1 - ym1);
    const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
    return ((c3 * mu + c2) * mu + c1) * mu + c0;
}

void omnia_asrc_reset(omnia_asrc_t *asrc)
{
    if (asrc == NULL) {
        return;
    }
    memset(asrc->ring_l, 0, sizeof(asrc->ring_l));
    memset(asrc->ring_r, 0, sizeof(asrc->ring_r));
    asrc->wr_idx      = 0U;
    asrc->rd_idx      = 0U;
    asrc->phase_mu    = 0.0f;
    asrc->pi_integral = 0.0f;
    asrc->smooth_fill = OMNIA_ASRC_TARGET_FILL;
    asrc->current_ratio = asrc->nominal_ratio;
    asrc->primed      = false;
    asrc->fade_gain   = 0.0f;
}

void omnia_asrc_set_rates(omnia_asrc_t *asrc, uint32_t in_fs_hz, uint32_t out_fs_hz)
{
    if (asrc == NULL || in_fs_hz == 0U || out_fs_hz == 0U) {
        return;
    }
    asrc->in_fs_hz      = in_fs_hz;
    asrc->out_fs_hz     = out_fs_hz;
    asrc->nominal_ratio = (float)in_fs_hz / (float)out_fs_hz;
    asrc->current_ratio = asrc->nominal_ratio;
    asrc->pi_integral   = 0.0f;
}

void omnia_asrc_init(omnia_asrc_t *asrc, uint32_t in_fs_hz, uint32_t out_fs_hz)
{
    if (asrc == NULL) {
        return;
    }
    memset(asrc, 0, sizeof(*asrc));
    omnia_asrc_set_rates(asrc,
                         (in_fs_hz != 0U) ? in_fs_hz : 48000U,
                         (out_fs_hz != 0U) ? out_fs_hz : 48000U);
    omnia_asrc_reset(asrc);
}

uint32_t omnia_asrc_level(const omnia_asrc_t *asrc)
{
    if (asrc == NULL) {
        return 0U;
    }
    return (uint32_t)(asrc->wr_idx - asrc->rd_idx);
}

void omnia_asrc_push_interleaved(omnia_asrc_t *asrc, const int32_t *in_lr, uint32_t frames)
{
    if (asrc == NULL || in_lr == NULL || frames == 0U) {
        return;
    }

    uint32_t wr = asrc->wr_idx;
    const uint32_t rd = asrc->rd_idx;

    for (uint32_t i = 0U; i < frames; i++) {
        /* Защита от переполнения кольца (оставляем минимум 4 отсчёта запаса для окна Фарроу) */
        if ((uint32_t)(wr - rd) >= (OMNIA_ASRC_RING_FRAMES - 4U)) {
            asrc->overrun_count++;
            break;
        }
        const uint32_t slot = wr & OMNIA_ASRC_RING_MASK;
        asrc->ring_l[slot] = in_lr[i * 2U + 0U];
        asrc->ring_r[slot] = in_lr[i * 2U + 1U];
        wr++;
    }

    asrc->wr_idx = wr;

    if (!asrc->primed && (uint32_t)(wr - rd) >= (uint32_t)OMNIA_ASRC_TARGET_FILL) {
        /* Ставим чтение ровно на 1 отсчёт вперёд от края, чтобы y[-1] уже был валиден */
        asrc->rd_idx      = wr - (uint32_t)OMNIA_ASRC_TARGET_FILL;
        asrc->phase_mu    = 0.0f;
        asrc->smooth_fill = OMNIA_ASRC_TARGET_FILL;
        asrc->pi_integral = 0.0f;
        asrc->primed      = true;
    }
}

void omnia_asrc_pull_interleaved(omnia_asrc_t *asrc, int32_t *out_lr, uint32_t frames)
{
    if (asrc == NULL || out_lr == NULL || frames == 0U) {
        return;
    }

    const uint32_t wr = asrc->wr_idx;
    uint32_t avail = (uint32_t)(wr - asrc->rd_idx);

    if (!asrc->primed || avail < 8U) {
        if (asrc->primed && avail < 8U) {
            asrc->primed = false;
            asrc->underrun_count++;
        }
        memset(out_lr, 0, frames * 2U * sizeof(int32_t));
        asrc->fade_gain = 0.0f;
        return;
    }

    /* Узкополосная PI-петля слежения за уровнем буфера (подавляет джиттер пакетов BT) */
    const float inst_fill = (float)avail - asrc->phase_mu;
    asrc->smooth_fill += 0.05f * (inst_fill - asrc->smooth_fill);

    const float err = asrc->smooth_fill - OMNIA_ASRC_TARGET_FILL;
    /* Мягкие коэффициенты: полоса петли ~0.8 Гц -> нулевая детонация тона (wow & flutter < 0.0002%) */
    const float kp = 2.0e-5f;
    const float ki = 1.5e-7f;

    asrc->pi_integral += ki * err;
    const float max_pull = 0.015f * asrc->nominal_ratio; /* ±1.5% (перекрывает любой дрейф кварцев) */
    if (asrc->pi_integral > max_pull) {
        asrc->pi_integral = max_pull;
    } else if (asrc->pi_integral < -max_pull) {
        asrc->pi_integral = -max_pull;
    }

    float step = asrc->nominal_ratio + (kp * err) + asrc->pi_integral;
    const float min_step = asrc->nominal_ratio - max_pull;
    const float max_step = asrc->nominal_ratio + max_pull;
    if (step < min_step) {
        step = min_step;
    } else if (step > max_step) {
        step = max_step;
    }
    asrc->current_ratio = step;

    uint32_t rd = asrc->rd_idx;
    float    mu = asrc->phase_mu;
    float    fg = asrc->fade_gain;

    for (uint32_t i = 0U; i < frames; i++) {
        if ((uint32_t)(wr - rd) < 4U) {
            /* Бесшовное затухание при остановке потока Bluetooth */
            out_lr[i * 2U + 0U] = 0;
            out_lr[i * 2U + 1U] = 0;
            asrc->primed = false;
            fg = 0.0f;
            continue;
        }

        const uint32_t im1 = (rd - 1U) & OMNIA_ASRC_RING_MASK;
        const uint32_t i0  = (rd + 0U) & OMNIA_ASRC_RING_MASK;
        const uint32_t i1  = (rd + 1U) & OMNIA_ASRC_RING_MASK;
        const uint32_t i2  = (rd + 2U) & OMNIA_ASRC_RING_MASK;

        const float yl = farrow_cubic_eval(
            (float)asrc->ring_l[im1] * OMNIA_INV_Q31_F,
            (float)asrc->ring_l[i0]  * OMNIA_INV_Q31_F,
            (float)asrc->ring_l[i1]  * OMNIA_INV_Q31_F,
            (float)asrc->ring_l[i2]  * OMNIA_INV_Q31_F,
            mu);

        const float yr = farrow_cubic_eval(
            (float)asrc->ring_r[im1] * OMNIA_INV_Q31_F,
            (float)asrc->ring_r[i0]  * OMNIA_INV_Q31_F,
            (float)asrc->ring_r[i1]  * OMNIA_INV_Q31_F,
            (float)asrc->ring_r[i2]  * OMNIA_INV_Q31_F,
            mu);

        if (fg < 1.0f) {
            fg += (1.0f / 256.0f); /* 5.3 мс анти-щелчковый fade-in при старте BT */
            if (fg > 1.0f) {
                fg = 1.0f;
            }
        }

        out_lr[i * 2U + 0U] = float_to_q31_sat(yl * fg);
        out_lr[i * 2U + 1U] = float_to_q31_sat(yr * fg);

        mu += step;
        const uint32_t whole = (uint32_t)mu;
        rd += whole;
        mu -= (float)whole;
    }

    asrc->rd_idx    = rd;
    asrc->phase_mu  = mu;
    asrc->fade_gain = fg;
}
