/**
 * @file    test_encoder.c
 * @brief   Хост-тесты анти-дрожания/анти-прыжка квадратурного энкодера (shared/omnia_encoder.h).
 *          Моделирует: валидное вращение CW/CCW, дребезг контактов, двухбитный скачок
 *          бракованного энкодера, осцилляцию на границе детента, лимит скорости.
 *          Опрос считаем 10 раз в миллисекунду (TICK_US=100); интегратор дребезга —
 *          3 одинаковых опроса, поэтому каждое состояние держим ≥3 опроса.
 */
#include "omnia_encoder.h"
#include <assert.h>
#include <stdio.h>

#define TICK_US 100u

/* Полный оборот одной фазы: 00→10→11→01→00 (CW в нашей таблице) и обратно */
static const uint8_t cw_cycle[4][2] = { {1,0}, {1,1}, {0,1}, {0,0} };
static const uint8_t ccw_cycle[4][2] = { {0,1}, {1,1}, {1,0}, {0,0} };

static int rotated(omnia_encoder_t *enc, const uint8_t cycle[][2], unsigned cycles, uint64_t t0, unsigned ticks_per_state)
{
    int total = 0;
    uint64_t now = t0;
    for (unsigned c = 0; c < cycles; ++c) {
        for (unsigned i = 0; i < 4; ++i) {
            for (unsigned t = 0; t < ticks_per_state; ++t) {
                total += omnia_encoder_poll(enc, cycle[i][0], cycle[i][1], now);
                now += TICK_US;
            }
        }
    }
    return total;
}

static void test_cw_ccw_counts(void)
{
    omnia_encoder_t enc;
    omnia_encoder_init(&enc, 4, 0);
    int d_cw = rotated(&enc, cw_cycle, 3, 100000u, 4);
    assert(d_cw == 3);
    assert(enc.position == 3);
    int d_ccw = rotated(&enc, ccw_cycle, 2, 400000u, 4);
    assert(d_ccw == -2);
    assert(enc.position == 1);
    assert(enc.illegal_events == 0u);
    assert(enc.suppressed_events == 0u);
    printf("[OK] CW/CCW: позиции считаются верно (cw=%+d, ccw=%+d, pos=%+d)\n", d_cw, d_ccw, enc.position);
}

static void test_bounce_rejected(void)
{
    omnia_encoder_t enc;
    omnia_encoder_init(&enc, 4, 0);
    /* Один полный оборот с дребезгом: каждое «новое» сырьё дёргается туда-сюда
     * быстрее интегратора (3 одинаковых подряд не набирается в отскоках) */
    static const uint8_t bounce_seq[][2] = {
        {1,0},{0,0},{1,0},{0,0},{1,0},{1,0},{1,0},{1,0}, /* переход 00→10 с отскоками A */
        {1,1},{1,0},{1,1},{1,1},{1,1},                   /* переход 10→11 с отскоками B */
        {0,1},{1,1},{0,1},{0,1},{0,1},                   /* переход 11→01 с отскоками A */
        {0,0},{0,1},{0,0},{0,0},{0,0},                   /* переход 01→00 с отскоками B */
        {0,0},{0,0},{0,0},                               /* стабилизация */
    };
    uint64_t now = 500000u;
    int total = 0;
    for (size_t i = 0; i < sizeof(bounce_seq)/sizeof(bounce_seq[0]); ++i) {
        total += omnia_encoder_poll(&enc, bounce_seq[i][0], bounce_seq[i][1], now);
        now += TICK_US;
    }
    printf("[BOUNCE] детентов за оборот с дребезгом: %+d, illegal=%u\n", total, enc.illegal_events);
    assert(total == 1);                 /* ровно один детент, без ложных */
    assert(enc.illegal_events == 0u);   /* отскоки — одиночные биты, не двухбитные */
    printf("[OK] Дребезг контактов: ложных шагов нет\n");
}

