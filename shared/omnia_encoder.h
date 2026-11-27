/**
 * @file    omnia_encoder.h
 * @brief   Анти-дрожание и защита от «прыжков» механического квадратурного энкодера
 *          (EC11 и аналоги) для ESP32-S3. Только логика на CPU: не заменяет
 *          RC-фильтр на плате и аппаратный PCNT, а дополняет их.
 *
 * Механика «прыжка» брака: в одном месте оборота канал A/B может переключиться
 * «скачком» (оба канала разом, либо невозможный переход на 2 состояния).
 * Подходы:
 *   1) Декодер по таблице 4-битных окон: шаг даётся ТОЛЬКО на валидном
 *      одиночном переходе. Переход на 2 бита (00→11 и т.п.) шагов не даёт,
 *      фиксируется в illegal_events, состояние ресинхронизируется без шага.
 *   2) Интегратор дребезга: канал считается изменившимся только после
 *      OMNIA_ENC_DEBOUNCE_COUNT одинаковых подряд опросов.
 *   3) Ограничение скорости: не более одного детента за OMNIA_ENC_MIN_STEP_US
 *      (по умолчанию 1000 мкс ≈ 1000 детентов/с; человек ~10–40/с).
 *      Избыточные шаги отбрасываются, suppressed_events++.
 *   4) Накопитель фазы: шаги считаются в 4×-дискретизации квадатуры, детент
 *      выдаётся каждые steps_per_detent (обычно 4) фазовых шагов — недокрут
 *      не теряется и не даёт ложных срабатываний на неполном обороте.
 *
 * Ручной гейтинг по времени удобно сочетать с PCNT: PCNT считает аппаратно,
 * этот модуль фильтрует сырые/аппаратные счёты программно (и наоборот —
 * может работать автономно на отсчётах по таймеру).
 *
 * Гарантии (host-тесты tests/test_encoder.c):
 *   - вал поворота даёт точное число детентов (CW/CCW);
 *   - дребезг до 3 опросов не меняет результат;
 *   - двухбитный скачок бракованного энкодера шага не даёт (illegal_events++),
 *     ресинхронизация без залипания;
 *   - пачка «валидных» переходов быстрее min_step_us ограничивается лимитом;
 *   - реверс направления не создаёт шагов, пока не завершён полупериод.
 * На железе не проверено: временные константы подобраны под EC11 типичный
 * (дребезг 1–5 мс) + RC 10k/100nF, конкретный экземпляр нужно измерить.
 */

#ifndef OMNIA_ENCODER_H
#define OMNIA_ENCODER_H

#include <stdint.h>

#define OMNIA_ENC_DEBOUNCE_COUNT   3u     /* одинаковых опросов подряд для смены канала */
#define OMNIA_ENC_MIN_STEP_US      1000u  /* не чаще одного детента в 1 мс (0 = выкл.) */
#define OMNIA_ENC_ILLEGAL          0x7f   /* маркер «невозможного» перехода */

typedef struct {
    uint8_t  raw_ab;          /* последнее сырьё (A<<1)|B */
    uint8_t  stable_ab;       /* сырьё после интегратора дребезга */
    uint8_t  deb_a, deb_b;    /* интеграторы дребезга по каналам */
    int8_t   detent_accum;    /* накопитель фазовых шагов к детенту */
    uint8_t  steps_per_detent;/* обычно 4 */
    uint8_t  dir_invert;      /* 1 = поменять направление */
    uint16_t min_step_us;     /* лимит скорости, мкс (0 = выкл.) */
    uint64_t last_step_us;    /* время последнего выданного детента */
    int32_t  position;        /* позиция в детентах */
    uint32_t illegal_events;  /* двухбитные скачки (брак/шум) */
    uint32_t suppressed_events; /* шаги, отброшенные лимитом скорости */
    uint32_t total_phase_steps;/* всего валидных фазовых переходов (для диагностики) */
} omnia_encoder_t;

/* Таблица переходов (prev<<2)|cur -> +1/-1/0/«невозможно».
 * Канонический цикл A/B: 00→01→11→10→00. */
static const int8_t omnia_enc_table[16] = {
    /* prev=00 */  0,  -1,  +1,  OMNIA_ENC_ILLEGAL,
    /* prev=01 */ +1,   0,  OMNIA_ENC_ILLEGAL,  -1,
    /* prev=10 */ -1,   OMNIA_ENC_ILLEGAL,  0,  +1,
    /* prev=11 */ OMNIA_ENC_ILLEGAL,  +1,  -1,   0,
};

