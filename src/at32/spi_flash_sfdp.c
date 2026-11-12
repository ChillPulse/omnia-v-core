/**
 * @file    spi_flash_sfdp.c
 * @brief   Универсальный драйвер ЛЮБОЙ 3.3V SPI NOR Flash 25-й серии на площадке BlackPill (PA4..PA7).
 *          Автоматически распознаёт микросхемы любых брендов (Winbond, GigaDevice, Boya, Puya, Zbit,
 *          Macronix, XMC, ISSI и др.) через стандарт JESD216 SFDP (0x5A) + резерв по коду JEDEC (0x9F).
 *          Если флешка не припаяна — прозрачно сохраняет пресеты DSP в верхних секторах внутренней Flash AT32!
 */

#include "omnia_at32.h"
#include "omnia_crc16.h"
#include <string.h>

/* Команды стандарта SPI NOR JESD216 */
#define NOR_CMD_WRITE_ENABLE        0x06U
#define NOR_CMD_READ_SR1            0x05U
#define NOR_CMD_PAGE_PROGRAM        0x02U
#define NOR_CMD_READ_DATA           0x03U
#define NOR_CMD_SECTOR_ERASE_4K     0x20U
#define NOR_CMD_RELEASE_POWERDOWN   0xABU
#define NOR_CMD_ENABLE_RESET        0x66U
#define NOR_CMD_RESET_DEVICE        0x99U
#define NOR_CMD_JEDEC_ID            0x9FU
#define NOR_CMD_READ_SFDP           0x5AU

/* Резервная область во внутренней Flash AT32 (верхние 64 КБ из 1024 КБ: 0x080F0000..0x080FFFFF) */
#define INT_FLASH_PRESET_BASE       0x080F0000UL
#define INT_FLASH_SECTOR_SIZE       2048UL          /* Сектор Bank2 на AT32F403AxG = 2 КБ */
#define EXT_FLASH_PRESET_BASE       0x00000000UL
#define EXT_FLASH_SLOT_STRIDE       4096UL          /* 1 сектор 4 КБ на каждый из 32 пресетов */

#define PRESET_BLOB_MAGIC           0x4F4D5032UL    /* "OMP2" */

#pragma pack(push, 1)
typedef struct {
    uint32_t                magic;
    uint16_t                version;
    uint16_t                active_mask;
    char                    name[24];
    omnia_hpf_headroom_t    hpf;
    omnia_room_peq_t        room_peq;
    omnia_user_peq_t        user_peq;
    omnia_anti_ad_t         anti_ad;
    omnia_vbb_params_t      vbb;
    omnia_aether_params_t   aether;
    omnia_loudness_params_t loudness;
    omnia_limiter_params_t  limiter;
    uint16_t                crc16;
    uint16_t                pad;
} omnia_preset_blob_t;
#pragma pack(pop)

static omnia_spi_flash_info_t g_flash_info;
static uint8_t                g_active_bus = 0U; /* 0 = нет, 1 = SPI1 (PA4..7), 2 = Belly U3 (PA8/PB1/PB10/PB11) */

static inline void cs_low(void)
{
    if (g_active_bus == 2U) {
        gpio_bits_reset(GPIOA, GPIO_PINS_8);
    } else {
        gpio_bits_reset(GPIOA, GPIO_PINS_4);
    }
}

static inline void cs_high(void)
{
    if (g_active_bus == 2U) {
        gpio_bits_set(GPIOA, GPIO_PINS_8);
    } else {
        gpio_bits_set(GPIOA, GPIO_PINS_4);
    }
}

static uint8_t spi1_xfer(uint8_t tx)
{
    if (g_active_bus == 2U) {
        /* Штатная площадка U3 SOIC-8 на брюшке WeAct BlackPill V1.0:
         * PA8=CS#, PB1=SCK, PB10=DI(IO0), PB11=DO(IO1), PB7=WP#(1), PB6=HOLD#(1) */
        uint8_t rx = 0U;
        for (uint8_t bit = 0U; bit < 8U; bit++) {
            if ((tx & 0x80U) != 0U) {
                GPIOB->scr = GPIO_PINS_10;
            } else {
                GPIOB->clr = GPIO_PINS_10;
            }
            tx <<= 1;
            __NOP(); __NOP();
            GPIOB->scr = GPIO_PINS_1;
            __NOP(); __NOP();
            rx = (uint8_t)((rx << 1) | ((GPIOB->idt & GPIO_PINS_11) ? 1U : 0U));
            GPIOB->clr = GPIO_PINS_1;
        }
        return rx;
    }

    while (spi_i2s_flag_get(SPI1, SPI_I2S_TDBE_FLAG) == RESET) {}
    spi_i2s_data_transmit(SPI1, tx);
    while (spi_i2s_flag_get(SPI1, SPI_I2S_RDBF_FLAG) == RESET) {}
    return (uint8_t)spi_i2s_data_receive(SPI1);
}

