/* Предварительная диагностика VS1838B через ESP-IDF RMT RX. */
#include "omnia_esp32.h"
#include "omnia_ir_nec.h"
#include "driver/rmt_rx.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "omnia_ir";
static rmt_channel_handle_t ir_chan;
static QueueHandle_t ir_queue;
static rmt_symbol_word_t ir_symbols[80];
static portMUX_TYPE learn_lock = portMUX_INITIALIZER_UNLOCKED;
static char learn_name[16];
static int64_t learn_until_us;

static bool valid_name(const char *name)
{
    if (!name) return false;
    size_t n = 0;
    while (n < 15U && name[n] && ((name[n] >= 'A' && name[n] <= 'Z') ||
           (name[n] >= '0' && name[n] <= '9') || name[n] == '_')) ++n;
    return n > 0U && name[n] == '\0';
}

esp_err_t omnia_ir_learn_nec(const char *name)
{
    if (!valid_name(name)) return ESP_ERR_INVALID_ARG;
    if (!ir_chan || !ir_queue) return ESP_ERR_INVALID_STATE;
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&learn_lock);
    if (learn_until_us > now) {
        portEXIT_CRITICAL(&learn_lock);
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(learn_name, name, strlen(name) + 1U);
    learn_until_us = now + 30000000LL;
    portEXIT_CRITICAL(&learn_lock);
    return ESP_OK;
}

esp_err_t omnia_ir_get_nec(const char *name, uint32_t *out_code)
{
    if (!valid_name(name) || !out_code) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open("omnia_ir", NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_u32(h, name, out_code);
    nvs_close(h);
    return err;
}

esp_err_t omnia_ir_delete_nec(const char *name)
{
    if (!valid_name(name)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open("omnia_ir", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, name);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void capture_if_armed(uint32_t code)
{
    char name[16] = {0};
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&learn_lock);
    if (learn_until_us > now) memcpy(name, learn_name, sizeof(name));
    learn_until_us = 0;
    portEXIT_CRITICAL(&learn_lock);
    if (!name[0]) return;
    nvs_handle_t h;
    esp_err_t err = nvs_open("omnia_ir", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u32(h, name, code);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err == ESP_OK) ESP_LOGI(TAG, "Сохранён IR %s=0x%08lX; действие пока не назначено", name, (unsigned long)code);
    else ESP_LOGE(TAG, "Не удалось сохранить IR %s: %s", name, esp_err_to_name(err));
}

static bool ir_on_done(rmt_channel_handle_t chan, const rmt_rx_done_event_data_t *event, void *context)
{
    (void)chan;
    BaseType_t wake = pdFALSE;
    QueueHandle_t queue = (QueueHandle_t)context;
    (void)xQueueSendFromISR(queue, event, &wake);
    return wake == pdTRUE;
}

static void ir_task(void *arg)
{
    (void)arg;
    const rmt_receive_config_t config = {
        .signal_range_min_ns = 8000,
        .signal_range_max_ns = 12000000,
    };
    omnia_ir_pair_t pairs[80];
    while (1) {
        esp_err_t err = rmt_receive(ir_chan, ir_symbols, sizeof(ir_symbols), &config);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Не удалось начать RMT RX: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        rmt_rx_done_event_data_t evt;
        if (xQueueReceive(ir_queue, &evt, portMAX_DELAY) != pdTRUE) continue;
        size_t n = 0;
        bool malformed = evt.num_symbols >= 80; /* buffer may have been truncated */
        for (size_t i = 0; i < evt.num_symbols && i < 80; ++i) {
            const rmt_symbol_word_t s = evt.received_symbols[i];
            /* An RMT end marker may have an empty second half. Never skip a
             * nonempty malformed symbol: that could splice an invalid frame. */
            if (!s.duration0 && !s.duration1 && i + 1U == evt.num_symbols) break;
            if (!s.duration0 || s.level0 != 0 ||
                (s.duration1 && s.level1 != 1)) { malformed = true; break; }
            pairs[n++] = (omnia_ir_pair_t){ s.duration0, s.duration1 };
        }
        omnia_ir_nec_t decoded;
        const omnia_ir_result_t result = malformed ? OMNIA_IR_BAD : omnia_ir_nec_decode(pairs, n, &decoded);
        if (result == OMNIA_IR_NEC_FRAME) {
            ESP_LOGI(TAG, "NEC raw=0x%08lX addr=0x%04X cmd=0x%02X",
                     (unsigned long)decoded.raw_lsb_first, decoded.address, decoded.command);
            capture_if_armed(decoded.raw_lsb_first);
        } else if (result == OMNIA_IR_NEC_REPEAT) {
            ESP_LOGI(TAG, "NEC повтор удержания (без команды)");
        } else if (evt.num_symbols > 0) {
            ESP_LOGI(TAG, "ИК-пакет: %u символов; неизвестный или ошибочный формат", (unsigned)evt.num_symbols);
        }
    }
}

esp_err_t omnia_ir_receiver_init(void)
{
    ir_queue = xQueueCreate(2, sizeof(rmt_rx_done_event_data_t));
    if (!ir_queue) return ESP_ERR_NO_MEM;
    const rmt_rx_channel_config_t chan_cfg = {
        .gpio_num = OMNIA_ESP_IR_RX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,
        .mem_block_symbols = 48, /* ESP32-S3: один блок RMT, кадр NEC помещается */
    };
    esp_err_t err = rmt_new_rx_channel(&chan_cfg, &ir_chan);
    if (err != ESP_OK) goto fail;
    /* Выход приёмника в покое HIGH; незанятый вход не должен плавать. */
    err = gpio_set_pull_mode(OMNIA_ESP_IR_RX_GPIO, GPIO_PULLUP_ONLY);
    if (err != ESP_OK) goto fail;
    const rmt_rx_event_callbacks_t callbacks = { .on_recv_done = ir_on_done };
    err = rmt_rx_register_event_callbacks(ir_chan, &callbacks, ir_queue);
    if (err != ESP_OK) goto fail;
    err = rmt_enable(ir_chan);
    if (err != ESP_OK) goto fail;
    if (xTaskCreatePinnedToCore(ir_task, "omnia_ir_rx", 4096, NULL, 5, NULL, 0) != pdPASS) {
        (void)rmt_disable(ir_chan);
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    ESP_LOGI(TAG, "VS1838B RMT RX GPIO%d: NEC + сохранение кодов в NVS; действия не назначены", OMNIA_ESP_IR_RX_GPIO);
    return ESP_OK;
fail:
    if (ir_chan) (void)rmt_del_channel(ir_chan);
    ir_chan = NULL;
    vQueueDelete(ir_queue);
    ir_queue = NULL;
    return err;
}