static inline void omnia_encoder_init(omnia_encoder_t *enc, uint8_t steps_per_detent, uint8_t dir_invert)
{
    enc->raw_ab = 0u;
    enc->stable_ab = 0u;
    enc->deb_a = 0u;
    enc->deb_b = 0u;
    enc->detent_accum = 0;
    enc->steps_per_detent = (steps_per_detent == 0u) ? 4u : steps_per_detent;
    enc->dir_invert = dir_invert ? 1u : 0u;
    enc->min_step_us = OMNIA_ENC_MIN_STEP_US;
    enc->last_step_us = 0u;
    enc->position = 0;
    enc->illegal_events = 0u;
    enc->suppressed_events = 0u;
    enc->total_phase_steps = 0u;
}

/* Один опрос сырых линий A,B (0/1) в момент now_us (монотонные микросекунды).
 * Возвращает приращение позиции в детентах: -1, 0 или +1. */
static inline int omnia_encoder_poll(omnia_encoder_t *enc, uint8_t a, uint8_t b, uint64_t now_us)
{
    a = a ? 1u : 0u;
    b = b ? 1u : 0u;

    /* --- 1. Интегратор дребезга по каналам --- */
    enc->deb_a = (uint8_t)((enc->deb_a << 1) | a);
    enc->deb_a = (uint8_t)(enc->deb_a & ((1u << OMNIA_ENC_DEBOUNCE_COUNT) - 1u));
    if (a) { if (enc->deb_a == ((1u << OMNIA_ENC_DEBOUNCE_COUNT) - 1u)) enc->stable_ab |= 0x02u; }
    else    { if (enc->deb_a == 0u)                              enc->stable_ab &= (uint8_t)~0x02u; }

    enc->deb_b = (uint8_t)((enc->deb_b << 1) | b);
    enc->deb_b = (uint8_t)(enc->deb_b & ((1u << OMNIA_ENC_DEBOUNCE_COUNT) - 1u));
    if (b) { if (enc->deb_b == ((1u << OMNIA_ENC_DEBOUNCE_COUNT) - 1u)) enc->stable_ab |= 0x01u; }
    else    { if (enc->deb_b == 0u)                              enc->stable_ab &= (uint8_t)~0x01u; }

    const uint8_t cur = enc->stable_ab;
    if (cur == enc->raw_ab) {
        return 0; /* нет изменений */
    }

    /* --- 2. Таблица переходов на стабильном состоянии --- */
    const uint8_t idx = (uint8_t)((enc->raw_ab << 2) | cur);
    const int8_t step = omnia_enc_table[idx];
    enc->raw_ab = cur; /* ресинхронизация: следующий опрос считается от нового состояния */
    if (step == OMNIA_ENC_ILLEGAL) {
        enc->illegal_events++;
        return 0;  /* двухбитный скачок брака: шагов не даём, молча ресинхронизируемся */
    }
    if (step == 0) {
        return 0;
    }

    int signed_step = (int)(enc->dir_invert ? -step : step);
    enc->total_phase_steps++;

    /* --- 3. Накопитель фазы: детент = N фазовых шагов --- */
    enc->detent_accum = (int8_t)(enc->detent_accum + signed_step);
    int detents = 0;
    if (enc->detent_accum >= (int8_t)enc->steps_per_detent) {
        detents = 1;
        enc->detent_accum = (int8_t)(enc->detent_accum - (int8_t)enc->steps_per_detent);
    } else if (enc->detent_accum <= -(int8_t)enc->steps_per_detent) {
        detents = -1;
        enc->detent_accum = (int8_t)(enc->detent_accum + (int8_t)enc->steps_per_detent);
    }
    if (detents == 0) {
        return 0;
    }

    /* --- 4. Лимит скорости: отсев невозможных для человека пачек --- */
    if (enc->min_step_us != 0u && enc->last_step_us != 0u) {
        if ((now_us - enc->last_step_us) < enc->min_step_us) {
            enc->suppressed_events++;
            return 0;
        }
    }
    enc->last_step_us = now_us;
    enc->position += detents;
    return detents;
}

#endif /* OMNIA_ENCODER_H */
