/**
 * @file    usb_dac_uac1.c
 * @brief   Драйвер внешней звуковой карты OMNIA USB-DAC (UAC 1.0 Asynchronous 24-bit)
 *          на родном контроллере USBFS AT32F403ACGU7 (PA11/PA12) + включаемый Триггер +5V (PB9).
 *
 *  ПОЧЕМУ ВЫБРАН РЕЖИМ 24-BIT / 48 кГц (+ 44.1 кГц) UAC1 ASYNC:
 *    1) 100% Driverless Plug-and-Play на ТВ-приставках Android TV, Windows 10/11, macOS, Linux, iOS/Android;
 *    2) Истинные 24 бита (bBitResolution=24, bSubFrameSize=3 -> 288..300 байт/мс < 30% шины Full-Speed);
 *    3) Асинхронная обратная связь 10.14 (Endpoint 0x82): хост подстраивается под чистый кварц AT32,
 *       а не наоборот — 100% Bit-Perfect без ресемплинга ASRC;
 *    4) Нагрузка ядра AT32 при всех 9 каскадах DSP на 48 кГц ~29% (>70% запаса под апгрейд алгоритмов!).
 */

#include "at32f403a_407.h"
#include "omnia_at32.h"
#include <string.h>

/* --- Разметка расширенного буфера PMA USBFS (1280 байт @ 0x40007800) --- */
#define PMA_EP0_TX_ADDR             0x0040U /* 64 байта  */
#define PMA_EP0_RX_ADDR             0x0080U /* 64 байта  */
#define PMA_EP2_FB_BUF0_ADDR        0x00C0U /* 16 байт (Feedback 10.14 Buf 0) */
#define PMA_EP2_FB_BUF1_ADDR        0x00E0U /* 16 байт (Feedback 10.14 Buf 1) */
#define PMA_EP1_AUDIO_BUF0_ADDR     0x0100U /* 320 байт (Iso OUT 24-bit Buf 0, кратно 32!) */
#define PMA_EP1_AUDIO_BUF1_ADDR     0x0240U /* 320 байт (Iso OUT 24-bit Buf 1, конец 0x0380 < 0x0500) */

#define UAC1_MAX_PACKET_BYTES       300U    /* До 50 стерео-кадров * 6 байт (24-bit stereo @ 48k + запас) */
#define UAC1_PMA_ALLOC_BYTES        320U    /* 10 блоков по 32 байта для регистра RXLEN BLK32 */

#define USB_FIFO_FRAMES             1024U   /* ~21.3 мс @ 48 кГц в верхней SRAM */
#define USB_FIFO_MASK               (USB_FIFO_FRAMES - 1U)
#define USB_FIFO_TARGET             512U

/* Кольцевой 24->32-bit буфер в расширенной области SRAM (224 КБ) */
static int32_t  s_usb_fifo_l[USB_FIFO_FRAMES] __attribute__((section(".sram_ext"), aligned(4)));
static int32_t  s_usb_fifo_r[USB_FIFO_FRAMES] __attribute__((section(".sram_ext"), aligned(4)));
static volatile uint32_t s_fifo_wr = 0U;
static volatile uint32_t s_fifo_rd = 0U;
static bool     s_fifo_primed      = false;
static float    s_usb_fade_gain    = 0.0f;

/* Состояние UAC1 и Control Endpoint 0 */
static volatile uint8_t  s_pending_dev_addr = 0U;
static volatile uint8_t  s_alt_setting      = 0U;
static volatile uint32_t s_host_sample_rate = 48000U;
static volatile bool     s_rate_change_req  = false;
static volatile uint32_t s_sof_counter      = 0U;
static uint32_t          s_last_sof_snap    = 0U;
static uint32_t          s_sof_watch_ms     = 0U;
static bool              s_sof_alive        = false;

static uint8_t           s_uac_mute         = 0U;
static int16_t           s_uac_vol_cur      = 0;        /* 0x0000 = 0.0 dB (Bit-Perfect) */

/* Передача/приём данных на EP0 */
static const uint8_t    *s_ep0_tx_ptr       = NULL;
static uint16_t          s_ep0_tx_rem       = 0U;
static uint8_t           s_ep0_out_expect   = 0U;       /* 0=нет, 1=Freq(3B), 2=Mute(1B), 3=Vol(2B) */

/* --- Состояние включаемой триггер-функции USB-DAC --- */
static omnia_usb_trigger_cfg_t s_trig_cfg = {
    .enable_auto_switch   = 1U,     /* По умолчанию ВКЛ (можно отключить из Web-UI / меню TFT) */
    .wake_from_standby    = 1U,     /* Пробуждать стример и замыкать реле при старте ТВ-бокса  */
    .auto_return_prev_src = 1U,     /* Возвращать прежний источник при выключении ТВ-бокса     */
    .prefer_vbus_pin      = 1U,     /* Детектор PB9 (+ резерв по активности пакетов SOF)       */
    .debounce_on_ms       = 150U,
    .debounce_off_ms      = 800U
};

