/**
 * @file    omnia_fw_version.h
 * @brief   Единственный источник версии ядра DSP (AT32), сообщается в OMNIA_EVT_SYS_INFO.
 *
 * Семантическая версия ядра DSP — только здесь; uart_link.c её больше не хардкодит.
 * Диагностические метки сборки приходят из scripts/pio_fw_version.py (PlatformIO
 * pre-script): OMNIA_FW_GIT_DESCRIBE / OMNIA_FW_GIT_BUILD — git-derived, по проводу
 * не уходят, меняются сами и ошибок не вызывают (fallback "unknown"/0).
 *
 * Правила:
 *  - bump при любом изменении кода, попадающего в AT32-прошивку (host-тесты, docs — нет);
 *  - docs-only релизы версию НЕ трогают (поэтому вывод из CHANGELOG отклонён —
 *    он бы врал на docs-only релизах);
 *  - после bump — новая секция CHANGELOG с той же версией.
 */
#ifndef OMNIA_FW_VERSION_H
#define OMNIA_FW_VERSION_H

#define OMNIA_FW_MAJOR 2U
#define OMNIA_FW_MINOR 4U
#define OMNIA_FW_PATCH 0U

#if !defined(OMNIA_FW_GIT_DESCRIBE)
#define OMNIA_FW_GIT_DESCRIBE "unknown"
#endif
#if !defined(OMNIA_FW_GIT_BUILD)
#define OMNIA_FW_GIT_BUILD 0
#endif

#endif /* OMNIA_FW_VERSION_H */
