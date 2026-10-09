/*
 * Веб-сервер Omnia (ТЗ, Том 4): один httpd на порту 80.
 *   GET  /            -> PWA index.html.gz из LittleFS
 *   WS   /ws          -> JSON-команды и широковещательный статус раз в 200 мс
 *   GET  /dlna/*      -> DLNA DMR (network_services.cpp)
 *   POST /api/ota_upgrade -> фоновое OTA-обновление (seamless_ota.cpp)
 *
 * Валидация: пакеты вне схемы отбрасываются (ТЗ, Том 4, Гл. 2).
 * Команды-расширения (KEY, TEST_NOISE, ADD_STATION, IR_LEARN, PLAY_URL, ANTI_AD) описаны в README.
 */

#include "omnia.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "cJSON.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* TAG = "OMNIA_WEB";

#define WS_MAX_PAYLOAD      2048
#define USER_STATIONS_PATH  "/littlefs/UserStations.json"
#define USER_STATIONS_MAX   16384

static httpd_handle_t web_server = NULL;

// ---------- Утилиты валидации ----------
static bool get_num(const cJSON* root, const char* key, double lo, double hi, double* out)
{
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(it) || it->valuedouble < lo || it->valuedouble > hi) return false;
    *out = it->valuedouble;
    return true;
}

static bool get_str(const cJSON* root, const char* key, const char** out)
{
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(it) || it->valuestring == NULL) return false;
    *out = it->valuestring;
    return true;
}

static bool get_bool(const cJSON* root, const char* key, bool* out)
{
    const cJSON* it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsBool(it)) return false;
    *out = cJSON_IsTrue(it);
    return true;
}

static ir_action_t ir_action_from_name(const char* s, bool* ok)
{
    static const struct { const char* name; ir_action_t act; } MAP[] = {
        {"POWER", IR_CMD_POWER}, {"MUTE", IR_CMD_MUTE}, {"SOURCE", IR_CMD_SOURCE},
        {"FAVORITE", IR_CMD_FAVORITE}, {"PLAY", IR_CMD_PLAY}, {"VOL_UP", IR_CMD_VOL_UP},
        {"VOL_DOWN", IR_CMD_VOL_DOWN}, {"NEXT", IR_CMD_TRACK_NEXT}, {"PREV", IR_CMD_TRACK_PREV},
    };
    *ok = false;
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
        if (strcmp(s, MAP[i].name) == 0) { *ok = true; return MAP[i].act; }
    }
    return IR_CMD_POWER;
}

static omnia_key_t key_from_name(const char* s)
{
    static const struct { const char* name; omnia_key_t key; } MAP[] = {
        {"POWER", OMNIA_KEY_POWER}, {"MUTE", OMNIA_KEY_MUTE}, {"SOURCE", OMNIA_KEY_SOURCE},
        {"FAVORITE", OMNIA_KEY_FAVORITE}, {"PREV", OMNIA_KEY_PREV}, {"NEXT", OMNIA_KEY_NEXT},
        {"PLAY", OMNIA_KEY_PLAY}, {"VOL_UP", OMNIA_KEY_VOL_UP}, {"VOL_DOWN", OMNIA_KEY_VOL_DOWN},
    };
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
        if (strcmp(s, MAP[i].name) == 0) return MAP[i].key;
    }
    return OMNIA_KEY_NONE;
}

// ---------- Пользовательские станции (Быстрый Линк) ----------
static bool add_user_station(const char* name, const char* url)
{
    cJSON* arr = NULL;
    FILE* f = fopen(USER_STATIONS_PATH, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < USER_STATIONS_MAX) {
            char* buf = (char*)malloc(sz + 1);
            if (buf) {
                size_t rd = fread(buf, 1, sz, f);
                buf[rd] = '\0';
                arr = cJSON_Parse(buf);
                free(buf);
            }
        }
        fclose(f);
    }
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        arr = cJSON_CreateArray();
    }

    cJSON* st = cJSON_CreateObject();
    cJSON_AddStringToObject(st, "name", name);
    cJSON_AddStringToObject(st, "url", url);
    cJSON_AddItemToArray(arr, st);

    char* out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!out) return false;

    bool ok = false;
    f = fopen(USER_STATIONS_PATH, "w");
    if (f) {
        ok = fputs(out, f) >= 0;
        fclose(f);
    }
    cJSON_free(out);
    return ok;
}

