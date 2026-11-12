#include "omnia_ir_nec.h"
#include <assert.h>
#include <stdio.h>

static void make_frame(omnia_ir_pair_t p[34], uint32_t raw)
{
    p[0] = (omnia_ir_pair_t){9000, 4500};
    for (unsigned i = 0; i < 32; ++i)
        p[i + 1] = (omnia_ir_pair_t){560, (raw & (1UL << i)) ? 1690 : 560};
    p[33] = (omnia_ir_pair_t){560, 12000};
}

int main(void)
{
    omnia_ir_pair_t frame[34];
    omnia_ir_nec_t result = {0};
    make_frame(frame, 0xCB34ED12UL);
    assert(omnia_ir_nec_decode(frame, 34, &result) == OMNIA_IR_NEC_FRAME);
    assert(result.raw_lsb_first == 0xCB34ED12UL && result.address == 0xED12 && result.command == 0x34);
    assert(omnia_ir_nec_decode(frame, 32, &result) == OMNIA_IR_BAD);
    assert(omnia_ir_nec_decode(frame, 33, &result) == OMNIA_IR_BAD);
    frame[33].low_us = 0;
    assert(omnia_ir_nec_decode(frame, 34, &result) == OMNIA_IR_BAD);
    frame[33].low_us = 560;
    assert(omnia_ir_nec_decode(NULL, 34, &result) == OMNIA_IR_BAD);
    assert(omnia_ir_nec_decode(frame, 34, NULL) == OMNIA_IR_BAD);
    frame[8].low_us = 2000;
    assert(omnia_ir_nec_decode(frame, 34, &result) == OMNIA_IR_BAD);
    make_frame(frame, 0xCC34ED12UL); /* invalid command complement */
    assert(omnia_ir_nec_decode(frame, 34, &result) == OMNIA_IR_BAD);
    frame[0] = (omnia_ir_pair_t){9000, 2250};
    frame[1] = (omnia_ir_pair_t){560, 12000};
    assert(omnia_ir_nec_decode(frame, 2, &result) == OMNIA_IR_NEC_REPEAT);
    assert(omnia_ir_nec_decode(frame, 1, &result) == OMNIA_IR_BAD);
    puts("[OK] NEC: кадр, инверсия команды, повреждение, укороченный кадр, повтор");
    return 0;
}
