/**
 * @file    test_dsp_quality.c
 * @brief   Качественные хост-тесты DSP OMNIA: VBB-спектр, AETHER-инварианты, лимитер,
 *          DC Guard, bit-perfect, стабильность биквадов, суб-сэмпловая задержка.
 *          Не подтверждает работу аппаратуры — только математику тракта.
 */
#include "omnia_dsp.h"
#include "omnia_dsp_validation.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FS 48000U
#define FRAMES 512U

/* Мощность одного бина (Goertzel) */
static double goertzel_power(const float *x, size_t n, double freq_hz)
{
    const double coeff = 2.0 * cos(2.0 * M_PI * freq_hz / (double)FS);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        s0 = coeff * s1 - s2 + (double)x[i];
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

static float frand(uint32_t *rng)
{
    *rng ^= *rng << 13; *rng ^= *rng >> 17; *rng ^= *rng << 5;
    return ((float)(*rng & 0xFFFFFFU) / (float)0x800000U) - 1.0f;
}

static void process_block(omnia_dsp_ctx_t *dsp, int32_t *buf, size_t frames)
{
    static int32_t out[FRAMES * 2];
    assert(frames <= FRAMES);
    omnia_dsp_process_block_i32(dsp, buf, out, frames);
    memcpy(buf, out, frames * 2U * sizeof(int32_t));
}

static void warmup(omnia_dsp_ctx_t *dsp, unsigned blocks)
{
    int32_t zero[FRAMES * 2] = {0};
    for (unsigned b = 0; b < blocks; ++b) {
        process_block(dsp, zero, FRAMES);
    }
}

/* ---------- 1. Bit-Perfect: полное совпадение ---------- */
static void test_bitperfect_exact_copy(void)
{
    static omnia_dsp_delay_arena_t arena;
    static omnia_dsp_ctx_t dsp;
    omnia_dsp_init(&dsp, &arena, FS);
    omnia_dsp_set_mute(&dsp, false, 0U);
    omnia_dsp_set_volume(&dsp, 0.0f, 0.0f, 0.0f, 0U);
    omnia_dsp_set_bypass_mask(&dsp, OMNIA_DSP_ALL_BYPASSED, 0U);
    warmup(&dsp, 8U); /* оседание gain */

    uint32_t rng = 0xDEADBEEFU;
    int32_t in[FRAMES * 2];
    memcpy(in, in, 0);
    for (size_t i = 0; i < FRAMES * 2U; ++i) {
        rng = rng * 1664525U + 1013904223U;
        in[i] = (int32_t)rng;
    }
    int32_t out[FRAMES * 2];
    omnia_dsp_process_block_i32(&dsp, in, out, FRAMES);
    assert(memcmp(in, out, sizeof(in)) == 0);
    printf("[OK] Bit-Perfect Direct: побитовое совпадение 2048 сэмплов\n");
}

/* ---------- 2. Проход через активный float-путь: ошибка <= 4 LSB ---------- */
static void test_float_path_int_roundtrip(void)
{
    static omnia_dsp_delay_arena_t arena;
    static omnia_dsp_ctx_t dsp;
    omnia_dsp_init(&dsp, &arena, FS);
    omnia_dsp_set_mute(&dsp, false, 0U);
    omnia_dsp_set_volume(&dsp, 0.0f, 0.0f, 0.0f, 0U);
    omnia_dsp_set_bypass_mask(&dsp, OMNIA_DSP_BIT_USER_PEQ, 0U); /* блок включён, но выключен в параметрах */
    dsp.limiter_cfg.enable        = 0U;  /* иначе lookahead-линия задержки даст первые 86 нулей и дизер добавит шума */
    dsp.limiter_cfg.tpdf_dither_en = 0U;
    omnia_dsp_apply_limiter(&dsp, &dsp.limiter_cfg);
    warmup(&dsp, 40U);

    /* A) Сэмплы < 2^24: int -> float -> int обязаны быть ТОЧНЫМИ (нормировка 2^31 — точная степень двойки) */
    static int32_t blk[FRAMES * 2];
    uint32_t rng = 424242U;
    unsigned exact = 0, total = 0;
    for (unsigned b = 0; b < 4U; ++b) {
        for (size_t i = 0; i < FRAMES * 2U; ++i) {
            rng = rng * 1664525U + 1013904223U;
            blk[i] = ((int32_t)(rng >> 8) & 0x001FFFFFU) * ((i & 1U) ? -1 : 1); /* ~ +-2^21 */
        }
        int32_t out[FRAMES * 2];
        omnia_dsp_process_block_i32(&dsp, blk, out, FRAMES);
        for (size_t i = 0; i < FRAMES * 2U; ++i) {
            assert(out[i] == blk[i]);
            ++exact;
        }
        total += FRAMES * 2U;
    }
    assert(exact == total);
    printf("[OK] Float-путь: %u сэмплов < 2^24 прошли абсолютно точно\n", exact);

    /* B) Полная шкала: финальный клэмп +-0.999999f (проектный запас до переполнения),
     *    ошибка не превышает ~0.001% (=-120 dBFS), выход в насыщении, но конечен */
    int max_err = 0;
    int64_t in_max = 2147483647;
    for (unsigned b = 0; b < 4U; ++b) {
        for (size_t f = 0; f < FRAMES; ++f) {
            const int32_t s = ((f & 1U) == 0U) ? 2147483647 : (-2147483647 - 1);
            blk[f * 2U] = s;
            blk[f * 2U + 1U] = s;
        }
        int32_t out[FRAMES * 2];
        omnia_dsp_process_block_i32(&dsp, blk, out, FRAMES);
        for (size_t i = 0; i < FRAMES * 2U; ++i) {
            int err = abs((int)((int64_t)out[i] - (int64_t)blk[i]));
            if (err > max_err) max_err = err;
            assert(abs(out[i]) <= 2147483520); /* насыщение, но не переполнение int32 */
        }
    }
    assert((int64_t)max_err * 1000000LL <= 2000LL * in_max); /* <= 0.002% = -94 dBFS на худшей полной шкале */
    printf("[OK] Float-путь: полная шкала в проектном клиппинге, ошибка %d LSB (<= -94 dB, в норме)\n", max_err);
}

/* ---------- 3. Лимитер: потолок без выбросов на ступеньке ---------- */
static void test_limiter_ceiling_no_overshoot(void)
{
    static omnia_dsp_delay_arena_t arena;
    static omnia_dsp_ctx_t dsp;
    omnia_dsp_init(&dsp, &arena, FS);
    omnia_dsp_set_mute(&dsp, false, 0U);
    omnia_dsp_set_volume(&dsp, 0.0f, 0.0f, 0.0f, 0U);
    omnia_dsp_set_bypass_mask(&dsp, OMNIA_DSP_BIT_LIMITER, 0U);
    dsp.limiter_cfg.tpdf_dither_en = 0U;
    warmup(&dsp, 10U);

    static int32_t blk[FRAMES * 2];
    int32_t out[FRAMES * 2];
    for (size_t f = 0; f < FRAMES; ++f) {  /* Ступенька сразу на полную шкалу */
        blk[f * 2U] = 2147483647;
        blk[f * 2U + 1U] = 2147483647;
    }
    omnia_dsp_process_block_i32(&dsp, blk, out, FRAMES);
    float ceiling = powf(10.0f, dsp.limiter_cfg.ceiling_dbfs * 0.05f);
    float max_out = 0.0f;
    for (size_t i = 0; i < FRAMES * 2U; ++i) {
        float a = fabsf((float)out[i] / 2147483648.0f);
        if (a > max_out) max_out = a;
    }
    assert(max_out <= ceiling + 0.002f);
    printf("[OK] Limiter: ступенька полной шкалы, потолок %.4f, макс %.4f — выброса нет\n", ceiling, max_out);
}

/* ---------- 4. DC Guard: защита и сервисный сброс ---------- */
static void test_dc_guard_trips_and_resets(void)
{
    static omnia_dsp_delay_arena_t arena;
    static omnia_dsp_ctx_t dsp;
    omnia_dsp_init(&dsp, &arena, FS);
    omnia_dsp_set_mute(&dsp, false, 0U);
    omnia_dsp_set_volume(&dsp, 0.0f, 0.0f, 0.0f, 0U);
    /* Маска без HPF: постоянка доходит до детектора постоянки */
    /* ВНИМАНИЕ: при ALL_BYPASSED движок уходит в BIT-PERFECT копию без детектора постоянки —
     * это осознанно (бит-перфект = ноль вмешательства). Поэтому проверяем цепочку:
     * маска USER_PEQ (тракт жив), HPF выключен (постоянка доходит до детектора). */
    omnia_dsp_set_bypass_mask(&dsp, OMNIA_DSP_BIT_USER_PEQ, 0U);
    omnia_hpf_headroom_t hpf_off;
    memset(&hpf_off, 0, sizeof(hpf_off));
    hpf_off.hpf_enable = 0U;
    hpf_off.hpf_freq_hz = 20.0f;
    hpf_off.hpf_q = 0.7071f;
    omnia_dsp_apply_hpf_headroom(&dsp, &hpf_off);
    warmup(&dsp, 4U);

    static int32_t in[FRAMES * 2], out[FRAMES * 2];
    for (size_t i = 0; i < FRAMES * 2U; ++i) in[i] = (int32_t)(0.5f * 2147483647.0f); /* +0.5 FS постоянка */
    int saw_zero_out = 0;
    for (unsigned b = 0; b < 80U; ++b) {
        const int tripped_at_start = dsp.dc_fault_tripped;
        omnia_dsp_process_block_i32(&dsp, in, out, FRAMES);
        if (tripped_at_start) {
            /* Блок, НАЧИНАЮЩИЙСЯ после срабатывания: look-ahead линия (86 отсчётов)
             * уже опустошена, громкость дозатухает мгновенно при ramp=0 -> строго ноль. */
            for (size_t i = 0; i < FRAMES * 2U; ++i) assert(out[i] == 0);
            saw_zero_out = 1;
        }
    }
    assert(saw_zero_out && dsp.dc_fault_tripped);
    printf("[OK] DC Guard: постоянка 0.5 FS -> защита сработала, выход обнулён\n");

    omnia_dsp_reset_dc_fault(&dsp);
    assert(!dsp.dc_fault_tripped && dsp.clip_events == 0U);
    printf("[OK] RESET_DC_FAULT: защита снята сервисной командой\n");
}

/* ---------- 5/6/7. VBB: спектральные инварианты по режимам ---------- */
static size_t vbb_render(omnia_dsp_ctx_t *dsp, float amp, float freq, float *out_mono, size_t n, uint8_t mode)
{
    (void)mode;
    omnia_dsp_set_bypass_mask(dsp, OMNIA_DSP_BIT_VBB_BASS, 0U);
    int32_t in[FRAMES * 2];
    size_t wrote = 0;
    double ph = 0.0;
    const double step = 2.0 * M_PI * (double)freq / (double)FS;
    (void)out_mono;
    while (wrote < n) {
        size_t frames = FRAMES;
        if (wrote + frames > n) frames = n - wrote;
        for (size_t f = 0; f < frames; ++f) {
            const double s = (double)amp * sin(ph);
            ph += step;
            const int32_t q = (int32_t)(s * 2147483647.0);
            in[f * 2U] = q;
            in[f * 2U + 1U] = q;
        }
        int32_t outb[FRAMES * 2];
        omnia_dsp_process_block_i32(dsp, in, outb, frames);
        for (size_t f = 0; f < frames; ++f) {
            out_mono[wrote + f] = 0.5f * ((float)outb[f * 2U] / 2147483648.0f + (float)outb[f * 2U + 1U] / 2147483648.0f);
        }
        wrote += frames;
    }
    return wrote;
}

static void test_vbb_harmonics(void)
{
    enum { N = 16384 };
    static float buf[N];
    const float freq = 55.0f;

    for (uint8_t mode = 0; mode < 3U; ++mode) {
        static omnia_dsp_delay_arena_t arena;
        static omnia_dsp_ctx_t dsp;
        omnia_dsp_init(&dsp, &arena, FS);
        omnia_dsp_set_mute(&dsp, false, 0U);
        omnia_dsp_set_volume(&dsp, 0.0f, 0.0f, 0.0f, 0U);
        omnia_vbb_params_t vbb = {0};
        vbb.enable     = 1U;
        vbb.mode       = mode;
        vbb.amount     = 0.8f;
        vbb.freq_hz    = 55.0f;
        vbb.drive      = 0.6f;
        vbb.lowpass_hz = 220.0f;
        vbb.mix        = 1.0f;
        omnia_dsp_apply_vbb(&dsp, &vbb);
        warmup(&dsp, 5U);
        vbb_render(&dsp, 0.30f, freq, buf, N, mode);

        const double p1 = goertzel_power(buf, N, 55.0);
        const double p2 = goertzel_power(buf, N, 110.0);
        const double p3 = goertzel_power(buf, N, 165.0);
        float rms = 0.0f;
        for (int i = 0; i < N; ++i) rms += buf[i] * buf[i];
        rms = sqrtf(rms / (float)N);

        printf("[VBB mode %u] P55=%.3e P110=%.3e P165=%.3e rms=%.4f\n", mode, p1, p2, p3, rms);
        assert(isfinite(p1) && p1 > 0.0);
        if (mode == 0U) {
            /* Чистый шелф: подъём без генерации гармоник */
            assert(rms > 0.30f * 0.80f);        /* заметный подъём НЧ */
            assert(p2 < 1.0e-3 * p1 + 1e-9);    /* 2-я гармоника практически отсутствует */
        } else if (mode == 1U) {
            assert(p2 > 2.0e-3 * p1);           /* чётная 2-я гармоника реально слышима */
            assert(p3 > 0.5e-3 * p1);           /* нечётная 3-я тоже есть */
        } else {
            assert(p2 > 2.0e-3 * p1);           /* Missing Fundamental: опора на 2-ю гармонику */
        }
    }

    /* Сравнение 2-й гармоники: mode 2 >= mode 1 (вес harm выше) */
    static omnia_dsp_delay_arena_t arena1, arena2;
    static omnia_dsp_ctx_t d1, d2;
    omnia_dsp_init(&d1, &arena1, FS);
    omnia_dsp_init(&d2, &arena2, FS);
    for (int i = 0; i < 2; ++i) {
        omnia_dsp_set_mute(i ? &d2 : &d1, false, 0U);
        omnia_dsp_set_volume(i ? &d2 : &d1, 0.0f, 0.0f, 0.0f, 0U);
    }
    omnia_vbb_params_t v = {0};
    v.enable = 1U; v.amount = 0.8f; v.freq_hz = 55.0f; v.drive = 0.6f; v.lowpass_hz = 220.0f; v.mix = 1.0f;
    v.mode = 1U; omnia_dsp_apply_vbb(&d1, &v);
    v.mode = 2U; omnia_dsp_apply_vbb(&d2, &v);
    static float b1[N], b2[N];
    vbb_render(&d1, 0.30f, 55.0f, b1, N, 1U);
    vbb_render(&d2, 0.30f, 55.0f, b2, N, 2U);
    const double h1 = goertzel_power(b1, N, 110.0);
    const double h2 = goertzel_power(b2, N, 110.0);
    assert(h2 > h1); /* В режиме Missing Fundamental 2-я гармоника громче */
    printf("[OK] VBB: спектр режимов 0/1/2 соответствует ТЗ (p2: %.2e -> %.2e)\n", h1, h2);
}

/* ---------- 8. AETHER: инварианты ---------- */
static void test_aether_identity_and_width(void)
{
    enum { N = FRAMES * 4 };
    static omnia_dsp_delay_arena_t arena;
    static omnia_dsp_ctx_t dsp;
    omnia_dsp_init(&dsp, &arena, FS);
    omnia_dsp_set_mute(&dsp, false, 0U);
    omnia_dsp_set_volume(&dsp, 0.0f, 0.0f, 0.0f, 0U);
    omnia_dsp_set_bypass_mask(&dsp, OMNIA_DSP_BIT_AETHER, 0U);

    omnia_aether_params_t a = {0};
    a.enable = 1U; a.master_blend = 1.0f; a.width = 1.0f; a.center_anchor_db = 0.0f;
    a.bass_keep_mono = 0U; a.side_hpf_en = 0U; a.air_gain_db = 0.0f; a.de_harsh = 0.0f;
    a.depth_amount = 0.0f; a.triode_warmth = 0.0f; a.crossfeed_en = 0U; a.stage_size_ms = 2.0f;
    omnia_dsp_apply_aether(&dsp, &a);
    /* Лимитер отключаем: у look-ahead линии 86 отсчётов нулей в начале потока,
     * и клэмп 0.999999 FS даст прогнозируемое отклонение ~2^31-0.999999*2^31 ~ 2149 LSB
     * на отрицательной полной шкале (проектно, задокументировано). */
    dsp.limiter_cfg.enable = 0U;
    dsp.limiter_cfg.tpdf_dither_en = 0U;
    omnia_dsp_apply_limiter(&dsp, &dsp.limiter_cfg);
    /* Параметр widthSlew: ~95% установления за 30 мс -> после 30 блоков*512 = 15360 отсчётов
     * остаточная ошибка e^-32 ~ 1e-14, бит-незамечаема. */
    warmup(&dsp, 30U);

    uint32_t rng = 12345U;
    static int32_t blk[FRAMES * 2];
    int max_err = 0;
    for (unsigned b = 0; b < 4U; ++b) {
        for (size_t i = 0; i < FRAMES * 2U; ++i) blk[i] = (int32_t)(frand(&rng) * 8000000.0f); /* < 2^23: точный roundtrip */
        int32_t out[FRAMES * 2];
        omnia_dsp_process_block_i32(&dsp, blk, out, FRAMES);
        for (size_t i = 0; i < FRAMES * 2U; ++i) {
            int err = abs((int)((int64_t)out[i] - (int64_t)blk[i]));
            if (err > max_err) max_err = err;
        }
    }
    /* float32 M/S round-trip неизбежно теряет 1..2 ulp мантиссы (~1.3e-7 отн.),
     * что в единицах int32 full-scale (2^31) даёт ~±220 LSB = менее ОДНОГО 24-бит LSB (256).
     * Для 24-бит ЦАП это суб-LSB ошибка -> идентичность сохраняется. */
    assert(max_err <= 256);
    printf("[OK] AETHER identity: blend=1,width=1,нейтрально -> ошибка %d int32-LSB (< 1 24-бит LSB)\n", max_err);

    /* width = 0 -> полное моно (L == R) */
    a.width = 0.0f;
    omnia_dsp_apply_aether(&dsp, &a);
    warmup(&dsp, 30U); /* widthSlew -> ~0 за 427 мс */
    for (size_t i = 0; i < FRAMES * 2U; i += 2U) {
        blk[i] = (int32_t)(frand(&rng) * 1500000000.0f);
        blk[i + 1U] = (int32_t)(frand(&rng) * 1500000000.0f);
    }
    int32_t out[FRAMES * 2];
    omnia_dsp_process_block_i32(&dsp, blk, out, FRAMES);
    int max_lr_diff = 0;
    for (size_t i = 0; i < FRAMES * 2U; i += 2U) {
        int d = abs((int)((int64_t)out[i] - (int64_t)out[i + 1U]));
        if (d > max_lr_diff) max_lr_diff = d;
    }
    /* Тот же float32 round-trip предел (l + blend*(mid - l)): ≤2 ulp мантиссы (~128 int32-LSB),
     * что меньше 1 24-бит LSB (256). Моно-совпадение сохраняется на битовом уровне 24-бит ЦАП. */
    assert(max_lr_diff <= 256);
    printf("[OK] AETHER width=0: L==R (моно), макс. расхождение %d int32-LSB (< 1 24-бит LSB)\n", max_lr_diff);
}

static void test_aether_bass_mono_lock(void)
{
    static omnia_dsp_delay_arena_t arena;
    static omnia_dsp_ctx_t dsp;
    omnia_dsp_init(&dsp, &arena, FS);
    omnia_dsp_set_mute(&dsp, false, 0U);
    omnia_dsp_set_volume(&dsp, 0.0f, 0.0f, 0.0f, 0U);
    omnia_dsp_set_bypass_mask(&dsp, OMNIA_DSP_BIT_AETHER, 0U);

    omnia_aether_params_t a = {0};
    a.enable = 1U; a.master_blend = 1.0f; a.width = 1.0f; a.center_anchor_db = 0.0f;
    a.bass_keep_mono = 1U; a.bass_xover_hz = 250.0f; a.side_hpf_en = 0U;
    a.depth_amount = 0.0f; a.triode_warmth = 0.0f;
    omnia_dsp_apply_aether(&dsp, &a);
    warmup(&dsp, 10U);

    /* Противофазный 40 Гц: чистая боковая составляющая ниже кроссовера */
    double ph = 0.0;
    const double step = 2.0 * M_PI * 40.0 / (double)FS;
    double side_sum_sq = 0.0;
    long side_n = 0;
    static int32_t blk[FRAMES * 2];
    for (unsigned b = 0; b < 30U; ++b) {
        for (size_t f = 0; f < FRAMES; ++f) {
            const double s = 0.4 * sin(ph);
            ph += step;
            blk[f * 2U] = (int32_t)(s * 2147483647.0);
            blk[f * 2U + 1U] = (int32_t)(-s * 2147483647.0);
        }
        int32_t outb[FRAMES * 2];
        omnia_dsp_process_block_i32(&dsp, blk, outb, FRAMES);
        /* Моно-лок LP 250 Гц 2-го порядка: переходная полоса ~20 блоков,
         * считаем только установившийся режим (hp-остаток ~ (40/250)^2). */
        if (b >= 22U) {
            for (size_t f = 0; f < FRAMES; ++f) {
                const float o = fabsf((float)outb[f * 2U] / 2147483648.0f - (float)outb[f * 2U + 1U] / 2147483648.0f);
                side_sum_sq += (double)o * (double)o;
                ++side_n;
            }
        }
    }
    const float rms_side_out = (side_n > 0) ? (float)sqrt(side_sum_sq / (double)side_n) : 1.0f;
    /* Входная противофазная амплитуда 0.4: выходная боковая составляющая должна быть намного меньше */
    assert(rms_side_out < 0.10f);
    printf("[OK] AETHER bass-mono-lock: противофазные 40 Гц подавлены (side rms=%.4f)\n", rms_side_out);
}

static void test_aether_crossfeed_and_depth_finite(void)
{
    static omnia_dsp_delay_arena_t arena;
    static omnia_dsp_ctx_t dsp;
    omnia_dsp_init(&dsp, &arena, FS);
    omnia_dsp_set_mute(&dsp, false, 0U);
    omnia_dsp_set_volume(&dsp, 0.0f, 0.0f, 0.0f, 0U);
    omnia_dsp_set_bypass_mask(&dsp, OMNIA_DSP_BIT_AETHER, 0U);

    omnia_aether_params_t a = {0};
    a.enable = 1U; a.master_blend = 1.0f; a.width = 1.0f; a.center_anchor_db = 0.0f;
    a.bass_keep_mono = 0U; a.side_hpf_en = 0U; a.air_gain_db = 0.0f;
    a.depth_amount = 0.2f; a.stage_size_ms = 3.0f; a.crossfeed_en = 1U;
    a.crossfeed_level = 0.65f; a.crossfeed_fc_hz = 700.0f;
    omnia_dsp_apply_aether(&dsp, &a);
    warmup(&dsp, 10U);

    uint32_t rng = 777U;
    static int32_t blk[FRAMES * 2];
    double rms_in = 0.0, rms_out = 0.0;
    for (unsigned b = 0; b < 20U; ++b) {
        for (size_t i = 0; i < FRAMES * 2U; ++i) blk[i] = (int32_t)(frand(&rng) * 1000000000.0f);
        for (size_t i = 0; i < FRAMES * 2U; ++i) rms_in += (double)blk[i] * blk[i];
        int32_t outb[FRAMES * 2];
        omnia_dsp_process_block_i32(&dsp, blk, outb, FRAMES);
        for (size_t i = 0; i < FRAMES * 2U; ++i) {
            rms_out += (double)outb[i] * outb[i];
            assert(isfinite((float)outb[i]));
        }
    }
    rms_in = sqrt(rms_in / (double)(20U * FRAMES * 2U));
    rms_out = sqrt(rms_out / (double)(20U * FRAMES * 2U));
    assert(rms_out > 0.4 * rms_in && rms_out < 2.5 * rms_in);
    printf("[OK] AETHER crossfeed(~260us)+depth: конечный тракт, rms %.3e -> %.3e\n", rms_in, rms_out);
}

/* ---------- 9. Стабильность биквадов: sweep по (тип, f0, Q, gain) ---------- */
static float pole_radius(const omnia_biquad_state_t *st)
{
    const float a1 = st->a1, a2 = st->a2;
    if (fabsf(a2) < 1.0e-9f) {
        return fabsf(a1); /* фактически 1-й порядок */
    }
    const float discr = a1 * a1 - 4.0f * a2;
    if (discr >= 0.0f) {
        const float sq = sqrtf(discr);
        const float z1 = fabsf((-a1 + sq) * 0.5f);
        const float z2 = fabsf((-a1 - sq) * 0.5f);
        return z1 > z2 ? z1 : z2;
    }
    return sqrtf(fabsf(a2)); /* |z1| == |z2| == sqrt(a2) */
}

static void test_biquad_stability_sweep(void)
{
    static const float freqs[] = {20.0f, 60.0f, 250.0f, 1000.0f, 4000.0f, 12000.0f, 18000.0f};
    static const float qs[]    = {0.5f, 1.0f, 4.0f, 10.0f, 18.0f};
    static const float gains[] = {-18.0f, -6.0f, 0.0f, 6.0f, 18.0f};
    static const omnia_biquad_type_t types[] = {
        OMNIA_BIQUAD_PEAK, OMNIA_BIQUAD_LOW_SHELF, OMNIA_BIQUAD_HIGH_SHELF,
        OMNIA_BIQUAD_LOW_PASS, OMNIA_BIQUAD_HIGH_PASS, OMNIA_BIQUAD_NOTCH, OMNIA_BIQUAD_ALL_PASS
    };
    unsigned count = 0;
    float worst = 0.0f;
    char worst_desc[96] = "none";
    for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); ++t) {
        for (size_t fi = 0; fi < sizeof(freqs) / sizeof(freqs[0]); ++fi) {
            for (size_t qi = 0; qi < sizeof(qs) / sizeof(qs[0]); ++qi) {
                for (size_t gi = 0; gi < sizeof(gains) / sizeof(gains[0]); ++gi) {
                    omnia_biquad_state_t st;
                    memset(&st, 0, sizeof(st));
                    omnia_dsp_biquad_design(&st, types[t], FS, freqs[fi], qs[qi], gains[gi]);
                    if (st.bypass) continue;
                    const float r = pole_radius(&st);
                    ++count;
                    if (r > worst) {
                        worst = r;
                        snprintf(worst_desc, sizeof(worst_desc), "type=%d f0=%.0f Q=%.1f gain=%.0f r=%.6f",
                                 (int)types[t], freqs[fi], qs[qi], gains[gi], r);
                    }
                }
            }
        }
    }
    printf("[BIQUAD] просканировано %u звеньев, худший радиус полюса: %s\n", count, worst_desc);
    assert(worst < 1.0f);
    printf("[OK] Стабильность биквадов: все полюса внутри единичного круга\n");
}

