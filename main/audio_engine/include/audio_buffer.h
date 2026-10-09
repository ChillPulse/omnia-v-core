#ifndef AUDIO_BUFFER_H
#define AUDIO_BUFFER_H

#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ТЗ, Том 2, Гл. 1: кольцевой буфер 4 МБ в Octal PSRAM
#define AUDIO_BUFFER_SIZE (4 * 1024 * 1024)

class OmniaAudioBuffer {
private:
    uint8_t*          m_buffer;
    size_t            m_head;
    size_t            m_tail;
    size_t            m_fill_count;
    SemaphoreHandle_t m_mutex;

public:
    OmniaAudioBuffer();
    ~OmniaAudioBuffer();
    bool   init();
    size_t write(const uint8_t* data, size_t size);   // возвращает сколько реально записано
    size_t read(uint8_t* dest, size_t size);          // возвращает сколько реально прочитано
    void   clear();
    size_t get_free_space();
    uint8_t get_fill_percentage();
};

#endif // AUDIO_BUFFER_H
