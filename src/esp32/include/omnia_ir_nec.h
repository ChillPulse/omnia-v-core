#ifndef OMNIA_IR_NEC_H
#define OMNIA_IR_NEC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Один активный LOW импульс приёмника и последующий HIGH интервал, микросекунды. */
typedef struct { uint16_t low_us; uint16_t high_us; } omnia_ir_pair_t;
typedef struct { uint32_t raw_lsb_first; uint16_t address; uint8_t command; } omnia_ir_nec_t;
typedef enum { OMNIA_IR_BAD = 0, OMNIA_IR_NEC_FRAME, OMNIA_IR_NEC_REPEAT } omnia_ir_result_t;

omnia_ir_result_t omnia_ir_nec_decode(const omnia_ir_pair_t *pairs, size_t count, omnia_ir_nec_t *out);
#endif