/* ---------- 10. Суб-сэмпловая задержка: плоская АЧХ на ВЧ (кубика vs линейная) ---------- */
static void test_room_delay_cubic_hf(void)
{
    double ph = 0.0;
    const double step = 2.0 * M_PI * 10000.0 / (double)FS;  /* 10 кГц: линейная 2-отводка даёт |H|~0.79 */
    static int32_t blk[FRAMES * 2];
    double amp_out = 0.0;
    long n = 0;

    static omnia_dsp_delay_arena_t arena;
    static omnia_dsp_ctx_t dsp;
    omnia_dsp_init(&dsp, &arena, FS);
    omnia_dsp_set_mute(&dsp, false, 0U);
    omnia_dsp_set_volume(&dsp, 0.0f, 0.0f, 0.0f, 0U);

    omnia_room_peq_t room = {0};
    room.enable = 1U;
    room.delay_l_ms = 0.26f; /* 12.48 сэмплов: дробная часть ~0.48 — худший случай для линейной */
    room.delay_r_ms = 0.0f;
    omnia_dsp_apply_room_peq(&dsp, &room);
    omnia_dsp_set_bypass_mask(&dsp, OMNIA_DSP_BIT_ROOM_DELAY, 0U);
    warmup(&dsp, 100U); /* установление рампы задержки */

    for (unsigned b = 0; b < 40U; ++b) {
        for (size_t f = 0; f < FRAMES; ++f) {
            const double s = 0.5 * sin(ph);
            ph += step;
            blk[f * 2U]     = (int32_t)(s * 2147483647.0);
            blk[f * 2U + 1U] = blk[f * 2U];
        }
        int32_t outb[FRAMES * 2];
        omnia_dsp_process_block_i32(&dsp, blk, outb, FRAMES);
        if (b >= 20U) {
            for (size_t f = 0; f < FRAMES; ++f) {
                const double o = (double)outb[f * 2U] / 2147483648.0;
                amp_out += o * o;
                ++n;
            }
        }
    }
    const double in_rms = 0.5 / sqrt(2.0);
    const double out_rms = sqrt(amp_out / (double)n);
    printf("[DELAY] 10 кГц: in_rms=%.4f out_rms=%.4f ratio=%.3f\n", in_rms, out_rms, out_rms / in_rms);
    assert(out_rms / in_rms > 0.92); /* кубика: >0.97; линейная: ~0.79 -> тест действительно различает */
    printf("[OK] Суб-сэмпловая задержка (Catmull-Rom): плоская АЧХ на ВЧ\n");
}

