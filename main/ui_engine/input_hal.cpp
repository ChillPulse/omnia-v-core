/*
 * Ввод: энкодер (PCNT, 4x квадратура), кнопка энкодера, АЦП-клавиатура из 7 кнопок.
 * ТЗ, Том 1, Гл. 3–4: все кнопки на GPIO 1 через делитель, R_pull = 1 кОм к 3.3 В.
 *
 * Расчетные уровни (мВ) при R_pull = 1 кОм, U = 3.3 В:
 *   POWER 0 (0 Ом), MUTE 1650 (1 кОм), SOURCE 2269 (2.2 кОм), FAVORITE 2532 (3.3 кОм),
 *   PREV 2721 (4.7 кОм), NEXT 2877 (6.8 кОм), PLAY 3000 (10 кОм), idle 3300.
 * Верхние уровни отстоят друг от друга на ~120–150 мВ — проверяйте на железе.
 */

#include "omnia.h"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include <stdlib.h>

static const char* TAG = "OMNIA_INPUT";

typedef struct {
    int mv;
    omnia_key_t key;
} kbd_level_t;

static const kbd_level_t KBD_LEVELS[] = {
    {0,    OMNIA_KEY_POWER},
    {1650, OMNIA_KEY_MUTE},
    {2269, OMNIA_KEY_SOURCE},
    {2532, OMNIA_KEY_FAVORITE},
    {2721, OMNIA_KEY_PREV},
    {2877, OMNIA_KEY_NEXT},
    {3000, OMNIA_KEY_PLAY},
};
#define KBD_TOLERANCE_MV 50
#define KBD_IDLE_MV      3200

static adc_oneshot_unit_handle_t s_adc  = NULL;
static adc_cali_handle_t         s_cali = NULL;
static pcnt_unit_handle_t        s_pcnt = NULL;
static int s_pcnt_last = 0;
static int s_enc_acc   = 0;
static omnia_key_t s_kbd_last = OMNIA_KEY_NONE;
static int s_kbd_count = 0;
static int s_btn_low_cnt = 0;

void input_init(void)
{
    // GPIO энкодера и кнопки — входы с подтяжкой
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << PIN_ENCODER_A) | (1ULL << PIN_ENCODER_B) | (1ULL << PIN_ENCODER_SW);
    io.mode         = GPIO_MODE_INPUT;
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type    = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io));

    // АЦП-клавиатура
    adc_oneshot_unit_init_cfg_t ucfg = {};
    ucfg.unit_id  = ADC_UNIT_1;
    ucfg.ulp_mode = ADC_ULP_MODE_DISABLE;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&ucfg, &s_adc));

    adc_oneshot_chan_cfg_t ccfg = {};
    ccfg.atten    = ADC_ATTEN_DB_12;
    ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, ADC_CHANNEL_0, &ccfg));   // GPIO1 = ADC1_CH0

    adc_cali_line_fitting_config_t cal = {};
    cal.unit_id  = ADC_UNIT_1;
    cal.atten    = ADC_ATTEN_DB_12;
    cal.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_line_fitting(&cal, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "Калибровка ADC недоступна: используется линейная оценка");
        s_cali = NULL;
    }

    // Энкодер: PCNT в режиме 4x квадратуры (оба канала, оба фронта)
    pcnt_unit_config_t ucfg2 = {};
    ucfg2.low_limit  = -32000;
    ucfg2.high_limit =  32000;
    ESP_ERROR_CHECK(pcnt_new_unit(&ucfg2, &s_pcnt));

    pcnt_glitch_filter_config_t fcfg = {};
    fcfg.max_glitch_ns = 1000;
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_pcnt, &fcfg));

    pcnt_chan_config_t ca = {};
    ca.edge_gpio_num  = PIN_ENCODER_A;
    ca.level_gpio_num = PIN_ENCODER_B;
    pcnt_channel_handle_t cha = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt, &ca, &cha));

    pcnt_chan_config_t cb = {};
    cb.edge_gpio_num  = PIN_ENCODER_B;
    cb.level_gpio_num = PIN_ENCODER_A;
    pcnt_channel_handle_t chb = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt, &cb, &chb));

    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(cha, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(cha, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chb, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chb, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    ESP_ERROR_CHECK(pcnt_unit_enable(s_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(s_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_start(s_pcnt));

    ESP_LOGI(TAG, "Ввод готов: энкодер PCNT, ADC-клавиатура GPIO%d", PIN_ADC_KEYBOARD);
}

void input_poll(void)
{
    // 1. Энкодер: 4 отсчета на щелчок
    int cnt = 0;
    if (pcnt_unit_get_count(s_pcnt, &cnt) == ESP_OK) {
        int d = cnt - s_pcnt_last;
        s_pcnt_last = cnt;
        if (cnt > 20000 || cnt < -20000) {
            pcnt_unit_clear_count(s_pcnt);
            s_pcnt_last = 0;
        }
        s_enc_acc += d;
        while (s_enc_acc >= 4) {
            omnia_dispatch_key(OMNIA_KEY_ENC_CW);
            s_enc_acc -= 4;
        }
        while (s_enc_acc <= -4) {
            omnia_dispatch_key(OMNIA_KEY_ENC_CCW);
            s_enc_acc += 4;
        }
    }

    // 2. Кнопка энкодера (активный LOW), срабатывание после 3 подряд низких отсчетов
    if (gpio_get_level((gpio_num_t)PIN_ENCODER_SW) == 0) {
        if (s_btn_low_cnt < 1000) s_btn_low_cnt++;
        if (s_btn_low_cnt == 3) omnia_dispatch_key(OMNIA_KEY_PLAY);
    } else {
        s_btn_low_cnt = 0;
    }

    // 3. АЦП-клавиатура
    int raw = 0;
    int mv = 0;
    if (adc_oneshot_read(s_adc, ADC_CHANNEL_0, &raw) != ESP_OK) return;
    if (s_cali) {
        if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) mv = raw * 3100 / 4095;
    } else {
        mv = raw * 3100 / 4095;
    }

    omnia_key_t k = OMNIA_KEY_NONE;
    if (mv < KBD_IDLE_MV) {
        for (size_t i = 0; i < sizeof(KBD_LEVELS) / sizeof(KBD_LEVELS[0]); i++) {
            if (abs(mv - KBD_LEVELS[i].mv) <= KBD_TOLERANCE_MV) {
                k = KBD_LEVELS[i].key;
                break;
            }
        }
    }

    // Дребезг: кнопка засчитывается после 2 подряд одинаковых отсчетов (~20 мс)
    if (k == s_kbd_last) {
        if (s_kbd_count < 1000) s_kbd_count++;
    } else {
        s_kbd_last = k;
        s_kbd_count = 1;
    }
    if (k != OMNIA_KEY_NONE && s_kbd_count == 2) {
        omnia_dispatch_key(k);
    }
}
