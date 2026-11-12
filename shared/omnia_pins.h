/**
 * @file    omnia_pins.h
 * @brief   Единая верифицированная карта пинов OMNIA Flagship Streamer v2.2 (24-Bit Direct)
 *          Проверено по распиновкам ESP32-S3-DevKitC-1 N16R8 и WeAct AT32F403ACGU7 BlackPill V1.0.
 *
 *  КЛЮЧЕВЫЕ ОСОБЕННОСТИ РАЗВОДКИ v2.2:
 *    - Полный отказ от внешних мультиплексоров: каждый интерфейс подключён к своему выделенному
 *      аппаратному контроллеру МК напрямую (сквозной 24-битный тракт в 32-битных слотах I2S).
 *    - Выделенный порт USB-A Host MSC на ESP32-S3 (GPIO19/20) навсегда под флешки и SSD.
 *    - Выделенный порт USB-DAC UAC1 Async 24-bit (48 / 44.1 кГц) на AT32F403 (PA11/PA12)
 *      с включаемым Авто-триггером +5V (PB9 + контроль пакетов USB SOF).
 *    - Прямой 24-битный вход Bluetooth (SJR-BTM334 QCC3034 aptX-HD / BT401) на порту I2S3 Slave RX
 *      (PA15/PB3/PB5) + студийный ресемплер Farrow ASRC + мост метаданных UART4 (PA0/PA1) + PA2 Reset.
 *    - Раздельное полноценное управление усилителем мощности (УМЗЧ) и линейным выходом ЦАПа:
 *        * PB0 = DAC_XSMT      — цифровой Soft-Mute ЦАПа PCM5102A;
 *        * PB8 = AUDIO_RELAY   — сигнальное реле Anti-Pop (K1) линейного выхода ЦАПа;
 *        * PB2 = AMP_PWR_RELAY — отдельное реле включения питания УМЗЧ / БП усилителя (K2 / SSR);
 *        * PB4 = AMP_MUTE_OUT  — отдельный 5V-толерантный пин управления входом MUTE / CMUTE / SDZ
 *                                плат TPA3116D2 / TPA3221 (настраивается в Сервисном меню:
 *                                Active-LOW Push-Pull, Active-LOW Open-Drain или Active-HIGH Push-Pull!).
 *    - Свободны ОБА посадочных места под опциональную внешнюю SPI NOR Flash на AT32:
 *      штатное место U3 на обороте WeAct BlackPill (PA8/PB1/PB6/PB7/PB10/PB11) И внешний SPI1 (PA4..PA7).
 */

#ifndef OMNIA_PINS_H
#define OMNIA_PINS_H

/* ========================================================================== *
 *  ЧАСТЬ 1. ESP32-S3-N16R8 DevKitC-1 (44-pin, Octal PSRAM 8 МБ = GPIO35..37)
 *  ЗАПРЕЩЁННЫЕ ПИНЫ (Octal PSRAM + SPI Flash): GPIO26 .. GPIO37 (НЕ ТРОГАТЬ!)
 * ========================================================================== */

/* --- Выделенный порт USB-A Host MSC (флешки / внешние диски FAT32/exFAT) --- */
#define OMNIA_ESP_USB_DM_GPIO           19      /* Гнездо USB-A pin 2 (D-) напрямую */
#define OMNIA_ESP_USB_DP_GPIO           20      /* Гнездо USB-A pin 3 (D+) напрямую */

/* --- Экран TENSTAR 2.8" ST7789 (параллельная шина 8080 8-bit I80 LCD_CAM DMA) --- */
#define OMNIA_ESP_LCD_D0_GPIO           9
#define OMNIA_ESP_LCD_D1_GPIO           10
#define OMNIA_ESP_LCD_D2_GPIO           11
#define OMNIA_ESP_LCD_D3_GPIO           12
#define OMNIA_ESP_LCD_D4_GPIO           13
#define OMNIA_ESP_LCD_D5_GPIO           14
#define OMNIA_ESP_LCD_D6_GPIO           21
#define OMNIA_ESP_LCD_D7_GPIO           47
#define OMNIA_ESP_LCD_WR_GPIO           38      /* Строб записи PCLK/WR (до 20 МГц DMA) */
#define OMNIA_ESP_LCD_DC_GPIO           46      /* RS / Data-Command (strapping, только выход) */
#define OMNIA_ESP_LCD_RST_GPIO          3       /* Аппаратный сброс дисплея */
#define OMNIA_ESP_LCD_BL_GPIO           45      /* ШИМ подсветки LEDC (ОБЯЗАТЕЛЬНО 10 кОм к GND -> VDD_SPI=3.3V!) */
/* Примечание: вывод CS дисплея жёстко притянут к GND, вывод RD дисплея жёстко притянут к +3.3V */

