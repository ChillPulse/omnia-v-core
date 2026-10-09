/*
 * Хранилища: SD-карта на выделенной шине SPI3 (ТЗ, Том 2, Гл. 2) и USB-Host.
 * ⚠ Пины SD (35–38) по ТЗ конфликтуют с Octal PSRAM — см. README.
 * ⚠ USB-Host MSC пока заглушка (управление MUX и питанием реализовано).
 */

#include "omnia.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "OMNIA_STORAGE";
static sdmmc_card_t* s_card = NULL;

bool init_sd_card_spi3(void)
{
    ESP_LOGI(TAG, "Инициализация SD-карты на SPI3 (SCK=%d MOSI=%d MISO=%d CS=%d)",
             PIN_SD_SCK, PIN_SD_MOSI, PIN_SD_MISO, PIN_SD_CS);

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num     = PIN_SD_MOSI;
    bus_cfg.miso_io_num     = PIN_SD_MISO;
    bus_cfg.sclk_io_num     = PIN_SD_SCK;
    bus_cfg.quadwp_io_num   = -1;
    bus_cfg.quadhd_io_num   = -1;
    bus_cfg.max_transfer_sz = 16384;

    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Сбой инициализации шины SPI3: %s", esp_err_to_name(ret));
        return false;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs  = (gpio_num_t)PIN_SD_CS;
    slot_config.host_id  = SPI3_HOST;

    esp_vfs_fat_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 5;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    host.max_freq_khz = 20000;   // 20 МГц (ТЗ)

    ret = esp_vfs_fat_sdspi_mount("/sd", &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ошибка монтирования /sd: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "SD-карта смонтирована в /sd");
    return true;
}

void init_usb_host_msc(void)
{
    ESP_LOGI(TAG, "USB MUX -> режим HOST (флешка)");
    gpio_set_level((gpio_num_t)PIN_USB_SEL, 0);      // 74HC4052 на флеш-накопитель
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level((gpio_num_t)PIN_USB_VBUS_EN, 1);  // AO3401: +5V на USB-A
    // TODO: usb_host_install() + msc-драйвер + монтирование в /usb (не реализовано)
    ESP_LOGW(TAG, "USB Host MSC: драйвер не реализован, питание порта включено");
}