static void test_two_bit_jump_ignored(void)
{
    omnia_encoder_t enc;
    omnia_encoder_init(&enc, 4, 0);
    uint64_t now = 900000u;
    for (int t = 0; t < 5; ++t) { omnia_encoder_poll(&enc, 0, 0, now); now += TICK_US; }
    /* брак энкодера: оба канала переключились разом (00→11) в одном опросе */
    for (int t = 0; t < 8; ++t) { omnia_encoder_poll(&enc, 1, 1, now); now += TICK_US; }
    printf("[JUMP] illegal=%u, pos=%+d (ожидание: illegal>=1, pos==0)\n", enc.illegal_events, enc.position);
    assert(enc.illegal_events >= 1u);
    assert(enc.position == 0);
    /* после ресинхронизации вращение считается нормально */
    int d = 0;
    static const uint8_t after[][2] = { {1,0},{1,0},{1,0},{0,0},{0,0},{0,0},{0,1},{0,1},{0,1}, {1,1},{1,1},{1,1}, {1,0},{1,0},{1,0} };
    for (size_t i = 0; i < sizeof(after)/sizeof(after[0]); ++i) {
        d += omnia_encoder_poll(&enc, after[i][0], after[i][1], now);
        now += TICK_US;
    }
    printf("[JUMP] вращение после ребра: %+d детентов, pos=%+d\n", d, enc.position);
    assert(enc.position == d);   /* ребро не «спрятало» позицию и не создало её */
    printf("[OK] Двухбитный скачок бракованного энкодера: шага нет, ресинхронизация работает\n");
}

static void test_speed_limit(void)
{
    omnia_encoder_t enc;
    omnia_encoder_init(&enc, 4, 0);
    enc.min_step_us = 5000u;   /* жёсткий лимит: не чаще детента в 5 мс */
    uint64_t now = 2000000u;
    int d1 = rotated(&enc, cw_cycle, 1, now, 3);   /* ~1.2 мс на оборот */
    int d2 = rotated(&enc, cw_cycle, 1, now + 300u, 3);
    printf("[SPEED] первый=%+d, второй (через 300 мкс)=%+d, suppressed=%u\n", d1, d2, enc.suppressed_events);
    assert(d1 == 1);
    assert(d2 == 0);                    /* отброшен лимитом */
    assert(enc.suppressed_events >= 1u);
    printf("[OK] Лимит скорости: пачки быстрее физически возможного отбрасываются\n");
}

static void test_reversal_no_detent(void)
{
    omnia_encoder_t enc;
    omnia_encoder_init(&enc, 4, 0);
    uint64_t now = 3000000u;
    /* осцилляция на границе детента с удержанием ≥3 опроса: +1,-1,+1,-1 фазовых шага */
    static const uint8_t wiggle[][2] = {
        {1,0},{1,0},{1,0},   /* +1 */
        {0,0},{0,0},{0,0},   /* -1 */
        {1,0},{1,0},{1,0},   /* +1 */
        {0,0},{0,0},{0,0},   /* -1 */
        {1,0},{1,0},{1,0},   /* +1 */
        {0,0},{0,0},{0,0},   /* -1 */
    };
    int total = 0;
    for (size_t i = 0; i < sizeof(wiggle)/sizeof(wiggle[0]); ++i) {
        total += omnia_encoder_poll(&enc, wiggle[i][0], wiggle[i][1], now);
        now += TICK_US;
    }
    printf("[REVERSE] осцилляция: детентов=%+d, pos=%+d\n", total, enc.position);
    assert(total == 0);
    assert(enc.position == 0);
    printf("[OK] Реверс на границе детента: ложных шагов нет\n");
}

int main(void)
{
    printf("=== OMNIA encoder anti-skip tests ===\n");
    test_cw_ccw_counts();
    test_bounce_rejected();
    test_two_bit_jump_ignored();
    test_speed_limit();
    test_reversal_no_detent();
    printf("=== ВСЕ ТЕСТЫ ЭНКОДЕРА ПРОЙДЕНЫ ===\n");
    return 0;
}
