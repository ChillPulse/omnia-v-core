#include "omnia.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char* TAG = "OMNIA_STATE";

static SemaphoreHandle_t s_mutex = NULL;
static omnia_state_t     s_state;

// Конфигурация по умолчанию (ТЗ, Том 4, Гл. 4)
bool    g_usb_autowake_enabled = true;
uint8_t g_standby_display_mode = 2;
uint8_t g_standby_ring_mode    = 1;
uint8_t g_amp_mute_polarity    = 0;

static const char* SOURCE_NAMES[] = {"SD", "USB", "DLNA", "AIRPLAY", "NAS", "RADIO", "PC_DAC", "BT"};
static const char* SOURCE_CYCLE[] = {"SD", "USB", "DLNA", "NAS", "RADIO", "PC_DAC"};

void omnia_state_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    memset(&s_state, 0, sizeof(s_state));
    s_state.play_state = OMNIA_PLAY_STANDBY;   // при старте УМЗЧ обесточен (безопасный режим)
    snprintf(s_state.source, sizeof(s_state.source), "SD");
    snprintf(s_state.format, sizeof(s_state.format), "—");
    snprintf(s_state.track, sizeof(s_state.track), "Нет трека");
    snprintf(s_state.artist, sizeof(s_state.artist), "—");
    s_state.volume = 45;
    s_state.wifi_rssi = -100;
}

void omnia_state_lock(void)   { xSemaphoreTake(s_mutex, portMAX_DELAY); }
void omnia_state_unlock(void) { xSemaphoreGive(s_mutex); }

void omnia_state_snapshot(omnia_state_t* out)
{
    omnia_state_lock();
    *out = s_state;
    omnia_state_unlock();
}

const char* omnia_play_state_name(omnia_play_state_t st)
{
    switch (st) {
        case OMNIA_PLAY_STANDBY: return "STANDBY";
        case OMNIA_PLAY_STOPPED: return "STOPPED";
        case OMNIA_PLAY_PLAYING: return "PLAYING";
        case OMNIA_PLAY_PAUSED:  return "PAUSED";
        case OMNIA_PLAY_SEEKING: return "SEEKING";
        default:                 return "UNKNOWN";
    }
}

void omnia_set_play_state(omnia_play_state_t st)
{
    omnia_state_lock();
    s_state.play_state = st;
    omnia_state_unlock();
}

void omnia_set_volume(uint8_t pct)
{
    omnia_state_lock();
    s_state.volume = pct > 100 ? 100 : pct;
    omnia_state_unlock();
}

bool omnia_is_muted(void)
{
    omnia_state_lock();
    bool m = s_state.is_muted;
    omnia_state_unlock();
    return m;
}

void omnia_set_mute(bool mute)
{
    omnia_state_lock();
    s_state.is_muted = mute;
    omnia_state_unlock();
    hw_set_mute(mute);
}

void omnia_set_test_noise(bool on)
{
    omnia_state_lock();
    s_state.test_noise = on;
    omnia_state_unlock();
}

bool omnia_select_source(const char* name)
{
    bool ok = false;
    for (size_t i = 0; i < sizeof(SOURCE_NAMES) / sizeof(SOURCE_NAMES[0]); i++) {
        if (strcmp(name, SOURCE_NAMES[i]) == 0) { ok = true; break; }
    }
    if (!ok) {
        ESP_LOGW(TAG, "Неизвестный источник: %s", name);
        return false;
    }
    omnia_state_lock();
    snprintf(s_state.source, sizeof(s_state.source), "%s", name);
    omnia_state_unlock();
    if (strcmp(name, "USB") == 0) {
        init_usb_host_msc();
    }
    ESP_LOGI(TAG, "Источник: %s", name);
    return true;
}

