/*
 * 512-точечный FFT (radix-2, Hann) и логарифмические полосы для спектроанализатора.
 * ТЗ, Том 3, Гл. 3: 64 полосы, считаются на Core 0, рисуются на Core 1.
 */

#include "omnia.h"
#include <math.h>
#include <string.h>

#define FFT_HALF (FFT_SIZE / 2)

static float s_cos[FFT_HALF];
static float s_sin[FFT_HALF];
static float s_win[FFT_SIZE];
static bool  s_inited = false;

static void fft_init(void)
{
    for (int i = 0; i < FFT_HALF; i++) {
        float a = -2.0f * (float)M_PI * i / FFT_SIZE;
        s_cos[i] = cosf(a);
        s_sin[i] = sinf(a);
    }
    for (int i = 0; i < FFT_SIZE; i++) {
        s_win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (FFT_SIZE - 1));
    }
    s_inited = true;
}

static void fft_inplace(float* re, float* im)
{
    const int n = FFT_SIZE;
    // Перестановка бит
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    // Бабочки
    for (int len = 2; len <= n; len <<= 1) {
        int half = len >> 1;
        int step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; k++) {
                float wr = s_cos[k * step];
                float wi = s_sin[k * step];
                int a = i + k;
                int b = a + half;
                float tr = re[b] * wr - im[b] * wi;
                float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
}

void compute_spectrum_bands(const float* in, uint32_t sample_rate, float* out)
{
    static float re[FFT_SIZE];
    static float im[FFT_SIZE];
    static float mag[FFT_HALF];

    if (!s_inited) fft_init();

    for (int i = 0; i < FFT_SIZE; i++) {
        re[i] = in[i] * s_win[i];
        im[i] = 0.0f;
    }
    fft_inplace(re, im);

    for (int k = 0; k < FFT_HALF; k++) {
        mag[k] = sqrtf(re[k] * re[k] + im[k] * im[k]) / FFT_HALF;
    }

    // Логарифмические полосы 20 Гц .. 20 кГц
    for (int b = 0; b < SPECTRUM_BANDS; b++) {
        float f_lo = 20.0f * powf(1000.0f, (float)b / SPECTRUM_BANDS);
        float f_hi = 20.0f * powf(1000.0f, (float)(b + 1) / SPECTRUM_BANDS);
        int k_lo = (int)(f_lo * FFT_SIZE / sample_rate);
        int k_hi = (int)(f_hi * FFT_SIZE / sample_rate) + 1;
        if (k_lo < 1) k_lo = 1;
        if (k_hi <= k_lo) k_hi = k_lo + 1;
        if (k_hi > FFT_HALF - 1) k_hi = FFT_HALF - 1;

        float peak = 0.0f;
        for (int k = k_lo; k <= k_hi; k++) {
            if (mag[k] > peak) peak = mag[k];
        }
        float db = 20.0f * log10f(peak + 1e-9f);
        float norm = (db + 80.0f) / 80.0f;   // -80 dB .. 0 dB -> 0..1
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;
        out[b] = norm;
    }
}
