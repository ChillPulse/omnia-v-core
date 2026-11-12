/**
 * @file    anti_pop_hw.c
 * @brief   Студийный бесщёлчковый секвенсор питания УМЗЧ и линейного выхода ЦАПа:
 *            PB0  = DAC_XSMT       (LOW = Mute PCM5102A, HIGH = Работа)
 *            PB2  = AMP_PWR_EN     (Отдельное реле включения питания БП УМЗЧ K2 / SSR;
 *                                   полярность и задержки настраиваются в Сервисном меню)
 *            PB4  = AMP_MUTE_OUT   (Отдельный 5V-толерантный пин для входов MUTE / CMUTE / SDZ / RESET
 *                                   плат TPA3116D2 и TPA3221 — 3 режима в Сервисном меню:
 *                                   0: Active-LOW Push-Pull, 1: Active-LOW Open-Drain, 2: Active-HIGH Push-Pull)
 *            PB8  = AUDIO_RELAY_EN (Сигнальное реле Anti-Pop K1 линейного выхода ЦАПа)
 *            PB9  = USB_VBUS_DET   (Вход делителя +5V от ТВ-бокса / ПК для триггера USB-DAC)
 *            PA2  = BT_CTRL_EN     (Сброс / спаривание модуля Bluetooth SJR-BTM334 / BT401)
 *            PC13 = STATUS_LED     (Бортовой индикатор WeAct BlackPill, активный LOW)
 */

#include "omnia_at32.h"
#include <string.h>

static bool     g_in_standby        = false;
/* Доступ к K2 блокируется до получения конфигурации от ESP32. */
static bool     g_amp_cfg_ready      = false;
static bool     g_amp_fault_latched  = false;
static bool     g_amp_pwr_is_on     = false;
static bool     g_amp_mute_asserted = true;
static uint32_t g_bt_rst_end_ms     = 0U;

/* Настройки Сервисного меню по умолчанию (безопасны для TPA3221 RESET/SDZ и стандартного NPN-ключа реле K2) */
static omnia_amp_ctrl_cfg_t g_amp_cfg = {
    .mute_pin_mode        = (uint8_t)OMNIA_AMP_MUTE_ACTIVE_LOW_PP,
    .amp_pwr_active_high  = 1U,     /* HIGH на PB2 открывает NPN-ключ VT2 / SSR реле питания УМЗЧ */
    .amp_pwr_auto_standby = 1U,     /* Автоматически отключать питание УМЗЧ в режиме Standby       */
    .reserved             = 0U,
    .warmup_delay_ms      = 500U,   /* 500 мс заряд конденсаторов БП УМЗЧ при зажатом Mute PB4     */
    .mute_to_pwr_off_ms   = 80U     /* 80 мс удержания Mute до снятия силового питания УМЗЧ        */
};

/* Настраивает тип драйвера пина PB4 (Push-Pull или Open-Drain под TPA3221 CMUTE) и сразу выставляет уровень */
static void apply_pb4_hardware_state(bool mute_active)
{
    gpio_init_type gpio_cfg;
    gpio_default_para_init(&gpio_cfg);
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_cfg.gpio_mode           = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_pull           = GPIO_PULL_NONE;
    gpio_cfg.gpio_pins           = OMNIA_AT32_AMP_MUTE_PIN;

    const omnia_amp_mute_mode_t mode = (omnia_amp_mute_mode_t)g_amp_cfg.mute_pin_mode;

    if (mode == OMNIA_AMP_MUTE_ACTIVE_LOW_OD) {
        /* Рекомендация TI для TPA3221 CMUTE: Mute = 0V, Play = чистый Hi-Z (Open-Drain) */
        gpio_cfg.gpio_out_type = GPIO_OUTPUT_OPEN_DRAIN;
        if (mute_active) {
            gpio_bits_reset(OMNIA_AT32_AMP_MUTE_PORT, OMNIA_AT32_AMP_MUTE_PIN);
        } else {
            gpio_bits_set(OMNIA_AT32_AMP_MUTE_PORT, OMNIA_AT32_AMP_MUTE_PIN);
        }
    } else if (mode == OMNIA_AMP_MUTE_ACTIVE_HIGH_PP) {
        /* Для прямого пина 12 MUTE микросхемы TPA3116D2: Mute = HIGH (3.3V > 2.0V), Play = LOW (0V) */
        gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
        if (mute_active) {
            gpio_bits_set(OMNIA_AT32_AMP_MUTE_PORT, OMNIA_AT32_AMP_MUTE_PIN);
        } else {
            gpio_bits_reset(OMNIA_AT32_AMP_MUTE_PORT, OMNIA_AT32_AMP_MUTE_PIN);
        }
    } else {
        /* Стандартный режим Active-LOW Push-Pull: Mute = LOW (0V), Play = HIGH (3.3V) */
        gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
        if (mute_active) {
            gpio_bits_reset(OMNIA_AT32_AMP_MUTE_PORT, OMNIA_AT32_AMP_MUTE_PIN);
        } else {
            gpio_bits_set(OMNIA_AT32_AMP_MUTE_PORT, OMNIA_AT32_AMP_MUTE_PIN);
        }
    }

    gpio_init(OMNIA_AT32_AMP_MUTE_PORT, &gpio_cfg);
}

