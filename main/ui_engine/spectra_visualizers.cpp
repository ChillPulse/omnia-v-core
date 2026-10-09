/*
 * Баллистический спектроанализатор на LVGL (ТЗ, Том 3, Гл. 3).
 * 64 полосы, мгновенный взлет, плавное падение, пиковая отметка с задержкой.
 * Отрисовка — набором lv_obj прямоугольников (без canvas), чтобы не тратить кадровый буфер.
 */

#include "ui_engine.h"
#include "omnia.h"
#include <math.h>

#define BANDS_COUNT   SPECTRUM_BANDS
#define SPEC_W        320
#define SPEC_H        110
#define BAR_STEP      5     // 64 * 5 = 320 px
#define BAR_W         4

static lv_obj_t* s_bars[BANDS_COUNT];
static lv_obj_t* s_peaks[BANDS_COUNT];
static uint8_t   peak_hold_buffer[BANDS_COUNT];
static uint8_t   current_bars_levels[BANDS_COUNT];

void spectra_ui_create(lv_obj_t* parent)
{
    for (int i = 0; i < BANDS_COUNT; i++) {
        lv_obj_t* bar = lv_obj_create(parent);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, BAR_W, 1);
        lv_obj_set_pos(bar, i * BAR_STEP, SPEC_H - 1);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x3DDC97), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        s_bars[i] = bar;

        lv_obj_t* peak = lv_obj_create(parent);
        lv_obj_remove_style_all(peak);
        lv_obj_set_size(peak, BAR_W, 2);
        lv_obj_set_pos(peak, i * BAR_STEP, SPEC_H - 2);
        lv_obj_set_style_bg_color(peak, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(peak, LV_OPA_COVER, LV_PART_MAIN);
        s_peaks[i] = peak;
    }
}

// Баллистика: взлет почти мгновенный, падение плавное (аналоговый стиль)
static void apply_ballistic_smoothing(const float* raw)
{
    const float alpha_rise = 0.85f;
    const float alpha_fall = 0.12f;

    for (int i = 0; i < BANDS_COUNT; i++) {
        float v = raw[i];
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        uint8_t target = (uint8_t)(v * 255.0f);

        if (target > current_bars_levels[i]) {
            current_bars_levels[i] = (uint8_t)(target * alpha_rise + current_bars_levels[i] * (1.0f - alpha_rise));
        } else {
            current_bars_levels[i] = (uint8_t)(target * alpha_fall + current_bars_levels[i] * (1.0f - alpha_fall));
        }

        if (current_bars_levels[i] > peak_hold_buffer[i]) {
            peak_hold_buffer[i] = current_bars_levels[i];
        } else if (peak_hold_buffer[i] > 0) {
            peak_hold_buffer[i]--;   // пик падает медленно
        }
    }
}

void spectra_ui_update(const float* bands)
{
    apply_ballistic_smoothing(bands);

    for (int i = 0; i < BANDS_COUNT; i++) {
        int h = (int)current_bars_levels[i] * SPEC_H / 255;
        if (h < 1) h = 1;
        lv_obj_set_size(s_bars[i], BAR_W, h);
        lv_obj_set_y(s_bars[i], SPEC_H - h);

        int py = SPEC_H - (int)peak_hold_buffer[i] * SPEC_H / 255 - 2;
        if (py < 0) py = 0;
        lv_obj_set_y(s_peaks[i], py);
    }
}
