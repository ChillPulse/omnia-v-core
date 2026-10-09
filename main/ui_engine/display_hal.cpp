/*
 * Дисплей ST7789 (320x240 ландшафт), SPI2_HOST до 60 МГц, двойная буферизация LVGL 9.2
 * в внутренней DMA-памяти (ТЗ, Том 3, Гл. 1). Подсветка — LEDC ШИМ 12 бит, 5 кГц.
 */

#include "omnia.h"
#include "ui_engine.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7789.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char* TAG = "OMNIA_DISPLAY";

#define SCREEN_WIDTH          320
#define SCREEN_HEIGHT         240
#define DISPLAY_BUFFER_LINES  40                                  // 1/6 экрана
#define DISPLAY_BUFFER_PIXELS (SCREEN_WIDTH * DISPLAY_BUFFER_LINES)

static lv_display_t* disp_handle = NULL;
static uint16_t*     display_buf1 = NULL;
static uint16_t*     display_buf2 = NULL;

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t* edata, void* user_ctx)
{
    (void)panel_io; (void)edata;
    lv_display_flush_ready((lv_display_t*)user_ctx);
    return false;
}

static void lvgl_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}

static void lv_tick_cb(void* arg)
{
    (void)arg;
    lv_tick_inc(1);
}

static void backlight_init(void)
{
    ledc_timer_config_t t = {};
    t.speed_mode      = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = LEDC_TIMER_12_BIT;
    t.timer_num       = LEDC_TIMER_0;
    t.freq_hz         = 5000;
    t.clk_cfg         = LEDC_AUTO_CLK;
    ledc_timer_config(&t);

    ledc_channel_config_t c = {};
    c.gpio_num   = PIN_DISPLAY_BL;
    c.speed_mode = LEDC_LOW_SPEED_MODE;
    c.channel    = LEDC_CHANNEL_0;
    c.timer_sel  = LEDC_TIMER_0;
    c.duty       = 0;
    c.hpoint     = 0;
    ledc_channel_config(&c);
}

void display_set_backlight(uint8_t pct)
{
    if (pct > 100) pct = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (uint32_t)pct * 4095 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

bool init_st7789_vcore_graphics(void)
{
    ESP_LOGI(TAG, "SPI2 @60 МГц, инициализация ST7789 через esp_lcd");

    // LVGL сначала — чтобы передать дескриптор дисплея в колбэк SPI
    lv_init();
    disp_handle = lv_display_create(SCREEN_WIDTH, SCREEN_HEIGHT);
    if (!disp_handle) return false;

    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num     = PIN_DISPLAY_SDA;
    buscfg.miso_io_num     = -1;
    buscfg.sclk_io_num     = PIN_DISPLAY_SCL;
    buscfg.quadwp_io_num   = -1;
    buscfg.quadhd_io_num   = -1;
    buscfg.max_transfer_sz = DISPLAY_BUFFER_PIXELS * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.dc_gpio_num       = PIN_DISPLAY_DC;
    io_cfg.cs_gpio_num       = PIN_DISPLAY_CS;
    io_cfg.pclk_hz           = 60 * 1000 * 1000;   // 60 МГц (ТЗ)
    io_cfg.lcd_cmd_bits      = 8;
    io_cfg.lcd_param_bits    = 8;
    io_cfg.spi_mode          = 0;
    io_cfg.trans_queue_depth = 10;
    io_cfg.on_color_trans_done = notify_lvgl_flush_ready;
    io_cfg.user_ctx          = disp_handle;        // задаем ДО создания io
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t pcfg = {};
    pcfg.reset_gpio_num  = PIN_DISPLAY_RES;
    pcfg.rgb_ele_order   = LCD_RGB_ELEMENT_ORDER_BGR;
    pcfg.bits_per_pixel  = 16;
    pcfg.flags.reset_active_high = false;
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &pcfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    // Ориентация и инверсия подбираются под конкретную панель 2.8" IPS
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    // Двойной буфер во внутренней SRAM с DMA (вне PSRAM)
    display_buf1 = (uint16_t*)heap_caps_malloc(DISPLAY_BUFFER_PIXELS * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    display_buf2 = (uint16_t*)heap_caps_malloc(DISPLAY_BUFFER_PIXELS * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!display_buf1 || !display_buf2) {
        ESP_LOGE(TAG, "Нет внутренней DMA-памяти под буферы LVGL");
        return false;
    }

    lv_display_set_user_data(disp_handle, panel);
    lv_display_set_buffers(disp_handle, display_buf1, display_buf2,
                           DISPLAY_BUFFER_PIXELS * sizeof(uint16_t), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp_handle, lvgl_flush_cb);

    // Тик LVGL: 1 мс
    esp_timer_handle_t tick = NULL;
    esp_timer_create_args_t targs = {};
    targs.callback = lv_tick_cb;
    targs.name = "lv_tick";
    ESP_ERROR_CHECK(esp_timer_create(&targs, &tick));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick, 1000));

    backlight_init();
    display_set_backlight(80);
    ESP_LOGI(TAG, "Дисплей готов: %dx%d", SCREEN_WIDTH, SCREEN_HEIGHT);
    return true;
}