static void apply_pb2_amp_pwr_level(bool power_on)
{
    const bool drive_high = (g_amp_cfg.amp_pwr_active_high != 0U) ? power_on : (!power_on);
    if (drive_high) {
        gpio_bits_set(OMNIA_AT32_AMP_PWR_PORT, OMNIA_AT32_AMP_PWR_PIN);
    } else {
        gpio_bits_reset(OMNIA_AT32_AMP_PWR_PORT, OMNIA_AT32_AMP_PWR_PIN);
    }
    g_amp_pwr_is_on = power_on;
}

void omnia_hw_gpio_init(void)
{
    gpio_init_type gpio_cfg;

    crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_IOMUX_PERIPH_CLOCK, TRUE);

    /* Отключаем устаревший JTAG, полностью сохраняя двухпроводную отладку SWD (PA13 / PA14):
     * это сразу освобождает PB4 (AMP_MUTE_OUT), а также PA15/PB3 для I2S3 входа Bluetooth. */
    gpio_pin_remap_config(SWJTAG_GMUX_010, TRUE);

    /* 1. До переключения выводов на выход выставляем гарантированно тихие уровни:
     *    PB0 (DAC_XSMT)=LOW, PB8 (AUDIO_RELAY)=LOW, PB2 (AMP_PWR)=ВЫКЛ, PB4 (AMP_MUTE)=MUTE */
    gpio_bits_reset(GPIOB, OMNIA_AT32_DAC_XSMT_PIN | OMNIA_AT32_RELAY_PIN);
    apply_pb2_amp_pwr_level(false);

    gpio_default_para_init(&gpio_cfg);
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_cfg.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_mode           = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_pull           = GPIO_PULL_NONE;
    gpio_cfg.gpio_pins           = OMNIA_AT32_DAC_XSMT_PIN |
                                   OMNIA_AT32_AMP_PWR_PIN  |
                                   OMNIA_AT32_RELAY_PIN;
    gpio_init(GPIOB, &gpio_cfg);

    g_amp_mute_asserted = true;
    apply_pb4_hardware_state(true);

    /* 2. PA2 (BT_CTRL_EN) — по умолчанию HIGH (работа модуля Bluetooth разрешена) */
    gpio_bits_set(OMNIA_AT32_BT_CTRL_PORT, OMNIA_AT32_BT_CTRL_PIN);
    gpio_cfg.gpio_pins = OMNIA_AT32_BT_CTRL_PIN;
    gpio_init(OMNIA_AT32_BT_CTRL_PORT, &gpio_cfg);

    /* 3. PB9 (USB_VBUS_DET) — вход с внутренней подтяжкой к земле */
    gpio_cfg.gpio_mode = GPIO_MODE_INPUT;
    gpio_cfg.gpio_pull = GPIO_PULL_DOWN;
    gpio_cfg.gpio_pins = OMNIA_AT32_VBUS_DET_PIN;
    gpio_init(OMNIA_AT32_VBUS_DET_PORT, &gpio_cfg);

    /* 4. PC13 — бортовой светодиод WeAct BlackPill (HIGH = погашен) */
    gpio_bits_set(OMNIA_AT32_LED_PORT, OMNIA_AT32_LED_PIN);
    gpio_cfg.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_pull = GPIO_PULL_NONE;
    gpio_cfg.gpio_pins = OMNIA_AT32_LED_PIN;
    gpio_init(OMNIA_AT32_LED_PORT, &gpio_cfg);
}

