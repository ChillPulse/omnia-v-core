#include "omnia_ir_nec.h"

static bool in_range(uint16_t v, unsigned lo, unsigned hi)
{
    return v >= lo && v <= hi;
}

omnia_ir_result_t omnia_ir_nec_decode(const omnia_ir_pair_t *p, size_t n, omnia_ir_nec_t *out)
{
    if (!p || !n || !out) return OMNIA_IR_BAD;
    /* Выход VS1838B активен LOW. Повторы не содержат адрес/команду. */
    if (!in_range(p[0].low_us, 8000, 10000)) return OMNIA_IR_BAD;
    if (in_range(p[0].high_us, 1900, 2700)) {
        return n >= 2 && in_range(p[1].low_us, 350, 800) ? OMNIA_IR_NEC_REPEAT : OMNIA_IR_BAD;
    }
    /* Require the final stop mark as well as all 32 bits: do not learn a truncated frame. */
    if (!in_range(p[0].high_us, 3800, 5200) || n != 34 ||
        !in_range(p[33].low_us, 350, 800)) return OMNIA_IR_BAD;
    uint32_t bits = 0;
    for (unsigned i = 0; i < 32; ++i) {
        const omnia_ir_pair_t pair = p[i + 1U];
        if (!in_range(pair.low_us, 350, 800)) return OMNIA_IR_BAD;
        if (in_range(pair.high_us, 1250, 2100)) bits |= 1UL << i;
        else if (!in_range(pair.high_us, 350, 900)) return OMNIA_IR_BAD;
    }
    uint8_t cmd = (uint8_t)(bits >> 16);
    uint8_t inv = (uint8_t)(bits >> 24);
    if ((uint8_t)(cmd ^ inv) != UINT8_MAX) return OMNIA_IR_BAD;
    out->raw_lsb_first = bits;
    out->address = (uint16_t)bits; /* raw address bytes, supports classic and extended NEC */
    out->command = cmd;
    return OMNIA_IR_NEC_FRAME;
}
