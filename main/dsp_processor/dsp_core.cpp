/*
 * DSP: двухслойный параметрический фильтр в float32.
 *
 * Отличия от исходного кода ТЗ (исправлены ошибки, см. README):
 *   - Усиление полосы хранится отдельно, а не в коэффициенте a2 (раньше headroom считался по a2).
 *   - Левый и правый каналы фильтруются отдельно (раньше интерливед-буфер шел как моно).
 *   - Коэффициенты считаются по RBJ, фильтрация — DF2T. Соглашение совпадает с
 *     вызовом dsps_biquad_f32_ae32 из esp-dsp; при желании подмените process-функцию.
 */

#include "dsp_core.h"
#include <string.h>
#include <math.h>

#define OMNIA_PI 3.14159265358979f

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Фильтрация одного канала: data[offset + i*stride]
static void biquad_channel(float* data, size_t n, const float* c, float* w, size_t offset, size_t stride)
{
    float w0 = w[0];
    float w1 = w[1];
    for (size_t i = 0; i < n; i++) {
        float* p = &data[i * stride + offset];
        float x = *p;
        float y = c[0] * x + w0;
        w0 = c[1] * x - c[3] * y + w1;
        w1 = c[2] * x - c[4] * y;
        *p = y;
    }
    w[0] = w0;
    w[1] = w1;
}

OmniaDSPCore::OmniaDSPCore()
    : m_pre_amp_attenuation(1.0f), m_limiter_gain(1.0f), m_sample_rate(44100), m_lock(NULL)
{
}

void OmniaDSPCore::design_peaking(float* c, float freq, float q, float gain_db) const
{
    freq = clampf(freq, 10.0f, m_sample_rate * 0.45f);
    q    = clampf(q, 0.1f, 50.0f);
    gain_db = clampf(gain_db, -18.0f, 18.0f);

    float A     = powf(10.0f, gain_db / 40.0f);
    float w0    = 2.0f * OMNIA_PI * freq / (float)m_sample_rate;
    float cw    = cosf(w0);
    float alpha = sinf(w0) / (2.0f * q);

    float b0 = 1.0f + alpha * A;
    float b1 = -2.0f * cw;
    float b2 = 1.0f - alpha * A;
    float a0 = 1.0f + alpha / A;
    float a1 = -2.0f * cw;
    float a2 = 1.0f - alpha / A;

    c[0] = b0 / a0;
    c[1] = b1 / a0;
    c[2] = b2 / a0;
    c[3] = a1 / a0;
    c[4] = a2 / a0;
}

bool OmniaDSPCore::init(uint32_t sample_rate)
{
    m_lock = xSemaphoreCreateMutex();
    if (!m_lock) return false;

    m_sample_rate = sample_rate;
    memset(m_room_w_left,  0, sizeof(m_room_w_left));
    memset(m_room_w_right, 0, sizeof(m_room_w_right));
    memset(m_user_w_left,  0, sizeof(m_user_w_left));
    memset(m_user_w_right, 0, sizeof(m_user_w_right));

    xSemaphoreTake(m_lock, portMAX_DELAY);
    for (int i = 0; i < OMNIA_ROOM_BANDS; i++) {
        m_room_freq[i] = 100.0f;
        m_room_q[i] = 1.0f;
        m_room_gain_db[i] = 0.0f;
        design_peaking(m_room_coeffs[i], m_room_freq[i], m_room_q[i], 0.0f);
    }
    for (int i = 0; i < OMNIA_USER_BANDS; i++) {
        // Логарифмическая сетка 25 Гц .. 16 кГц
        m_user_freq[i] = 25.0f * powf(16000.0f / 25.0f, (float)i / (OMNIA_USER_BANDS - 1));
        m_user_q[i] = 1.4f;
        m_user_gain_db[i] = 0.0f;
        design_peaking(m_user_coeffs[i], m_user_freq[i], m_user_q[i], 0.0f);
    }
    recalc_headroom_locked();
    xSemaphoreGive(m_lock);
    return true;
}

void OmniaDSPCore::set_sample_rate(uint32_t sample_rate)
{
    xSemaphoreTake(m_lock, portMAX_DELAY);
    m_sample_rate = sample_rate;
    for (int i = 0; i < OMNIA_ROOM_BANDS; i++)
        design_peaking(m_room_coeffs[i], m_room_freq[i], m_room_q[i], m_room_gain_db[i]);
    for (int i = 0; i < OMNIA_USER_BANDS; i++)
        design_peaking(m_user_coeffs[i], m_user_freq[i], m_user_q[i], m_user_gain_db[i]);
    xSemaphoreGive(m_lock);
}

