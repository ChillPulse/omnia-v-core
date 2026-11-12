/**
 * @file    omnia_asrc.h
 * @brief   Студийный 24/32-битный асинхронный ресемплер (Cubic Farrow ASRC + PI Servo)
 *          Развязывает тактовый домен Bluetooth (SJR-BTM334 QCC3034 / BT401 Master на I2S3)
 *          от эталонного генератора I2S2 Master AT32F403ACGU7 без проскальзывания и щелчков.
 */

#ifndef OMNIA_ASRC_H
#define OMNIA_ASRC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OMNIA_ASRC_RING_FRAMES      512U    /* Степень двойки: 512 стерео-кадров (~10.6 мс @ 48 кГц) */
#define OMNIA_ASRC_RING_MASK        (OMNIA_ASRC_RING_FRAMES - 1U)
#define OMNIA_ASRC_TARGET_FILL      256.0f  /* Рабочая точка буфера (~5.3 мс) */

typedef struct {
    int32_t  ring_l[OMNIA_ASRC_RING_FRAMES]; /* Left-justified 24/32-bit PCM L */
    int32_t  ring_r[OMNIA_ASRC_RING_FRAMES]; /* Left-justified 24/32-bit PCM R */
    volatile uint32_t wr_idx;                /* Индекс записи (продюсер I2S3 DMA) */
    uint32_t rd_idx;                         /* Целая часть индекса чтения (консьюмер I2S2) */
    float    phase_mu;                       /* Дробная фаза [0.0 .. 1.0) интерполятора Фарроу */

    uint32_t in_fs_hz;                       /* Входная частота BT (48000 или 44100 Гц) */
    uint32_t out_fs_hz;                      /* Выходная сетка I2S2 Master (48000 или 44100 Гц) */
    float    nominal_ratio;                  /* in_fs_hz / out_fs_hz */
    float    current_ratio;                  /* Мгновенный шаг чтения с учётом PI-подстройки */
    float    pi_integral;                    /* Интегратор петли слежения за дрейфом кварцев */
    float    smooth_fill;                    /* Экспоненциально сглаженная заполненность буфера */

    bool     primed;                         /* true после накопления половины буфера */
    float    fade_gain;                      /* Плавный анти-щелчковый fade-in/out 0.0 .. 1.0 */
    uint32_t underrun_count;
    uint32_t overrun_count;
} omnia_asrc_t;

void  omnia_asrc_init(omnia_asrc_t *asrc, uint32_t in_fs_hz, uint32_t out_fs_hz);
void  omnia_asrc_reset(omnia_asrc_t *asrc);
void  omnia_asrc_set_rates(omnia_asrc_t *asrc, uint32_t in_fs_hz, uint32_t out_fs_hz);

/**
 * @brief Записать пакет входных 24/32-bit стерео-сэмплов (interleaved L, R в 32-bit слотах)
 */
void  omnia_asrc_push_interleaved(omnia_asrc_t *asrc, const int32_t *in_lr, uint32_t frames);

/**
 * @brief Считать ровно `frames` ресемплированных стерео-кадров в сетке I2S2 Master
 */
void  omnia_asrc_pull_interleaved(omnia_asrc_t *asrc, int32_t *out_lr, uint32_t frames);

/**
 * @brief Текущая заполненность кольцевого буфера (в кадрах)
 */
uint32_t omnia_asrc_level(const omnia_asrc_t *asrc);

#ifdef __cplusplus
}
#endif

#endif /* OMNIA_ASRC_H */
