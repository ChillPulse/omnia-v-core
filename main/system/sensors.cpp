/*
 * Датчик освещенности BH1750 (адрес 0x23) на I2C_NUM_0, 400 кГц (ТЗ, Том 1, Гл. 3).
 * Непрерывный режим H-Resolution (0x10): чтение без блокировки, 120–180 мс на отсчет.
 * Внешние подтяжки SDA/SCL 4.7 кОм к 3.3 В обязательны.
 */

#include "omnia.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char* TAG = "OMNIA_SENSORS";

#define BH1750_ADDR 0x23
#define BH1750_CONT_HRES 0x10

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static uint16_t s_last_lux = 0;

bool sensors_init(void)
{
    i2c_master_bus_config_t bc = {};
    bc.i2c_port          = I2C_NUM_0;
    bc.sda_io_num        = (gpio_num_t)PIN_I2C_SDA;
    bc.scl_io_num        = (gpio_num_t)PIN_I2C_SCL;
    bc.clk_source        = I2C_CLK_SRC_DEFAULT;
    bc.glitch_ignore_cnt = 7;
    bc.flags.enable_internal_pullup = false;
    if (i2c_new_master_bus(&bc, &s_bus) != ESP_OK) return false;

    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address  = BH1750_ADDR;
    dc.scl_speed_hz    = 400000;
    if (i2c_master_bus_add_device(s_bus, &dc, &s_dev) != ESP_OK) return false;

    uint8_t cmd = BH1750_CONT_HRES;
    if (i2c_master_transmit(s_dev, &cmd, 1, 100) != ESP_OK) {
        ESP_LOGW(TAG, "BH1750 не ответил на 0x%02X", BH1750_ADDR);
        s_dev = NULL;
        return false;
    }
    ESP_LOGI(TAG, "BH1750 инициализирован");
    return true;
}

uint16_t sensors_read_lux(void)
{
    if (!s_dev) return 0;
    uint8_t d[2];
    if (i2c_master_receive(s_dev, d, 2, 50) == ESP_OK) {
        uint32_t raw = ((uint32_t)d[0] << 8) | d[1];
        s_last_lux = (uint16_t)(raw * 10 / 12);   // люкс = raw / 1.2
    }
    return s_last_lux;
}
