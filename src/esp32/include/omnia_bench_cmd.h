#ifndef OMNIA_BENCH_CMD_H
#define OMNIA_BENCH_CMD_H

/* Стендовые команды не являются интерфейсом конечного продукта. */
typedef enum {
    OMNIA_BENCH_INVALID = 0,
    OMNIA_BENCH_HELP,
    OMNIA_BENCH_STATUS,
    OMNIA_BENCH_STOP,
    OMNIA_BENCH_TONE
} omnia_bench_action_t;

typedef struct {
    omnia_bench_action_t action;
    unsigned seconds;
} omnia_bench_command_t;

/* Строгая ASCII-команда без завершающего CR/LF. */
omnia_bench_command_t omnia_bench_parse(const char *line);

#endif
