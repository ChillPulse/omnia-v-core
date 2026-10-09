#include "audio_buffer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>

static const char* TAG = "OMNIA_BUFFER";

OmniaAudioBuffer::OmniaAudioBuffer()
    : m_buffer(nullptr), m_head(0), m_tail(0), m_fill_count(0), m_mutex(nullptr) {}

OmniaAudioBuffer::~OmniaAudioBuffer()
{
    if (m_buffer) heap_caps_free(m_buffer);
    if (m_mutex)  vSemaphoreDelete(m_mutex);
}

bool OmniaAudioBuffer::init()
{
    m_mutex = xSemaphoreCreateMutex();
    if (!m_mutex) return false;

    // Строго во внешней Octal PSRAM (ТЗ: не тратим внутреннюю SRAM под поток)
    m_buffer = (uint8_t*)heap_caps_malloc(AUDIO_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m_buffer) {
        ESP_LOGE(TAG, "Не удалось выделить Ring Buffer 4 МБ в PSRAM");
        return false;
    }
    clear();
    ESP_LOGI(TAG, "Ring Buffer %d байт выделен в PSRAM", AUDIO_BUFFER_SIZE);
    return true;
}

void OmniaAudioBuffer::clear()
{
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE) {
        m_head = 0;
        m_tail = 0;
        m_fill_count = 0;
        xSemaphoreGive(m_mutex);
    }
}

size_t OmniaAudioBuffer::write(const uint8_t* data, size_t size)
{
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) return 0;

    size_t free_space = AUDIO_BUFFER_SIZE - m_fill_count;
    if (size > free_space) size = free_space;
    if (size == 0) {
        xSemaphoreGive(m_mutex);
        return 0;
    }

    size_t first = AUDIO_BUFFER_SIZE - m_head;
    if (first > size) first = size;
    memcpy(m_buffer + m_head, data, first);
    m_head = (m_head + first) % AUDIO_BUFFER_SIZE;

    size_t rest = size - first;
    if (rest > 0) {
        memcpy(m_buffer + m_head, data + first, rest);
        m_head = (m_head + rest) % AUDIO_BUFFER_SIZE;
    }

    m_fill_count += size;
    xSemaphoreGive(m_mutex);
    return size;
}

size_t OmniaAudioBuffer::read(uint8_t* dest, size_t size)
{
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) return 0;

    if (size > m_fill_count) size = m_fill_count;
    if (size == 0) {
        xSemaphoreGive(m_mutex);
        return 0;
    }

    size_t first = AUDIO_BUFFER_SIZE - m_tail;
    if (first > size) first = size;
    memcpy(dest, m_buffer + m_tail, first);
    m_tail = (m_tail + first) % AUDIO_BUFFER_SIZE;

    size_t rest = size - first;
    if (rest > 0) {
        memcpy(dest + first, m_buffer + m_tail, rest);
        m_tail = (m_tail + rest) % AUDIO_BUFFER_SIZE;
    }

    m_fill_count -= size;
    xSemaphoreGive(m_mutex);
    return size;
}

size_t OmniaAudioBuffer::get_free_space()
{
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) return 0;
    size_t f = AUDIO_BUFFER_SIZE - m_fill_count;
    xSemaphoreGive(m_mutex);
    return f;
}

uint8_t OmniaAudioBuffer::get_fill_percentage()
{
    if (xSemaphoreTake(m_mutex, portMAX_DELAY) != pdTRUE) return 0;
    uint8_t pct = (uint8_t)((m_fill_count * 100ULL) / AUDIO_BUFFER_SIZE);
    xSemaphoreGive(m_mutex);
    return pct;
}
