#include "ir_learn.h"
#include "omnia.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_attr.h"
#include <string.h>

static const char* TAG = "OMNIA_IR";

// Элемент очереди: копия принятого фрейма (ISR -> задача Core 1)
typedef struct {
    uint16_t n;
    rmt_symbol_word_t sym[IR_RAW_BUFFER_SIZE];
} ir_frame_t;

static omnia_key_t ir_action_to_key(ir_action_t a)
{
    switch (a) {
        case IR_CMD_POWER:      return OMNIA_KEY_POWER;
        case IR_CMD_MUTE:       return OMNIA_KEY_MUTE;
        case IR_CMD_SOURCE:     return OMNIA_KEY_SOURCE;
        case IR_CMD_FAVORITE:   return OMNIA_KEY_FAVORITE;
        case IR_CMD_PLAY:       return OMNIA_KEY_PLAY;
        case IR_CMD_VOL_UP:     return OMNIA_KEY_VOL_UP;
        case IR_CMD_VOL_DOWN:   return OMNIA_KEY_VOL_DOWN;
        case IR_CMD_TRACK_NEXT: return OMNIA_KEY_NEXT;
        case IR_CMD_TRACK_PREV: return OMNIA_KEY_PREV;
        default:                return OMNIA_KEY_NONE;
    }
}

OmniaIRLearn::OmniaIRLearn()
    : m_registered_cmds_count(0), m_is_learning_mode(false),
      m_target_learning_action(IR_CMD_POWER), m_rx_chan(NULL), m_queue(NULL)
{
    memset(m_saved_templates, 0, sizeof(m_saved_templates));
    memset(m_rx_buf, 0, sizeof(m_rx_buf));
    memset(&m_rx_cfg, 0, sizeof(m_rx_cfg));
}

uint32_t OmniaIRLearn::calculate_raw_hash(const rmt_symbol_word_t* s, size_t len)
{
    // FNV-1a по длительностям, округленным до 100 мкс (отсечение джиттера и помех)
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < len; i++) {
        uint32_t d0 = (s[i].duration0 / 100) * 100;
        uint32_t d1 = (s[i].duration1 / 100) * 100;
        hash ^= d0; hash *= 16777619U;
        hash ^= d1; hash *= 16777619U;
    }
    return hash;
}

// Колбэк RMT RX выполняется в ISR: только копируем фрейм в очередь
bool IRAM_ATTR OmniaIRLearn::on_rx_done(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t* edata, void* user_data)
{
    (void)ch;
    OmniaIRLearn* self = (OmniaIRLearn*)user_data;
    ir_frame_t f;
    f.n = edata->num_symbols > IR_RAW_BUFFER_SIZE ? IR_RAW_BUFFER_SIZE : (uint16_t)edata->num_symbols;
    memcpy(f.sym, edata->received_symbols, f.n * sizeof(rmt_symbol_word_t));
    BaseType_t hpw = pdFALSE;
    xQueueSendFromISR(self->m_queue, &f, &hpw);
    return hpw == pdTRUE;
}

void OmniaIRLearn::ir_task(void* arg)
{
    OmniaIRLearn* self = (OmniaIRLearn*)arg;
    ir_frame_t f;
    while (1) {
        if (xQueueReceive(self->m_queue, &f, portMAX_DELAY) == pdTRUE) {
            self->process_rx_symbols(f.sym, f.n);
            // Перевзвод приема после обработки
            rmt_receive(self->m_rx_chan, self->m_rx_buf, sizeof(self->m_rx_buf), &self->m_rx_cfg);
        }
    }
}

void OmniaIRLearn::init()
{
    ESP_LOGI(TAG, "RMT RX на GPIO %d", PIN_IR_RECEIVER);
    m_queue = xQueueCreate(8, sizeof(ir_frame_t));

    rmt_rx_channel_config_t rx_config = {};
    rx_config.gpio_num         = (gpio_num_t)PIN_IR_RECEIVER;
    rx_config.clk_src          = RMT_CLK_SRC_DEFAULT;
    rx_config.resolution_hz    = 1000000;   // 1 мкс на такт
    rx_config.mem_block_symbols = 64;
    rx_config.flags.with_dma   = false;
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_config, &m_rx_chan));

    rmt_rx_event_callbacks_t cbs = {};
    cbs.on_recv_done = on_rx_done;
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(m_rx_chan, &cbs, this));
    ESP_ERROR_CHECK(rmt_enable(m_rx_chan));

    m_rx_cfg.signal_range_min_ns = 20000;       // импульсы короче 20 мкс — помеха
    m_rx_cfg.signal_range_max_ns = 100000000;   // пауза дольше 100 мс — конец кадра
    ESP_ERROR_CHECK(rmt_receive(m_rx_chan, m_rx_buf, sizeof(m_rx_buf), &m_rx_cfg));

    load_from_nvs();
    xTaskCreatePinnedToCore(ir_task, "omnia_ir", 4096, this, 5, NULL, 1);
}

void OmniaIRLearn::start_learning(ir_action_t action)
{
    if (action >= IR_MAX_COMMANDS) return;
    m_target_learning_action = action;
    m_is_learning_mode = true;
    ESP_LOGI(TAG, "Обучение ИК-команды: %d", (int)action);
}

void OmniaIRLearn::process_rx_symbols(const rmt_symbol_word_t* symbols, size_t len)
{
    if (len < 4) return;   // слишком короткий фрагмент — шум
    uint32_t h = calculate_raw_hash(symbols, len);

    if (m_is_learning_mode) {
        ir_template_t* t = &m_saved_templates[m_target_learning_action];
        t->action = m_target_learning_action;
        t->hash   = h;
        t->length = (uint16_t)len;
        t->valid  = 1;
        m_is_learning_mode = false;
        save_to_nvs();
        ESP_LOGI(TAG, "Команда %d заучена, хэш 0x%08X", (int)t->action, (unsigned)t->hash);
        return;
    }

    for (int i = 0; i < IR_MAX_COMMANDS; i++) {
        if (m_saved_templates[i].valid && m_saved_templates[i].hash == h) {
            ESP_LOGI(TAG, "ИК: команда %d", (int)m_saved_templates[i].action);
            omnia_dispatch_key(ir_action_to_key(m_saved_templates[i].action));
            return;
        }
    }
}

// Запись во флеш только при обучении (ТЗ: нулевой износ Flash)
void OmniaIRLearn::save_to_nvs()
{
    nvs_handle_t h;
    if (nvs_open("omnia_ir", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "templates", m_saved_templates, sizeof(m_saved_templates));
        nvs_commit(h);
        nvs_close(h);
    }
}

void OmniaIRLearn::load_from_nvs()
{
    nvs_handle_t h;
    if (nvs_open("omnia_ir", NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(m_saved_templates);
        if (nvs_get_blob(h, "templates", m_saved_templates, &sz) == ESP_OK) {
            m_registered_cmds_count = 0;
            for (int i = 0; i < IR_MAX_COMMANDS; i++) {
                if (m_saved_templates[i].valid) m_registered_cmds_count++;
            }
            ESP_LOGI(TAG, "ИК-шаблонов загружено из NVS: %u", (unsigned)m_registered_cmds_count);
        }
        nvs_close(h);
    }
}
