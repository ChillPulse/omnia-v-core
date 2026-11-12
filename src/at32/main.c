/**
 * @file    main.c
 * @brief   Точка входа аудиопроцессора и тактового мастера OMNIA на AT32F403ACGU7
 */

#include "omnia_at32.h"

/* Размещаем линии задержки AETHER и Look-Ahead Limiter в расширенной области 128 КБ (.sram_ext) */
static omnia_dsp_delay_arena_t g_dsp_arena __attribute__((section(".sram_ext")));
static omnia_dsp_ctx_t         g_dsp_ctx;

/* Базовые обработчики исключений ядра Cortex-M4: при аварии мгновенно гасим ЦАП и реле */
void NMI_Handler(void) {}

void HardFault_Handler(void)
{
    omnia_hw_set_amp_mute(true);
    omnia_hw_set_dac_xsmt(false);
    omnia_hw_set_relay(false);
    omnia_hw_set_amp_pwr(false);
    while (1) {}
}

void MemManage_Handler(void)
{
    omnia_hw_set_amp_mute(true);
    omnia_hw_set_dac_xsmt(false);
    omnia_hw_set_relay(false);
    omnia_hw_set_amp_pwr(false);
    while (1) {}
}

void BusFault_Handler(void)
{
    omnia_hw_set_amp_mute(true);
    omnia_hw_set_dac_xsmt(false);
    omnia_hw_set_relay(false);
    omnia_hw_set_amp_pwr(false);
    while (1) {}
}

void UsageFault_Handler(void)
{
    omnia_hw_set_amp_mute(true);
    omnia_hw_set_dac_xsmt(false);
    omnia_hw_set_relay(false);
    omnia_hw_set_amp_pwr(false);
    while (1) {}
}

int main(void)
{
    /* 1. FPU, проверка SRAM без изменения USD, PLL, DWT и SysTick */
    omnia_system_boot_init();

    /* 2. Безопасная инициализация всех ключей Mute/Relay/XSMT/USB до запуска клоков */
    omnia_hw_gpio_init();

    /* Не обращаться к .sram_ext до подтверждения конфигурации: медленно мигаем
     * и сохраняем выходы в безопасных состояниях. USD программирует только оператор. */
    if (omnia_get_sram_size_kb() != 224U) {
        while (1) {
            omnia_hw_led_toggle();
            omnia_delay_ms(250U);
        }
    }

    /* 3. Автоопределение любой внешней SPI NOR Flash (JEDEC + SFDP) на PA4..PA7 */
    omnia_spi_flash_info_t flash_info;
    omnia_ext_flash_init(&flash_info);

    /* 4. Инициализация студийного 9-звенного DSP (по умолчанию Muted до команды ESP32, 48 кГц) */
    omnia_dsp_init(&g_dsp_ctx, &g_dsp_arena, 48000U);

    /* 5. Запуск полнодуплексного мастера I2S2 + I2S2EXT DMA (раздаёт стабильный клок на ESP32 и ЦАП) */
    omnia_i2s_audio_init(&g_dsp_ctx);

    /* 6. Запуск прямого цифрового 24-bit входа Bluetooth I2S3 Slave + ASRC + UART4 (PA15/PB3/PB5 + PA0/PA1) */
#if OMNIA_ENABLE_BT
    omnia_bt_receiver_init(48000U);
#endif

    /* 7. Запуск нативной звуковой карты USB-DAC 24-bit UAC1 Async (PA11/PA12 + HICK/ACC + Триггер PB9) */
#if OMNIA_ENABLE_USB_DAC
    omnia_usb_dac_init();
#endif

    /* До настройки конкретной платы усилителя не подаём питание на K2. */
    omnia_hw_enter_standby(true);

    /* 9. Запуск протокола USART1 DMA (921600 бод) и немедленное уведомление ESP32 о готовности */
    omnia_uart_link_init(&g_dsp_ctx);
    omnia_hw_led_set(true);
    omnia_uart_send_status_now();

    uint32_t last_led_ms = 0U;

    while (1) {
        const uint32_t now = omnia_millis();

        /* Неблокирующий опрос сброса BT, Триггера +5V USB-DAC, метаданных Bluetooth и протокола ESP32 */
        omnia_hw_poll(now);
#if OMNIA_ENABLE_USB_DAC
        omnia_usb_dac_poll(now);
#endif
#if OMNIA_ENABLE_BT
        omnia_bt_receiver_poll(now);
#endif
        omnia_uart_link_poll(now);

        /* Это контроль цифрового DC, а не защита от постоянки на клеммах динамиков. */
        if (g_dsp_ctx.dc_fault_tripped && !omnia_hw_has_fault()) {
            omnia_hw_latch_fault();
        }

        /* Плавное мигание сервисного диода на плате AT32 (раз в 2 с в простое, быстро при аварии DC) */
        const uint32_t blink_period = g_dsp_ctx.dc_fault_tripped ? 120U : 2000U;
        if ((now - last_led_ms) >= blink_period) {
            last_led_ms = now;
            omnia_hw_led_toggle();
        }
    }
}
