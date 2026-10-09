# Omnia (v-core)

Hi-Fi аудиостример и DSP на **YD-ESP32-S3** (ESP32-S3 N16R8: 16 МБ flash, 8 МБ Octal PSRAM).
Реализация по «Мастер-ТЗ: Аудиостример OMNIA (v-core)», Тома 1–5.

> **Статус: код написан, но НЕ собран и НЕ прошит.** ESP-IDF в среде разработки не установлен.
> Матрица статусов — в разделе «Статус реализации». Первая сборка и проверка на железе — обязательны.

## Требования

- ESP-IDF **v5.3** (CI настроен на 5.3; `idf_component.yml` допускает ≥5.3.0)
- Компоненты (через IDF Component Manager): `lvgl/lvgl ~9.2.0`, `joltwallet/littlefs ^1.14.0`
- Python 3 (для сборки PWA)

## Сборка

```bash
# 1. PWA -> littlefs_data/index.html.gz (обязательно до idf.py build)
python3 tools/build_pwa.py

# 2. Настройка Wi-Fi (SSID и пароль НЕ хранятся в репозитории)
idf.py set-target esp32s3
idf.py menuconfig          # Omnia (v-core) -> Wi-Fi SSID / Password

# 3. Сборка и прошивка
idf.py build flash monitor
```

Параметры `sdkconfig.defaults`: flash QIO 80 МГц, Octal PSRAM 80 МГц, CPU 240 МГц,
PM включен, `-O3` с LTO (в корневом CMake), httpd WebSocket, LVGL 16 бит с `LV_COLOR_16_SWAP`.

Разделы (`partitions.csv`): nvs, phy_init, otadata, ota_0 и ota_1 (по 4 МБ), littlefs (0x7E0000).

## Структура

```
main/
  main.cpp                  app_main: NVS, PSRAM, буфер, DSP, очереди, задачи Core 0 / Core 1
  omnia_state.cpp           состояние (мьютекс), транспорт, источники, seek, ключи
  include/config_pins.h     карта GPIO (ТЗ Том 1) — менять пины ТОЛЬКО здесь
  include/omnia.h           общие типы и прототипы модулей
  audio_engine/             Core 0: I2S0 bit-perfect, decode -> float32, FFT, DLNA-ingest, кольцевой буфер 4 МБ PSRAM
  dsp_processor/            двухслойный DF2T-биквад DSP (5 Room + 15 User), Dynamic Headroom, Anti-AD
  media_indexer/            SD (SPI3, /sd), USB MUX/VBUS; драйвер USB MSC — заглушка
  ui_engine/                Core 1: LVGL 9 на ST7789 (SPI2, 60 МГц), энкодер, ADC-клавиатура,
                            спектр-визуализатор, IR RMT RX, httpd (WS /ws + PWA + DLNA + OTA)
  system/                   Wi-Fi STA, BH1750, управление УМЗЧ/питанием, OTA
web/index.html              PWA (источник, один файл без сборщика)
littlefs_data/index.html.gz сгенерированный бандл PWA (tools/build_pwa.py)
tools/build_pwa.py          gzip-сборка PWA, лимит 150 КБ
.github/workflows/build.yml CI: сборка esp32s3 на ESP-IDF v5.3
```

## Конфликты аппаратной карты (требуют решения до сборки железа)

Пины оставлены как в ТЗ, в `config_pins.h` помечены ⚠.

1. **SD-карта на GPIO 35/36/37 конфликтовала с Octal PSRAM.** На плате N16R8 эти линии заняты внутренней шиной
   PSRAM (на фото подписаны SPIIO6 / SPIIO7 / SPIDQS). **Решено переносом:** SCK=GPIO10, MOSI=GPIO8, MISO=GPIO15,
   CS=GPIO38 (`config_pins.h`). GPIO 38 на плате подписан как BULTIN LED; код его не использует. Перенос проверить на железе.
2. **GPIO 39–42 — JTAG (MTCK/MTDO/MTDI/MTMS).** Используются для TPA mute (39) и энкодера (40–42).
   Аппаратная отладка через JTAG на этих пинах будет недоступна.