// ---------- Обработка JSON-команд (ТЗ, Том 4, Гл. 2) ----------
static void handle_incoming_json_command(char* json_str)
{
    cJSON* root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGW(TAG, "Пакет отброшен: не JSON");
        return;
    }
    const char* cmd = NULL;
    if (!get_str(root, "cmd", &cmd)) {
        cJSON_Delete(root);
        return;
    }
    double d1 = 0, d2 = 0, d3 = 0, d4 = 0;
    const char* s1 = NULL;
    const char* s2 = NULL;
    bool b1 = false;

    if (strcmp(cmd, "TRANSPORT") == 0) {
        if (get_str(root, "action", &s1)) {
            if (strcmp(s1, "SOURCE_SELECT") == 0) {
                if (get_str(root, "value", &s2)) omnia_select_source(s2);
            } else if (strcmp(s1, "PLAY") == 0 || strcmp(s1, "PAUSE") == 0 ||
                       strcmp(s1, "STOP") == 0 || strcmp(s1, "TOGGLE") == 0 ||
                       strcmp(s1, "NEXT") == 0 || strcmp(s1, "PREV") == 0) {
                omnia_transport(s1);
            }
        }
    }
    else if (strcmp(cmd, "SEEK") == 0) {
        if (get_num(root, "position_sec", 0, 86400, &d1)) omnia_seek((uint32_t)d1);
    }
    else if (strcmp(cmd, "VOLUME") == 0) {
        if (get_num(root, "value", 0, 100, &d1)) omnia_set_volume((uint8_t)d1);
    }
    else if (strcmp(cmd, "MUTE") == 0) {
        if (get_bool(root, "active", &b1)) omnia_set_mute(b1);
    }
    else if (strcmp(cmd, "KEY") == 0) {
        if (get_str(root, "key", &s1)) {
            omnia_key_t k = key_from_name(s1);
            if (k != OMNIA_KEY_NONE) omnia_dispatch_key(k);
        }
    }
    else if (strcmp(cmd, "USER_PEQ") == 0) {
        if (get_num(root, "band", 0, OMNIA_USER_BANDS - 1, &d1) &&
            get_num(root, "freq", 20, 20000, &d2) &&
            get_num(root, "q", 0.1, 20, &d3) &&
            get_num(root, "gain", -18, 18, &d4)) {
            dsp_processor.set_user_peq((uint8_t)d1, (float)d2, (float)d3, (float)d4);
        }
    }
    else if (strcmp(cmd, "CALIBRATE_ROOM") == 0) {
        if (get_num(root, "band", 0, OMNIA_ROOM_BANDS - 1, &d1) &&
            get_num(root, "freq", 20, 400, &d2) &&
            get_num(root, "q", 0.1, 20, &d3) &&
            get_num(root, "gain", -18, 0, &d4)) {
            dsp_processor.set_room_correction((uint8_t)d1, (float)d2, (float)d3, (float)d4);
        }
    }
    else if (strcmp(cmd, "TEST_NOISE") == 0) {
        if (get_bool(root, "active", &b1)) omnia_set_test_noise(b1);
    }
    else if (strcmp(cmd, "SET_CONFIG") == 0) {
        if (get_str(root, "param", &s1) && get_num(root, "value", 0, 100, &d1)) {
            int v = (int)d1;
            if (strcmp(s1, "USB_AUTOWAKE") == 0 && v <= 1) {
                g_usb_autowake_enabled = (v == 1);
            } else if (strcmp(s1, "STANDBY_DISPLAY_MODE") == 0 && v <= 2) {
                g_standby_display_mode = (uint8_t)v;
            } else if (strcmp(s1, "STANDBY_RING_MODE") == 0 && v <= 3) {
                g_standby_ring_mode = (uint8_t)v;
            } else if (strcmp(s1, "AMP_MUTE_POLARITY") == 0 && v <= 1) {
                g_amp_mute_polarity = (uint8_t)v;
                hw_set_mute(omnia_is_muted());   // переприменить уровень на пине
            } else if (strcmp(s1, "ANTI_AD") == 0 && v <= 2) {
                omnia_set_anti_ad((uint8_t)v);
            }
        }
    }
    else if (strcmp(cmd, "ADD_STATION") == 0) {
        const char *name = NULL, *url = NULL;
        if (get_str(root, "name", &name) && get_str(root, "url", &url) &&
            strlen(name) < 64 && strlen(url) < 256 &&
            (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0)) {
            if (add_user_station(name, url)) ESP_LOGI(TAG, "Станция добавлена: %s", name);
        }
    }
    else if (strcmp(cmd, "IR_LEARN") == 0) {
        if (get_str(root, "action", &s1)) {
            bool ok = false;
            ir_action_t a = ir_action_from_name(s1, &ok);
            if (ok) ir_learn.start_learning(a);
        }
    }
    else if (strcmp(cmd, "PLAY_URL") == 0) {
        // Воспроизведение HTTP-потока (станции Radio API) — в разработке, см. README
        if (get_str(root, "url", &s1)) ESP_LOGW(TAG, "PLAY_URL принят, потоковый клиент не реализован");
    }
    else {
        ESP_LOGD(TAG, "Неизвестная команда: %s", cmd);
    }

    cJSON_Delete(root);
}

