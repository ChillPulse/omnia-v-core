/**
 * @file    at32_bridge.c
 * @brief   Драйвер мастера протокола OMNIA v2 (ESP32-S3 UART1 GPIO7/8 @ 921600 бод):
 *          гарантированная доставка с подтверждением ACK, до 3 повторов при ошибке CRC,
 *          бесщёлчковое переключение Fs (Policy 1) и приём телеметрии VU / VBUS от AT32F403.
 */

#include "omnia_esp32.h"
#include <string.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "omnia_at32_link";

#define BRIDGE_UART_PORT        UART_NUM_1
#define BRIDGE_RX_BUF_SIZE      2048
#define BRIDGE_TX_BUF_SIZE      1024
#define BRIDGE_MAX_RETRIES      3

static SemaphoreHandle_t     g_tx_mutex       = NULL;
static SemaphoreHandle_t     g_ack_sem        = NULL;
static SemaphoreHandle_t     g_info_sem       = NULL;
static SemaphoreHandle_t     g_state_lock     = NULL;

static uint8_t               g_seq_counter    = 1U;
static volatile uint8_t      g_wait_seq       = 0U;
static volatile uint8_t      g_wait_cmd       = 0U;
static volatile uint8_t      g_last_ack_code  = OMNIA_STATUS_OK;

static omnia_sys_info_t      g_cached_info;
static omnia_at32_status_t   g_cached_status;
static omnia_signal_meters_t g_cached_meters;
static uint8_t               g_prev_vbus      = 0xFFU;
static omnia_vbus_event_cb_t g_vbus_cb        = NULL;

static void handle_at32_frame(const omnia_frame_hdr_t *hdr, const uint8_t *payload)
{
    const uint8_t flags = OMNIA_GET_FLAGS(hdr->ver_flags);

    if (hdr->cmd == OMNIA_EVT_ACK_NACK && hdr->payload_len >= sizeof(omnia_ack_payload_t)) {
        const omnia_ack_payload_t *ack = (const omnia_ack_payload_t *)payload;
        if (hdr->seq == g_wait_seq && ack->acked_cmd == g_wait_cmd) {
            g_last_ack_code = (flags & OMNIA_FLAG_IS_NACK) ? ack->status_code : OMNIA_STATUS_OK;
            xSemaphoreGive(g_ack_sem);
        }
        return;
    }

    if (hdr->cmd == OMNIA_EVT_SYS_INFO && hdr->payload_len == sizeof(omnia_sys_info_t)) {
        xSemaphoreTake(g_state_lock, portMAX_DELAY);
        memcpy(&g_cached_info, payload, sizeof(g_cached_info));
        xSemaphoreGive(g_state_lock);
        if (hdr->seq == g_wait_seq) {
            xSemaphoreGive(g_info_sem);
        }
        return;
    }

    if (hdr->cmd == OMNIA_EVT_STATUS && hdr->payload_len == sizeof(omnia_at32_status_t)) {
        omnia_at32_status_t st;
        memcpy(&st, payload, sizeof(st));

        xSemaphoreTake(g_state_lock, portMAX_DELAY);
        g_cached_status = st;
        xSemaphoreGive(g_state_lock);

        if (st.usb_vbus_detected != g_prev_vbus) {
            g_prev_vbus = st.usb_vbus_detected;
            ESP_LOGI(TAG, "Событие USB-C VBUS от AT32: vbus=%u, активный порт=%s",
                     st.usb_vbus_detected, st.usb_active_port ? "USB-C (UAC1 DAC)" : "USB-A (MSC Flash)");
            if (g_vbus_cb) {
                g_vbus_cb(st.usb_vbus_detected != 0U, st.usb_active_port);
            }
        }
        return;
    }

    if (hdr->cmd == OMNIA_EVT_SIGNAL_METERS && hdr->payload_len == sizeof(omnia_signal_meters_t)) {
        xSemaphoreTake(g_state_lock, portMAX_DELAY);
        memcpy(&g_cached_meters, payload, sizeof(g_cached_meters));
        xSemaphoreGive(g_state_lock);
    }
}

static void uart_rx_task(void *arg)
{
    (void)arg;
    omnia_proto_parser_t parser;
    uint8_t rx_chunk[256];

    omnia_proto_parser_init(&parser);

    while (1) {
        const int n = uart_read_bytes(BRIDGE_UART_PORT, rx_chunk, sizeof(rx_chunk), pdMS_TO_TICKS(20));
        for (int i = 0; i < n; ++i) {
            if (omnia_proto_parser_feed(&parser, rx_chunk[i])) {
                handle_at32_frame(&parser.hdr, parser.payload);
            }
        }
    }
}