// Dynamic Headroom: сумма положительных усилений обоих слоев -> ослабление входа
void OmniaDSPCore::recalc_headroom_locked()
{
    float sum_db = 0.0f;
    for (int i = 0; i < OMNIA_ROOM_BANDS; i++)
        if (m_room_gain_db[i] > 0.0f) sum_db += m_room_gain_db[i];
    for (int i = 0; i < OMNIA_USER_BANDS; i++)
        if (m_user_gain_db[i] > 0.0f) sum_db += m_user_gain_db[i];
    m_pre_amp_attenuation = powf(10.0f, -sum_db / 20.0f);
}

void OmniaDSPCore::calculate_dynamic_headroom()
{
    xSemaphoreTake(m_lock, portMAX_DELAY);
    recalc_headroom_locked();
    xSemaphoreGive(m_lock);
}

void OmniaDSPCore::set_user_peq(uint8_t band_idx, float freq, float q, float gain_db)
{
    if (band_idx >= OMNIA_USER_BANDS) return;
    xSemaphoreTake(m_lock, portMAX_DELAY);
    m_user_freq[band_idx] = freq;
    m_user_q[band_idx] = q;
    m_user_gain_db[band_idx] = clampf(gain_db, -18.0f, 18.0f);
    design_peaking(m_user_coeffs[band_idx], freq, q, m_user_gain_db[band_idx]);
    recalc_headroom_locked();
    xSemaphoreGive(m_lock);
}

void OmniaDSPCore::set_room_correction(uint8_t band_idx, float freq, float q, float gain_db)
{
    if (band_idx >= OMNIA_ROOM_BANDS) return;
    xSemaphoreTake(m_lock, portMAX_DELAY);
    m_room_freq[band_idx] = freq;
    m_room_q[band_idx] = q;
    m_room_gain_db[band_idx] = clampf(gain_db, -18.0f, 18.0f);
    design_peaking(m_room_coeffs[band_idx], freq, q, m_room_gain_db[band_idx]);
    recalc_headroom_locked();
    xSemaphoreGive(m_lock);
}

void OmniaDSPCore::process_stereo_samples(float* buffer, size_t frames, uint8_t mode_anti_ad)
{
    if (!m_lock || frames == 0) return;
    xSemaphoreTake(m_lock, portMAX_DELAY);

    // 1. Предусиление (защита от клиппинга после EQ)
    size_t n = frames * 2;
    for (size_t i = 0; i < n; i++) {
        buffer[i] *= m_pre_amp_attenuation;
    }

    // 2. Слой 1: Room Correction
    for (int b = 0; b < OMNIA_ROOM_BANDS; b++) {
        biquad_channel(buffer, frames, m_room_coeffs[b], m_room_w_left[b],  0, 2);
        biquad_channel(buffer, frames, m_room_coeffs[b], m_room_w_right[b], 1, 2);
    }

    // 3. Слой 2: пользовательский PEQ
    for (int b = 0; b < OMNIA_USER_BANDS; b++) {
        biquad_channel(buffer, frames, m_user_coeffs[b], m_user_w_left[b],  0, 2);
        biquad_channel(buffer, frames, m_user_coeffs[b], m_user_w_right[b], 1, 2);
    }

    // 4. Anti-AD: связанный по каналам лимитер (мягкий 0.7, агрессивный 0.4 от FS)
    if (mode_anti_ad == 0) {
        m_limiter_gain = 1.0f;
    } else {
        const float thr = (mode_anti_ad >= 2) ? 0.4f : 0.7f;
        float g = m_limiter_gain;
        for (size_t i = 0; i < frames; i++) {
            float l = buffer[2 * i];
            float r = buffer[2 * i + 1];
            float peak = fmaxf(fabsf(l), fabsf(r));
            float target = (peak > thr) ? thr / peak : 1.0f;
            // Быстрая атака, медленное восстановление (без «качания» громкости)
            float coef = (target < g) ? 0.25f : 0.00015f;
            g += (target - g) * coef;
            buffer[2 * i]     = l * g;
            buffer[2 * i + 1] = r * g;
        }
        m_limiter_gain = g;
    }

    xSemaphoreGive(m_lock);
}