static bool nor_wait_ready(uint32_t timeout_ms)
{
    const uint32_t start = omnia_millis();
    uint8_t sr = 0x01U;
    do {
        cs_low();
        (void)spi1_xfer(NOR_CMD_READ_SR1);
        sr = spi1_xfer(0xFFU);
        cs_high();
        if ((sr & 0x01U) == 0U) {
            return true;
        }
    } while ((omnia_millis() - start) < timeout_ms);
    return false;
}

static void nor_send_addr(uint32_t addr)
{
    if (g_flash_info.addr_4byte) {
        (void)spi1_xfer((uint8_t)((addr >> 24) & 0xFFU));
    }
    (void)spi1_xfer((uint8_t)((addr >> 16) & 0xFFU));
    (void)spi1_xfer((uint8_t)((addr >> 8)  & 0xFFU));
    (void)spi1_xfer((uint8_t)(addr & 0xFFU));
}

static void nor_read_sfdp_block(uint32_t sfdp_addr, uint8_t *dst, size_t len)
{
    cs_low();
    (void)spi1_xfer(NOR_CMD_READ_SFDP);
    (void)spi1_xfer((uint8_t)((sfdp_addr >> 16) & 0xFFU));
    (void)spi1_xfer((uint8_t)((sfdp_addr >> 8)  & 0xFFU));
    (void)spi1_xfer((uint8_t)(sfdp_addr & 0xFFU));
    (void)spi1_xfer(0xFFU); /* 8 dummy cycles по стандарту JESD216 */
    for (size_t i = 0; i < len; ++i) {
        dst[i] = spi1_xfer(0xFFU);
    }
    cs_high();
}

/**
 * @brief  Читает стандартную таблицу SFDP (JESD216), поддерживаемую всеми современными флешками любых фирм.
 */
static uint32_t detect_capacity_via_sfdp(void)
{
    uint8_t hdr[16];
    nor_read_sfdp_block(0x000000U, hdr, sizeof(hdr));

    /* Сигнатура "SFDP" = 0x50444653 LE */
    if (hdr[0] != 'S' || hdr[1] != 'F' || hdr[2] != 'D' || hdr[3] != 'P') {
        return 0U;
    }

    /* Указатель на Basic Flash Parameter Table (BFPT) лежит в первом заголовке параметров [11..13] */
    const uint32_t ptp = ((uint32_t)hdr[11]) | ((uint32_t)hdr[12] << 8) | ((uint32_t)hdr[13] << 16);
    uint8_t bfpt[8];
    nor_read_sfdp_block(ptp, bfpt, sizeof(bfpt));

    /* 2-е двойное слово BFPT (байты 4..7) хранит плотность кристалла в битах минус 1 */
    const uint32_t density = ((uint32_t)bfpt[4]) |
                             ((uint32_t)bfpt[5] << 8) |
                             ((uint32_t)bfpt[6] << 16) |
                             ((uint32_t)bfpt[7] << 24);
    if ((density & 0x80000000UL) == 0UL && density > 0xFFFFUL) {
        return (density + 1UL) >> 3; /* Перевод бит в байты */
    }
    return 0U;
}

static bool probe_bus_id(uint8_t bus_id)
{
    g_active_bus = bus_id;

    /* Пробуждение и мягкий сброс микросхемы (на случай глубокого сна после ресета МК) */
    cs_low(); (void)spi1_xfer(NOR_CMD_RELEASE_POWERDOWN); cs_high();
    omnia_delay_us(40U);
    cs_low(); (void)spi1_xfer(NOR_CMD_ENABLE_RESET);      cs_high();
    omnia_delay_us(5U);
    cs_low(); (void)spi1_xfer(NOR_CMD_RESET_DEVICE);      cs_high();
    omnia_delay_us(60U);

    /* Опрос универсального JEDEC ID (0x9F) */
    cs_low();
    (void)spi1_xfer(NOR_CMD_JEDEC_ID);
    const uint8_t mfg  = spi1_xfer(0xFFU);
    const uint8_t type = spi1_xfer(0xFFU);
    const uint8_t cap  = spi1_xfer(0xFFU);
    cs_high();

    if (mfg != 0x00U && mfg != 0xFFU && cap >= 0x11U && cap <= 0x22U) {
        g_flash_info.present     = true;
        g_flash_info.bus_type    = bus_id;
        g_flash_info.jedec_id    = ((uint32_t)mfg << 16) | ((uint32_t)type << 8) | cap;
        g_flash_info.sector_size = 4096U;
        g_flash_info.page_size   = 256U;

        uint32_t bytes = detect_capacity_via_sfdp();
        if (bytes == 0U) {
            bytes = 1UL << cap;
        }
        g_flash_info.capacity_bytes = bytes;
        g_flash_info.addr_4byte     = (bytes > (16UL * 1024UL * 1024UL));
        return true;
    }
    return false;
}