void omnia_hw_set_dac_xsmt(bool unmute)
{
    if (unmute) {
        gpio_bits_set(OMNIA_AT32_DAC_XSMT_PORT, OMNIA_AT32_DAC_XSMT_PIN);
    } else {
        gpio_bits_reset(OMNIA_AT32_DAC_XSMT_PORT, OMNIA_AT32_DAC_XSMT_PIN);
    }
}

void omnia_hw_set_amp_pwr(bool power_on)
{
    apply_pb2_amp_pwr_level(power_on);
}

bool omnia_hw_amp_cfg_ready(void)
{
    return g_amp_cfg_ready && !g_amp_fault_latched;
}

void omnia_hw_latch_fault(void)
{
    g_amp_fault_latched = true;
    omnia_hw_set_amp_mute(true);
    omnia_hw_set_dac_xsmt(false);
    omnia_hw_set_relay(false);
    omnia_hw_set_amp_pwr(false);
    g_in_standby = true;
}

bool omnia_hw_is_amp_pwr_on(void)
{
    return g_amp_pwr_is_on;
}

void omnia_hw_set_amp_mute(bool mute_active)
{
    g_amp_mute_asserted = mute_active;
    apply_pb4_hardware_state(mute_active);
}

void omnia_hw_set_relay(bool closed)
{
    if (closed) {
        gpio_bits_set(OMNIA_AT32_RELAY_PORT, OMNIA_AT32_RELAY_PIN);
    } else {
        gpio_bits_reset(OMNIA_AT32_RELAY_PORT, OMNIA_AT32_RELAY_PIN);
    }
}

void omnia_hw_apply_amp_ctrl_cfg(const omnia_amp_ctrl_cfg_t *cfg)
{
    if (!cfg) {
        return;
    }
    omnia_amp_ctrl_cfg_t sanitized = *cfg;
    if (sanitized.mute_pin_mode > (uint8_t)OMNIA_AMP_MUTE_ACTIVE_HIGH_PP) {
        sanitized.mute_pin_mode = (uint8_t)OMNIA_AMP_MUTE_ACTIVE_LOW_PP;
    }
    if (sanitized.warmup_delay_ms < 50U) {
        sanitized.warmup_delay_ms = 50U;
    } else if (sanitized.warmup_delay_ms > 5000U) {
        sanitized.warmup_delay_ms = 5000U;
    }
    if (sanitized.mute_to_pwr_off_ms < 20U) {
        sanitized.mute_to_pwr_off_ms = 20U;
    } else if (sanitized.mute_to_pwr_off_ms > 2000U) {
        sanitized.mute_to_pwr_off_ms = 2000U;
    }

    if (g_amp_pwr_is_on || sanitized.amp_pwr_active_high != 1U) return;
    g_amp_cfg = sanitized;
    g_amp_cfg_ready = true;

    /* Немедленно переприменяем актуальную полярность и тип выхода к пинам PB4 и PB2 */
    apply_pb4_hardware_state(g_amp_mute_asserted);
    apply_pb2_amp_pwr_level(g_amp_pwr_is_on);
}

void omnia_hw_get_amp_ctrl_cfg(omnia_amp_ctrl_cfg_t *out_cfg)
{
    if (out_cfg) {
        *out_cfg = g_amp_cfg;
    }
}

bool omnia_hw_vbus_pin_raw(void)
{
    return (gpio_input_data_bit_read(OMNIA_AT32_VBUS_DET_PORT, OMNIA_AT32_VBUS_DET_PIN) == SET);
}

