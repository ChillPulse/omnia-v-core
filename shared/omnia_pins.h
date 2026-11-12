/**
 * @file    omnia_pins.h
 * @brief   Единая верифицированная карта пинов OMNIA Flagship Streamer v2.1 (Zero-Mux 24-Bit)
 *          ESP32-S3-DevKitC-1 N16R8 + WeAct AT32F403ACGU7 BlackPill + BTM334 (QCC3034) / BT401 Master
 *
 *  КЛЮЧЕВЫЕ ПРЕИМУЩЕСТВА АРХИТЕКТУРЫ ZERO-MUX 24-BIT:
 *    - 0 микросхем мультиплексоров (отказ от FSUSB30MUX, 74HC125 и ключа питания AO3401):
 *      * ESP32-S3 GPIO5 (DOUT) идёт НАПРЯМУЮ в AT32 PB14 (I2S2EXT_SD);
 *      * Bluetooth (BTM334 / BT401) идёт НАПРЯМУЮ во второй аппаратный порт AT32 I2S3 (PA15/PB3/PB5);
 *      * Гнездо USB-A (флешка/HDD) навсегда закреплено за ESP32-S3 (GPIO19/20 + самовосст. предохранитель PPTC);
 *      * Вход USB-DAC (24-bit / 48 и 44.1 кГц UAC1 Async) работает НАПРЯМУЮ на родном USBFS AT32 (PA11/PA12)
 *        с включаемой триггер-функцией автопереключения по +5V (PB9 VBUS_DET + резерв по USB SOF)!
 */

#ifndef OMNIA_PINS_H
#define OMNIA_PINS_H

/* ========================================================================== *
 *  ЧАСТЬ 1. ESP32-S3-DevKitC-1 N16R8 (44-pin, 16MB Quad Flash + 8MB Octal PSRAM)
 *  ЗАПРЕЩЕНЫ К ИСПОЛЬЗОВАНИЮ: GPIO35, GPIO36, GPIO37 (шина Octal PSRAM)!
 * ========================================================================== */

/* --- Отладочная консоль / первичная прошивка (бортовой USB-UART) --- */
#define OMNIA_ESP_UART0_TX_GPIO         43
#define OMNIA_ESP_UART0_RX_GPIO         44

/* --- Выделенный аппаратный USB Host Full-Speed (напрямую на гнездо USB-A для флешки/HDD) --- */
#define OMNIA_ESP_USB_DM_GPIO           19      /* USB-A pin 2 (D-) напрямую, без FSUSB30MUX! */
#define OMNIA_ESP_USB_DP_GPIO           20      /* USB-A pin 3 (D+) напрямую, без FSUSB30MUX! */

/* --- Дисплей TENSTAR 2.8" IPS ST7789 (разъём 8080-8P, FFC 16-pin 1.0mm) --- */
#define OMNIA_ESP_LCD_BL_GPIO           45      /* FFC #1  (BL)  — встроенный ключ U7 на экране + 10k к GND у ESP32 */
#define OMNIA_ESP_LCD_D7_GPIO           47      /* FFC #2  (D7)  */
#define OMNIA_ESP_LCD_D6_GPIO           21      /* FFC #3  (D6)  */
#define OMNIA_ESP_LCD_D5_GPIO           14      /* FFC #4  (D5)  */
#define OMNIA_ESP_LCD_D4_GPIO           13      /* FFC #5  (D4)  */
#define OMNIA_ESP_LCD_D3_GPIO           12      /* FFC #6  (D3)  */
#define OMNIA_ESP_LCD_D2_GPIO           11      /* FFC #7  (D2)  */
#define OMNIA_ESP_LCD_D1_GPIO           10      /* FFC #8  (D1)  */
#define OMNIA_ESP_LCD_D0_GPIO           9       /* FFC #9  (D0)  */
#define OMNIA_ESP_LCD_RST_GPIO          3       /* FFC #10 (RST) — выход без внешней подтяжки */
/* FFC #11 (RD)  -> жёстко на +3.3V */
#define OMNIA_ESP_LCD_WR_GPIO           38      /* FFC #12 (WR / PCLK 20..30 МГц) */
#define OMNIA_ESP_LCD_DC_GPIO           46      /* FFC #13 (DC)  — выход без внешней подтяжки */
#define OMNIA_ESP_LCD_CS_GPIO           (-1)    /* FFC #14 (CS)  — притянут к GND через 4.7 кОм */

