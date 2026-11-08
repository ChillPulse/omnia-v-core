/**
 * @file    omnia_crc16.h
 * @brief   Быстрый расчёт контрольной суммы CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF)
 *          Единая реализация без внешних зависимостей для ESP32-S3, AT32F403 и тестов.
 */

#ifndef OMNIA_CRC16_H
#define OMNIA_CRC16_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline uint16_t omnia_crc16_update(uint16_t crc, uint8_t byte)
{
    uint16_t x = (uint16_t)((crc >> 8) ^ byte);
    x ^= (uint16_t)(x >> 4);
    return (uint16_t)((crc << 8) ^ (uint16_t)(x << 12) ^ (uint16_t)(x << 5) ^ x);
}

static inline uint16_t omnia_crc16_ccitt(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint16_t crc = 0xFFFFU;
    for (size_t i = 0; i < len; ++i) {
        crc = omnia_crc16_update(crc, p[i]);
    }
    return crc;
}

#ifdef __cplusplus
}
#endif

#endif /* OMNIA_CRC16_H */
