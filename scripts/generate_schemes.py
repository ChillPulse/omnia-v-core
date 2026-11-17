#!/usr/bin/env python3
"""Render current Direct connection plan; not a verified PCB schematic."""
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.backends.backend_pdf import PdfPages
from matplotlib.patches import FancyBboxPatch, Rectangle
from pathlib import Path

plt.rcParams.update({'font.family':'DejaVu Sans','svg.fonttype':'none','pdf.fonttype':42})
D=Path(__file__).resolve().parents[1]/'docs'/'schemes';D.mkdir(parents=True,exist_ok=True)
BLUE='#143a58'; DARK='#162536'; CYAN='#047c8d'; RED='#a02524'; GREEN='#20704c'; GRAY='#516172'; GOLD='#9a6510'

def page(num,title,subtitle):
 fig,ax=plt.subplots(figsize=(16.54,11.69)); fig.subplots_adjust(0,0,1,1);ax.set_xlim(0,100);ax.set_ylim(0,100);ax.axis('off');
 ax.add_patch(Rectangle((0,93),100,7,color=BLUE));ax.text(3,96.5,title,color='white',fontsize=21,weight='bold',va='center');
 ax.text(3,90.2,subtitle,color=RED,fontsize=11,weight='bold',va='center');ax.text(3,1.2,'OMNIA • Direct v2.2  |  Проектные соединения, без аппаратной приёмки  |  См. SCHEMES_INDEX_RU.md',fontsize=9,color=GRAY);ax.text(97,1.2,f'{num} / 5',ha='right',fontsize=9,color=GRAY);return fig,ax

def box(ax,x,y,w,h,title,fc='#f4f8fb',color=BLUE):
 ax.add_patch(FancyBboxPatch((x,y),w,h,boxstyle='round,pad=0.45,rounding_size=0.8',linewidth=1.1,edgecolor='#a9b9c7',facecolor=fc));ax.text(x+1.2,y+h-2.2,title,color=color,fontsize=14,weight='bold',va='top')

def text(ax,x,y,s,size=11,c=DARK,weight=None):ax.text(x,y,s,fontsize=size,color=c,va='top',weight=weight,linespacing=1.5)
def rows(ax,x,y,rows,step=3.6,size=11.3):
 for n,(a,b) in enumerate(rows):
  yy=y-n*step;ax.text(x,yy,a,fontsize=size,color=DARK,weight='bold',va='center');ax.text(x+18,yy,'→',fontsize=13,color=CYAN,weight='bold',va='center');ax.text(x+22,yy,b,fontsize=size,color=DARK,va='center')

