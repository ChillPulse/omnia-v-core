#include "omnia_bench_cmd.h"
#include <string.h>

omnia_bench_command_t omnia_bench_parse(const char *line)
{
    omnia_bench_command_t result = { OMNIA_BENCH_INVALID, 0U };
    if (!line) return result;
    if (strcmp(line, "HELP") == 0) result.action = OMNIA_BENCH_HELP;
    else if (strcmp(line, "STATUS") == 0) result.action = OMNIA_BENCH_STATUS;
    else if (strcmp(line, "STOP") == 0) result.action = OMNIA_BENCH_STOP;
    else if (strncmp(line, "TONE NOAMP ", 11) == 0) {
        const char *p = line + 11;
        unsigned value = 0U;
        unsigned digits = 0U;
        while (*p >= '0' && *p <= '9' && digits < 2U) {
            value = value * 10U + (unsigned)(*p - '0');
            ++p;
            ++digits;
        }
        if (*p == '\0' && digits > 0U && value >= 1U && value <= 10U) {
            result.action = OMNIA_BENCH_TONE;
            result.seconds = value;
        }
    }
    return result;
}