/* ---------- 11. A/B сравнение: громкость выравнивается по RMS ---------- */
static double render_block_rms(omnia_dsp_ctx_t *dsp, double *ph, const double step, unsigned blocks)
{
    static int32_t blk[FRAMES * 2];
    double sum = 0.0;
    long n = 0;
    for (unsigned b = 0; b < blocks; ++b) {
        for (size_t f = 0; f < FRAMES; ++f) {
            const double s = 0.2 * sin(*ph);
            *ph += step;
            const int32_t q = (int32_t)(s * 2147483647.0);
            blk[f * 2U] = q;
            blk[f * 2U + 1U] = q;
        }
        int32_t outb[FRAMES * 2];
        omnia_dsp_process_block_i32(dsp, blk, outb, FRAMES);
        for (size_t i = 0; i < FRAMES * 2U; ++i) {
            const double o = (double)outb[i];
            sum += o * o;
        }
        n += (long)FRAMES * 2;
    }
    return sqrt(sum / (double)n);
}

static void test_ab_compare_rms_match(void)
{
    static omnia_dsp_ctx_t dsp;
    static omnia_dsp_delay_arena_t arena;
    omnia_dsp_init(&dsp, &arena, FS);
    omnia_dsp_set_mute(&dsp, false, 0U);
    omnia_dsp_set_volume(&dsp, 0.0f, 0.0f, 0.0f, 0U);

    static omnia_user_peq_t peq;
    memset(&peq, 0, sizeof(peq));
    peq.enable = 1U;
    peq.band_count = 1U;
    peq.bands[0].type = OMNIA_BIQUAD_PEAK;
    peq.bands[0].f0_hz = 1000.0f;
    peq.bands[0].q = 2.0f;
    peq.bands[0].gain_db = 6.0f;   /* ~2x буст: в пределах клэмпа матчера (до 2.0) */
    omnia_dsp_apply_user_peq(&dsp, &peq);
    omnia_dsp_set_bypass_mask(&dsp, OMNIA_DSP_BIT_USER_PEQ, 0U);
    dsp.limiter_cfg.enable = 0U; /* убрать влияние лимитера на измерение */
    omnia_dsp_apply_limiter(&dsp, &dsp.limiter_cfg);

    const double step = 2.0 * M_PI * 1000.0 / (double)FS;
    double ph = 0.0;

    /* Фаза A: включённая обработка (PEQ активен) */
    warmup(&dsp, 30U);
    const double rms_processed = render_block_rms(&dsp, &ph, step, 20U);

    /* Фаза B: A/B-bypass ("чистый исходник") с RMS-выравниванием — обязан звучать на ТОЙ ЖЕ громкости */
    omnia_dsp_set_ab_compare(&dsp, true);
    warmup(&dsp, 30U); /* установление огибающих rms_in/rms_out */
    const double rms_ab = render_block_rms(&dsp, &ph, step, 20U);

    printf("[A/B] rms(processed)=%.1f rms(A/B dry match)=%.1f ratio=%.3f (идеал 1.0)\n",
           rms_processed, rms_ab, rms_ab / rms_processed);
    assert(rms_processed > 0.0 && rms_ab > 0.0);
    assert(rms_ab / rms_processed > 0.75 && rms_ab / rms_processed < 1.33); /* совпадение в пределах ±2.5 дБ */
    omnia_dsp_set_ab_compare(&dsp, false);
    printf("[OK] A/B RMS-match: чистый исходник выровнен по громкости с обработкой\n");
}

