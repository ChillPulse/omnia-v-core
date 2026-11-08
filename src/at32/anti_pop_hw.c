/**
 * @file    anti_pop_hw.c
 * @brief   Управление бесщёлчковым трактом (XSMT, AMP_MUTE, RELAY), селектором 74HC125
 *          и автокоммутатором USB «на лету» (FSUSB30MUX + ключ AO3401 + VBUS_DET с антидребезгом).
 */

#include "omnia_at32.h"

#define VBUS_DEBOUNCE_MS    250U

static omnia_usb_mux_mode_t g_usb_mode         = OMNIA_USB_MODE_AUTO_VBUS;
static bool                 g_vbus_raw_last    = false;
static bool                 g_vbus_debounced   = false;
static uint32_t             g_vbus_change_ts   = 0U;
static uint8_t              g_active_usb_port  = 0U; /* 0 = USB-A (Host флешка), 1 = USB-C (DAC) */

static void apply_usb_mux_hardware(uint8_t target_port)
{
    g_active_usb_port = target_port ? 1U : 0U;

    if (g_active_usb_port == 1U) {
        /* Переход на вход USB-C (ПК / Android / ТВ-бокс):
         * 1) Отключаем раздачу +5V на гнездо флешки USB-A (PB8 = LOW)
         * 2) Переключаем FSUSB30MUX на канал HSD2 (PB11 = HIGH) */
        gpio_bits_reset(GPIOB, GPIO_PINS_8);
        omnia_delay_us(50U);
        gpio_bits_set(GPIOB, GPIO_PINS_11);
    } else {
        /* Возврат на гнездо флешки USB-A:
         * 1) Переключаем FSUSB30MUX на канал HSD1 (PB11 = LOW)
         * 2) Подаём +5V на USB-A через AO3401 + PPTC (PB8 = HIGH) */
        gpio_bits_reset(GPIOB, GPIO_PINS_11);
        omnia_delay_us(50U);
        gpio_bits_set(GPIOB, GPIO_PINS_8);
    }
}

void omnia_hw_gpio_init(void)
{
    gpio_init_type gpio_cfg;

    crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_IOMUX_PERIPH_CLOCK, TRUE);

    /*
     * 1. БЕЗОПАСНОЕ ПРЕДУСТАНОВЛЕННОЕ СОСТОЯНИЕ ДО ВКЛЮЧЕНИЯ ВЫХОДОВ (0 щелчков!):
     *    - PB0  (DAC_XSMT) = LOW  -> PCM5102A жёстко в Soft-Mute
     *    - PB1  (AMP_MUTE) = HIGH -> Усилитель приглушён (или LOW по ключу: закрываем реле ниже)
     *    - PB10 (RELAY_EN) = LOW  -> Реле выхода разомкнуто
     *    - PB6  (OE_ESP_N) = HIGH -> Буфер 74HC125 канал 1 в Hi-Z
     *    - PB7  (OE_BT_N)  = HIGH -> Буфер 74HC125 канал 2 в Hi-Z
     *    - PB8  (VBUS_EN)  = LOW  -> Питание USB-A пока выключено
     *    - PB11 (USB_SEL)  = LOW  -> Канал HSD1 (USB-A)
     */
    gpio_bits_reset(GPIOB, GPIO_PINS_0 | GPIO_PINS_1 | GPIO_PINS_8 | GPIO_PINS_10 | GPIO_PINS_11);
    gpio_bits_set(GPIOB, GPIO_PINS_6 | GPIO_PINS_7);
    gpio_bits_set(GPIOC, GPIO_PINS_13); /* Бортовой LED (активный LOW) выключен */

    gpio_default_para_init(&gpio_cfg);
    gpio_cfg.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_mode           = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_pull           = GPIO_PULL_NONE;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
    gpio_cfg.gpio_pins           = GPIO_PINS_0 | GPIO_PINS_1 | GPIO_PINS_6 | GPIO_PINS_7 |
                                   GPIO_PINS_8 | GPIO_PINS_10 | GPIO_PINS_11;
    gpio_init(GPIOB, &gpio_cfg);

    /* Светодиод индикации WeAct BlackPill на PC13 */
    gpio_cfg.gpio_pins = GPIO_PINS_13;
    gpio_init(GPIOC, &gpio_cfg);

    /* Вход детектора VBUS_DET с гнезда USB-C на PB9 (с внутренней подтяжкой к земле) */
    gpio_cfg.gpio_mode = GPIO_MODE_INPUT;
    gpio_cfg.gpio_pull = GPIO_PULL_DOWN;
    gpio_cfg.gpio_pins = GPIO_PINS_9;
    gpio_init(GPIOB, &gpio_cfg);
}

void omnia_hw_set_dac_xsmt(bool unmute)
{
    if (unmute) {
        gpio_bits_set(GPIOB, GPIO_PINS_0);      /* HIGH = звук открыт */
    } else {
        gpio_bits_reset(GPIOB, GPIO_PINS_0);    /* LOW  = Soft-Mute PCM5102A */
    }
}