void omnia_ext_flash_init(omnia_spi_flash_info_t *out_info)
{
    gpio_init_type gpio_cfg;
    spi_init_type  spi_cfg;

    memset(&g_flash_info, 0, sizeof(g_flash_info));
    g_active_bus = 0U;
    crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_SPI1_PERIPH_CLOCK, TRUE);

    /* 1. Инициализация внешней шины SPI1 (PA4..PA7) */
    gpio_bits_set(GPIOA, GPIO_PINS_4 | GPIO_PINS_8);
    gpio_default_para_init(&gpio_cfg);
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_cfg.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_mode           = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_pins           = GPIO_PINS_4 | GPIO_PINS_8; /* PA4 = SPI1 CS, PA8 = Belly U3 CS */
    gpio_init(GPIOA, &gpio_cfg);

    gpio_cfg.gpio_mode = GPIO_MODE_MUX;
    gpio_cfg.gpio_pins = GPIO_PINS_5 | GPIO_PINS_7;
    gpio_init(GPIOA, &gpio_cfg);

    gpio_cfg.gpio_mode = GPIO_MODE_INPUT;
    gpio_cfg.gpio_pull = GPIO_PULL_UP;
    gpio_cfg.gpio_pins = GPIO_PINS_6;
    gpio_init(GPIOA, &gpio_cfg);

    /* 2. Инициализация штатной нижней площадки U3 SOIC-8 на BlackPill V1.0 (PB1, PB10, PB11, PB6, PB7) */
    gpio_bits_reset(GPIOB, GPIO_PINS_1 | GPIO_PINS_10);
    gpio_bits_set(GPIOB, GPIO_PINS_6 | GPIO_PINS_7); /* WP#=1, HOLD#=1 */
    gpio_cfg.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_pull = GPIO_PULL_NONE;
    gpio_cfg.gpio_pins = GPIO_PINS_1 | GPIO_PINS_10 | GPIO_PINS_6 | GPIO_PINS_7;
    gpio_init(GPIOB, &gpio_cfg);

    gpio_cfg.gpio_mode = GPIO_MODE_INPUT;
    gpio_cfg.gpio_pull = GPIO_PULL_UP;
    gpio_cfg.gpio_pins = GPIO_PINS_11; /* DO / MISO */
    gpio_init(GPIOB, &gpio_cfg);

    spi_default_para_init(&spi_cfg);
    spi_cfg.transmission_mode = SPI_TRANSMIT_FULL_DUPLEX;
    spi_cfg.master_slave_mode = SPI_MODE_MASTER;
    spi_cfg.mclk_freq_division = SPI_MCLK_DIV_4; /* 120 МГц / 4 = 30 МГц */
    spi_cfg.first_bit_transmission = SPI_FIRST_BIT_MSB;
    spi_cfg.frame_bit_num     = SPI_FRAME_8BIT;
    spi_cfg.clock_polarity    = SPI_CLOCK_POLARITY_LOW;
    spi_cfg.clock_phase       = SPI_CLOCK_PHASE_1EDGE;
    spi_cfg.cs_mode_selection = SPI_CS_SOFTWARE_MODE;
    spi_init(SPI1, &spi_cfg);
    spi_enable(SPI1, TRUE);

    /* Сначала проверяем штатную площадку U3 на брюшке BlackPill, затем внешнюю гребёнку SPI1 */
    if (!probe_bus_id(2U)) {
        if (!probe_bus_id(1U)) {
            g_active_bus = 0U;
        }
    }

    if (out_info) {
        *out_info = g_flash_info;
    }
}

const omnia_spi_flash_info_t *omnia_ext_flash_get_info(void)
{
    return &g_flash_info;
}

static bool ext_nor_erase_4k(uint32_t addr)
{
    cs_low(); (void)spi1_xfer(NOR_CMD_WRITE_ENABLE); cs_high();
    cs_low();
    (void)spi1_xfer(NOR_CMD_SECTOR_ERASE_4K);
    nor_send_addr(addr);
    cs_high();
    return nor_wait_ready(450U);
}

static bool ext_nor_write(uint32_t addr, const uint8_t *src, size_t len)
{
    while (len > 0U) {
        const size_t page_off = addr & 0xFFU;
        size_t chunk = 256U - page_off;
        if (chunk > len) {
            chunk = len;
        }

        cs_low(); (void)spi1_xfer(NOR_CMD_WRITE_ENABLE); cs_high();
        cs_low();
        (void)spi1_xfer(NOR_CMD_PAGE_PROGRAM);
        nor_send_addr(addr);
        for (size_t i = 0; i < chunk; ++i) {
            (void)spi1_xfer(src[i]);
        }
        cs_high();

        if (!nor_wait_ready(25U)) {
            return false;
        }
        addr += (uint32_t)chunk;
        src  += chunk;
        len  -= chunk;
    }
    return true;
}