/* --- Карта памяти MicroSD (выделенный SPI DMA) --- */
#define OMNIA_ESP_SD_MOSI_GPIO          15
#define OMNIA_ESP_SD_SCK_GPIO           16
#define OMNIA_ESP_SD_MISO_GPIO          17
#define OMNIA_ESP_SD_CS_GPIO            18

/* --- Цифровой аудиовыход 24/32-bit I2S Slave TX (напрямую в AT32F403 без 74HC125!) --- */
#define OMNIA_ESP_I2S_BCK_GPIO          4       /* Вход BCLK <- напрямую от AT32 PB13 */
#define OMNIA_ESP_I2S_DOUT_GPIO         5       /* Выход DATA -> напрямую на AT32 PB14 (I2S2EXT_SD) */
#define OMNIA_ESP_I2S_WS_GPIO           6       /* Вход LRCK <- напрямую от AT32 PB12 */

/* --- Высокоскоростной канал управления, метаданных BT и Smart OTA с AT32F403 (UART1 DMA) --- */
#define OMNIA_ESP_AT32_TX_GPIO          7       /* -> AT32 PA10 (USART1_RX) */
#define OMNIA_ESP_AT32_RX_GPIO          8       /* <- AT32 PA9  (USART1_TX) */

/* --- Шина I2C (датчик освещённости BH1750 0x23 + резерв расширения) --- */
#define OMNIA_ESP_I2C_SDA_GPIO          2       /* 4.7k pull-up к +3.3V */
#define OMNIA_ESP_I2C_SCL_GPIO          39      /* 4.7k pull-up к +3.3V */
#define OMNIA_BH1750_I2C_ADDR           0x23

/* --- Органы управления и светодиодное кольцо --- */
#define OMNIA_ESP_ENC_A_GPIO            41      /* Энкодер фаза A (PCNT) */
#define OMNIA_ESP_ENC_B_GPIO            42      /* Энкодер фаза B (PCNT) */
#define OMNIA_ESP_ENC_BTN_GPIO          0       /* Кнопка энкодера (pull-up) + сервисный вход при старте */
#define OMNIA_ESP_IR_RX_GPIO            40      /* Самообучаемый ИК-приёмник TSOP38238 (RMT RX) */
#define OMNIA_ESP_ADC_KEYS_GPIO         1       /* ADC1_CH0 — резистивная лестница на 7 кнопок */
#define OMNIA_ESP_RGB_RING_GPIO         48      /* RMT TX -> буфер 5V / развязка -> 12x WS2812B */
#define OMNIA_RGB_RING_LED_COUNT        12


/* ========================================================================== *
 *  ЧАСТЬ 2. WeAct AT32F403ACGU7 BlackPill V1.0 (Cortex-M4F @ 240 МГц, 224 КБ SRAM)
 *  Сверено по официальной схеме BlackPill_AT32F403ACxU_V10.SchDoc!
 * ========================================================================== */