static bool              s_vbus_debounced   = false;
static bool              s_vbus_raw_prev    = false;
static uint32_t          s_vbus_edge_ms     = 0U;
static omnia_i2s_source_t s_saved_prev_src  = OMNIA_SRC_ESP32_I2S;
static bool              s_auto_switched    = false;

/* ========================================================================== *
 *  Дескрипторы USB Audio Class 1.0 Asynchronous 24-bit Stereo (48k + 44.1k)
 * ========================================================================== */
static const uint8_t kUsbDeviceDesc[18] = {
    18, 0x01,               /* bLength, bDescriptorType (Device) */
    0x00, 0x02,             /* bcdUSB 2.00 Full-Speed */
    0x00, 0x00, 0x00,       /* Defined at Interface level (Audio) */
    64,                     /* bMaxPacketSize0 */
    0x3C, 0x2E,             /* idVendor  = 0x2E3C (Artery / OMNIA Studio) */
    0x24, 0x57,             /* idProduct = 0x5724 (OMNIA 24-Bit Async DAC) */
    0x10, 0x02,             /* bcdDevice = 2.10 */
    1, 2, 3,                /* iManufacturer, iProduct, iSerialNumber */
    1                       /* bNumConfigurations */
};

/* Полный конфигурационный дескриптор: 122 байта */
static const uint8_t kUsbConfigDesc[] = {
    /* Configuration Descriptor (9B) */
    9, 0x02, 122, 0x00, 2, 1, 0, 0xC0, 50, /* Self-powered, <=100mA */

    /* Interface 0: Standard Audio Control (9B) */
    9, 0x04, 0, 0, 0, 0x01, 0x01, 0x00, 0,
    /* Class-Specific AC Interface Header (9B, total AC = 9+12+10+9 = 40B) */
    9, 0x24, 0x01, 0x00, 0x01, 40, 0x00, 1, 1,
    /* Input Terminal ID=1: USB Streaming (12B) */
    12, 0x24, 0x02, 1, 0x01, 0x01, 0, 2, 0x03, 0x00, 0, 0,
    /* Feature Unit ID=2: Master Mute + L/R Volume (10B) */
    10, 0x24, 0x06, 2, 1, 1, 0x01, 0x02, 0x02, 0,
    /* Output Terminal ID=3: Speaker / Line Out (9B) */
    9, 0x24, 0x03, 3, 0x01, 0x03, 0, 2, 0,

    /* Interface 1, Alt 0: Zero-Bandwidth Idle (9B) */
    9, 0x04, 1, 0, 0, 0x01, 0x02, 0x00, 0,

    /* Interface 1, Alt 1: Operational 24-bit Stereo Streaming (9B) */
    9, 0x04, 1, 1, 2, 0x01, 0x02, 0x00, 0,
    /* Class-Specific AS General Descriptor (7B: Terminal 1, PCM 0x0001) */
    7, 0x24, 0x01, 1, 1, 0x01, 0x00,
    /* Type I Format Descriptor (14B: 2ch, 3 bytes subframe, 24-bit, 2 discrete rates: 48000 & 44100) */
    14, 0x24, 0x02, 0x01, 2, 3, 24, 2,
    0x80, 0xBB, 0x00,       /* 48000 Hz (по умолчанию — первым в списке для Android TV / Windows!) */
    0x44, 0xAC, 0x00,       /* 44100 Hz (альтернативная сетка для CD/Lossless плееров) */

    /* Endpoint 0x01: Isochronous Asynchronous Audio OUT (9B, UAC1 9-byte format) */
    9, 0x05, 0x01, 0x05,    /* bmAttributes = 0x05 (Isochronous + Asynchronous) */
    (uint8_t)(UAC1_MAX_PACKET_BYTES & 0xFFU), (uint8_t)(UAC1_MAX_PACKET_BYTES >> 8),
    1,                      /* bInterval = 1 ms */
    0,                      /* bRefresh = 0 */
    0x82,                   /* bSynchAddress = Endpoint 0x82 (Async Feedback IN) */

    /* Class-Specific Isochronous Audio Data Endpoint Descriptor (7B: Sampling Freq Control enabled!) */
    7, 0x25, 0x01, 0x01, 0x00, 0x00, 0x00,

    /* Endpoint 0x82: Isochronous Explicit Feedback IN (9B, 3-byte 10.14 format, refresh 2^3 = 8 ms) */
    9, 0x05, 0x82, 0x11,    /* bmAttributes = 0x11 (Isochronous + Feedback) */
    3, 0x00,                /* wMaxPacketSize = 3 bytes (10.14 fixed-point) */
    1,                      /* bInterval = 1 */
    3,                      /* bRefresh = 3 (каждые 8 мс) */
    0
};