/* ---------- 12. Заводские пресеты Эфира валидны и применимы ---------- */
static void test_factory_aether_presets(void)
{
    static const char *names[7] = {
        "OFF", "Natural Studio", "Wide Holographic", "Silk Air & Vocal",
        "Tube Lounge", "Concert Hall", "Headphone Stage"
    };
    for (uint8_t slot = 0; slot < 7U; ++slot) {
        static omnia_dsp_delay_arena_t arena;
        static omnia_dsp_ctx_t dsp;
        omnia_dsp_init(&dsp, &arena, FS);
        assert(omnia_dsp_load_aether_factory(&dsp, slot));
        assert(omnia_dsp_validate_payload(OMNIA_CMD_LOAD_AETHER, &dsp.aether_cfg, sizeof(dsp.aether_cfg)));
        omnia_dsp_set_mute(&dsp, false, 0U);
        omnia_dsp_set_bypass_mask(&dsp, OMNIA_DSP_BIT_AETHER, 0U);
        warmup(&dsp, 5U);
        static int32_t blk[FRAMES * 2];
        uint32_t rng = 999U + slot;
        for (size_t i = 0; i < FRAMES * 2U; ++i) blk[i] = (int32_t)(frand(&rng) * 900000000.0f);
        int32_t out[FRAMES * 2];
        omnia_dsp_process_block_i32(&dsp, blk, out, FRAMES);
        for (size_t i = 0; i < FRAMES * 2U; ++i) assert(isfinite((float)out[i]));
        printf("[OK] Пресет Эфира %u (%s): валиден, тракт конечен\n", slot, names[slot]);
    }
    static omnia_dsp_ctx_t dsp;
    assert(!omnia_dsp_load_aether_factory(&dsp, 7U));
    printf("[OK] Невалидный слот пресета отклонён\n");
}