/*
 * 1) I2S2 Full-Duplex Master + MCLK (24/32-bit тактовый мастер для ESP32-S3 и ЦАПа PCM5102A):
 *    PB12 = I2S2_WS    (LRCK Master Out -> ESP32 GPIO6 & PCM5102A LCK)
 *    PB13 = I2S2_CK    (BCLK Master Out -> ESP32 GPIO4 & PCM5102A BCK)
 *    PB14 = I2S2EXT_SD (Audio RX In     <- НАПРЯМУЮ от ESP32 GPIO5 DOUT)
 *    PB15 = I2S2_SD    (Audio TX Out    -> НАПРЯМУЮ на PCM5102A DIN)
 *    PA3  = I2S2_MCK   (MCLK 256*Fs Out -> опц. для внешних Hi-End ЦАПов; ремап SPI2_GMUX_0001)
 *
 * 2) I2S3 Slave RX на DMA2 (Прямой 24-bit цифровой вход от Bluetooth BTM334 QCC3034 / BT401 Master):
 *    Активируется через SWJTAG_GMUX_010 (SWD PA13/PA14 остаётся полностью рабочим!) + SPI3_GMUX_0010:
 *    PA15 = I2S3_WS    (BT LRCK In <- BTM334 Pin 4 PIO17 / BT401 PB1)
 *    PB3  = I2S3_CK    (BT BCLK In <- BTM334 Pin 5 PIO16 / BT401 PB0)
 *    PB5  = I2S3_SD    (BT DATA In <- BTM334 Pin 3 PIO18 / BT401 PB2)
 *
 * 3) UART4 на DMA2/IRQ (Управление Bluetooth и приём метаданных треков AVRCP 1.6 @ 115200 8N1):
 *    Активируется через ремап UART4_GMUX_0010:
 *    PA0  = UART4_TX   (-> RX модуля BTM334 / BT401 PA4)
 *    PA1  = UART4_RX   (<- TX модуля BTM334 / BT401 PA3)
 *    PA2  = BT_CTRL_EN (Выход сброса / принудительного спаривания BT-модуля)
 *
 * 4) Нативный вход звуковой карты USB-DAC 24-bit / 48 (44.1) кГц UAC1 Async + Триггер +5V:
 *    PA11 = USB_DM     (Родной Type-C на BlackPill ИЛИ выносное гнездо USB-C/USB-B на задней панели)
 *    PA12 = USB_DP     (Родной Type-C на BlackPill ИЛИ выносное гнездо USB-C/USB-B на задней панели)
 *    PB9  = VBUS_DET   (Триггер +5V от ТВ-бокса/ПК/смартфона через делитель 10 кОм / 15 кОм + 100 нФ;
 *                       поддерживает включаемое автопереключение, пробуждение из Standby и автовозврат!)
 *
 * 5) USART1 (Протокол с ESP32-S3, 921600 / 2000000 бод):
 *    PA9  = USART1_TX  (-> ESP32 GPIO8)
 *    PA10 = USART1_RX  (<- ESP32 GPIO7)
 *
 * 6) Линии бесщёлчкового Anti-Pop (разведены на чистые пины БЕЗ конфликта с SOIC-8 флешкой U3!):
 *    PB0  = DAC_XSMT   (10 кОм pull-DOWN к GND! LOW = Mute при старте, HIGH = Unmute PCM5102A)
 *    PB4  = AMP_MUTE   (10 кОм pull-DOWN к GND, управление mute внешнего УМЗЧ)
 *    PB8  = RELAY_EN   (10 кОм pull-DOWN к GND -> 1 кОм -> база NPN -> выходное реле Anti-Pop)
 *    PC13 = STATUS_LED (бортовой светодиод WeAct BlackPill, активный LOW)
 *
 * 7) Опциональная внешняя SPI NOR Flash (автоопределение JEDEC + SFDP сразу на ДВУХ площадках!):
 *    - Нижняя площадка U3 SOIC-8 на брюшке WeAct BlackPill: PA8(CS), PB1(SCK), PB10(DI), PB11(DO), PB7(WP), PB6(HOLD)
 *    - Внешняя гребёнка SPI1: PA4(CS), PA5(SCK), PA6(MISO), PA7(MOSI)
 *    (Если флешка не распаяна — пресеты прозрачно пишутся в верхние сектора внутренней 1 МБ Flash AT32).
 */

#define OMNIA_AT32_DAC_XSMT_PIN         GPIO_PINS_0     /* GPIOB */
#define OMNIA_AT32_AMP_MUTE_PIN         GPIO_PINS_4     /* GPIOB */
#define OMNIA_AT32_RELAY_EN_PIN         GPIO_PINS_8     /* GPIOB */
#define OMNIA_AT32_VBUS_DET_PIN         GPIO_PINS_9     /* GPIOB */
#define OMNIA_AT32_BT_CTRL_PIN          GPIO_PINS_2     /* GPIOA */

#endif /* OMNIA_PINS_H */