// ---------- Широковещательный статус ----------
static char* build_state_json(void)
{
    omnia_state_t s;
    omnia_state_snapshot(&s);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", omnia_play_state_name(s.play_state));
    cJSON_AddStringToObject(root, "source", s.source);
    cJSON_AddNumberToObject(root, "volume", s.volume);
    cJSON_AddBoolToObject(root, "is_muted", s.is_muted);
    cJSON_AddStringToObject(root, "track", s.track);
    cJSON_AddStringToObject(root, "artist", s.artist);
    cJSON_AddNumberToObject(root, "elapsed", s.elapsed);
    cJSON_AddNumberToObject(root, "total", s.total);
    cJSON_AddNumberToObject(root, "bitrate", s.bitrate);
    cJSON_AddStringToObject(root, "format", s.format);
    cJSON_AddNumberToObject(root, "wifi_rssi", s.wifi_rssi);
    cJSON_AddNumberToObject(root, "dsp_idx", s.dsp_idx);
    cJSON_AddNumberToObject(root, "lux", s.lux);
    cJSON_AddNumberToObject(root, "anti_ad", s.anti_ad);
    cJSON_AddBoolToObject(root, "test_noise", s.test_noise);

    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

// Выполняется в задаче httpd: безопасная отправка всем WebSocket-клиентам
static void ws_send_work(void* arg)
{
    char* json = (char*)arg;
    if (!web_server) { free(json); return; }

    httpd_ws_frame_t frame = {};
    frame.type    = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t*)json;
    frame.len     = strlen(json);

    size_t n = 8;
    int fds[8];
    if (httpd_get_client_list(web_server, &n, fds) == ESP_OK) {
        for (size_t i = 0; i < n; i++) {
            if (httpd_ws_get_fd_info(web_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_send_data(web_server, fds[i], &frame);
            }
        }
    }
    free(json);
}

void ws_broadcast_state(void)
{
    if (!web_server) return;
    char* json = build_state_json();
    if (!json) return;
    if (httpd_queue_work(web_server, ws_send_work, json) != ESP_OK) {
        free(json);
    }
}

// ---------- Обработчики ----------
static esp_err_t ws_handler(httpd_req_t* req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket: рукопожатие выполнено");
        return ESP_OK;
    }

    httpd_ws_frame_t pkt = {};
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK) return ret;
    if (pkt.type != HTTPD_WS_TYPE_TEXT || pkt.len == 0 || pkt.len > WS_MAX_PAYLOAD) {
        return ESP_OK;   // бинарные/слишком большие фреймы игнорируем
    }

    uint8_t* buf = (uint8_t*)malloc(pkt.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (ret == ESP_OK) {
        buf[pkt.len] = '\0';
        handle_incoming_json_command((char*)buf);
        ws_broadcast_state();   // мгновенное обновление интерфейса
    }
    free(buf);
    return ret;
}

// Отдача сжатого PWA-бандла блоками по 4 КБ (ТЗ, Том 4, Гл. 4 — исправленная версия)
static esp_err_t root_get_handler(httpd_req_t* req)
{
    FILE* f = fopen("/littlefs/index.html.gz", "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Файл интерфейса не найден во Flash");
        return ESP_FAIL;
    }

    const size_t chunk_size = 4096;
    char* buffer = (char*)malloc(chunk_size);
    if (!buffer) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

    size_t rd;
    while ((rd = fread(buffer, 1, chunk_size, f)) > 0) {
        if (httpd_resp_send_chunk(req, buffer, rd) != ESP_OK) {
            free(buffer);
            fclose(f);
            return ESP_FAIL;
        }
    }
    free(buffer);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

void init_omnia_pwa_webserver(void)
{
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path              = "/littlefs";
    conf.partition_label        = "littlefs";
    conf.format_if_mount_failed = false;
    conf.dont_mount             = false;
    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Сбой монтирования LittleFS: %s", esp_err_to_name(ret));
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port      = 80;
    config.max_uri_handlers = 16;
    config.stack_size       = 10240;
    config.lru_purge_enable = true;

    if (httpd_start(&web_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Не удалось запустить httpd");
        return;
    }

    httpd_uri_t root_uri = {};
    root_uri.uri = "/";
    root_uri.method = HTTP_GET;
    root_uri.handler = root_get_handler;
    httpd_register_uri_handler(web_server, &root_uri);

    httpd_uri_t ws_uri = {};
    ws_uri.uri = "/ws";
    ws_uri.method = HTTP_GET;
    ws_uri.handler = ws_handler;
    ws_uri.is_websocket = true;
    httpd_register_uri_handler(web_server, &ws_uri);

    dlna_register_handlers(web_server);
    register_ota_handlers(web_server);

    ESP_LOGI(TAG, "Web-сервер Omnia запущен на порту 80");
}
