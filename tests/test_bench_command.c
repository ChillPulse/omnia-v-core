#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "omnia_bench_cmd.h"

int main(void)
{
    assert(omnia_bench_parse(NULL).action == OMNIA_BENCH_INVALID);
    assert(omnia_bench_parse("HELP").action == OMNIA_BENCH_HELP);
    assert(omnia_bench_parse("STATUS").action == OMNIA_BENCH_STATUS);
    assert(omnia_bench_parse("STOP").action == OMNIA_BENCH_STOP);
    const omnia_bench_command_t a = omnia_bench_parse("TONE NOAMP 5");
    assert(a.action == OMNIA_BENCH_TONE && a.seconds == 5U);
    assert(omnia_bench_parse("TONE NOAMP 10").seconds == 10U);
    assert(omnia_bench_parse("TONE 5").action == OMNIA_BENCH_INVALID);
    assert(omnia_bench_parse("TONE NOAMP 0").action == OMNIA_BENCH_INVALID);
    assert(omnia_bench_parse("TONE NOAMP 11").action == OMNIA_BENCH_INVALID);
    assert(omnia_bench_parse("TONE NOAMP 999").action == OMNIA_BENCH_INVALID);
    assert(omnia_bench_parse("TONE NOAMP -1").action == OMNIA_BENCH_INVALID);
    assert(omnia_bench_parse("TONE NOAMP 5 extra").action == OMNIA_BENCH_INVALID);
    assert(omnia_bench_parse("TONE NOAMP 5\n").action == OMNIA_BENCH_INVALID);
    const omnia_bench_command_t ir = omnia_bench_parse("IR LEARN PLAY_PAUSE");
    assert(ir.action == OMNIA_BENCH_IR_LEARN && strcmp(ir.name, "PLAY_PAUSE") == 0);
    assert(omnia_bench_parse("IR GET VOL1").action == OMNIA_BENCH_IR_GET);
    assert(omnia_bench_parse("IR DEL VOL1").action == OMNIA_BENCH_IR_DELETE);
    assert(omnia_bench_parse("IR LEARN ").action == OMNIA_BENCH_INVALID);
    assert(omnia_bench_parse("IR LEARN abc").action == OMNIA_BENCH_INVALID);
    assert(omnia_bench_parse("IR LEARN 1234567890123456").action == OMNIA_BENCH_INVALID);
    puts("[OK] Строгий парсер стендовой консоли: безопасный диапазон и подтверждение NOAMP");
    return 0;
}
