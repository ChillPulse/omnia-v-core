#ifndef IR_LEARN_H
#define IR_LEARN_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "driver/rmt_rx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// ТЗ, Том 5, Гл. 1: самообучение ИК-пульта по RAW-таймингам (RMT RX)
#define IR_RAW_BUFFER_SIZE 128
#define IR_MAX_COMMANDS    9

typedef enum {
    IR_CMD_POWER = 0,
    IR_CMD_MUTE,
    IR_CMD_SOURCE,
    IR_CMD_FAVORITE,
    IR_CMD_PLAY,
    IR_CMD_VOL_UP,
    IR_CMD_VOL_DOWN,
    IR_CMD_TRACK_NEXT,
    IR_CMD_TRACK_PREV
} ir_action_t;

typedef struct {
    ir_action_t action;
    uint32_t    hash;     // хэш RAW-массива с допуском по джиттеру
    uint16_t    length;
    uint8_t     valid;
} ir_template_t;

class OmniaIRLearn {
private:
    ir_template_t        m_saved_templates[IR_MAX_COMMANDS];
    uint8_t              m_registered_cmds_count;
    volatile bool        m_is_learning_mode;
    ir_action_t          m_target_learning_action;
    rmt_channel_handle_t m_rx_chan;
    QueueHandle_t        m_queue;
    rmt_symbol_word_t    m_rx_buf[IR_RAW_BUFFER_SIZE];
    rmt_receive_config_t m_rx_cfg;

    uint32_t calculate_raw_hash(const rmt_symbol_word_t* rmt_symbols, size_t len);
    static bool on_rx_done(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t* edata, void* user_data);
    static void ir_task(void* arg);

public:
    OmniaIRLearn();
    void init();
    void start_learning(ir_action_t action);
    void save_to_nvs();
    void load_from_nvs();
    void process_rx_symbols(const rmt_symbol_word_t* symbols, size_t len);
};

#endif // IR_LEARN_H