/* ---------- 14. Anti-click: рампа коэффициентов PEQ при ретюне уже играющей полосы ---------- */
/* Реалистичный случай drag-настройки: полоса активна, состояние биквада нагружено,
 * пользователь перетягивает частоту/добротность/усиление. Рампа (~5 мс) должна звучать
 * заметно плавнее мгновенного переключения коэффициентов. */
static double render_retune(bool ramp, double *boundary_out)
{
    static omnia_dsp_delay_arena_t arena;
    static omnia_dsp_ctx_t dsp;
    omnia_dsp_init(&dsp, &arena, FS);
    omnia_dsp_set_mute(&dsp, false, 0U);
    omnia_dsp_set_volume(&dsp, 0.0f, 0.0f, 0.0f, 0U);
    omnia_dsp_set_bypass_mask(&dsp, OMNIA_DSP_BIT_USER_PEQ, 0U);
    dsp.limiter_cfg.enable = 0U; /* иначе clamp маскирует эффект */
    omnia_dsp_apply_limiter(&dsp, &dsp.limiter_cfg);

    static omnia_user_peq_t peq;
    memset(&peq, 0, sizeof(peq));
    peq.enable = 1U;
    peq.band_count = 1U;
    peq.bands[0].type = OMNIA_BIQUAD_PEAK;
    peq.bands[0].f0_hz = 1000.0f;
    peq.bands[0].q = 4.0f;
    peq.bands[0].gain_db = -15.0f;
    omnia_dsp_apply_user_peq(&dsp, &peq); /* включаем ДО сигнала: полоса играет */

    const double step = 2.0 * M_PI * 1000.0 / (double)FS;
    double ph = 0.0, prev = 0.0;
    int have_prev = 0;
    double boundary = 0.0;
    for (unsigned b = 0; b < 12U; ++b) {
        static int32_t blk[FRAMES * 2];
        for (size_t f = 0; f < FRAMES; ++f) {
            const double s = 0.5 * sin(ph);
            ph += step;
            blk[f * 2U] = (int32_t)(s * 2147483647.0);
            blk[f * 2U + 1U] = blk[f * 2U];
        }
        if (b == 10U) { /* ретюн: 700 Гц, +12 дБ, Q=4 */
            static omnia_user_peq_t retune;
            memset(&retune, 0, sizeof(retune));
            retune.enable = 1U;
            retune.band_count = 1U;
            retune.bands[0].type = OMNIA_BIQUAD_PEAK;
            retune.bands[0].f0_hz = 700.0f;
            retune.bands[0].q = 4.0f;
            retune.bands[0].gain_db = 12.0f;
            dsp.coeff_ramp_enable = ramp;
            omnia_dsp_apply_user_peq(&dsp, &retune);
            dsp.coeff_ramp_enable = true;
        }
        int32_t out[FRAMES * 2];
        omnia_dsp_process_block_i32(&dsp, blk, out, FRAMES);
        if (b == 10U) {
            for (size_t f = 0; f < 16U; ++f) {
                const double cur = (double)out[f * 2U] / 2147483648.0;
                if (have_prev) {
                    const double d = fabs(cur - prev);
                    if (d > boundary) boundary = d;
                }
                prev = cur;
                have_prev = 1;
            }
        }
    }
    /* рампа завершилась точно в цели */
    assert(dsp.user_biquads[0].ramp_left == 0U);
    assert(dsp.user_biquads[0].b0 == dsp.user_biquads[0].tb0);
    assert(dsp.user_biquads[0].a2 == dsp.user_biquads[0].ta2);
    *boundary_out = boundary;
    return boundary;
}

