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
    omnia_hw_set_dac_xsmt(false);
    omnia_hw_set_relay(false);
    while (1) {}
}

void MemManage_Handler(void)
{
    omnia_hw_set_dac_xsmt(false);
    omnia_hw_set_relay(false);
    while (1) {}
}

void BusFault_Handler(void)
{
    omnia_hw_set_dac_xsmt(false);
    omnia_hw_set_relay(false);
    while (1) {}
}

void UsageFault_Handler(void)
{
    omnia_hw_set_dac_xsmt(false);
    omnia_hw_set_relay(false);
    while (1) {}
}

int main(void)
{
    /* 1. Подъём FPU, автораскрытие 224 КБ SRAM (EOPB0 = 0xFE), PLL 240 МГц, DWT и SysTick */
    omnia_system_boot_init();

    /* 2. Безопасная инициализация всех ключей Mute/Relay/XSMT/USB до запуска клоков */
    omnia_hw_gpio_init();

    /* 3. Автоопределение любой внешней SPI NOR Flash (JEDEC + SFDP) на PA4..PA7 */
    omnia_spi_flash_info_t flash_info;
    omnia_ext_flash_init(&flash_info);

    /* 4. Инициализация студийного 9-звенного DSP (по умолчанию Muted до команды ESP32, 48 кГц) */
    omnia_dsp_init(&g_dsp_ctx, &g_dsp_arena, 48000U);

    /* 5. Запуск полнодуплексного мастера I2S2 + I2S2EXT DMA (раздаёт стабильный клок на ESP32 и ЦАП) */
    omnia_i2s_audio_init(&g_dsp_ctx);

    /* 6. Бесщёлчковое открытие аналогового тракта (150 мс стабилизации ЦАПа -> Relay -> XSMT) */
    omnia_hw_power_sequence_boot();

    /* 7. Запуск протокола USART1 DMA (921600 бод) и немедленное уведомление ESP32 о готовности */
    omnia_uart_link_init(&g_dsp_ctx);
    omnia_hw_led_set(true);
    omnia_uart_send_status_now();

    uint32_t last_led_ms = 0U;

    while (1) {
        const uint32_t now = omnia_millis();

        /* Непрерывный опрос детектора +5V на гнезде USB-C для горячего автоперехода в USB-DAC */
        bool vbus_changed = false;
        omnia_hw_poll_usb_vbus(now, &vbus_changed);
        if (vbus_changed) {
            /* Мгновенно оповещаем ESP32-S3 о подключении/отключении кабеля ПК/Android/ТВ-бокса! */
            omnia_uart_send_status_now();
        }

        /* Обработка входящих пакетов DSP/громкости/частоты и отправка пикметров VU */
        omnia_uart_link_poll(now);

        /* Плавное мигание сервисного диода на плате AT32 (раз в 2 с в простое, быстро при аварии DC) */
        const uint32_t blink_period = g_dsp_ctx.dc_fault_tripped ? 120U : 2000U;
        if ((now - last_led_ms) >= blink_period) {
            last_led_ms = now;
            omnia_hw_led_toggle();
        }
    }
}
