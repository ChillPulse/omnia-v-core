#include "omnia_bench_cmd.h"
#include <string.h>

omnia_bench_command_t omnia_bench_parse(const char *line)
{
    omnia_bench_command_t result = { .action = OMNIA_BENCH_INVALID };
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
    } else {
        const char *name = NULL;
        omnia_bench_action_t action = OMNIA_BENCH_INVALID;
        if (strncmp(line, "IR LEARN ", 9) == 0) { name = line + 9; action = OMNIA_BENCH_IR_LEARN; }
        else if (strncmp(line, "IR GET ", 7) == 0) { name = line + 7; action = OMNIA_BENCH_IR_GET; }
        else if (strncmp(line, "IR DEL ", 7) == 0) { name = line + 7; action = OMNIA_BENCH_IR_DELETE; }
        if (name) {
            size_t n = 0;
            while (name[n] && n < 15U &&
                   ((name[n] >= 'A' && name[n] <= 'Z') ||
                    (name[n] >= '0' && name[n] <= '9') || name[n] == '_')) ++n;
            if (n > 0U && name[n] == '\0') {
                memcpy(result.name, name, n);
                result.name[n] = '\0';
                result.action = action;
            }
        }
    }
    return result;
}