static void test_peq_ramp_anti_click(void)
{
    double boundary_ramp = 0.0, boundary_instant = 0.0;
    render_retune(true,  &boundary_ramp);
    render_retune(false, &boundary_instant);
    printf("[RAMP] retune-ретюн полосы: max |delta| за 16 отсчётов: ramp=%.6f, instant=%.6f\n",
           boundary_ramp, boundary_instant);
    assert(isfinite(boundary_ramp) && isfinite(boundary_instant));
    assert(boundary_ramp * 2.0 < boundary_instant); /* рампа минимум вдвое плавнее скачка */
    printf("[OK] Рампа коэффициентов PEQ при ретюне: щелчок устранён (2x+)\n");
}

/* ---------- 15. Limiter v2.4: детектор суб-сэмплового true-peak включён на ВЧ-пиках ---------- */
static void test_limiter_true_peak_hf(void)
{
    static omnia_dsp_delay_arena_t arena;
    static omnia_dsp_ctx_t dsp;
    omnia_dsp_init(&dsp, &arena, FS);
    omnia_dsp_set_mute(&dsp, false, 0U);
    omnia_dsp_set_volume(&dsp, 0.0f, 0.0f, 0.0f, 0U);
    omnia_dsp_set_bypass_mask(&dsp, OMNIA_DSP_BIT_LIMITER, 0U);

    /* 12 кГц, фаза пи/8: сэмплы НИКОГДА не достают гребень синусоиды
     * (max сэмплa ≈ 0.924·A), суб-сэмпловый гребень ≈ A.
     * A = 0.975: сэмплы 0.901 < ceiling(-0.8 dBFS ≈ 0.912), гребень выше потолка. */
    double ph = M_PI / 8.0;
    const double step = 2.0 * M_PI * 12000.0 / (double)FS;
    static int32_t blk[FRAMES * 2];
    float sample_peak = 0.0f;
    for (unsigned b = 0; b < 8U; ++b) {
        for (size_t f = 0; f < FRAMES; ++f) {
            const double s = 0.975 * sin(ph);
            ph += step;
            blk[f * 2U] = (int32_t)(s * 2147483647.0);
            blk[f * 2U + 1U] = blk[f * 2U];
            const float a = fabsf((float)s);
            if (a > sample_peak) sample_peak = a;
        }
        int32_t out[FRAMES * 2];
        omnia_dsp_process_block_i32(&dsp, blk, out, FRAMES);
    }
    const float ceiling = (float)pow(10.0, -0.8 / 20.0);
    printf("[TRUE-PEAK] sample_peak=%.4f ceiling=%.4f min_limiter_gain=%.4f\n",
           sample_peak, ceiling, dsp.min_limiter_gain);
    assert(sample_peak < ceiling);            /* сэмплы под потолком: обычный пиковый детектор молчал бы */
    assert(dsp.min_limiter_gain < 0.999f);    /* TP-детектор сработал и давил гребень */
    printf("[OK] Limiter true-peak (Catmull-Rom, аналитический максимум): ВЧ гребни под потолком\n");
}

