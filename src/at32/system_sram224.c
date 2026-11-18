/**
 * @file    system_sram224.c
 * @brief   Инициализация ядра AT32F403ACGU7 на 240 МГц, проверка 224 КБ SRAM (EOPB0 = 0xFE),
 *          системный миллисекундный таймер SysTick и аппаратный счётчик тактов DWT.
 */

#include "omnia_at32.h"
#include <string.h>

extern uint8_t _ssram_ext;
extern uint8_t _esram_ext;

static volatile uint32_t g_systick_ms = 0U;
static uint16_t          g_sram_kb    = 96U;

void SysTick_Handler(void)
{
    g_systick_ms++;
}

/**
 * @brief  Проверяет заводской байт конфигурации EOPB0 (0x1FFFF810).
 *         В режиме без 224 КБ сообщает о недоступной памяти; USD не изменяется.
 */
static void ensure_224kb_sram_enabled(void)
{
    const uint8_t cur_eopb0 = (uint8_t)(USD->eopb0 & 0xFFU);

    /* Никогда не стираем USD/option bytes автоматически: там могут храниться
     * защита и другие заводские настройки. Без 224 КБ запуск DSP запрещён. */
    if (cur_eopb0 != 0xFEU) {
        g_sram_kb = 96U;
        return;
    }

    /* Пробная запись/чтение в верхний адрес 224 КБ SRAM (0x20037FFC) для самопроверки */
    volatile uint32_t *probe = (volatile uint32_t *)0x20037FFCU;
    const uint32_t saved = *probe;
    *probe = 0x5AA5F00DU;
    __DSB();
    if (*probe == 0x5AA5F00DU) {
        g_sram_kb = 224U;
        *probe = saved;
        /* Очищаем секцию расширенной памяти .sram_ext (линии задержки AETHER и Limiter) */
        const size_t ext_len = (size_t)(&_esram_ext - &_ssram_ext);
        if (ext_len > 0U && ext_len <= (128U * 1024U)) {
            memset(&_ssram_ext, 0, ext_len);
        }
    } else {
        g_sram_kb = 96U;
    }
}

/**
 * @brief  Стендовое тактирование от HICK (номинально 240 МГц, точность не подтверждена).
 */
static void clock_240mhz_init(void)
{
    crm_reset();

    /* На BlackPill встречаются кварцы 8 и 25 МГц. До проверки маркировки
     * нельзя предполагать HEXT=8: это может привести к опасному разгону.
     * Стендовый вариант использует HICK 8 МГц / 2 * 60; частота неточная. */
    crm_clock_source_enable(CRM_CLOCK_SOURCE_HICK, TRUE);
    while (crm_flag_get(CRM_HICK_STABLE_FLAG) != SET) {}
    crm_pll_config(CRM_PLL_SOURCE_HICK, CRM_PLL_MULT_60, CRM_PLL_OUTPUT_RANGE_GT72MHZ);

    crm_clock_source_enable(CRM_CLOCK_SOURCE_PLL, TRUE);
    while (crm_flag_get(CRM_PLL_STABLE_FLAG) != SET) {}

    crm_ahb_div_set(CRM_AHB_DIV_1);     /* AHB  = 240 МГц */
    crm_apb2_div_set(CRM_APB2_DIV_2);   /* APB2 = 120 МГц */
    crm_apb1_div_set(CRM_APB1_DIV_2);   /* APB1 = 120 МГц */

    crm_auto_step_mode_enable(TRUE);
    crm_sysclk_switch(CRM_SCLK_PLL);
    while (crm_sysclk_switch_status_get() != CRM_SCLK_PLL) {}
    crm_auto_step_mode_enable(FALSE);

    system_core_clock_update();
}

void omnia_system_boot_init(void)
{
    /* Включаем аппаратный FPU Cortex-M4F (CP10 + CP11 Full Access) */
    SCB->CPACR |= ((3UL << (10U * 2U)) | (3UL << (11U * 2U)));
    __DSB();
    __ISB();

    nvic_priority_group_config(NVIC_PRIORITY_GROUP_4);

    ensure_224kb_sram_enabled();
    clock_240mhz_init();

    /* Включаем аппаратный счётчик тактов DWT->CYCCNT для точного замера задержек и % CPU */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    /* Запускаем прерывание SysTick строго на 1000 Гц (1 мс) */
    SysTick_Config(system_core_clock / 1000U);
    nvic_irq_enable(SysTick_IRQn, 3, 0);
}

uint16_t omnia_get_sram_size_kb(void)
{
    return g_sram_kb;
}

uint32_t omnia_millis(void)
{
    return g_systick_ms;
}

uint32_t omnia_dwt_cycles(void)
{
    return DWT->CYCCNT;
}

void omnia_delay_us(uint32_t us)
{
    const uint32_t start = DWT->CYCCNT;
    const uint32_t ticks = us * (system_core_clock / 1000000U);
    while ((DWT->CYCCNT - start) < ticks) {
        __NOP();
    }
}

void omnia_delay_ms(uint32_t ms)
{
    while (ms-- > 0U) {
        omnia_delay_us(1000U);
    }
}