void omnia_hw_set_amp_mute(bool mute_active)
{
    if (mute_active) {
        gpio_bits_reset(GPIOB, GPIO_PINS_1);
    } else {
        gpio_bits_set(GPIOB, GPIO_PINS_1);
    }
}

void omnia_hw_set_relay(bool closed)
{
    if (closed) {
        gpio_bits_set(GPIOB, GPIO_PINS_10);
    } else {
        gpio_bits_reset(GPIOB, GPIO_PINS_10);
    }
}

void omnia_hw_select_i2s_mux(omnia_i2s_source_t src)
{
    /* Гарантированный dead-time 3 мкс: сначала оба буфера 74HC125 переводим в Hi-Z */
    gpio_bits_set(GPIOB, GPIO_PINS_6 | GPIO_PINS_7);
    omnia_delay_us(3U);

    if (src == OMNIA_SRC_ESP32_I2S) {
        gpio_bits_reset(GPIOB, GPIO_PINS_6);    /* Открываем вход от ESP32-S3 GPIO5 */
    } else if (src == OMNIA_SRC_EXT_BT_I2S) {
        gpio_bits_reset(GPIOB, GPIO_PINS_7);    /* Открываем резервный I2S вход (BT) */
    }
}

void omnia_hw_set_usb_mux_mode(omnia_usb_mux_mode_t mode)
{
    g_usb_mode = mode;
    if (mode == OMNIA_USB_MODE_FORCE_HOST_A) {
        apply_usb_mux_hardware(0U);
    } else if (mode == OMNIA_USB_MODE_FORCE_DEVICE_C) {
        apply_usb_mux_hardware(1U);
    } else {
        apply_usb_mux_hardware(g_vbus_debounced ? 1U : 0U);
    }
}

void omnia_hw_poll_usb_vbus(uint32_t now_ms, bool *changed_out)
{
    if (changed_out) {
        *changed_out = false;
    }

    const bool raw = (gpio_input_data_bit_read(GPIOB, GPIO_PINS_9) == SET);
    if (raw != g_vbus_raw_last) {
        g_vbus_raw_last  = raw;
        g_vbus_change_ts = now_ms;
        return;
    }

    if (raw != g_vbus_debounced && (now_ms - g_vbus_change_ts) >= VBUS_DEBOUNCE_MS) {
        g_vbus_debounced = raw;
        if (g_usb_mode == OMNIA_USB_MODE_AUTO_VBUS) {
            apply_usb_mux_hardware(raw ? 1U : 0U);
        }
        if (changed_out) {
            *changed_out = true;
        }
    }
}

bool omnia_hw_is_vbus_detected(void)
{
    return g_vbus_debounced;
}

uint8_t omnia_hw_get_active_usb_port(void)
{
    return g_active_usb_port;
}

void omnia_hw_led_set(bool on)
{
    if (on) {
        gpio_bits_reset(GPIOC, GPIO_PINS_13);   /* Активный LOW на плате BlackPill */
    } else {
        gpio_bits_set(GPIOC, GPIO_PINS_13);
    }
}

void omnia_hw_led_toggle(void)
{
    GPIOC->odt ^= GPIO_PINS_13;
}

/**
 * @brief  Бесщёлчковая последовательность старта аналогового выхода (вызывается ПОСЛЕ старта I2S2!):
 *         150 мс тишины на стабильном клоке -> включение реле -> снятие AMP_MUTE -> открытие XSMT.
 */
void omnia_hw_power_sequence_boot(void)
{
    omnia_hw_select_i2s_mux(OMNIA_SRC_ESP32_I2S);

    /* Первичная проверка наличия кабеля USB-C при подаче питания */
    g_vbus_raw_last  = (gpio_input_data_bit_read(GPIOB, GPIO_PINS_9) == SET);
    g_vbus_debounced = g_vbus_raw_last;
    apply_usb_mux_hardware(g_vbus_debounced ? 1U : 0U);

    omnia_delay_ms(150U);
    omnia_hw_set_relay(true);
    omnia_delay_ms(30U);
    omnia_hw_set_amp_mute(false);
    omnia_delay_ms(15U);
    omnia_hw_set_dac_xsmt(true);
}

void omnia_hw_enter_standby(bool standby)
{
    if (standby) {
        omnia_hw_set_dac_xsmt(false);
        omnia_delay_ms(15U);
        omnia_hw_set_amp_mute(true);
        omnia_delay_ms(20U);
        omnia_hw_set_relay(false);
    } else {
        omnia_hw_set_relay(true);
        omnia_delay_ms(30U);
        omnia_hw_set_amp_mute(false);
        omnia_delay_ms(15U);
        omnia_hw_set_dac_xsmt(true);
    }
}