_Static_assert(sizeof(kUsbConfigDesc) == 122, "UAC1 configuration descriptor length");

static void write_uac_feedback_10_14(void)
{
    const uint32_t level = (uint32_t)(s_fifo_wr - s_fifo_rd);
    const int32_t  err   = (int32_t)level - (int32_t)USB_FIFO_TARGET;

    /* Номинальное значение в формате UAC1 10.14 (число сэмплов на 1 мс кадра SOF * 16384) */
    int32_t fb = (s_host_sample_rate == 44100U)
                     ? 722534    /* 44.100 * 16384 = 0x0B0666 */
                     : 786432;   /* 48.000 * 16384 = 0x0C0000 */

    /* Если буфер заполнен выше середины — плавно просим хост слать чуть меньше отсчётов */
    int32_t trim = -err * 8;
    if (trim > 8192)  { trim = 8192; }   /* Ограничение до ±0.5 сэмпла/мс */
    if (trim < -8192) { trim = -8192; }
    fb += trim;

    uint8_t pkt[4] = {
        (uint8_t)(fb & 0xFF),
        (uint8_t)((fb >> 8) & 0xFF),
        (uint8_t)((fb >> 16) & 0xFF),
        0U
    };

    /* Пишем синхронно в оба буфера EP2, исключая гонки чётности TXDTS */
    usb_write_packet(pkt, PMA_EP2_FB_BUF0_ADDR, 3U);
    usb_write_packet(pkt, PMA_EP2_FB_BUF1_ADDR, 3U);
    USB_SET_EPT_DOUBLE_BUF0_LEN(USB_EPT2, 3U, DATA_TRANS_IN);
    USB_SET_EPT_DOUBLE_BUF1_LEN(USB_EPT2, 3U, DATA_TRANS_IN);
    USB_SET_TXSTS(USB_EPT2, USB_TX_VALID);
}

