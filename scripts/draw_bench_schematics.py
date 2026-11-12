#!/usr/bin/env python3
"""Generate current Russian bench wiring diagrams; requires Pillow."""
from PIL import Image, ImageDraw, ImageFont
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
F = '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'
FB = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
font = lambda size, bold=False: ImageFont.truetype(FB if bold else F, size)
INK, BLUE, GREEN, RED, GREY = '#111827', '#175f9b', '#006c50', '#a92121', '#525b67'


def setup(title, subtitle):
    im = Image.new('RGB', (2200, 1500), '#ffffff')
    d = ImageDraw.Draw(im)
    d.rectangle((0, 0, 2200, 108), fill='#102a43')
    d.text((55, 16), title, font=font(41, True), fill='white')
    d.text((58, 122), subtitle, font=font(27), fill=RED)
    return im, d


def box(d, xy, title, fill='#eff6fc'):
    d.rounded_rectangle(xy, radius=15, fill=fill, outline=INK, width=3)
    x, y, _, _ = xy
    d.text((x+25, y+18), title, font=font(30, True), fill=INK)


def line(d, xy, color=BLUE):
    d.line(xy, fill=color, width=5)


def txt(d, xy, text, size=25, color=INK, bold=False):
    d.text(xy, text, font=font(size, bold), fill=color, spacing=10)


def first():
    im, d = setup('OMNIA • ЭТАПЫ 1–5 • СТЕНДОВОЕ ЯДРО',
                  'УМЗЧ, реле, динамики, USB-A, BT и дисплей пока отключены. Не подключать сеть 230 В.')
    box(d, (55, 190, 695, 870), 'AT32F403A • WeAct BlackPill')
    box(d, (1500, 190, 2145, 590), 'ESP32-S3 • соединить позже')
    box(d, (1500, 650, 2145, 1080), 'PCM5102A • после проверки платы')
    for y, left, right in [(285, 'PA9   UART TX', 'GPIO8  UART RX'),
                            (345, 'PA10  UART RX', 'GPIO7  UART TX'),
                            (405, 'PB12  WS / LRCK', 'GPIO6  WS'),
                            (465, 'PB13  BCLK', 'GPIO4  BCLK'),
                            (525, 'PB14  I²S RX', 'GPIO5  I²S DATA OUT')]:
        txt(d, (95, y), left, 26); line(d, [(555,y+16),(1475,y+16)])
        # Right GPIO5 row is outside box: make ESP box tall enough below.
        txt(d, (1530,y), right, 26)
    for y, left, right in [(725, 'PB12  LRCK', 'LCK / LRCK'), (785, 'PB13  BCLK', 'BCK'),
                            (845, 'PB15  I²S TX', 'DIN')]:
        # AT32 labels below original box are still clear in an external legend.
        txt(d, (760, y-13), left, 26); line(d, [(1050,y+4),(1475,y+4)])
        txt(d, (1530,y-13), right, 26)
    txt(d, (1530, 930), 'GND → общая земля\nSCK → GND: только после сверки платы\nPB0 → XSMT: после проверки перемычки', 24)
    box(d, (55, 930, 690, 1265), 'Отдельная диагностика UART', '#f6f8fa')
    txt(d, (80, 1000), 'USB-UART 3,3 В, не 5 В!\nTX → PA10     RX ← PA9\nGND ↔ GND      921600 8N1\nНе подключать вместе с GPIO7/8.', 25)
    box(d, (760, 1105, 2145, 1265), 'Питание и программирование', '#fff9e8')
    txt(d, (785, 1160), 'Каждая плата получает правильное питание отдельно; GND общий.\nSWD: PA13 ↔ SWDIO, PA14 ↔ SWCLK, GND и Vref 3,3 В.', 25)
    txt(d, (55, 1300), 'Сначала отдельно проверить платы и SRAM. Затем UART → I²S → ЦАП.\nРазводка модулей и питание DAC требуют проверки; не соединять 5 В источники наугад.', 27, RED, True)
    txt(d, (55, 1430), 'Рис. 1  •  Пошаговые инструкции: docs/START_HERE_RU.md  •  Не испытано на реальном устройстве', 22, GREY)
    im.save(ROOT/'docs/OMNIA_WIRING_STAGE1_CORE.png')


def second():
    im, d = setup('OMNIA • УПРАВЛЕНИЕ УСИЛИТЕЛЕМ • ПРОЕКТ',
                  'НЕ МОНТАЖНАЯ СХЕМА: сначала определить плату УМЗЧ, питание, реле и согласование PB4.')
    box(d, (55, 205, 1010, 665), 'AT32F403A • назначение сигналов')
    txt(d, (85, 282), 'PB0  ─────  XSMT ЦАПа PCM5102A\n\nPB2  ─────  драйвер катушки K2 питания УМЗЧ\n\nPB4  ─────  согласование → MUTE / RESET / CMUTE\n\nPB8  ─────  драйвер сигнального K1 (необязательно)', 29)
    box(d, (1100, 205, 2145, 665), 'K1 и K2: разные задачи', '#f5faf5')
    txt(d, (1130, 282), 'K1  разрывает L/R линейного выхода ЦАПа.\n       Можно не ставить, если щелчков нет.\n\nK2  управляет питанием УМЗЧ через\n       специально рассчитанный силовой узел.\n\nНи K1, ни цифровой DSP не защищают\nакустические системы при аварии УМЗЧ.', 27)
    box(d, (55, 715, 1010, 1190), 'Катушка K2: принцип, НЕ готовый расчёт', '#fff9e8')
    txt(d, (85, 785), '+V катушки ───── катушка K2 ───── C ключа\n\nPB2 ── резистор ── B   NPN   E ── GND\n                      │\n                 резистор\n                      │\n                    GND', 26)
    txt(d, (85, 1050), 'Диод параллельно катушке: катод к +V,\nанод к коллектору. Номиналы, тип и распиновку\nвыбрать после определения катушки и транзистора.', 24, RED)
    box(d, (1100, 715, 2145, 1190), 'PB4: зависит от схемы конкретного УМЗЧ', '#fff3f3')
    txt(d, (1130, 790), 'TPA3116 SDZ       MUTE = LOW\nTPA3116 MUTE     MUTE = HIGH\nTPA3221 RESET   MUTE = LOW\nTPA3221 CMUTE  LOW / высокое сопротивление\n\nCMUTE: штатный 33 нФ; не ставить 10 кОм\nна землю. Согласование и защита от обратного\nпитания требуют расчёта под конкретную плату.', 25)
    txt(d, (55, 1230), 'Сеть 230 В и коммутируемые контакты K2 здесь намеренно не показаны.\nДля AC/DC нагрузки учитываются пусковой ток, изоляция, защита и номиналы контактов.', 27, RED, True)
    txt(d, (55, 1430), 'Рис. 2  •  Подробные ограничения: docs/AMP_CONTROL_SAFETY.md  •  Нужны фото платы УМЗЧ', 22, GREY)
    im.save(ROOT/'docs/OMNIA_SCHEMATIC_IC_TRANSISTORS.png')


if __name__ == '__main__':
    first()
    second()
