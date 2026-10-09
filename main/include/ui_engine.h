#ifndef UI_ENGINE_H
#define UI_ENGINE_H

#include "lvgl.h"

// Экран «Главный плеер» (320x240) и спектр (ТЗ, Том 3, Гл. 3)
void ui_init(void);
void ui_refresh(void);
void spectra_ui_create(lv_obj_t* parent);
void spectra_ui_update(const float* bands);

#endif // UI_ENGINE_H
