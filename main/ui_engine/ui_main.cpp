/*
 * Экран «Главный плеер» 320x240 (ТЗ, Том 3 — UI контроллера).
 * Все LVGL-вызовы — только из задачи UI (Core 1).
 */

#include "ui_engine.h"
#include "omnia.h"
#include <stdio.h>

static lv_obj_t* s_lb_status;
static lv_obj_t* s_lb_source;
static lv_obj_t* s_lb_track;
static lv_obj_t* s_lb_artist;
static lv_obj_t* s_lb_time;
static lv_obj_t* s_lb_format;
static lv_obj_t* s_spec_box;

void ui_init(void)
{
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B0D10), LV_PART_MAIN);
    lv_obj_set_style_text_color(scr, lv_color_hex(0xE9EDF2), LV_PART_MAIN);

    s_lb_status = lv_label_create(scr);
    lv_obj_align(s_lb_status, LV_ALIGN_TOP_LEFT, 6, 4);

    s_lb_source = lv_label_create(scr);
    lv_obj_align(s_lb_source, LV_ALIGN_TOP_RIGHT, -6, 4);

    s_lb_track = lv_label_create(scr);
    lv_obj_set_width(s_lb_track, 308);
    lv_label_set_long_mode(s_lb_track, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_lb_track, LV_ALIGN_TOP_LEFT, 6, 28);

    s_lb_artist = lv_label_create(scr);
    lv_obj_set_width(s_lb_artist, 308);
    lv_obj_set_style_text_color(s_lb_artist, lv_color_hex(0x8B95A1), LV_PART_MAIN);
    lv_obj_align(s_lb_artist, LV_ALIGN_TOP_LEFT, 6, 52);

    s_spec_box = lv_obj_create(scr);
    lv_obj_remove_style_all(s_spec_box);
    lv_obj_set_size(s_spec_box, 320, 110);
    lv_obj_align(s_spec_box, LV_ALIGN_TOP_LEFT, 0, 80);
    spectra_ui_create(s_spec_box);

    s_lb_format = lv_label_create(scr);
    lv_obj_align(s_lb_format, LV_ALIGN_BOTTOM_LEFT, 6, -22);
    lv_obj_set_style_text_color(s_lb_format, lv_color_hex(0x8B95A1), LV_PART_MAIN);

    s_lb_time = lv_label_create(scr);
    lv_obj_align(s_lb_time, LV_ALIGN_BOTTOM_RIGHT, -6, -22);

    lv_label_set_text(s_lb_status, "Omnia v-core");
    lv_label_set_text(s_lb_source, "SD");
}

void ui_refresh(void)
{
    omnia_state_t s;
    omnia_state_snapshot(&s);
    char buf[128];

    snprintf(buf, sizeof(buf), "%s  %s  Vol %u%%", omnia_play_state_name(s.play_state),
             s.is_muted ? "MUTE" : "", s.volume);
    lv_label_set_text(s_lb_status, buf);

    lv_label_set_text(s_lb_source, s.source);
    lv_label_set_text(s_lb_track, s.track);
    lv_label_set_text(s_lb_artist, s.artist);
    lv_label_set_text(s_lb_format, s.format);

    snprintf(buf, sizeof(buf), "%02u:%02u / %02u:%02u",
             (unsigned)(s.elapsed / 60), (unsigned)(s.elapsed % 60),
             (unsigned)(s.total / 60), (unsigned)(s.total % 60));
    lv_label_set_text(s_lb_time, buf);
}