esp_err_t omnia_at32_bridge_init(omnia_vbus_event_cb_t vbus_cb)
{
    g_vbus_cb    = vbus_cb;
    g_tx_mutex   = xSemaphoreCreateMutex();
    g_ack_sem    = xSemaphoreCreateBinary();
    g_info_sem   = xSemaphoreCreateBinary();
    g_state_lock = xSemaphoreCreateMutex();

    if (!g_tx_mutex || !g_ack_sem || !g_info_sem || !g_state_lock) {
        return ESP_ERR_NO_MEM;
    }

    const uart_config_t uart_cfg = {
        .baud_rate  = OMNIA_UART_DEFAULT_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(BRIDGE_UART_PORT, BRIDGE_RX_BUF_SIZE, BRIDGE_TX_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(BRIDGE_UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(BRIDGE_UART_PORT,
                                 OMNIA_ESP_AT32_TX_GPIO,
                                 OMNIA_ESP_AT32_RX_GPIO,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    /* Привязываем приёмник протокола к Core 0 (ядро управления и UI) */
    xTaskCreatePinnedToCore(uart_rx_task, "at32_uart_rx", 4096, NULL, 12, NULL, 0);
    ESP_LOGI(TAG, "Канал связи с AT32F403 запущен (UART1 TX=GPIO%d, RX=GPIO%d @ %u бод)",
             OMNIA_ESP_AT32_TX_GPIO, OMNIA_ESP_AT32_RX_GPIO, (unsigned)OMNIA_UART_DEFAULT_BAUD);
    return ESP_OK;
}

esp_err_t omnia_at32_send_cmd_sync(uint8_t cmd, const void *payload, uint16_t len, uint32_t timeout_ms)
{
    uint8_t frame[OMNIA_PROTO_MAX_FRAME_SIZE];

    xSemaphoreTake(g_tx_mutex, portMAX_DELAY);
    const uint8_t seq = g_seq_counter++;
    const size_t frame_bytes = omnia_proto_build_frame(
        frame, sizeof(frame), OMNIA_FLAG_ACK_REQ, seq, cmd, payload, len);
    if (frame_bytes == 0U) {
        xSemaphoreGive(g_tx_mutex);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t result = ESP_ERR_TIMEOUT;
    for (int attempt = 0; attempt < BRIDGE_MAX_RETRIES; ++attempt) {
        (void)xSemaphoreTake(g_ack_sem, 0);
        g_wait_seq      = seq;
        g_wait_cmd      = cmd;
        g_last_ack_code = OMNIA_STATUS_ERR_CRC;

        uart_write_bytes(BRIDGE_UART_PORT, frame, frame_bytes);
        if (xSemaphoreTake(g_ack_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
            if (g_last_ack_code == OMNIA_STATUS_OK) {
                result = ESP_OK;
                break;
            }
            if (g_last_ack_code != OMNIA_STATUS_ERR_CRC) {
                ESP_LOGW(TAG, "AT32 вернул отказ NACK=%u на команду 0x%02X", g_last_ack_code, cmd);
                result = ESP_ERR_INVALID_STATE;
                break;
            }
        }
        ESP_LOGW(TAG, "Повтор отправки команды 0x%02X (попытка %d/%d)", cmd, attempt + 1, BRIDGE_MAX_RETRIES);
    }

    xSemaphoreGive(g_tx_mutex);
    return result;
}

esp_err_t omnia_at32_query_sys_info(omnia_sys_info_t *out_info, uint32_t timeout_ms)
{
    uint8_t frame[32];

    xSemaphoreTake(g_tx_mutex, portMAX_DELAY);
    const uint8_t seq = g_seq_counter++;
    const size_t n = omnia_proto_build_frame(frame, sizeof(frame), OMNIA_FLAG_ACK_REQ,
                                             seq, OMNIA_CMD_GET_SYS_INFO, NULL, 0U);
    (void)xSemaphoreTake(g_info_sem, 0);
    g_wait_seq = seq;
    uart_write_bytes(BRIDGE_UART_PORT, frame, n);

    const BaseType_t got = xSemaphoreTake(g_info_sem, pdMS_TO_TICKS(timeout_ms));
    xSemaphoreGive(g_tx_mutex);

    if (got != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (out_info) {
        xSemaphoreTake(g_state_lock, portMAX_DELAY);
        *out_info = g_cached_info;
        xSemaphoreGive(g_state_lock);
    }
    return ESP_OK;
}

esp_err_t omnia_at32_set_volume(float vol_db, float replaygain_db, float balance, uint16_t ramp_ms)
{
    const omnia_cmd_volume_t v = {
        .volume_db     = vol_db,
        .replaygain_db = replaygain_db,
        .balance       = balance,
        .ramp_ms       = ramp_ms
    };
    return omnia_at32_send_cmd_sync(OMNIA_CMD_SET_VOLUME, &v, sizeof(v), 40U);
}

/**
 * @brief  Атомарный бесщёлчковый протокол смены сетки частот 44.1 <-> 48 кГц (ТЗ v2.0 Раздел 4.2):
 *         1) Команда мягкого MUTE на AT32 + закрытие XSMT
 *         2) Переключение мастера I2S2 на AT32 на новую частоту
 *         3) Перенастройка ведомого I2S Slave TX на ESP32-S3
 *         4) Плавное открытие звука UNMUTE
 */
esp_err_t omnia_at32_switch_sample_rate_atomic(uint32_t new_fs_hz)
{
    const omnia_cmd_mute_t mute_req = { .ramp_ms = 20U, .hard_xsmt = 1U };
    esp_err_t err = omnia_at32_send_cmd_sync(OMNIA_CMD_MUTE, &mute_req, sizeof(mute_req), 60U);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(25));

    const omnia_cmd_set_fs_t fs_req = { .sample_rate_hz = new_fs_hz, .word_bits = 32U };
    err = omnia_at32_send_cmd_sync(OMNIA_CMD_SET_FS, &fs_req, sizeof(fs_req), 80U);
    if (err != ESP_OK) {
        return err;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(omnia_audio_i2s_reconfig_clock(new_fs_hz));
    vTaskDelay(pdMS_TO_TICKS(15));

    const omnia_cmd_mute_t unmute_req = { .ramp_ms = 25U, .hard_xsmt = 0U };
    return omnia_at32_send_cmd_sync(OMNIA_CMD_UNMUTE, &unmute_req, sizeof(unmute_req), 60U);
}

esp_err_t omnia_at32_set_bit_perfect(bool bit_perfect_direct)
{
    if (bit_perfect_direct) {
        (void)omnia_at32_set_volume(0.0f, 0.0f, 0.0f, 20U);
    }
    const omnia_bypass_mask_t bm = {
        .active_mask = bit_perfect_direct ? OMNIA_DSP_ALL_BYPASSED
                                          : (OMNIA_DSP_BIT_SUBSONIC_HPF |
                                             OMNIA_DSP_BIT_HEADROOM |
                                             OMNIA_DSP_BIT_AETHER |
                                             OMNIA_DSP_BIT_LOUDNESS |
                                             OMNIA_DSP_BIT_LIMITER),
        .ramp_ms = 35U
    };
    return omnia_at32_send_cmd_sync(OMNIA_CMD_SET_BYPASS_MASK, &bm, sizeof(bm), 40U);
}

esp_err_t omnia_at32_set_ab_compare(bool bypass_compare)
{
    const omnia_cmd_ab_compare_t ab = {
        .compare_bypass_en = bypass_compare ? 1U : 0U
    };
    return omnia_at32_send_cmd_sync(OMNIA_CMD_AB_COMPARE, &ab, sizeof(ab), 40U);
}

esp_err_t omnia_at32_load_default_studio_profile(void)
{
    const omnia_aether_params_t aether_studio = {
        .enable           = 1U,
        .bass_keep_mono   = 1U,
        .side_hpf_en      = 1U,
        .crossfeed_en     = 0U,
        .master_blend     = 0.55f,
        .width            = 1.28f,
        .center_anchor_db = 0.8f,
        .bass_xover_hz    = 95.0f,
        .side_hpf_hz      = 135.0f,
        .air_gain_db      = 2.2f,
        .air_freq_hz      = 11200.0f,
        .air_q            = 0.72f,
        .de_harsh         = 0.35f,
        .depth_amount     = 0.22f,
        .stage_size_ms    = 6.4f,
        .triode_warmth    = 0.14f,
        .crossfeed_level  = 0.0f,
        .crossfeed_fc_hz  = 700.0f
    };

    const omnia_loudness_params_t loud_cfg = {
        .enable            = 1U,
        .ref_volume_db     = -18.0f,
        .max_bass_boost_db = 6.5f,
        .max_high_boost_db = 3.0f
    };

    esp_err_t err = omnia_at32_send_cmd_sync(OMNIA_CMD_LOAD_AETHER, &aether_studio, sizeof(aether_studio), 50U);
    if (err != ESP_OK) {
        return err;
    }
    return omnia_at32_send_cmd_sync(OMNIA_CMD_LOAD_LOUDNESS, &loud_cfg, sizeof(loud_cfg), 50U);
}

void omnia_at32_get_latest_status(omnia_at32_status_t *out_status)
{
    if (!out_status || !g_state_lock) {
        return;
    }
    xSemaphoreTake(g_state_lock, portMAX_DELAY);
    *out_status = g_cached_status;
    xSemaphoreGive(g_state_lock);
}

void omnia_at32_get_latest_meters(omnia_signal_meters_t *out_meters)
{
    if (!out_meters || !g_state_lock) {
        return;
    }
    xSemaphoreTake(g_state_lock, portMAX_DELAY);
    *out_meters = g_cached_meters;
    xSemaphoreGive(g_state_lock);
}