pages=[]
fig,ax=page(2,'02  ЯДРО: ESP32-S3  ↔  AT32  →  PCM5102A','ПРОЕКТНЫЕ СЕТИ • Использовать по маркировке выводов, только после проверки питания и обвязки DAC')
box(ax,3,55,46,32,'A. UART · управляющие команды')
rows(ax,5,78,[('ESP GPIO7  TX','AT32 PA10  RX'),('AT32 PA9  TX','ESP GPIO8  RX'),('ESP GND','AT32 GND')],step=5)
text(ax,5,60,'Линии 3,3 В. При отдельном USB-UART для AT32\nотсоединить ESP от PA9/PA10: не объединять TX.',10)
box(ax,51,45,46,42,'B. I²S · один мастер, два приёмника')
rows(ax,53,78,[('AT32 PB12  WS','ESP GPIO6  WS  +  DAC LCK'),('AT32 PB13  BCK','ESP GPIO4  BCK +  DAC BCK'),('ESP GPIO5 DOUT','AT32 PB14  I²S RX'),('AT32 PB15 TX','DAC DIN'),('Общая GND','ESP GND + AT32 GND + DAC GND')],step=4.6,size=10.8)
text(ax,53,50,'AT32 master; ESP slave; DAC — приёмник. 24 полезных бита\nв 32-битных слотах. Стрелки показывают направление.',10)
box(ax,3,25,46,27,'C. PCM5102A · специальные входы',fc='#fff9ee',color=GOLD)
text(ax,5,47,'SCK → GND только после прозвонки обвязки;\nрежим PLL без MCLK — не подключать PA3.',11)
text(ax,5,39,'XSMT: отдельный провод в прежней сборке не\nпонадобился. PB0 → XSMT только после проверки\nперемычки и отсутствия жёсткого GND / 3,3 В.',10.5)
text(ax,5,28,'FMT: I²S = LOW. L/R — линейный выход, не динамики.',10.2,RED)
box(ax,51,25,46,16,'D. ST-Link clone · SWD')
text(ax,53,36,'SWDIO ↔ PA13   •   SWCLK ↔ PA14   •   GND ↔ GND\nNRST ↔ NRST при необходимости. 3,3 В подключать\nлишь если на клоне это вход Vref, а не выход питания.',10.5)
box(ax,3,5,94,16,'E. Питание и первая проверка',fc='#fff2f2',color=RED)
text(ax,5,16,'Организовать питание каждой платы по проверенной схеме; GND общая, но 5-вольтовые линии USB не объединять наугад.\nНе подавать 5 В на голый PCM5102A; вход питания модуля и его регулятор проверить. Следить за обратным питанием\nчерез GPIO при разных моментах включения. УМЗЧ, реле, динамики и 230 В на первом этапе отключены.',10.5)
pages.append(('01_direct_core',fig))
fig,ax=page(3,'03  ESP32-S3: НОСИТЕЛИ И ОРГАНЫ УПРАВЛЕНИЯ','ПРОЕКТНЫЕ НАЗНАЧЕНИЯ • Проверить ревизии плат, питание, уровни и подтяжки перед подключением')
box(ax,3,56,46,31,'A. microSD · SPI')
rows(ax,5,78,[('GPIO15  MOSI','microSD DI / MOSI'),('GPIO16  SCK','microSD CLK / SCK'),('GPIO17  MISO','microSD DO / MISO'),('GPIO18  CS','microSD CS'),('GND','GND носителя')],step=3.6,size=11)
text(ax,5,59,'Питание модуля и уровни SPI: 3,3 В; тип адаптера\nи его преобразователи уровней проверить.',10)
box(ax,51,56,46,31,'B. BH1750 · I²C')
rows(ax,53,78,[('GPIO2  SDA','BH1750 SDA'),('GPIO39  SCL','BH1750 SCL'),('GND','BH1750 GND'),('3,3 В','питание после проверки модуля')],step=3.6,size=10.8)
text(ax,53,59,'SDA и SCL: суммарные подтяжки к 3,3 В; исходный\nориентир 4,7 кОм, учитывая уже стоящие на модулях.',10)
box(ax,3,29,46,23,'C. VS1838B · приём ИК')
text(ax,5,47,'ESP 3,3 В → 100 Ω → VCC приёмника\nESP GND → GND; OUT → GPIO40\n100 нФ + 4,7 мкФ: между VCC и GND рядом с ИК.\nПорядок ножек брать по даташиту своего корпуса.',10.5)
text(ax,5,32,'Не питать выход 5 В при подключении к GPIO.',10.2,RED)
box(ax,51,29,46,23,'D. Энкодер и 7 кнопок')
text(ax,53,47,'Энкодер A → GPIO41; B → GPIO42; кнопка → GPIO0.\nКнопку и подтяжку на 3,3 В/GND проверить; GPIO0 —\nBOOT strap: не удерживать при включении питания.\nКлавиатура: узел резистивной лестницы → GPIO1;\nноминалы и допуски — до расчёта, не монтажная схема.',10.5)
box(ax,3,5,94,20,'E. RGB и освобождённые ресурсы',fc='#fff9ee',color=GOLD)
text(ax,5,20,'GPIO48 → проектируемый согласователь → DIN кольца 12× WS2812B. Отдельно рассчитать питание/ток и уровни.\nНа YD GPIO48 уже связан со штатным RGB: не считать цепь «1 + 12» автоматически последовательной.\nGPIO35–37 заняты Octal PSRAM; GPIO43/44 — UART0; GPIO0/3/45/46 — проверять уровни при старте.',10.5)
pages.append(('02_esp_peripherals',fig))
fig,ax=page(4,'04  USB, ЦАП И УСИЛИТЕЛЬ: ГРАНИЦЫ ПОДКЛЮЧЕНИЯ','ЭТИ УЗЛЫ НЕ ПРОШЛИ АППАРАТНУЮ ПРОВЕРКУ • Силовая часть и конкретный вход УМЗЧ требуют отдельной схемы')
box(ax,3,55,46,32,'A. Два разных USB-контроллера')
text(ax,5,81,'ESP GPIO19 D− / GPIO20 D+ → Host MSC\nЛинии уже на штатном native USB-C платы YD.\nНе вешать второе гнездо параллельно без схемы.\nПитание VBUS, ток, защита, CC и маршрутизация\nопределяются после выбора топологии.',10.4)
text(ax,5,65,'AT32 PA11 D− / PA12 D+ → USB-DAC UAC1\nШтатный USB-C может быть связан с 5 В платы;\nдо подключения проверить трассы и исключить\nвстречное питание. Оба USB ещё не готовы.',10.4)
box(ax,51,55,46,32,'B. Детектор VBUS · проект, не готовый монтаж')
text(ax,53,81,'USB-DAC VBUS (+5 В) → R1 10 кΩ → точка S → PB9\nТочка S → R2 15 кΩ → GND\nТочка S → C1 100 нФ → GND',11)
text(ax,53,68,'При 5,0 В расчётно S ≈ 3,0 В; допуски, диапазон\nVBUS, ток утечки и состояние выключенного AT32\nпроверить отдельно. Нельзя считать VBUS на\nштатном USB-C изолированным от 5 В платы.\nОбщая GND ≠ гальваническая изоляция.',10.3,RED)
box(ax,3,27,46,24,'C. Аналоговый выход PCM5102A')
text(ax,5,45,'Модуль LOUT → линейный вход L усилителя\nМодуль ROUT → линейный вход R усилителя\nАналоговая земля по схеме конкретного устройства.\nОпциональный K1 (PB8) — сигнальное реле L/R.\nОно не защищает акустику от аварии УМЗЧ.',10.4)
text(ax,5,30,'Усилитель подключать только после отдельной проверки.',10.1,RED)
box(ax,51,27,46,24,'D. Силовое реле и Mute — независимы')
text(ax,53,45,'PB2 → рассчитанный драйвер → катушка K2\nK2 управляет питанием УМЗЧ; GPIO ≠ катушка!\nPB4 → согласующий узел → MUTE / SDZ / RESET\nPB8 → драйвер необязательного сигнального K1\nPB0 → XSMT DAC при подходящей обвязке.',10.4)
text(ax,53,30,'Полярность и обвязка зависят от конкретной платы.',10.1,RED)
box(ax,3,5,94,18,'E. Почему нет универсальной схемы силового узла',fc='#fff2f2',color=RED)
text(ax,5,17,'Для K2 и PB4 нужны параметры реальной катушки, платы УМЗЧ и входов. CMUTE, MUTE, SDZ и RESET не взаимозаменяемы.\nДля питания 230 В необходимы изоляция, защита и расчёт контактов с учётом пускового тока.\nРеле K1 и программный Mute не заменяют независимую защиту динамиков от аварии усилителя.',10.4)
pages.append(('03_usb_amplifier',fig))
fig,ax=page(5,'05  ДИСПЛЕЙ I80 И КОНТРОЛЬНЫЕ ТОЧКИ','НАЗНАЧЕНИЯ В ПРОЕКТЕ • Не паять дисплей по старому рисунку: распиновка FFC и режимы не подтверждены')
box(ax,3,44,46,43,'A. Дисплей ST7789 · карта сигналов ESP')
text(ax,5,80,'D0 GPIO9    D1 GPIO10    D2 GPIO11    D3 GPIO12\nD4 GPIO13  D5 GPIO14    D6 GPIO21    D7 GPIO47\nWR GPIO38  •  DC GPIO46  •  RST GPIO3  •  BL GPIO45\nCS и RD: аппаратное назначение требует проверки.',11)
text(ax,5,66,'Подписи на фото модуля не устанавливают надёжно\nнумерацию/ориентацию FFC, распиновку подсветки\nи электрический режим. Подтвердить IM0/1/2, трассы,\nпитание, тип шлейфа и схему до пайки.',10.5,RED)
text(ax,5,52,'GPIO3/45/46 — strap; проверить уровни на старте.\nГотового драйвера I80 и теста экрана ещё нет.',10.5)
box(ax,51,44,46,43,'B. Контроль связки до усилителя')
text(ax,53,80,'1  Питание отключено: проверить GND и перемычки.\n2  Подать корректное питание; измерить напряжения\n    и отсутствие обратного питания при выключениях.\n3  Проверить UART и статусы, без УМЗЧ и K2.\n4  При 48 кГц и 2 × 32-бит: LRCK ≈ 48 кГц,\n    BCLK ≈ 3,072 МГц; сравнить оба приёмника.\n5  Проверить данные PB14 и PB15, L/R ЦАПа и\n    штатное состояние XSMT конкретного модуля.\n6  Только затем поэтапно подключать остальные узлы.',10.6)
box(ax,3,21,94,19,'C. Действительные сведения против предположений',fc='#fff9ee',color=GOLD)
text(ax,5,34,'Требование Direct и номера GPIO сверены с текущим ТЗ / shared/omnia_pins.h. Клоки, питание, обвязка,\nустойчивость DMA, качество звука, безопасный mute и работа выбранного ST-Link не испытаны на устройстве.\nBluetooth и его распиновка/прошивка отложены до фото модуля; PA3 — резерв под будущий MCLK.',10.5)
box(ax,3,5,94,12,'D. Ссылки на подробную ведомость и решения',fc='#f4f8fb')
text(ax,5,11,'SCHEMES_INDEX_RU.md • CORE_SIGNAL_NETS_RU.md • AMP_CONTROL_SAFETY.md • START_HERE_RU.md',10.5)
pages.append(('04_display_testing',fig))
# Core connection graphic: lines show wires; repeated PB12/PB13 are the same pin/net.
fig,ax=page(1,'01  СХЕМА СОЕДИНЕНИЙ ЦИФРОВОГО ЯДРА','ПРОЕКТ • ESP32-S3 + WeAct AT32 + GY-PCM5102A; до подключения проверить питание и перемычки DAC')
for x,w,title,sub in [(3,25,'ESP32-S3','I²S slave'),(39,24,'AT32F403A','I²S master'),(77,20,'PCM5102A','модуль DAC')]:
 box(ax,x,42,w,44,title);text(ax,x+1.2,79,sub,10,GRAY)
