/*
 * DLNA DMR (Digital Media Renderer): HTTP-часть.
 * ТЗ, Том 2, Гл. 3. Эндпоинты регистрируются на ОДНОМ общем httpd-сервере
 * (порт 80 занят PWA/WebSocket), поэтому отдельный httpd_start здесь не вызывается.
 *
 * НЕ реализовано: SSDP-обнаружение и SOAP AVTransport/RenderingControl (см. README).
 */

#include "omnia.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <string.h>

static const char* TAG = "OMNIA_DLNA";

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static const char* DMR_XML_DESC =
    "<?xml version=\"1.0\"?>\r\n"
    "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\r\n"
    "  <specVersion><major>1</major><minor>0</minor></specVersion>\r\n"
    "  <device>\r\n"
    "    <deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>\r\n"
    "    <friendlyName>Omnia v-core Streamer</friendlyName>\r\n"
    "    <manufacturer>Hi-End DIY</manufacturer>\r\n"
    "    <modelName>Omnia-v-core</modelName>\r\n"
    "  </device>\r\n"
    "</root>";

static esp_err_t dlna_desc_get_handler(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/xml");
    httpd_resp_send(req, DMR_XML_DESC, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Пишем всё в кольцевой буфер; если он полон — ждем, а не теряем байты
static void write_all_to_ring(const uint8_t* p, size_t n)
{
    while (n > 0) {
        size_t w = audio_buffer.write(p, n);
        p += w;
        n -= w;
        if (w == 0) vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static esp_err_t dlna_stream_post_handler(httpd_req_t* req)
{
    char buf[2048];
    int remaining = (int)req->content_len;
    if (remaining <= 0) {
        httpd_resp_send_err(req, HTTPD_411_LENGTH_REQUIRED, "Content-Length required");
        return ESP_FAIL;
    }

    while (remaining > 0) {
        int ret = httpd_req_recv(req, buf, MIN(remaining, (int)sizeof(buf)));
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (ret <= 0) {
            ESP_LOGW(TAG, "Поток прерван клиентом");
            return ESP_FAIL;
        }
        write_all_to_ring((const uint8_t*)buf, (size_t)ret);
        remaining -= ret;
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

// Перекодировка CP1251 -> UTF-8 для тегов на русском языке (ТЗ, Том 2, Гл. 3)
void cp1251_to_utf8(char* out, const char* in, size_t max_out_len)
{
    size_t o = 0;
    while (*in && o + 3 < max_out_len) {
        uint8_t c = (uint8_t)*in++;
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c >= 0xC0) {
            uint16_t u = 0x0410 + (c - 0xC0);           // А..я
            out[o++] = (char)(0xC0 | (u >> 6));
            out[o++] = (char)(0x80 | (u & 0x3F));
        } else if (c == 0xA8) {                          // Ё
            out[o++] = (char)0xD0; out[o++] = (char)0x81;
        } else if (c == 0xB8) {                          // ё
            out[o++] = (char)0xD1; out[o++] = (char)0x91;
        } else {
            out[o++] = '?';
        }
    }
    out[o] = '\0';
}

void dlna_register_handlers(httpd_handle_t server)
{
    httpd_uri_t desc = {
        .uri = "/dlna/desc.xml", .method = HTTP_GET,
        .handler = dlna_desc_get_handler, .user_ctx = NULL,
    };
    httpd_uri_t stream = {
        .uri = "/dlna/stream", .method = HTTP_POST,
        .handler = dlna_stream_post_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &desc);
    httpd_register_uri_handler(server, &stream);
    ESP_LOGI(TAG, "DLNA DMR эндпоинты зарегистрированы");
}
