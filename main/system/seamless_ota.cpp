/*
 * Фоновое OTA-обновление во второй раздел (ota_0 / ota_1), ТЗ, Том 5, Гл. 4.
 * Прошивка POST-ом на /api/ota_upgrade (сырой бинарник omnia_firmware.bin).
 * Проверка целостности — esp_ota_end (образ валидируется встроенной подписью/CRC).
 *
 * Важно: запись во флеш приостанавливает кэш второго ядра. Плавность звука во время
 * прошивки требует проверки на железе (см. README).
 */

#include "omnia.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "OMNIA_OTA";

#ifndef OMNIA_MIN
#define OMNIA_MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static esp_err_t ota_upgrade_post_handler(httpd_req_t* req)
{
    int remaining = (int)req->content_len;
    if (remaining <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Пустое тело запроса");
        return ESP_FAIL;
    }

    const esp_partition_t* part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Пассивный OTA-раздел не найден");
        return ESP_FAIL;
    }
    if ((size_t)remaining > part->size) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Прошивка больше раздела");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Запись прошивки (%d байт) в раздел %s", remaining, part->label);
    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin");
        return ESP_FAIL;
    }

    char buf[1024];
    while (remaining > 0) {
        int ret = httpd_req_recv(req, buf, OMNIA_MIN(remaining, (int)sizeof(buf)));
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (ret <= 0) {
            esp_ota_abort(handle);
            ESP_LOGE(TAG, "Соединение прервано, OTA отменен");
            return ESP_FAIL;
        }
        err = esp_ota_write(handle, buf, ret);
        if (err != ESP_OK) {
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_write");
            return ESP_FAIL;
        }
        remaining -= ret;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Образ не прошел проверку: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Проверка образа не пройдена");
        return ESP_FAIL;
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_set_boot_partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Обновление записано. Перезагрузка в новый раздел.");
    httpd_resp_sendstr(req, "Upgrade Success. System rebooting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

void register_ota_handlers(httpd_handle_t server)
{
    httpd_uri_t ota_uri = {};
    ota_uri.uri     = "/api/ota_upgrade";
    ota_uri.method  = HTTP_POST;
    ota_uri.handler = ota_upgrade_post_handler;
    httpd_register_uri_handler(server, &ota_uri);
}
