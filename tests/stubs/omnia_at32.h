#ifndef OMNIA_AT32_STORAGE_TEST_STUB_H
#define OMNIA_AT32_STORAGE_TEST_STUB_H
#include <stdbool.h>
#include <stdint.h>
typedef struct { bool present; uint8_t bus_type; uint32_t jedec_id, capacity_bytes; uint16_t sector_size, page_size; bool addr_4byte; } omnia_spi_flash_info_t;
typedef struct { uint32_t sentinel; } omnia_dsp_ctx_t;
void omnia_ext_flash_init(omnia_spi_flash_info_t *out);
const omnia_spi_flash_info_t *omnia_ext_flash_get_info(void);
bool omnia_preset_save(uint8_t slot, const char *name, const omnia_dsp_ctx_t *dsp);
bool omnia_preset_load(uint8_t slot, omnia_dsp_ctx_t *dsp, char *out_name24);
#endif
