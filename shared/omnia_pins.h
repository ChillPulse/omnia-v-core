/**
 * @file    omnia_pins.h
 * @brief   Единая верифицированная карта пинов OMNIA Flagship Streamer v2.0
 *          (ESP32-S3-DevKitC-1 N16R8 44-pin + WeAct AT32F403ACGU7 BlackPill + TENSTAR ST7789 8080-8P)
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

/* --- Аппаратный USB OTG Full-Speed (на общие выводы 4/6 чипа FSUSB30MUX) --- */
#define OMNIA_ESP_USB_DM_GPIO           19      /* FSUSB30MUX pin 6 (D-) */
#define OMNIA_ESP_USB_DP_GPIO           20      /* FSUSB30MUX pin 4 (D+) */

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

/* --- Цифровой аудиовыход I2S Slave TX (тактируется от мастера AT32F403) --- */
#define OMNIA_ESP_I2S_BCK_GPIO          4       /* Вход BCLK от AT32 PB13 */
#define OMNIA_ESP_I2S_DOUT_GPIO         5       /* Выход DATA на 74HC125 1A */
#define OMNIA_ESP_I2S_WS_GPIO           6       /* Вход LRCK от AT32 PB12 */

/* --- Высокоскоростной канал управления и Smart OTA с AT32F403 (UART1 DMA) --- */
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
#define OMNIA_ESP_RGB_RING_GPIO         48      /* RMT TX -> SN74AHCT125N (5V) -> 12x WS2812B */
#define OMNIA_RGB_RING_LED_COUNT        12


/* ========================================================================== *
 *  ЧАСТЬ 2. WeAct AT32F403ACGU7 BlackPill (Cortex-M4F @ 240 МГц, 224 КБ SRAM)
 *  Свободны для отладки/прошивки: PA11/PA12 (USB DFU), PA13/PA14 (SWD)
 * ========================================================================== */

/*
 * I2S2 Full-Duplex Master + MCLK:
 *   PB12 = I2S2_WS    (LRCK Master Out -> ESP32 GPIO6 & PCM5102A LCK)
 *   PB13 = I2S2_CK    (BCLK Master Out -> ESP32 GPIO4 & PCM5102A BCK)
 *   PB14 = I2S2EXT_SD (Audio RX In     <- выход 1Y+2Y селектора 74HC125)
 *   PB15 = I2S2_SD    (Audio TX Out    -> PCM5102A DIN)
 *   PA3  = I2S2_MCK   (MCLK 256*Fs Out -> опц. перемычка на PCM5102A SCK / внешние ЦАПы; ремап SPI2_GMUX_0001)
 *
 * USART1 (протокол с ESP32-S3, 921600 / 2000000 бод):
 *   PA9  = USART1_TX  (-> ESP32 GPIO8)
 *   PA10 = USART1_RX  (<- ESP32 GPIO7)
 *
 * SPI1 (посадочное место SOIC-8 на брюшке WeAct BlackPill — универсальная NOR Flash JEDEC/SFDP):
 *   PA4  = SPI1_CS    (программный CS)
 *   PA5  = SPI1_SCK
 *   PA6  = SPI1_MISO
 *   PA7  = SPI1_MOSI
 *
 * Линии бесщёлчкового Anti-Pop, питания и селекторов:
 *   PB0  = DAC_XSMT   (10 кОм pull-DOWN к GND! LOW = Mute при старте, HIGH = Unmute PCM5102A)
 *   PB1  = AMP_MUTE   (10 кОм pull-down к GND, управление mute УМЗЧ)
 *   PB10 = RELAY_EN   (10 кОм pull-down к GND, выходное реле / питание аналога)
 *   PB6  = OE_ESP_N   (10 кОм pull-up к +3.3V, активный LOW -> 74HC125 1OE: поток от ESP32)
 *   PB7  = OE_BT_N    (10 кОм pull-up к +3.3V, активный LOW -> 74HC125 2OE: резервный I2S вход)
 *   PB8  = VBUS_EN    (ключ NPN + AO3401 подачи +5V на гнездо USB-A для флешки)
 *   PB9  = VBUS_DET   (вход с делителя 10k/15k от гнезда USB-C для автоперехода в USB-DAC)
 *   PB11 = USB_SEL    (пин S мультиплексора FSUSB30MUX: 0 = HSD1 USB-A Host, 1 = HSD2 USB-C DAC)
 *   PC13 = STATUS_LED (бортовой светодиод WeAct BlackPill, активный LOW)
 */

#endif /* OMNIA_PINS_H */
