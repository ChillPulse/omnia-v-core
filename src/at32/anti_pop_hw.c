/**
 * @file    anti_pop_hw.c
 * @brief   Аппаратная защита от щелчков (Zero-Mux 24-Bit):
 *            PB0  = DAC_XSMT   (LOW = Mute PCM5102A, HIGH = Unmute; внешний 10k к GND)
 *            PB4  = AMP_MUTE   (LOW = Mute УМЗЧ, HIGH = Работа; освобождён через SWJTAG_GMUX_010)
 *            PB8  = RELAY_EN   (LOW = Реле разомкнуто, HIGH = Замкнуто; внешний 10k к GND)
 *            PB9  = VBUS_DET   (Вход детектора +5V USB-DAC через делитель 10k/15k + 100 нФ)
 *            PA2  = BT_CTRL_EN (Управление сбросом/спариванием Bluetooth BTM334 / BT401)
 *            PC13 = STATUS_LED (Бортовой светодиод WeAct BlackPill, активный LOW)
 */

#include "omnia_at32.h"

static bool     s_in_standby  = false;
static bool     s_bt_in_reset = false;
static uint32_t s_bt_rst_end  = 0U;

void omnia_hw_gpio_init(void)
{
    gpio_init_type gpio_cfg;

    crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_IOMUX_PERIPH_CLOCK, TRUE);

    /*
     * КРИТИЧНО: Отключаем 5-проводной JTAG (сохраняя SWD на PA13/PA14)!
     * Мгновенно освобождает PB4 (JNTRST -> AMP_MUTE), а также PA15/PB3 для I2S3 входа Bluetooth.
     */
    gpio_pin_remap_config(SWJTAG_GMUX_010, TRUE);

    /* 1. Безопасное состояние ДО включения драйверов выходов (0 щелчков!):
     *    PB0 (DAC_XSMT)=0, PB4 (AMP_MUTE)=0, PB8 (RELAY_EN)=0 */
    gpio_bits_reset(GPIOB, OMNIA_AT32_DAC_XSMT_PIN |
                           OMNIA_AT32_AMP_MUTE_PIN |
                           OMNIA_AT32_RELAY_EN_PIN);
    gpio_bits_set(GPIOA, OMNIA_AT32_BT_CTRL_PIN); /* PA2 = HIGH (нормальная работа BT) */
    gpio_bits_set(GPIOC, GPIO_PINS_13);           /* PC13 LED = выкл */

    gpio_default_para_init(&gpio_cfg);
    gpio_cfg.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_mode           = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_pull           = GPIO_PULL_DOWN;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_cfg.gpio_pins           = OMNIA_AT32_DAC_XSMT_PIN |
                                   OMNIA_AT32_AMP_MUTE_PIN |
                                   OMNIA_AT32_RELAY_EN_PIN;
    gpio_init(GPIOB, &gpio_cfg);

    /* PA2: выход управления BT */
    gpio_cfg.gpio_pull = GPIO_PULL_UP;
    gpio_cfg.gpio_pins = OMNIA_AT32_BT_CTRL_PIN;
    gpio_init(GPIOA, &gpio_cfg);

    /* PC13: бортовой светодиод WeAct BlackPill */
    gpio_cfg.gpio_pull = GPIO_PULL_NONE;
    gpio_cfg.gpio_pins = GPIO_PINS_13;
    gpio_init(GPIOC, &gpio_cfg);

    /* PB9: вход детектора +5V USB-DAC (с внутренней + внешней подтяжкой к земле) */
    gpio_cfg.gpio_mode = GPIO_MODE_INPUT;
    gpio_cfg.gpio_pull = GPIO_PULL_DOWN;
    gpio_cfg.gpio_pins = OMNIA_AT32_VBUS_DET_PIN;
    gpio_init(GPIOB, &gpio_cfg);

    s_in_standby = true;
}

void omnia_hw_set_dac_xsmt(bool unmute)
{
    if (unmute) {
        gpio_bits_set(GPIOB, OMNIA_AT32_DAC_XSMT_PIN);
    } else {
        gpio_bits_reset(GPIOB, OMNIA_AT32_DAC_XSMT_PIN);
    }
}

void omnia_hw_set_amp_mute(bool mute_active)
{
    if (mute_active) {
        gpio_bits_reset(GPIOB, OMNIA_AT32_AMP_MUTE_PIN);
    } else {
        gpio_bits_set(GPIOB, OMNIA_AT32_AMP_MUTE_PIN);
    }
}

void omnia_hw_set_relay(bool closed)
{
    if (closed) {
        gpio_bits_set(GPIOB, OMNIA_AT32_RELAY_EN_PIN);
    } else {
        gpio_bits_reset(GPIOB, OMNIA_AT32_RELAY_EN_PIN);
    }
}

bool omnia_hw_vbus_pin_raw(void)
{
    return (gpio_input_data_bit_read(GPIOB, OMNIA_AT32_VBUS_DET_PIN) == SET);
}

void omnia_hw_bt_pulse_reset(uint32_t low_ms)
{
    gpio_bits_reset(GPIOA, OMNIA_AT32_BT_CTRL_PIN);
    s_bt_rst_end  = omnia_millis() + ((low_ms > 0U) ? low_ms : 150U);
    s_bt_in_reset = true;
}

void omnia_hw_poll(uint32_t now_ms)
{
    if (s_bt_in_reset && (int32_t)(now_ms - s_bt_rst_end) >= 0) {
        gpio_bits_set(GPIOA, OMNIA_AT32_BT_CTRL_PIN);
        s_bt_in_reset = false;
    }
}

void omnia_hw_led_set(bool on)
{
    if (on) {
        gpio_bits_reset(GPIOC, GPIO_PINS_13);
    } else {
        gpio_bits_set(GPIOC, GPIO_PINS_13);
    }
}

void omnia_hw_led_toggle(void)
{
    GPIOC->odt ^= GPIO_PINS_13;
}

void omnia_hw_power_sequence_boot(void)
{
    /* 150 мс тишины на стабильном клоке I2S2 -> снятие XSMT -> замыкание реле и открытие УМЗЧ */
    omnia_delay_ms(150U);
    omnia_hw_set_dac_xsmt(true);
    omnia_delay_ms(35U);
    omnia_hw_set_relay(true);
    omnia_delay_ms(15U);
    omnia_hw_set_amp_mute(false);
    s_in_standby = false;
}

void omnia_hw_enter_standby(bool standby)
{
    if (standby) {
        omnia_hw_set_dac_xsmt(false);
        omnia_hw_set_amp_mute(true);
        omnia_delay_ms(25U);
        omnia_hw_set_relay(false);
        s_in_standby = true;
    } else if (s_in_standby) {
        omnia_hw_set_dac_xsmt(true);
        omnia_delay_ms(35U);
        omnia_hw_set_relay(true);
        omnia_delay_ms(15U);
        omnia_hw_set_amp_mute(false);
        s_in_standby = false;
    }
}

bool omnia_hw_is_standby(void)
{
    return s_in_standby;
}