void omnia_transport(const char* action)
{
    omnia_state_lock();
    if (strcmp(action, "PLAY") == 0) {
        if (s_state.play_state != OMNIA_PLAY_STANDBY) s_state.play_state = OMNIA_PLAY_PLAYING;
    } else if (strcmp(action, "PAUSE") == 0) {
        if (s_state.play_state == OMNIA_PLAY_PLAYING) s_state.play_state = OMNIA_PLAY_PAUSED;
    } else if (strcmp(action, "STOP") == 0) {
        s_state.play_state = OMNIA_PLAY_STOPPED;
        s_state.elapsed = 0;
    } else if (strcmp(action, "TOGGLE") == 0) {
        if (s_state.play_state == OMNIA_PLAY_PLAYING)      s_state.play_state = OMNIA_PLAY_PAUSED;
        else if (s_state.play_state != OMNIA_PLAY_STANDBY) s_state.play_state = OMNIA_PLAY_PLAYING;
    } else if (strcmp(action, "NEXT") == 0 || strcmp(action, "PREV") == 0) {
        // Переход по медиатеке появится вместе с индексатором (см. README)
        ESP_LOGI(TAG, "Трек: %s (очередь пока не подключена)", action);
    }
    omnia_state_unlock();
}

void omnia_seek(uint32_t position_sec)
{
    omnia_state_lock();
    if (s_state.total && position_sec > s_state.total) position_sec = s_state.total;
    s_state.elapsed = position_sec;
    s_state.play_state = OMNIA_PLAY_SEEKING;   // аудиодвижок глушит звук и возобновляет поток
    omnia_state_unlock();
}

void omnia_set_anti_ad(uint8_t mode)
{
    omnia_state_lock();
    s_state.anti_ad = mode > 2 ? 2 : mode;
    omnia_state_unlock();
}

void omnia_update_sensors(int8_t wifi_rssi, uint16_t lux)
{
    omnia_state_lock();
    s_state.wifi_rssi = wifi_rssi;
    s_state.lux = lux;
    omnia_state_unlock();
}

void omnia_set_audio_format(const omnia_audio_format_t* f)
{
    if (f->channels == 0 || f->channels > 2) return;
    if (f->bits != 16 && f->bits != 24 && f->bits != 32) return;
    xQueueOverwrite(audio_format_queue, f);
    omnia_state_lock();
    snprintf(s_state.format, sizeof(s_state.format), "PCM %u-bit / %.1f kHz", f->bits, f->sample_rate / 1000.0);
    s_state.bitrate = f->sample_rate * f->bits * f->channels / 1000;
    omnia_state_unlock();
}

void omnia_dispatch_key(omnia_key_t key)
{
    switch (key) {
        case OMNIA_KEY_POWER: {
            omnia_state_lock();
            bool standby = (s_state.play_state == OMNIA_PLAY_STANDBY);
            omnia_state_unlock();
            omnia_request_power(standby);   // из STANDBY — включить, иначе — уйти в сон
            break;
        }
        case OMNIA_KEY_MUTE:
            omnia_set_mute(!omnia_is_muted());
            break;
        case OMNIA_KEY_SOURCE: {
            omnia_state_lock();
            size_t n = sizeof(SOURCE_CYCLE) / sizeof(SOURCE_CYCLE[0]);
            size_t idx = 0;
            for (size_t i = 0; i < n; i++) {
                if (strcmp(s_state.source, SOURCE_CYCLE[i]) == 0) { idx = (i + 1) % n; break; }
            }
            omnia_state_unlock();
            omnia_select_source(SOURCE_CYCLE[idx]);
            break;
        }
        case OMNIA_KEY_FAVORITE:
            omnia_state_lock();
            s_state.dsp_idx = (s_state.dsp_idx + 1) % 4;
            omnia_state_unlock();
            break;
        case OMNIA_KEY_PLAY:     omnia_transport("TOGGLE"); break;
        case OMNIA_KEY_PREV:     omnia_transport("PREV");   break;
        case OMNIA_KEY_NEXT:     omnia_transport("NEXT");   break;
        case OMNIA_KEY_VOL_UP:
        case OMNIA_KEY_VOL_DOWN:
        case OMNIA_KEY_ENC_CW:
        case OMNIA_KEY_ENC_CCW: {
            int step = (key == OMNIA_KEY_VOL_UP || key == OMNIA_KEY_VOL_DOWN) ? 5 : 1;
            if (key == OMNIA_KEY_VOL_DOWN || key == OMNIA_KEY_ENC_CCW) step = -step;
            omnia_state_lock();
            int v = (int)s_state.volume + step;
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            s_state.volume = (uint8_t)v;
            omnia_state_unlock();
            break;
        }
        default:
            break;
    }
}