left=[(74,'GPIO7  TX'),(70,'GPIO8  RX'),(64,'GPIO6  WS'),(60,'GPIO4  BCLK'),(56,'GPIO5  DATA'),(44,'GND')]
midleft=[(74,'PA10  RX'),(70,'PA9  TX'),(64,'PB12  WS'),(60,'PB13  BCLK'),(56,'PB14  RX'),(44,'GND')]
midright=[(64,'WS  PB12'),(60,'BCLK  PB13'),(52,'TX  PB15')]
right=[(64,'LCK'),(60,'BCK'),(52,'DIN'),(44,'GND')]
for y,t in left:ax.text(26.8,y,t,ha='right',va='center',fontsize=10.5,weight='bold',color=DARK)
for y,t in midleft:ax.text(40.2,y,t,va='center',fontsize=10.5,weight='bold',color=DARK)
for y,t in midright:ax.text(61.7,y,t,ha='right',va='center',fontsize=10.5,weight='bold',color=DARK)
for y,t in right:ax.text(78.2,y,t,va='center',fontsize=10.5,weight='bold',color=DARK)
for y,d in [(74,1),(70,-1),(64,-1),(60,-1),(56,1)]:
 a,b=((28.1,y),(38.8,y)) if d==1 else ((38.8,y),(28.1,y))
 ax.annotate('',xy=b,xytext=a,arrowprops={'arrowstyle':'-|>','lw':1.8,'color':CYAN})
