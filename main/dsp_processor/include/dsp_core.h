#ifndef DSP_CORE_H
#define DSP_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ТЗ, Том 3, Гл. 2: двухслойный DSP.
#define OMNIA_ROOM_BANDS 5     // Слой 1: Room Correction
#define OMNIA_USER_BANDS 15    // Слой 2: пользовательский PEQ

// Коэффициенты биквада: c[0..4] = b0, b1, b2, a1, a2 (знаменатель 1 + a1 z^-1 + a2 z^-2, a0 нормирован)
// Состояние: w[0], w[1] (Direct Form II Transposed) на каждый канал.
class OmniaDSPCore {
private:
    float m_room_coeffs[OMNIA_ROOM_BANDS][5];
    float m_room_w_left[OMNIA_ROOM_BANDS][2];
    float m_room_w_right[OMNIA_ROOM_BANDS][2];
    float m_room_freq[OMNIA_ROOM_BANDS];
    float m_room_q[OMNIA_ROOM_BANDS];
    float m_room_gain_db[OMNIA_ROOM_BANDS];

    float m_user_coeffs[OMNIA_USER_BANDS][5];
    float m_user_w_left[OMNIA_USER_BANDS][2];
    float m_user_w_right[OMNIA_USER_BANDS][2];
    float m_user_freq[OMNIA_USER_BANDS];
    float m_user_q[OMNIA_USER_BANDS];
    float m_user_gain_db[OMNIA_USER_BANDS];

    float m_pre_amp_attenuation;   // Dynamic Headroom
    float m_limiter_gain;          // состояние AGC / anti-AD
    uint32_t m_sample_rate;
    SemaphoreHandle_t m_lock;

    void design_peaking(float* c, float freq, float q, float gain_db) const;
    void recalc_headroom_locked();

public:
    OmniaDSPCore();
    bool init(uint32_t sample_rate);
    void set_sample_rate(uint32_t sample_rate);
    void calculate_dynamic_headroom();
    void set_user_peq(uint8_t band_idx, float freq, float q, float gain_db);
    void set_room_correction(uint8_t band_idx, float freq, float q, float gain_db);
    // Интерливед стерео (L,R,L,R...), frames — число стерео-кадров
    void process_stereo_samples(float* buffer, size_t frames, uint8_t mode_anti_ad);
};

#endif // DSP_CORE_H