void omnia_hw_bt_pulse_reset(uint32_t low_ms)
{
    gpio_bits_reset(OMNIA_AT32_BT_CTRL_PORT, OMNIA_AT32_BT_CTRL_PIN);
    g_bt_rst_end_ms = omnia_millis() + ((low_ms > 0U) ? low_ms : 120U);
}

void omnia_hw_poll(uint32_t now_ms)
{
    if (g_bt_rst_end_ms != 0U && (int32_t)(now_ms - g_bt_rst_end_ms) >= 0) {
        g_bt_rst_end_ms = 0U;
        gpio_bits_set(OMNIA_AT32_BT_CTRL_PORT, OMNIA_AT32_BT_CTRL_PIN);
    }
}

void omnia_hw_led_set(bool on)
{
    if (on) {
        gpio_bits_reset(OMNIA_AT32_LED_PORT, OMNIA_AT32_LED_PIN);
    } else {
        gpio_bits_set(OMNIA_AT32_LED_PORT, OMNIA_AT32_LED_PIN);
    }
}

void omnia_hw_led_toggle(void)
{
    OMNIA_AT32_LED_PORT->odt ^= OMNIA_AT32_LED_PIN;
}

/**
 * @brief  Эталонная бесщёлчковая последовательность старта ЦАПа и УМЗЧ (TPA3116 / TPA3221):
 *         1. Удерживаем Mute на ЦАПе (PB0=0) и на УМЗЧ (PB4=Mute), сигнальное реле PB8 разомкнуто.
 *         2. Включаем силовое реле питания УМЗЧ (PB2) -> ждём прогрева и заряда банок БП УМЗЧ (warmup_delay_ms).
 *         3. Снимаем Mute с ЦАПа PCM5102A (PB0=1) на цифровой тишине -> пауза 40 мс.
 *         4. Замыкаем сигнальное реле линейного выхода K1 (PB8=1) -> пауза 30 мс.
 *         5. Плавно отпускаем пин MUTE усилителя TPA3116/TPA3221 (PB4 -> рабочий уровень)!
 */
void omnia_hw_power_sequence_boot(void)
{
    if (!g_amp_cfg_ready || g_amp_fault_latched) return;
    g_in_standby = false;
    omnia_hw_set_amp_mute(true);
    omnia_hw_set_dac_xsmt(false);
    omnia_hw_set_relay(false);

    /* Шаг 1: подаём питание на УМЗЧ через реле PB2 при железно зажатом входе Mute PB4 */
    omnia_hw_set_amp_pwr(true);
    omnia_delay_ms(g_amp_cfg.warmup_delay_ms);

    /* Шаг 2: открываем внутренний ЦАП PCM5102A на нулевом потоке I2S2 */
    omnia_hw_set_dac_xsmt(true);
    omnia_delay_ms(40U);

    /* Шаг 3: замыкаем сигнальное реле линейного выхода K1 (PB8) */
    omnia_hw_set_relay(true);
    omnia_delay_ms(30U);

    /* Шаг 4: выводим TPA3116 / TPA3221 из Mute в рабочий режим */
    omnia_hw_set_amp_mute(false);
}

/**
 * @brief  Бесщёлчковый уход в Standby и пробуждение:
 *         - Уход в Standby: сначала мгновенный Mute на TPA3116/3221 (PB4) и ЦАПе (PB0) ->
 *           размыкание сигнального реле K1 (PB8) -> пауза закрытия ключей Class-D ->
 *           отключение силового реле БП УМЗЧ K2 (PB2).
 */
void omnia_hw_enter_standby(bool standby)
{
    if (standby) {
        if (g_in_standby) {
            return;
        }
        g_in_standby = true;
        omnia_hw_set_amp_mute(true);
        omnia_hw_set_dac_xsmt(false);
        omnia_delay_ms(25U);
        omnia_hw_set_relay(false);
        if (g_amp_cfg.amp_pwr_auto_standby != 0U) {
            omnia_delay_ms(g_amp_cfg.mute_to_pwr_off_ms);
            omnia_hw_set_amp_pwr(false);
        }
    } else {
        if (!g_in_standby && g_amp_pwr_is_on) {
            return;
        }
        omnia_hw_power_sequence_boot();
    }
}

bool omnia_hw_is_standby(void)
{
    return g_in_standby;
}
