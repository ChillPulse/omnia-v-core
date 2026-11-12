/**
 * @file    at32f403a_407_conf.h
 * @brief   Конфигурация драйверов периферии Artery AT32F403ACGU7 (WeAct BlackPill, кварц 8 МГц)
 */

#ifndef __AT32F403A_407_CONF_H
#define __AT32F403A_407_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

#if !defined HEXT_VALUE
#define HEXT_VALUE                       ((uint32_t)8000000)
#endif

#define HEXT_STARTUP_TIMEOUT             ((uint16_t)0x3000)
#define HICK_VALUE                       ((uint32_t)8000000)
#define LEXT_VALUE                       ((uint32_t)32768)

#define CRM_MODULE_ENABLED
#define TMR_MODULE_ENABLED
#define GPIO_MODULE_ENABLED
#define USART_MODULE_ENABLED
#define PWC_MODULE_ENABLED
#define SPI_MODULE_ENABLED
#define DMA_MODULE_ENABLED
#define DEBUG_MODULE_ENABLED
#define FLASH_MODULE_ENABLED
#define CRC_MODULE_ENABLED
#define WDT_MODULE_ENABLED
#define EXINT_MODULE_ENABLED
#define MISC_MODULE_ENABLED
#define ACC_MODULE_ENABLED
#define USB_MODULE_ENABLED

#include "at32f403a_407_crm.h"
#include "at32f403a_407_tmr.h"
#include "at32f403a_407_gpio.h"
#include "at32f403a_407_usart.h"
#include "at32f403a_407_pwc.h"
#include "at32f403a_407_spi.h"
#include "at32f403a_407_dma.h"
#include "at32f403a_407_debug.h"
#include "at32f403a_407_flash.h"
#include "at32f403a_407_crc.h"
#include "at32f403a_407_wdt.h"
#include "at32f403a_407_exint.h"
#include "at32f403a_407_misc.h"
#include "at32f403a_407_acc.h"
#include "at32f403a_407_usb.h"

#ifdef __cplusplus
}
#endif

#endif /* __AT32F403A_407_CONF_H */