for y in (64,60,52):ax.annotate('',xy=(76.8,y),xytext=(63.1,y),arrowprops={'arrowstyle':'-|>','lw':1.8,'color':CYAN})
for x in (15.5,51,87):ax.plot([x,x],[42,38],color=GREEN,lw=1.8)
ax.plot([15.5,87],[38,38],color=GREEN,lw=1.8);ax.text(51,36.7,'ОБЩАЯ СИГНАЛЬНАЯ ЗЕМЛЯ',ha='center',va='top',fontsize=10,weight='bold',color=GREEN)
box(ax,3,18,94,14,'Важные условия',fc='#fff9ee',color=GOLD)
text(ax,5,27,'PB12 и PB13 показаны с двух сторон AT32 для наглядности: каждый — одна физическая сеть с двумя приёмниками.\nSCK → GND только после проверки платы; PB0 → XSMT — только при совместимой обвязке; PA3 не подключать к этому DAC.',10.8)
box(ax,3,5,94,10,'Питание и безопасность',fc='#fff2f2',color=RED)
text(ax,5,10,'Источники питания не изображены: проверить допустимые входы плат и обратное питание; не объединять USB +5 В наугад.',10.5)
pages.insert(0,('00_core_wiring',fig))

with PdfPages(D/'OMNIA_CONNECTIONS_DIRECT_RU.pdf') as pdf:
 for name,fig in pages:
  pdf.savefig(fig,facecolor='white')
  svg = D/(name+'.svg')
  fig.savefig(svg,facecolor='white')
  svg.write_text('\n'.join(line.rstrip() for line in svg.read_text(encoding='utf-8').splitlines())+'\n',encoding='utf-8')
  fig.savefig(D/(name+'.png'),dpi=120,facecolor='white')
  plt.close(fig)
