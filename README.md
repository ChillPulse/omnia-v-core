# OMNIA Flagship Streamer & Studio DSP v2.0 (`omnia-v-core`)

Единый монорепозиторий прошивок двухпроцессорного флагманского аудиостримера, сетевого транспорта и студийного аудиопроцессора **OMNIA**:

- **Ядро 1 — `ESP32-S3-N16R8` (`[env:esp32s3_omnia]`, чистый `ESP-IDF v5.x`, `-O3 -flto`, строго без Arduino):**
  - **Core 1:** изолированный ведомый аудио-движок `I2S0 Slave TX` (32-bit Phillips, нулевой джиттер от мастера AT32), статические пулы в 8 МБ Octal PSRAM (`1.5 МБ` сжатый поток + `2.5 МБ` PCM) + `24 КБ` DMA во внутренней SRAM, Gapless / Web-Radio / USB OTG Hot-Switch (`Host MSC ↔ Device UAC1`).
  - **Core 0:** протокольный мастер `OMNIA v2` (`UART1` 921600 бод, `0xAA55 + CRC16-CCITT`, SEQ/ACK/Retry), дисплей `TENSTAR 2.8" ST7789` по **8-битной параллельной шине I80 DMA** (60 FPS), самообучаемый универсальный ИК-пульт (`RMT RX`), кольцо `12× WS2812` с биометрическим пульсом Standby **«lub-dub»** и сверхлёгкий смартфонный Web-UI (PWA).
- **Ядро 2 — `WeAct AT32F403ACGU7 BlackPill` (`[env:at32f403_dsp]`, `at32firmlib`, Cortex-M4F 240 МГц + FPU):**
  - Автоматическое безопасное раскрытие **224 КБ внутренней SRAM** (`EOPB0 = 0xFE`) при первом включении чипа.
  - Тактовый мастер `I2S2 + I2S2EXT` Full-Duplex DMA (`44.1 / 48 кГц` Bit-Perfect + опциональный `MCLK 256×Fs` на `PA3`), бесщёлчковая автоматика (`XSMT`, `AMP_MUTE`, `RELAY`), автопереключатель USB «на лету» (`FSUSB30MUX + AO3401 + VBUS_DET`).
  - Универсальный драйвер **любой 3.3V SPI NOR Flash 25-й серии** на площадке `PA4..PA7` (`JEDEC 0x9F + JESD216 SFDP 0x5A`) с прозрачным резервом во внутреннюю Flash AT32 (если внешняя микросхема не припаяна).
  - Полный **9-звенный 32-bit Float студийный DSP** с плавным сглаживанием коэффициентов без щелчков и честным режимом **`Bit-Perfect Direct`** (побитовая передача 1:1):
    1. `Input Sanitizer & Subsonic DC HPF (15–30 Гц)` + `Dynamic Headroom`
    2. `Room Acoustic PEQ` (5 полос RBJ DF2T + импорт REW)
    3. `User Master PEQ` (10 полос RBJ DF2T)
    4. `Anti-Ad 2.0 Smooth Limiter/AGC` (для веб-радио)
    5. `Psychoacoustic Bass (VBB)` (генератор чётных/нечётных гармоник баса для любой акустики)
    6. **Флагманский процессор сцены `Studio AETHER`** (12 параметров в реальном времени: `Master Blend`, `Stereo Width`, `Center Voice Anchor`, `Sub-Bass Mono Lock`, `Side HPF`, `Silk Air Exciter`, `Dynamic De-Harsh Guard`, `3D All-Pass Stage Depth`, `Triode Even Harmonics`, `Headphone Crossfeed`)
    7. `Dynamic Loudness (ISO 226)` + сглаженная громкость + **мгновенное сравнение `A/B Instant Compare`** с автовыравниванием субъективной громкости
    8. `Look-Ahead True-Peak Limiter` (`1.5 мс` линия предпросмотра в расширенной SRAM) + `DC Offset Guard` + `24-bit TPDF Dither`

---

## Документация и аппаратная схема

- Полное единое русскоязычное ТЗ и таблица всех пинов: [`docs/OMNIA_MASTER_SPEC_v2.0.md`](docs/OMNIA_MASTER_SPEC_v2.0.md)
- Иллюстрированная памятка переделки вашего экрана `TENSTAR ROBOT 2.8" ST7789` под 8-битную шину 8080 (`#1..#16`): [`docs/ST7789_8080_8P_MOD_GUIDE.png`](docs/ST7789_8080_8P_MOD_GUIDE.png)

---

## Быстрый старт в VS Code + PlatformIO

1. Установите расширение **PlatformIO IDE** в **VS Code**.
2. Склонируйте репозиторий и откройте папку `omnia-v-core`:
   ```bash
   git clone https://github.com/ChillPulse/omnia-v-core.git
   cd omnia-v-core
   code .
   ```
3. В нижней синей панели VS Code выберите нужное окружение (`Project Environment Switcher`) или используйте терминал PlatformIO:
   - **Сборка и прошивка DSP-ядра `AT32F403ACGU7`** (через CMSIS-DAP / DAPLink или ST-Link V2):
     ```bash
     pio run -e at32f403_dsp -t upload
     ```
     *(Если у вас программатор ST-Link V2 вместо CMSIS-DAP, поменяйте в `platformio.ini` строку `upload_protocol = cmsis-dap` на `upload_protocol = stlink`).*
   - **Сборка и прошивка стримера `ESP32-S3-N16R8`** (по кабелю USB):
     ```bash
     pio run -e esp32s3_omnia -t upload -t monitor
     ```

Также в разделе **Actions** на GitHub при каждом коммите автоматически прогоняются математические модульные тесты DSP/протокола и собираются готовые бинарные артефакты обеих плат (`omnia-at32f403-dsp-firmware` и `omnia-esp32s3-streamer-firmware`).