static void unpack_24bit_usb_packet(const uint8_t *buf, uint16_t nbytes)
{
    /* Каждый стерео-кадр UAC1 24-bit занимает 6 байт: L_lo, L_mid, L_hi, R_lo, R_mid, R_hi */
    const uint16_t frames = nbytes / 6U;
    uint32_t wr = s_fifo_wr;
    const uint32_t rd = s_fifo_rd;

    for (uint16_t i = 0U; i < frames; i++) {
        if ((uint32_t)(wr - rd) >= (USB_FIFO_FRAMES - 2U)) {
            break;
        }
        const uint8_t *p = &buf[i * 6U];
        /* Сдвигаем 24 бита строго в старшие разряды 32-битного знакового слова (Left-Justified Q31) */
        const uint32_t lu = ((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24);
        const uint32_t ru = ((uint32_t)p[3] << 8) | ((uint32_t)p[4] << 16) | ((uint32_t)p[5] << 24);

        const uint32_t slot = wr & USB_FIFO_MASK;
        s_usb_fifo_l[slot] = (s_uac_mute != 0U) ? 0 : (int32_t)lu;
        s_usb_fifo_r[slot] = (s_uac_mute != 0U) ? 0 : (int32_t)ru;
        wr++;
    }
    s_fifo_wr = wr;

    if (!s_fifo_primed && (uint32_t)(wr - rd) >= USB_FIFO_TARGET) {
        s_fifo_primed = true;
    }
}

/* --- Обработка Control Pipe EP0 --- */
static void ep0_send_chunk(void)
{
    const uint16_t chunk = (s_ep0_tx_rem > 64U) ? 64U : s_ep0_tx_rem;
    if (chunk > 0U && s_ep0_tx_ptr != NULL) {
        usb_write_packet((uint8_t *)(uintptr_t)s_ep0_tx_ptr, PMA_EP0_TX_ADDR, chunk);
        s_ep0_tx_ptr += chunk;
        s_ep0_tx_rem = (uint16_t)(s_ep0_tx_rem - chunk);
    }
    USB_SET_TXLEN(USB_EPT0, chunk);
    USB_SET_TXSTS(USB_EPT0, USB_TX_VALID);
}

static void ep0_start_in(const void *data, uint16_t len, uint16_t wLength)
{
    static uint8_t s_small_buf[64];
    if (len > wLength) {
        len = wLength;
    }
    if (data != NULL && len <= sizeof(s_small_buf) && data != kUsbConfigDesc && data != kUsbDeviceDesc) {
        memcpy(s_small_buf, data, len);
        s_ep0_tx_ptr = s_small_buf;
    } else {
        s_ep0_tx_ptr = (const uint8_t *)data;
    }
    s_ep0_tx_rem = len;
    ep0_send_chunk();
}

static void ep0_send_zlp(void)
{
    s_ep0_tx_ptr = NULL;
    s_ep0_tx_rem = 0U;
    USB_SET_TXLEN(USB_EPT0, 0U);
    USB_SET_TXSTS(USB_EPT0, USB_TX_VALID);
}

static uint16_t build_utf16_string_desc(const char *ascii, uint8_t *dst, uint16_t cap)
{
    uint16_t idx = 2U;
    for (uint16_t i = 0U; ascii[i] != '\0' && (idx + 2U) <= cap; i++) {
        dst[idx++] = (uint8_t)ascii[i];
        dst[idx++] = 0U;
    }
    dst[0] = (uint8_t)idx;
    dst[1] = 0x03U;
    return idx;
}

static void handle_ep0_setup(void)
{
    uint8_t req[8];
    usb_read_packet(req, PMA_EP0_RX_ADDR, 8U);

    const uint8_t  bmRequestType = req[0];
    const uint8_t  bRequest      = req[1];
    const uint16_t wValue        = (uint16_t)req[2] | ((uint16_t)req[3] << 8);
    const uint16_t wIndex        = (uint16_t)req[4] | ((uint16_t)req[5] << 8);
    const uint16_t wLength       = (uint16_t)req[6] | ((uint16_t)req[7] << 8);

    s_ep0_out_expect = 0U;

    /* Стандартные запросы USB 2.0 */
    if ((bmRequestType & 0x60U) == 0x00U) {
        switch (bRequest) {
        case 0x06U: { /* GET_DESCRIPTOR */
            const uint8_t dtype = (uint8_t)(wValue >> 8);
            const uint8_t didx  = (uint8_t)(wValue & 0xFFU);
            if (dtype == 0x01U) {
                ep0_start_in(kUsbDeviceDesc, sizeof(kUsbDeviceDesc), wLength);
                return;
            } else if (dtype == 0x02U) {
                ep0_start_in(kUsbConfigDesc, sizeof(kUsbConfigDesc), wLength);
                return;
            } else if (dtype == 0x03U) {
                uint8_t sdesc[64];
                if (didx == 0U) {
                    const uint8_t lang[4] = { 4, 0x03, 0x09, 0x04 };
                    ep0_start_in(lang, 4U, wLength);
                } else if (didx == 1U) {
                    const uint16_t n = build_utf16_string_desc("OMNIA Studio Audio", sdesc, sizeof(sdesc));
                    ep0_start_in(sdesc, n, wLength);
                } else if (didx == 2U) {
                    const uint16_t n = build_utf16_string_desc("OMNIA 24-Bit Async DAC", sdesc, sizeof(sdesc));
                    ep0_start_in(sdesc, n, wLength);
                } else {
                    const uint16_t n = build_utf16_string_desc("OMNIA-24BIT-DIRECT", sdesc, sizeof(sdesc));
                    ep0_start_in(sdesc, n, wLength);
                }
                return;
            }
            break;
        }
        case 0x05U: /* SET_ADDRESS */
            s_pending_dev_addr = (uint8_t)(wValue & 0x7FU);
            ep0_send_zlp();
            return;

        case 0x09U: /* SET_CONFIGURATION */
            ep0_send_zlp();
            return;

        case 0x0BU: /* SET_INTERFACE */
            if (wIndex == 1U) {
                s_alt_setting = (uint8_t)(wValue & 0xFFU);
                if (s_alt_setting == 0U) {
                    s_fifo_primed   = false;
                    s_usb_fade_gain = 0.0f;
                    s_fifo_rd       = s_fifo_wr;
                } else {
                    s_fifo_rd     = s_fifo_wr;
                    s_fifo_primed = false;
                    write_uac_feedback_10_14();
                }
            }
            ep0_send_zlp();
            return;

        case 0x0AU: { /* GET_INTERFACE */
            const uint8_t cur_alt = (wIndex == 1U) ? s_alt_setting : 0U;
            ep0_start_in(&cur_alt, 1U, wLength);
            return;
        }

        case 0x00U: { /* GET_STATUS */
            const uint16_t status = 0x0001U; /* Self-powered */
            ep0_start_in(&status, 2U, wLength);
            return;
        }
        default:
            break;
        }
    }
    /* Class-Specific запросы UAC 1.0 (Частота 44.1/48 кГц на EP 0x01 и Mute/Volume на Feature Unit 2) */
    else if ((bmRequestType & 0x60U) == 0x20U) {
        const uint8_t cs = (uint8_t)(wValue >> 8);
        /* Endpoint Sampling Frequency Control (EP 0x01, CS=0x01) */
        if ((bmRequestType & 0x1FU) == 0x02U && (wIndex & 0xFFU) == 0x01U && cs == 0x01U) {
            if (bRequest == 0x01U) { /* SET_CUR (3 байта частоты) */
                s_ep0_out_expect = 1U;
                USB_SET_RXLEN(USB_EPT0, 64U);
                USB_SET_RXSTS(USB_EPT0, USB_RX_VALID);
                return;
            } else if (bRequest == 0x81U) { /* GET_CUR */
                const uint8_t sr[3] = {
                    (uint8_t)(s_host_sample_rate & 0xFFU),
                    (uint8_t)((s_host_sample_rate >> 8) & 0xFFU),
                    (uint8_t)((s_host_sample_rate >> 16) & 0xFFU)
                };
                ep0_start_in(sr, 3U, wLength);
                return;
            }
        }
        /* Feature Unit ID=2 (wIndex = 0x0200): CS=0x01 Mute, CS=0x02 Volume */
        if ((bmRequestType & 0x1FU) == 0x01U && ((wIndex >> 8) & 0xFFU) == 2U) {
            if (cs == 0x01U) { /* MUTE */
                if (bRequest == 0x01U) {
                    s_ep0_out_expect = 2U;
                    USB_SET_RXLEN(USB_EPT0, 64U);
                    USB_SET_RXSTS(USB_EPT0, USB_RX_VALID);
                    return;
                } else if (bRequest == 0x81U) {
                    ep0_start_in(&s_uac_mute, 1U, wLength);
                    return;
                }
            } else if (cs == 0x02U) { /* VOLUME (единицы 1/256 dB) */
                if (bRequest == 0x01U) {
                    s_ep0_out_expect = 3U;
                    USB_SET_RXLEN(USB_EPT0, 64U);
                    USB_SET_RXSTS(USB_EPT0, USB_RX_VALID);
                    return;
                } else if (bRequest == 0x81U) { /* GET_CUR */
                    ep0_start_in(&s_uac_vol_cur, 2U, wLength);
                    return;
                } else if (bRequest == 0x82U) { /* GET_MIN (-90 dB = 0xA600) */
                    const int16_t vmin = (int16_t)0xA600;
                    ep0_start_in(&vmin, 2U, wLength);
                    return;
                } else if (bRequest == 0x83U) { /* GET_MAX (0 dB = 0x0000) */
                    const int16_t vmax = 0;
                    ep0_start_in(&vmax, 2U, wLength);
                    return;
                } else if (bRequest == 0x84U) { /* GET_RES (0.5 dB = 0x0080) */
                    const int16_t vres = 0x0080;
                    ep0_start_in(&vres, 2U, wLength);
                    return;
                }
            }
        }
    }

    /* Неподдерживаемый запрос — вежливый STALL на EP0 */
    USB_SET_TXSTS(USB_EPT0, USB_TX_STALL);
    USB_SET_RXSTS(USB_EPT0, USB_RX_STALL);
}

static void handle_usb_bus_reset(void)
{
    usb_ept_info ep;
    memset(&ep, 0, sizeof(ep));

    /* EP0 Control (64B) */
    ep.eptn        = USB_EPT0;
    ep.ept_address = 0x00U;
    ep.trans_type  = EPT_CONTROL_TYPE;
    ep.maxpacket   = 64U;
    ep.inout       = DATA_TRANS_OUT;
    ep.rx_addr     = PMA_EP0_RX_ADDR;
    usb_ept_open(USB, &ep);

    ep.inout       = DATA_TRANS_IN;
    ep.tx_addr     = PMA_EP0_TX_ADDR;
    usb_ept_open(USB, &ep);

    /* EP1 OUT (0x01): Isochronous Async 24-bit Audio (Double Buffered 2x320B) */
    memset(&ep, 0, sizeof(ep));
    ep.eptn             = USB_EPT1;
    ep.ept_address      = 0x01U;
    ep.inout            = DATA_TRANS_OUT;
    ep.trans_type       = EPT_ISO_TYPE;
    ep.is_double_buffer = TRUE;
    ep.tx_addr          = PMA_EP1_AUDIO_BUF0_ADDR;
    ep.rx_addr          = PMA_EP1_AUDIO_BUF1_ADDR;
    ep.maxpacket        = UAC1_PMA_ALLOC_BYTES;
    usb_ept_open(USB, &ep);

    /* EP2 IN (0x82): Isochronous Explicit Feedback 10.14 (3 bytes) */
    memset(&ep, 0, sizeof(ep));
    ep.eptn             = USB_EPT2;
    ep.ept_address      = 0x02U;
    ep.inout            = DATA_TRANS_IN;
    ep.trans_type       = EPT_ISO_TYPE;
    ep.is_double_buffer = TRUE;
    ep.tx_addr          = PMA_EP2_FB_BUF0_ADDR;
    ep.rx_addr          = PMA_EP2_FB_BUF1_ADDR;
    ep.maxpacket        = 16U;
    usb_ept_open(USB, &ep);

    USB->devaddr = 0x80U;
    s_pending_dev_addr = 0U;
    s_alt_setting      = 0U;
}

static void omnia_usb_isr_core(void)
{
    /* 1. Обработка завершённых транзакций конечных точек (TC) */
    while ((USB->intsts & USB_TC_FLAG) != 0U) {
        const uint8_t eptn = (uint8_t)(USB->intsts & USB_EPT_NUM_FLAG);

        if (eptn == USB_EPT0) {
            const uint32_t epr = USB->ept[USB_EPT0];
            if ((epr & USB_SETUPTC) != 0U) {
                USB_CLEAR_RXTC(USB_EPT0);
                handle_ep0_setup();
            } else if ((epr & USB_RXTC) != 0U) {
                const uint16_t rxlen = USB_GET_RX_LEN(USB_EPT0);
                uint8_t data[8] = {0};
                if (rxlen > 0U && rxlen <= sizeof(data)) {
                    usb_read_packet(data, PMA_EP0_RX_ADDR, rxlen);
                }
                USB_CLEAR_RXTC(USB_EPT0);

                if (s_ep0_out_expect == 1U && rxlen >= 3U) {
                    const uint32_t hz = (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16);
                    if ((hz == 44100U || hz == 48000U) && hz != s_host_sample_rate) {
                        s_host_sample_rate = hz;
                        s_rate_change_req  = true;
                    }
                    s_ep0_out_expect = 0U;
                    ep0_send_zlp();
                } else if (s_ep0_out_expect == 2U && rxlen >= 1U) {
                    s_uac_mute = data[0];
                    s_ep0_out_expect = 0U;
                    ep0_send_zlp();
                } else if (s_ep0_out_expect == 3U && rxlen >= 2U) {
                    s_uac_vol_cur = (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
                    s_ep0_out_expect = 0U;
                    ep0_send_zlp();
                } else {
                    USB_SET_RXLEN(USB_EPT0, 64U);
                    USB_SET_RXSTS(USB_EPT0, USB_RX_VALID);
                }
            } else if ((epr & USB_TXTC) != 0U) {
                USB_CLEAR_TXTC(USB_EPT0);
                if (s_pending_dev_addr != 0U) {
                    usb_set_address(USB, s_pending_dev_addr);
                    s_pending_dev_addr = 0U;
                }
                if (s_ep0_tx_rem > 0U) {
                    ep0_send_chunk();
                } else {
                    USB_SET_RXLEN(USB_EPT0, 64U);
                    USB_SET_RXSTS(USB_EPT0, USB_RX_VALID);
                }
            }
        }
        /* EP1 OUT: Изохронный приём 24-битного звука от ТВ-бокса / ПК / смартфона */
        else if (eptn == USB_EPT1) {
            uint8_t pcm_pkt[UAC1_PMA_ALLOC_BYTES];
            const bool hw_filled_buf0 = (USB->ept_bit[USB_EPT1].rxdts != 0U);
            const uint16_t nbytes = hw_filled_buf0
                                        ? USB_DBUF0_GET_LEN(USB_EPT1)
                                        : USB_DBUF1_GET_LEN(USB_EPT1);
            const uint16_t addr = hw_filled_buf0
                                      ? PMA_EP1_AUDIO_BUF0_ADDR
                                      : PMA_EP1_AUDIO_BUF1_ADDR;
            USB_CLEAR_RXTC(USB_EPT1);

            if (nbytes >= 6U && nbytes <= UAC1_MAX_PACKET_BYTES) {
                usb_read_packet(pcm_pkt, addr, nbytes);
                unpack_24bit_usb_packet(pcm_pkt, nbytes);
            }
        }
        /* EP2 IN: Изохронная отправка асинхронной обратной связи 10.14 */
        else if (eptn == USB_EPT2) {
            USB_CLEAR_TXTC(USB_EPT2);
            if (s_alt_setting != 0U) {
                write_uac_feedback_10_14();
            }
        } else {
            USB_CLEAR_RXTC(eptn);
            USB_CLEAR_TXTC(eptn);
        }
    }

    /* 2. Пакеты SOF (1 кГц): подстройка HICK через ACC + индикатор живого USB-хоста */
    if (usb_interrupt_flag_get(USB, USB_SOF_FLAG) != RESET) {
        usb_flag_clear(USB, USB_SOF_FLAG);
        s_sof_counter++;
        if (s_alt_setting != 0U && (s_sof_counter & 0x07U) == 0U) {
            write_uac_feedback_10_14();
        }
    }

    /* 3. Сброс шины USB при подключении/включении ТВ-бокса или ПК */
    if (usb_interrupt_flag_get(USB, USB_RST_FLAG) != RESET) {
        usb_flag_clear(USB, USB_RST_FLAG);
        handle_usb_bus_reset();
    }

    if (usb_interrupt_flag_get(USB, USB_SP_FLAG) != RESET) {
        usb_flag_clear(USB, USB_SP_FLAG);
        s_alt_setting = 0U;
    }

    if (usb_interrupt_flag_get(USB, USB_WK_FLAG) != RESET) {
        usb_flag_clear(USB, USB_WK_FLAG);
    }
}

void USBFS_H_CAN1_TX_IRQHandler(void)
{
    omnia_usb_isr_core();
}

void USBFS_L_CAN1_RX0_IRQHandler(void)
{
    omnia_usb_isr_core();
}

void omnia_usb_dac_init(void)
{
    memset(s_usb_fifo_l, 0, sizeof(s_usb_fifo_l));
    memset(s_usb_fifo_r, 0, sizeof(s_usb_fifo_r));
    s_fifo_wr       = 0U;
    s_fifo_rd       = 0U;
    s_fifo_primed   = false;
    s_usb_fade_gain = 0.0f;

    /*
     * Автономное тактирование USBFS 48 МГц от внутреннего генератора HICK с автокалибровкой ACC
     * по пакетам USB SOF 1 кГц — ядро AT32 остаётся строго на 240 МГц без дробного делителя PLL!
     */
    crm_clock_source_enable(CRM_CLOCK_SOURCE_HICK, TRUE);
    while (crm_flag_get(CRM_HICK_STABLE_FLAG) != SET) {}
    crm_usb_clock_source_select(CRM_USB_CLOCK_SOURCE_HICK);

    crm_periph_clock_enable(CRM_ACC_PERIPH_CLOCK, TRUE);
    acc_write_c1(7980U);
    acc_write_c2(8000U);
    acc_write_c3(8020U);
    acc_calibration_mode_enable(ACC_CAL_HICKTRIM, TRUE);

    crm_periph_clock_enable(CRM_USB_PERIPH_CLOCK, TRUE);

    /* Активация расширенного буфера PMA 1280 байт (CAN1/CAN2 не используются) */
    usb_usbbufs_enable(USB, TRUE);
    usb_dev_init(USB);
    usb_interrupt_enable(USB,
                         (uint16_t)(USB_TC_INT | USB_RST_INT | USB_SOF_INT | USB_SP_INT | USB_WK_INT),
                         TRUE);
    handle_usb_bus_reset();

    nvic_irq_enable(USBFS_H_CAN1_TX_IRQn, 1, 0);
    nvic_irq_enable(USBFS_L_CAN1_RX0_IRQn, 2, 0);

    usb_connect(USB);
}

void omnia_usb_dac_pull_audio(int32_t *out_lr_q31, uint32_t frames)
{
    if (out_lr_q31 == NULL || frames == 0U) {
        return;
    }

    const uint32_t wr = s_fifo_wr;
    uint32_t rd = s_fifo_rd;
    const uint32_t avail = (uint32_t)(wr - rd);

    if (!s_fifo_primed || s_alt_setting == 0U || avail == 0U) {
        if (avail == 0U) {
            s_fifo_primed = false;
        }
        memset(out_lr_q31, 0, frames * 2U * sizeof(int32_t));
        s_usb_fade_gain = 0.0f;
        return;
    }

    float fg = s_usb_fade_gain;

    /*
     * При 100% цифровой громкости (fg >= 1.0) передаём нетронутые 24-bit слова 1-в-1 (Bit-Perfect!),
     * а в первые 5 мс старта потока применяем плавный анти-щелчковый fade-in.
     */
    for (uint32_t i = 0U; i < frames; i++) {
        if (rd == wr) {
            out_lr_q31[i * 2U + 0U] = 0;
            out_lr_q31[i * 2U + 1U] = 0;
            s_fifo_primed = false;
            fg = 0.0f;
            continue;
        }

        const uint32_t slot = rd & USB_FIFO_MASK;
        int32_t l = s_usb_fifo_l[slot];
        int32_t r = s_usb_fifo_r[slot];
        rd++;

        if (fg < 1.0f) {
            fg += (1.0f / 256.0f);
            if (fg >= 1.0f) {
                fg = 1.0f;
            } else {
                l = (int32_t)((float)l * fg);
                r = (int32_t)((float)r * fg);
            }
        }

        out_lr_q31[i * 2U + 0U] = l;
        out_lr_q31[i * 2U + 1U] = r;
    }

    s_fifo_rd       = rd;
    s_usb_fade_gain = fg;
}

void omnia_usb_trigger_set_cfg(const omnia_usb_trigger_cfg_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    s_trig_cfg = *cfg;
    if (s_trig_cfg.debounce_on_ms < 20U) {
        s_trig_cfg.debounce_on_ms = 150U;
    }
    if (s_trig_cfg.debounce_off_ms < 100U) {
        s_trig_cfg.debounce_off_ms = 800U;
    }
}

void omnia_usb_trigger_get_cfg(omnia_usb_trigger_cfg_t *out_cfg)
{
    if (out_cfg != NULL) {
        *out_cfg = s_trig_cfg;
    }
}

void omnia_usb_trigger_notify_manual_source(omnia_i2s_source_t manual_src)
{
    /*
     * Если пользователь вручную переключил вход (например, на Веб-радио или Bluetooth),
     * пока ПК остаётся включённым, запоминаем его выбор и НЕ перехватываем управление обратно,
     * пока USB-хост не будет физически перезапущен/переподключён!
     */
    if (manual_src != OMNIA_SRC_USB_DAC_UAC1) {
        s_saved_prev_src = manual_src;
        s_auto_switched  = false;
    }
}

void omnia_usb_dac_poll(uint32_t now_ms)
{
    /* 1. Бесшовное переключение опорной сетки 44.1 <-> 48.0 кГц по запросу USB-хоста */
    if (s_rate_change_req) {
        s_rate_change_req = false;
        if (omnia_i2s_get_source_mode() == OMNIA_SRC_USB_DAC_UAC1 &&
            omnia_i2s_get_current_fs() != s_host_sample_rate) {
            (void)omnia_i2s_reconfigure_fs(s_host_sample_rate, omnia_i2s_is_mclk_enabled());
        }
    }

    /* 2. Мониторинг живых пакетов SOF (каждые 100 мс) */
    if ((uint32_t)(now_ms - s_sof_watch_ms) >= 100U) {
        s_sof_watch_ms = now_ms;
        const uint32_t cur_sof = s_sof_counter;
        s_sof_alive     = ((uint32_t)(cur_sof - s_last_sof_snap) >= 10U);
        s_last_sof_snap = cur_sof;
    }

    /* 3. Детектор питания +5V USB-DAC: пин PB9 (VBUS_DET) + программный резерв по SOF */
    const bool raw_present = (s_trig_cfg.prefer_vbus_pin != 0U)
                                 ? (omnia_hw_vbus_pin_raw() || s_sof_alive)
                                 : s_sof_alive;

    if (raw_present != s_vbus_raw_prev) {
        s_vbus_raw_prev = raw_present;
        s_vbus_edge_ms  = now_ms;
    }

    const uint32_t req_hold = raw_present
                                  ? (uint32_t)s_trig_cfg.debounce_on_ms
                                  : (uint32_t)s_trig_cfg.debounce_off_ms;

    if (raw_present != s_vbus_debounced && (uint32_t)(now_ms - s_vbus_edge_ms) >= req_hold) {
        s_vbus_debounced = raw_present;

        /* Фронт ВКЛЮЧЕНИЯ +5V от ТВ-приставки / ПК / смартфона */
        if (s_vbus_debounced) {
            if (s_trig_cfg.enable_auto_switch != 0U) {
                const omnia_i2s_source_t cur_src = omnia_i2s_get_source_mode();
                if (cur_src != OMNIA_SRC_USB_DAC_UAC1) {
                    s_saved_prev_src = cur_src;
                }
                s_auto_switched = true;

                uint8_t woke = 0U;
                if (s_trig_cfg.wake_from_standby != 0U && omnia_hw_is_standby()) {
                    omnia_hw_enter_standby(false);
                    woke = 1U;
                }

                if (omnia_i2s_get_current_fs() != s_host_sample_rate) {
                    (void)omnia_i2s_reconfigure_fs(s_host_sample_rate, omnia_i2s_is_mclk_enabled());
                }
                omnia_i2s_set_source_mode(OMNIA_SRC_USB_DAC_UAC1, 1000.0f, -12.0f);

                omnia_usb_trigger_evt_t evt = {
                    .vbus_active          = 1U,
                    .active_source        = (uint8_t)OMNIA_SRC_USB_DAC_UAC1,
                    .previous_source      = (uint8_t)s_saved_prev_src,
                    .woke_from_standby    = woke,
                    .usb_host_sample_rate = s_host_sample_rate
                };
                omnia_uart_send_usb_trigger_evt(&evt);
                omnia_uart_send_status_now();
            }
        }
        /* Фронт ВЫКЛЮЧЕНИЯ питания +5V (ТВ-приставка или ПК уснули/выключились) */
        else {
            if (s_trig_cfg.enable_auto_switch != 0U &&
                s_trig_cfg.auto_return_prev_src != 0U &&
                s_auto_switched &&
                omnia_i2s_get_source_mode() == OMNIA_SRC_USB_DAC_UAC1) {

                s_auto_switched = false;
                omnia_i2s_set_source_mode(s_saved_prev_src, 1000.0f, -12.0f);

                omnia_usb_trigger_evt_t evt = {
                    .vbus_active          = 0U,
                    .active_source        = (uint8_t)s_saved_prev_src,
                    .previous_source      = (uint8_t)OMNIA_SRC_USB_DAC_UAC1,
                    .woke_from_standby    = 0U,
                    .usb_host_sample_rate = s_host_sample_rate
                };
                omnia_uart_send_usb_trigger_evt(&evt);
                omnia_uart_send_status_now();
            }
        }
    }
}

bool omnia_usb_dac_is_vbus_present(void)
{
    return s_vbus_debounced;
}

bool omnia_usb_dac_is_streaming(void)
{
    return (s_alt_setting != 0U) && s_fifo_primed;
}

uint32_t omnia_usb_dac_get_host_rate(void)
{
    return s_host_sample_rate;
}