3. **GPIO 3 — strapping/JTAG-источник** (ИК-приемник TSOP). **GPIO 45 — strapping VDD_SPI** (I2C SCL для BH1750):
   внешняя подтяжка к 3.3 В может повлиять на загрузку. Рекомендуется проверка на железе.
4. **ADC-клавиатура (GPIO 1, 12 дБ аттенюация):** верхние кнопки близки к насыщению АЦП; разброс уровней
   ~120–160 мВ при допуске ±50 мВ. Требуется проверка на железе.

## Противоречия ТЗ, которые сейчас решены по умолчанию (подлежат подтверждению)

| Вопрос | Решение в коде | Причина |
|---|---|---|
| Полярность mute УМЗЧ | `g_amp_mute_polarity = 0`: HIGH = mute | **Подтверждено владельцем.** Тома 1 и 5 — HIGH = mute; Том 4 описывает Active LOW. Переключается через `SET_CONFIG AMP_MUTE_POLARITY`. |
| Standby: пробуждение по VBUS | **Light sleep, сессия PWA сохраняется.** Deep sleep не используется; CPU до 40 МГц, Wi-Fi modem sleep | **Подтверждено владельцем.** Deep sleep отключает Wi-Fi и вызывает холодный старт (5–10 с). Флаг `USB_AUTOWAKE` хранится, но на deep sleep не влияет. |

## Отклонения от ТЗ (сознательные)

- **DSP:** собственный DF2T-биквад на C с коэффициентами RBJ peaking EQ. Подпись `dsps_biquad_gen` в esp-dsp
  не проверена; `dsps_biquad_f32_ae32` — целевая замена для оптимизации.
- **Стерео в биквадах:** в ТЗ интерливед стерео обрабатывается как моно с общим состоянием. Исправлено: раздельные `w_left` / `w_right` на каждый канал.
- **Dynamic Headroom:** в ТЗ gain записывается в `coeffs[i][4]` (слот a2). Исправлено: отдельные массивы gain.
- **IR (RMT RX):** в ТЗ использованы `num1/num2` и `rmt_receive` с NULL-буфером — это неверно для ESP-IDF v5.x.
  Исправлено: `duration0/duration1`, валидный буфер; ISR копирует фрейм в очередь, задача Core 1 пишет в NVS
  (ESP_LOG и NVS в ISR недопустимы).
- **Инициализация I2S:** в ТЗ `i2s_channel_init_std_match` (не существует). Используется `i2s_channel_init_std_mode`.
- **HTTP:** один экземпляр `httpd` на порту 80 вместо двух (DLNA и web backend конфликтовали бы).
- **PWA:** один vanilla-JS файл, без Vue/Preact и без шага сборки, для лимита 150 КБ.
- **WebSocket:** рассылка через `httpd_queue_work`; входящий JSON валидируется по типам и диапазонам.
- **Аудиоформаты:** 16/24/32-бит декодируются в float32 без апсэмплинга. Контейнер I2S фиксирован на 32 бита
  (Padded) для PCM5102A. Частота дискретизации переконфигурируется при смене формата; буфер очищается при смене формата и при seek.
  Выход ограничен ±1.0.
- **Anti-AD (Dynamic Headroom):** headroom = минус сумма положительных gain обоих слоев.
  Лимитер: мягкий порог 0.7 FS, агрессивный 0.4 FS, attack 0.25, release 0.00015/сэмпл, связанный стерео.
- **Громкость:** квадратичная кривая `(vol/100)^2`; mute принудительно ставит gain 0.
- **Тест-шум:** розовый шум Kellett около −12 dBFS, в обход DSP.
- **Спектр:** Hann 512 точек, 64 лог-полосы 20 Гц–20 кГц, шкала −80..0 дБ; rise 0.85, fall 0.12, peak hold падает на 1 за кадр.
- **Энкодер:** PCNT 4x, 4 отсчета на щелчок; нажатие → PLAY после 3 низких чтений. Клавиатура: допуск ±50 мВ, debounce 2 чтения.
- **IR-самообучение:** хэш FNV-1a по длительностям, округленным до 100 мкс; 9 слотов; NVS `omnia_ir` / `templates`;
  импульсы короче 20 мкс отбрасываются, паузы длиннее 100 мс завершают фрейм.
