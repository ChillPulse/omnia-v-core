#ifndef CONFIG_PINS_H
#define CONFIG_PINS_H

/*
 * Карта GPIO Omnia (v-core), ТЗ Том 1, Глава 3 (Hardware Map v5.0).
 *
 * ВНИМАНИЕ: в карте есть конфликты с фото платы YD-ESP32-S3 и с Octal PSRAM (N16R8).
 * См. README.md, раздел «Конфликты аппаратной карты». Значения здесь оставлены
 * как в ТЗ; для исправления меняйте только этот файл.
 */

// === ЦАП PCM5102A (I2S_NUM_0, режим Master) ===
#define PIN_I2S_BCK         4
#define PIN_I2S_DIN         5
#define PIN_I2S_LRCK        6
#define PIN_I2S_XSMT        7   // HIGH = выход открыт, LOW = аппаратный mute

// === Дисплей ST7789 2.8" (SPI2_HOST, до 60 МГц) ===
#define PIN_DISPLAY_SCL    14   // SCLK
#define PIN_DISPLAY_SDA    12   // MOSI
#define PIN_DISPLAY_RES    13
#define PIN_DISPLAY_DC     11
#define PIN_DISPLAY_CS     21
#define PIN_DISPLAY_BL     47   // ШИМ подсветки (LEDC, 12 бит, 5 кГц)

// === SD-карта (SPI3_HOST) ===  ⚠ GPIO 35–37 заняты Octal PSRAM на N16R8
#define PIN_SD_SCK         36
#define PIN_SD_MOSI        37
#define PIN_SD_MISO        35
#define PIN_SD_CS          38

// === Коммутация USB (74HC4052D, AO3401 + BC847, делитель VBUS) ===
#define PIN_USB_SEL        18
#define PIN_USB_VBUS_EN    17
#define PIN_USB_VBUS_DET   16   // RTC GPIO16: вход пробуждения

// === Внешний УМЗЧ TPA3116 ===
#define PIN_AMP_MUTE       39   // ⚠ MTCK (JTAG)
#define PIN_AMP_POWER_REG   9   // Триггер реле блока питания УМЗЧ

// === Датчик освещенности BH1750 (I2C_NUM_0, 400 кГц) ===  ⚠ GPIO45 — strapping-пин
#define PIN_I2C_SDA         2
#define PIN_I2C_SCL        45

// === Инкрементальный энкодер (PCNT) ===  ⚠ GPIO40–42 — JTAG
#define PIN_ENCODER_A      41
#define PIN_ENCODER_B      42
#define PIN_ENCODER_SW     40

// === ИК-приемник TSOP (RMT RX) ===  ⚠ GPIO3 — strapping/JTAG source
#define PIN_IR_RECEIVER     3

// === АЦП-клавиатура из 7 кнопок (ADC1_CH0) ===
#define PIN_ADC_KEYBOARD    1

// === RGB-кольцо WS2812B (RMT TX), 1 встроенный + 12 внешних ===
#define PIN_RGB_RING       48
#define RGB_RING_LED_COUNT 13

// === USB (встроенный PHY ESP32-S3) ===
#define PIN_USB_DM         19
#define PIN_USB_DP         20

#endif // CONFIG_PINS_H