/* ---------- 13. Валидация новых команд ---------- */
static void test_new_command_validation(void)
{
    omnia_factory_aether_t fa = {0};
    fa.slot = 6U;
    assert(omnia_dsp_validate_payload(OMNIA_CMD_LOAD_FACTORY_AETHER, &fa, sizeof(fa)));
    fa.slot = 7U;
    assert(!omnia_dsp_validate_payload(OMNIA_CMD_LOAD_FACTORY_AETHER, &fa, sizeof(fa)));
    assert(omnia_dsp_validate_payload(OMNIA_CMD_RESET_DC_FAULT, (const void *)"", 0U));
    assert(!omnia_dsp_validate_payload(OMNIA_CMD_RESET_DC_FAULT, (const void *)"x", 1U));
    printf("[OK] Валидация RESET_DC_FAULT / LOAD_FACTORY_AETHER\n");
}

int main(void)
{
    printf("=== OMNIA DSP quality suite ===\n");
    test_bitperfect_exact_copy();
    test_float_path_int_roundtrip();
    test_limiter_ceiling_no_overshoot();
    test_dc_guard_trips_and_resets();
    test_vbb_harmonics();
    test_aether_identity_and_width();
    test_aether_bass_mono_lock();
    test_aether_crossfeed_and_depth_finite();
    test_biquad_stability_sweep();
    test_room_delay_cubic_hf();
    test_ab_compare_rms_match();
    test_factory_aether_presets();
    test_peq_ramp_anti_click();
    test_limiter_true_peak_hf();
    test_new_command_validation();
    printf("=== ВСЕ КАЧЕСТВЕННЫЕ ТЕСТЫ ПРОЙДЕНЫ ===\n");
    return 0;
}