- **Дисплей:** ST7789 320×240, BGR, invert on, swap_xy on, mirror on — **проверить на панели**.
  Подсветка LEDC 12 бит, 5 кГц, по умолчанию 80%.

## Статус реализации

Легенда: ✅ реализовано в коде · 🟡 частично / заглушка · ❌ не реализовано · ⚠ не собрано и не проверено на железе.
**Все пункты ⚠ — код не компилировался.**

| Модуль / функция | Статус |
|---|---|
| Структура проекта, CMake, partitions, sdkconfig, CI | ✅ ⚠ |
| Карта GPIO (`config_pins.h`) | ✅ (конфликты — см. выше) |
| Кольцевой буфер 4 МБ PSRAM | ✅ ⚠ |
| I2S0 bit-perfect, PCM5102A, 32-бит Padded | ✅ ⚠ |
| Декодирование 16/24/32 бит в float32 | ✅ ⚠ |
| Декодеры FLAC/ALAC/APE/WavPack/MP3/AAC | ❌ |
| DSP (2 слоя, Dynamic Headroom, Anti-AD) | ✅ ⚠ (свой биквад, не esp-dsp) |
| FFT 512 + 64 полосы | ✅ ⚠ |
| DLNA DMR (`/dlna/desc.xml`, POST `/dlna/stream`) | 🟡 (нет SSDP и SOAP) |
| Радио: поиск (Radio Browser API с телефона) | ✅ ⚠ |
| Радио: воспроизведение HTTP-потока на устройстве | ❌ (`PLAY_URL` сейчас только логируется) |
| Медиатека SD / индекс | ❌ (SD — пины конфликтуют, см. выше) |
| USB MSC | ❌ (заглушка) |
| USB MUX и VBUS | ✅ ⚠ |
| AirPlay, SMB, Bluetooth, UAC1 | ❌ |
| LVGL 9 на ST7789 (double buffer, DMA) | ✅ ⚠ |
| Экран плеера 320×240 | ✅ ⚠ |
| Спектр-визуализатор (rise/fall, peak hold) | ✅ ⚠ |
| Энкодер PCNT, ADC-клавиатура | ✅ ⚠ |
| IR RMT RX (самообучение, NVS) | ✅ ⚠ |
| RGB-кольцо WS2812B (RMT TX) | ❌ |
| Wi-Fi STA | ✅ ⚠ |
| httpd: WS `/ws`, JSON-команды, рассылка статуса | ✅ ⚠ |
| PWA (4 вкладки, WS, DSP-студия, Room Wizard, OTA) | ✅ ⚠ |
| Room Correction Wizard (микрофон браузера) | 🟡 (нужен HTTPS или localhost; калибровка по пиковым резонансам) |
| BH1750 (освещенность) | ✅ ⚠ |
| Питание УМЗЧ: реле → XSMT → mute TPA, задержки | ✅ ⚠ |
| Standby с deep sleep и VBUS-пробуждением | ❌ (не нужен по решению владельца) |
| Standby light sleep, Wi-Fi modem sleep, CPU 40 МГц | 🟡 ⚠ (задержка отклика зависит от DTIM роутера, не измерена) |
| OTA через `/api/ota_upgrade` | ✅ ⚠ (плавность звука во время записи не проверена) |

## Открытые вопросы к владельцу проекта

1. Проверить на железе новые пины SD (10/8/15/38), в частности BULTIN LED на GPIO 38.
2. Проверить на железе: ориентацию ST7789 (mirror / swap_xy), уровни ADC-клавиатуры, strapping GPIO 3 и 45.
3. Измерить задержку отклика PWA в standby (light sleep + Wi-Fi modem sleep).

## Лицензия

Не указана. Добавьте файл `LICENSE` по согласованию с автором.