static void ext_nor_read(uint32_t addr, uint8_t *dst, size_t len)
{
    cs_low();
    (void)spi1_xfer(NOR_CMD_READ_DATA);
    nor_send_addr(addr);
    for (size_t i = 0; i < len; ++i) {
        dst[i] = spi1_xfer(0xFFU);
    }
    cs_high();
}

bool omnia_preset_save(uint8_t slot, const char *name, const omnia_dsp_ctx_t *dsp)
{
    if (slot >= OMNIA_MAX_PRESETS || !dsp) {
        return false;
    }

    omnia_preset_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.magic       = PRESET_BLOB_MAGIC;
    blob.version     = OMNIA_PROTO_VERSION;
    blob.active_mask = dsp->active_mask;
    if (name) {
        strncpy(blob.name, name, sizeof(blob.name) - 1U);
    }
    blob.hpf      = dsp->hpf_cfg;
    blob.room_peq = dsp->room_cfg;
    blob.user_peq = dsp->user_cfg;
    blob.anti_ad  = dsp->anti_ad_cfg;
    blob.vbb      = dsp->vbb_cfg;
    blob.aether   = dsp->aether_cfg;
    blob.loudness = dsp->loudness_cfg;
    blob.limiter  = dsp->limiter_cfg;
    blob.crc16    = omnia_crc16_ccitt(&blob, offsetof(omnia_preset_blob_t, crc16));

    if (g_flash_info.present) {
        const uint32_t addr = EXT_FLASH_PRESET_BASE + (uint32_t)slot * EXT_FLASH_SLOT_STRIDE;
        if (!ext_nor_erase_4k(addr)) {
            return false;
        }
        return ext_nor_write(addr, (const uint8_t *)&blob, sizeof(blob));
    }

    /* Прозрачный резерв во внутренний Flash AT32 (Bank 2, по 2 КБ на слот — не тормозит Bank 1!) */
    const uint32_t flash_addr = INT_FLASH_PRESET_BASE + (uint32_t)slot * INT_FLASH_SECTOR_SIZE;
    flash_unlock();
    bool ok = (flash_sector_erase(flash_addr) == FLASH_OPERATE_DONE);
    const uint32_t *words = (const uint32_t *)&blob;
    const size_t nwords = (sizeof(blob) + 3U) / 4U;
    for (size_t i = 0; ok && i < nwords; ++i) {
        ok = (flash_word_program(flash_addr + (uint32_t)(i * 4U), words[i]) == FLASH_OPERATE_DONE);
    }
    flash_lock();
    return ok;
}

bool omnia_preset_load(uint8_t slot, omnia_dsp_ctx_t *dsp, char *out_name24)
{
    if (slot >= OMNIA_MAX_PRESETS || !dsp) {
        return false;
    }

    omnia_preset_blob_t blob;
    if (g_flash_info.present) {
        ext_nor_read(EXT_FLASH_PRESET_BASE + (uint32_t)slot * EXT_FLASH_SLOT_STRIDE,
                     (uint8_t *)&blob, sizeof(blob));
    } else {
        const uint32_t flash_addr = INT_FLASH_PRESET_BASE + (uint32_t)slot * INT_FLASH_SECTOR_SIZE;
        memcpy(&blob, (const void *)flash_addr, sizeof(blob));
    }

    if (blob.magic != PRESET_BLOB_MAGIC || blob.version != OMNIA_PROTO_VERSION) {
        return false;
    }
    const uint16_t calc = omnia_crc16_ccitt(&blob, offsetof(omnia_preset_blob_t, crc16));
    if (calc != blob.crc16) {
        return false;
    }

    omnia_dsp_apply_hpf_headroom(dsp, &blob.hpf);
    omnia_dsp_apply_room_peq(dsp, &blob.room_peq);
    omnia_dsp_apply_user_peq(dsp, &blob.user_peq);
    omnia_dsp_apply_anti_ad(dsp, &blob.anti_ad);
    omnia_dsp_apply_vbb(dsp, &blob.vbb);
    omnia_dsp_apply_aether(dsp, &blob.aether);
    omnia_dsp_apply_loudness(dsp, &blob.loudness);
    omnia_dsp_apply_limiter(dsp, &blob.limiter);
    omnia_dsp_set_bypass_mask(dsp, blob.active_mask, 30U);

    if (out_name24) {
        memcpy(out_name24, blob.name, 24U);
        out_name24[23] = '\0';
    }
    return true;
}