/* --- Слот MicroSD (выделенный контроллер SPI3 / VSPI, до 40 МГц) --- */
#define OMNIA_ESP_SD_MOSI_GPIO          15
#define OMNIA_ESP_SD_SCK_GPIO           16
#define OMNIA_ESP_SD_MISO_GPIO          17
#define OMNIA_ESP_SD_CS_GPIO            18

/* --- Цифровой аудиовыход 24-bit в 32-bit слотах I2S Slave TX (напрямую в AT32 PB14) --- */
#define OMNIA_ESP_I2S_BCK_GPIO          4       /* Вход BCLK от мастера AT32 PB13 */
#define OMNIA_ESP_I2S_DOUT_GPIO         5       /* Выход SDATA напрямую на вход AT32 PB14 (I2S2EXT_SD) */
#define OMNIA_ESP_I2S_WS_GPIO           6       /* Вход LRCK/WS от мастера AT32 PB12 */

/* --- Высокоскоростной канал управления и Smart Dual-OTA с AT32F403 (UART1 @ 921600/2M) --- */
#define OMNIA_ESP_AT32_TX_GPIO          7       /* -> AT32 PA10 (USART1_RX) */
#define OMNIA_ESP_AT32_RX_GPIO          8       /* <- AT32 PA9  (USART1_TX) */

/* --- Пользовательские органы управления, ИК, свет и адресное кольцо 12x WS2812B --- */
#define OMNIA_ESP_BTN_ADC_GPIO          1       /* ADC1_CH0: резистивная лестница на 7 кнопок */
#define OMNIA_ESP_ENC_A_GPIO            41      /* Аппаратный квадратурный счётчик PCNT (фаза A) */
#define OMNIA_ESP_ENC_B_GPIO            42      /* Аппаратный квадратурный счётчик PCNT (фаза B) */
#define OMNIA_ESP_ENC_SW_GPIO           0       /* Нажатие валкодера (внутр. pull-up, не зажимать при подаче питания) */
#define OMNIA_ESP_IR_RX_GPIO            40      /* VS1838B OUT (питание 3.3 В) -> RMT RX */
#define OMNIA_ESP_I2C_SDA_GPIO          2       /* Датчик внешней освещённости BH1750 (адрес 0x23) */
#define OMNIA_ESP_I2C_SCL_GPIO          39      /* Подтяжка 4.7 кОм к +3.3V на SDA и SCL */
#define OMNIA_ESP_RGB_RING_GPIO         48      /* RMT TX -> 5V буфер уровня -> 330 Ом -> 12x WS2812B */
#define OMNIA_RGB_RING_LED_COUNT        12      /* Число адресных светодиодов кольца подсветки */

/* Заводской порт прошивки и логов ESP-IDF: UART0 (GPIO43 TX, GPIO44 RX) — неприкосновенен! */


/* ========================================================================== *
 *  ЧАСТЬ 2. WeAct AT32F403ACGU7 BlackPill V1.0 (QFN48, 240 МГц, 224 КБ SRAM)
 *  Все 37 выводов распределены с НУЛЁМ пересечений!
 * ========================================================================== *
 *
 *  1) Главная шина ЦАПа и стримера (I2S2 Master Full-Duplex, 24-bit в 32-bit слотах):
 *     PB12 = I2S2_WS     (LRCK Master Out -> PCM5102A LCK + ESP32 GPIO6)
 *     PB13 = I2S2_CK     (BCLK Master Out -> PCM5102A BCK + ESP32 GPIO4)
 *     PB14 = I2S2EXT_SD  (Full-Duplex RX  <- напрямую от ESP32 GPIO5!)
 *     PB15 = I2S2_SD     (Full-Duplex TX  -> напрямую в PCM5102A DIN!)
 *     PA3  = I2S2_MCK    (MCLK 256*Fs Out -> задел под внешние Hi-End ЦАПы; ремап SPI2_GMUX_0001)
 *
 *  2) Прямой 24-битный вход Bluetooth (I2S3 Slave RX + Farrow ASRC -> SJR-BTM334 aptX-HD / BT401):
 *     Активируется через SWJTAG_GMUX_010 (отладка SWD PA13/PA14 остаётся полностью рабочей!) + SPI3_GMUX_0010:
 *     PA15 = I2S3_WS     (Slave LRCK In <- BTM334 Pin 4 PIO17 / BT401 PB1)
 *     PB3  = I2S3_CK     (Slave BCLK In <- BTM334 Pin 5 PIO16 / BT401 PB0)
 *     PB5  = I2S3_SD     (Slave DATA In <- BTM334 Pin 3 PIO18 / BT401 PB2)
 *
 *  3) Двусторонний канал команд и метаданных Bluetooth AVRCP 1.6 (UART4 @ 115200 бод):
 *     Активируется через ремап UART4_GMUX_0010:
 *     PA0  = UART4_TX    (-> BTM334 UART_RX / BT401 PA3 RX)
 *     PA1  = UART4_RX    (<- BTM334 UART_TX / BT401 PA2 TX — названия треков UTF-8 и кодек!)
 *     PA2  = BT_CTRL_EN  (аппаратный сброс / импульс Pairing для модуля Bluetooth)
 *
 *  4) Встроенная звуковая карта USB-DAC 24-bit UAC1 Async (48 / 44.1 кГц) + Триггер +5V:
 *     PA11 = USBFS1_DM   (гнездо «Вход USB-DAC» D- через 22 Ом, или штатный Type-C на BlackPill)
 *     PA12 = USBFS1_DP   (гнездо «Вход USB-DAC» D+ через 22 Ом, или штатный Type-C на BlackPill)
 *     PB9  = USB_VBUS_DET(делитель 10 кОм к VBUS гнезда + 15 кОм и 100 нФ к GND -> 3.0 В = ВКЛ!)
 *
 *  5) Мост управления с ESP32-S3 (USART1 @ 921600 / 2000000 бод):
 *     PA9  = USART1_TX   (-> ESP32 GPIO8)
 *     PA10 = USART1_RX   (<- ESP32 GPIO7)
 *
 *  6) Опциональная внешняя SPI NOR Flash любого бренда (автоопрос JEDEC + SFDP обеих шин!):
 *     Вариант А (штатное место U3 SOIC-8 на обороте платы WeAct BlackPill — 100% свободно!):
 *       PA8=CLK, PB1=CS, PB6=IO0(MOSI), PB7=IO1(MISO), PB10=IO2(WP), PB11=IO3(HOLD)
 *     Вариант Б (обычные штырьки SPI1):
 *       PA4=CS, PA5=SCK, PA6=MISO, PA7=MOSI
 *
 *  7) Полный бесщёлчковый секвенсор ЦАПа и УМЗЧ (4 независимых вывода, настраивается в Сервисном меню!):
 *     PB0  = DAC_XSMT       (10 кОм к GND; Soft-Mute ЦАПа PCM5102A: LOW = Mute, HIGH = Play)
 *     PB8  = AUDIO_RELAY_EN (10 кОм к GND; сигнальное реле Anti-Pop K1 выхода ЦАПа через VT1 NPN)
 *     PB2  = AMP_PWR_EN     (заводской 10 кОм к GND на плате BlackPill + каскад VT2 NPN / SSR:
 *                            отдельный пин включения силового реле питания УМЗЧ K2!)
 *     PB4  = AMP_MUTE_OUT   (5V-tolerant FT, 10 кОм к GND: отдельный настраиваемый в Сервисном меню
 *                            пин подачи LOW / HIGH / Open-Drain на вход MUTE / CMUTE / SDZ плат
 *                            TPA3116D2 и TPA3221!)
 *     PC13 = STATUS_LED     (синий светодиод на плате BlackPill, активный LOW)
 */

#define OMNIA_AT32_DAC_XSMT_PORT        GPIOB
#define OMNIA_AT32_DAC_XSMT_PIN         GPIO_PINS_0     /* PB0: Soft-Mute ЦАПа PCM5102A (XSMT)          */

#define OMNIA_AT32_AMP_PWR_PORT         GPIOB
#define OMNIA_AT32_AMP_PWR_PIN          GPIO_PINS_2     /* PB2: Реле включения силового питания УМЗЧ K2 */

#define OMNIA_AT32_AMP_MUTE_PORT        GPIOB
#define OMNIA_AT32_AMP_MUTE_PIN         GPIO_PINS_4     /* PB4: Настраиваемый Mute TPA3116 / TPA3221    */

#define OMNIA_AT32_RELAY_PORT           GPIOB
#define OMNIA_AT32_RELAY_PIN            GPIO_PINS_8     /* PB8: Сигнальное реле Anti-Pop K1 выхода ЦАПа */

#define OMNIA_AT32_VBUS_DET_PORT        GPIOB
#define OMNIA_AT32_VBUS_DET_PIN         GPIO_PINS_9     /* PB9: Детектор +5V от ТВ-бокса / ПК (10k/15k) */

#define OMNIA_AT32_BT_CTRL_PORT         GPIOA
#define OMNIA_AT32_BT_CTRL_PIN          GPIO_PINS_2     /* PA2: Аппаратный Reset / Pairing Bluetooth     */

#define OMNIA_AT32_LED_PORT             GPIOC
#define OMNIA_AT32_LED_PIN              GPIO_PINS_13    /* PC13: Бортовой светодиод BlackPill            */

#endif /* OMNIA_PINS_H */
