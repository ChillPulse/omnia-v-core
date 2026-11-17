#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_i2s_ex.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>

#define DEFAULT_AUDIO_RATE_HZ       48000U
#define DMA_FRAMES                  512U
#define DMA_WORDS                   (DMA_FRAMES * 2U)
#define HALF_WORDS                  (DMA_WORDS / 2U)

#define UART_TIMEOUT_MS             50U
#define DEFAULT_FADE_MS             12U
#define DEFAULT_UNMUTE_FADE_MS      35U
#define STARTUP_GRACE_MS            80U
#define SRC_DEBOUNCE_MS             15U
#define RATE_DEBOUNCE_MS            15U

#define SRC_MUTE_PORT               GPIOA
#define SRC_MUTE_PIN                GPIO_PIN_8

#define RATE_PORT                   GPIOA
#define RATE_PIN                    GPIO_PIN_9

I2S_HandleTypeDef hi2s3;
UART_HandleTypeDef huart2;

extern DMA_HandleTypeDef hdma_i2s3_ext_rx;
extern DMA_HandleTypeDef hdma_spi3_tx;

static int16_t rx_buf[DMA_WORDS] __attribute__((aligned(4)));
static int16_t tx_buf[DMA_WORDS] __attribute__((aligned(4)));

volatile uint32_t g_cb_half_count = 0;
volatile uint32_t g_cb_full_count = 0;
volatile uint32_t g_i2s_err_count = 0;
volatile uint32_t g_restart_req = 0;

volatile uint32_t g_rx_peak = 0;
volatile uint32_t g_tx_peak = 0;

volatile uint8_t  g_audio_running = 0;
volatile uint8_t  g_soft_muted = 1;

volatile uint32_t g_audio_rate_hz = DEFAULT_AUDIO_RATE_HZ;
volatile uint32_t g_pending_rate_hz = 0;
volatile uint8_t  g_rate_change_req = 0;

volatile int32_t  g_user_gain_q16 = 65536;   // 100%
volatile int32_t  g_gain_current_q16 = 0;
volatile int32_t  g_gain_target_q16  = 0;
volatile int32_t  g_gain_step_q16    = 0;
volatile uint32_t g_ramp_frames_left = 0;
volatile uint32_t g_fade_ms = DEFAULT_FADE_MS;

/* source pin logic */
volatile uint8_t  g_src_auto = 1;     // ON by default
volatile uint8_t  g_src_inv  = 0;     // 0: raw=1 => mute
volatile uint8_t  g_src_raw_sample = 1;
volatile uint8_t  g_src_raw_stable = 1;
volatile uint8_t  g_src_interpreted_mute = 1;

/* rate pin logic: LOW=44.1, HIGH=48 */
volatile uint8_t  g_rate_raw_sample = 1;
volatile uint8_t  g_rate_raw_stable = 1;

/* status auto */
volatile uint32_t g_status_period_ms = 0;

/* clock info */
volatile uint8_t  g_clk_mode = 0;     // 0 = HSI fallback, 1 = HSE25

static uint8_t  g_startup_applied = 0;
static uint32_t g_boot_ms = 0;
static uint32_t g_src_change_ms = 0;
static uint32_t g_rate_change_ms = 0;
static uint32_t g_last_status_ms = 0;

static char g_cmd_buf[64];
static uint32_t g_cmd_len = 0;

void SystemClock_Config(void);
static HAL_StatusTypeDef Clock_Config_HSE25(void);
static HAL_StatusTypeDef Clock_Config_HSI16(void);

static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_I2S3_Init_Rate(uint32_t rate_hz);

static void uart_log(const char *fmt, ...);
static void print_help(void);
static void print_status(void);

static inline uint8_t read_src_pin_raw(void);
static inline uint8_t read_rate_pin_raw(void);
static inline uint32_t rate_raw_to_hz(uint8_t raw);
static inline int16_t sat16_from_i32(int32_t x);
static inline uint32_t abs16_to_u32(int16_t x);

static void zero_audio_buffers(void);
static void process_block(uint32_t start_word, uint32_t count_words);

static void start_gain_ramp(int32_t target_q16, uint32_t fade_ms);
static void request_mute(uint32_t fade_ms);
static void request_unmute(uint32_t fade_ms);

static HAL_StatusTypeDef audio_start(void);
static HAL_StatusTypeDef audio_restart(void);
static HAL_StatusTypeDef audio_change_rate(uint32_t new_rate_hz);

static void apply_source_state(uint8_t raw_level);
static void poll_source_pin(void);
static void poll_rate_pin(void);
static void poll_uart_commands(void);
static void handle_command(const char *cmd);

static void normalize_command(char *s);
static int starts_with(const char *s, const char *prefix);

void Error_Handler(void);

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();

    g_boot_ms = HAL_GetTick();
    g_last_status_ms = g_boot_ms;

    g_src_raw_sample = read_src_pin_raw();
    g_src_raw_stable = g_src_raw_sample;
    g_src_change_ms = g_boot_ms;

    g_rate_raw_sample = read_rate_pin_raw();
    g_rate_raw_stable = g_rate_raw_sample;
    g_rate_change_ms = g_boot_ms;
    g_audio_rate_hz = rate_raw_to_hz(g_rate_raw_stable);

    MX_USART2_UART_Init();
    MX_I2S3_Init_Rate(g_audio_rate_hz);

    zero_audio_buffers();

    uart_log("\r\n=== OMNIA-V STM MASTER BASELINE V7G ===\r\n");
    uart_log("I2S master full-duplex DMA\r\n");
    uart_log("CLK %s\r\n", g_clk_mode ? "HSE25" : "HSI16-FALLBACK");
    uart_log("UART PA2/PA3 115200\r\n");
    uart_log("SRC auto=%lu inv=%lu raw=%lu sr=%lu ratepin=%lu\r\n",
             (unsigned long)g_src_auto,
             (unsigned long)g_src_inv,
             (unsigned long)g_src_raw_stable,
             (unsigned long)g_audio_rate_hz,
             (unsigned long)g_rate_raw_stable);
    uart_log("Type h for help\r\n");

    if (audio_start() != HAL_OK)
    {
        Error_Handler();
    }

    while (1)
    {
        uint32_t now = HAL_GetTick();

        if (!g_startup_applied && (now - g_boot_ms >= STARTUP_GRACE_MS))
        {
            g_startup_applied = 1;
            if (g_src_auto)
            {
                apply_source_state(g_src_raw_stable);
            }
            else
            {
                request_unmute(DEFAULT_UNMUTE_FADE_MS);
            }
        }

        poll_source_pin();
        poll_rate_pin();
        poll_uart_commands();

        if (g_rate_change_req)
        {
            uint32_t target = g_pending_rate_hz;
            g_rate_change_req = 0;
            g_pending_rate_hz = 0;

            uart_log("RATE REQ %lu\r\n", (unsigned long)target);

            if (audio_change_rate(target) == HAL_OK)
            {
                uart_log("RATE OK %lu\r\n", (unsigned long)g_audio_rate_hz);
            }
            else
            {
                uart_log("RATE FAIL %lu\r\n", (unsigned long)target);
            }
        }

        if (g_restart_req)
        {
            g_restart_req = 0;
            uart_log("RESTART\r\n");
            if (audio_restart() != HAL_OK)
            {
                uart_log("RESTART FAIL\r\n");
                Error_Handler();
            }
        }

        if (g_status_period_ms > 0U)
        {
            if ((now - g_last_status_ms) >= g_status_period_ms)
            {
                g_last_status_ms = now;
                print_status();
            }
        }
    }
}

static inline uint8_t read_src_pin_raw(void)
{
    return (HAL_GPIO_ReadPin(SRC_MUTE_PORT, SRC_MUTE_PIN) == GPIO_PIN_SET) ? 1u : 0u;
}

static inline uint8_t read_rate_pin_raw(void)
{
    return (HAL_GPIO_ReadPin(RATE_PORT, RATE_PIN) == GPIO_PIN_SET) ? 1u : 0u;
}

static inline uint32_t rate_raw_to_hz(uint8_t raw)
{
    return raw ? 48000U : 44100U;
}

static inline int16_t sat16_from_i32(int32_t x)
{
    if (x > 32767)  return 32767;
    if (x < -32768) return -32768;
    return (int16_t)x;
}

static inline uint32_t abs16_to_u32(int16_t x)
{
    return (x < 0) ? (uint32_t)(-(int32_t)x) : (uint32_t)x;
}

static void uart_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n <= 0) return;
    if (n > (int)sizeof(buf)) n = sizeof(buf);

    HAL_UART_Transmit(&huart2, (uint8_t *)buf, (uint16_t)n, UART_TIMEOUT_MS);
}

static void print_help(void)
{
    uart_log(
        "Cmds:\r\n"
        "  h       - help\r\n"
        "  s       - one status\r\n"
        "  s0      - auto status off\r\n"
        "  sN      - auto status every N ms\r\n"
        "  m       - mute\r\n"
        "  u       - unmute\r\n"
        "  gN      - gain, ex: g100 g50\r\n"
        "  fN      - fade ms, ex: f30 f150\r\n"
        "  a0/a1   - auto SRC_MUTE off/on\r\n"
        "  i0/i1   - SRC_MUTE invert off/on\r\n"
        "  k44100  - manual I2S rate 44.1 kHz\r\n"
        "  k48000  - manual I2S rate 48 kHz\r\n"
        "  r       - restart I2S DMA\r\n"
        "  z       - clear counters\r\n"
    );
}

static void print_status(void)
{
    int32_t cur_pct = (int32_t)(((int64_t)g_gain_current_q16 * 100LL) / 65536LL);
    int32_t tgt_pct = (int32_t)(((int64_t)g_gain_target_q16  * 100LL) / 65536LL);

    uart_log(
        "st clk=%s sr=%lu rpin=%lu run=%lu auto=%lu inv=%lu raw=%lu smute=%lu gain=%ld%%->%ld%% ramp=%lu rxpk=%lu txpk=%lu cb=%lu/%lu err=%lu fade=%lu sp=%lu\r\n",
        g_clk_mode ? "HSE25" : "HSI16",
        (unsigned long)g_audio_rate_hz,
        (unsigned long)g_rate_raw_stable,
        (unsigned long)g_audio_running,
        (unsigned long)g_src_auto,
        (unsigned long)g_src_inv,
        (unsigned long)g_src_raw_stable,
        (unsigned long)g_soft_muted,
        (long)cur_pct,
        (long)tgt_pct,
        (unsigned long)g_ramp_frames_left,
        (unsigned long)g_rx_peak,
        (unsigned long)g_tx_peak,
        (unsigned long)g_cb_half_count,
        (unsigned long)g_cb_full_count,
        (unsigned long)g_i2s_err_count,
        (unsigned long)g_fade_ms,
        (unsigned long)g_status_period_ms
    );
}

static void zero_audio_buffers(void)
{
    memset((void *)rx_buf, 0, sizeof(rx_buf));
    memset((void *)tx_buf, 0, sizeof(tx_buf));
}

static void start_gain_ramp(int32_t target_q16, uint32_t fade_ms)
{
    if (target_q16 < 0) target_q16 = 0;
    if (target_q16 > 131072) target_q16 = 131072; // 200%

    uint32_t frames = (g_audio_rate_hz * fade_ms) / 1000U;
    if (frames == 0U) frames = 1U;

    __disable_irq();

    int32_t cur = g_gain_current_q16;
    int32_t diff = target_q16 - cur;
    int32_t step = diff / (int32_t)frames;

    if ((step == 0) && (diff != 0))
    {
        step = (diff > 0) ? 1 : -1;
    }

    g_gain_target_q16  = target_q16;
    g_gain_step_q16    = step;
    g_ramp_frames_left = frames;

    __enable_irq();
}

static void request_mute(uint32_t fade_ms)
{
    g_soft_muted = 1;
    start_gain_ramp(0, fade_ms);
}

static void request_unmute(uint32_t fade_ms)
{
    g_soft_muted = 0;
    start_gain_ramp(g_user_gain_q16, fade_ms);
}

static HAL_StatusTypeDef audio_start(void)
{
    zero_audio_buffers();

    __disable_irq();
    g_gain_current_q16 = 0;
    g_gain_target_q16  = 0;
    g_gain_step_q16    = 0;
    g_ramp_frames_left = 0;
    g_soft_muted = 1;
    __enable_irq();

    if (HAL_I2SEx_TransmitReceive_DMA(&hi2s3,
                                      (uint16_t *)tx_buf,
                                      (uint16_t *)rx_buf,
                                      DMA_WORDS) != HAL_OK)
    {
        return HAL_ERROR;
    }

    g_audio_running = 1;
    return HAL_OK;
}

static HAL_StatusTypeDef audio_restart(void)
{
    HAL_I2S_DMAStop(&hi2s3);
    g_audio_running = 0;
    HAL_Delay(2);

    if (audio_start() != HAL_OK)
    {
        return HAL_ERROR;
    }

    if (g_src_auto)
    {
        apply_source_state(g_src_raw_stable);
    }
    else
    {
        request_unmute(DEFAULT_UNMUTE_FADE_MS);
    }

    return HAL_OK;
}

static HAL_StatusTypeDef audio_change_rate(uint32_t new_rate_hz)
{
    if (new_rate_hz != 44100U && new_rate_hz != 48000U)
    {
        return HAL_ERROR;
    }

    if (new_rate_hz == g_audio_rate_hz)
    {
        return HAL_OK;
    }

    request_mute(g_fade_ms);
    HAL_Delay(g_fade_ms + 10U);

    HAL_I2S_DMAStop(&hi2s3);
    g_audio_running = 0;
    HAL_Delay(2U);

    if (HAL_I2S_DeInit(&hi2s3) != HAL_OK)
    {
        return HAL_ERROR;
    }

    g_audio_rate_hz = new_rate_hz;
    MX_I2S3_Init_Rate(g_audio_rate_hz);

    if (audio_start() != HAL_OK)
    {
        return HAL_ERROR;
    }

    HAL_Delay(4U);

    if (g_src_auto)
    {
        apply_source_state(g_src_raw_stable);
    }
    else
    {
        request_unmute(DEFAULT_UNMUTE_FADE_MS);
    }

    return HAL_OK;
}

static void process_block(uint32_t start_word, uint32_t count_words)
{
    uint32_t end = start_word + count_words;

    for (uint32_t i = start_word; (i + 1U) < end; i += 2U)
    {
        int16_t in_l = rx_buf[i];
        int16_t in_r = rx_buf[i + 1U];

        uint32_t abs_l = abs16_to_u32(in_l);
        uint32_t abs_r = abs16_to_u32(in_r);
        if (abs_l > g_rx_peak) g_rx_peak = abs_l;
        if (abs_r > g_rx_peak) g_rx_peak = abs_r;

        if (g_ramp_frames_left > 0U)
        {
            int32_t next = g_gain_current_q16 + g_gain_step_q16;
            uint32_t left = g_ramp_frames_left - 1U;

            if (left == 0U)
            {
                next = g_gain_target_q16;
            }

            g_gain_current_q16 = next;
            g_ramp_frames_left = left;
        }

        int32_t gain_q16 = g_gain_current_q16;
        int32_t out_l = ((int32_t)in_l * gain_q16) >> 16;
        int32_t out_r = ((int32_t)in_r * gain_q16) >> 16;

        int16_t s_l = sat16_from_i32(out_l);
        int16_t s_r = sat16_from_i32(out_r);

        tx_buf[i]      = s_l;
        tx_buf[i + 1U] = s_r;

        uint32_t oabs_l = abs16_to_u32(s_l);
        uint32_t oabs_r = abs16_to_u32(s_r);
        if (oabs_l > g_tx_peak) g_tx_peak = oabs_l;
        if (oabs_r > g_tx_peak) g_tx_peak = oabs_r;
    }
}

static void apply_source_state(uint8_t raw_level)
{
    uint8_t interpreted_mute = raw_level ^ g_src_inv;

    if (interpreted_mute != g_src_interpreted_mute)
    {
        g_src_interpreted_mute = interpreted_mute;

        if (interpreted_mute)
        {
            uart_log("SRC MUTE raw=%lu inv=%lu\r\n",
                     (unsigned long)raw_level,
                     (unsigned long)g_src_inv);
            request_mute(g_fade_ms);
        }
        else
        {
            uart_log("SRC UNMUTE raw=%lu inv=%lu\r\n",
                     (unsigned long)raw_level,
                     (unsigned long)g_src_inv);
            request_unmute(DEFAULT_UNMUTE_FADE_MS);
        }
    }
}

static void poll_source_pin(void)
{
    uint8_t raw = read_src_pin_raw();

    if (raw != g_src_raw_sample)
    {
        g_src_raw_sample = raw;
        g_src_change_ms = HAL_GetTick();
    }

    if ((raw != g_src_raw_stable) &&
        ((HAL_GetTick() - g_src_change_ms) >= SRC_DEBOUNCE_MS))
    {
        g_src_raw_stable = raw;

        if (g_src_auto && g_startup_applied)
        {
            apply_source_state(raw);
        }
    }
}

static void poll_rate_pin(void)
{
    uint8_t raw = read_rate_pin_raw();

    if (raw != g_rate_raw_sample)
    {
        g_rate_raw_sample = raw;
        g_rate_change_ms = HAL_GetTick();
    }

    if ((raw != g_rate_raw_stable) &&
        ((HAL_GetTick() - g_rate_change_ms) >= RATE_DEBOUNCE_MS))
    {
        uint32_t new_rate;

        g_rate_raw_stable = raw;
        new_rate = rate_raw_to_hz(raw);

        if (new_rate != g_audio_rate_hz)
        {
            g_pending_rate_hz = new_rate;
            g_rate_change_req = 1;
            uart_log("RATE PIN raw=%lu => %lu\r\n",
                     (unsigned long)raw,
                     (unsigned long)new_rate);
        }
    }
}

static void normalize_command(char *s)
{
    size_t len = strlen(s);
    size_t start = 0;

    while (start < len && isspace((unsigned char)s[start]))
        start++;

    while (len > start && isspace((unsigned char)s[len - 1]))
        len--;

    if (start > 0 || len < strlen(s))
    {
        memmove(s, s + start, len - start);
        s[len - start] = 0;
    }

    for (size_t i = 0; s[i]; i++)
    {
        s[i] = (char)tolower((unsigned char)s[i]);
    }
}

static int starts_with(const char *s, const char *prefix)
{
    while (*prefix)
    {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

static void handle_command(const char *cmd_in)
{
    char cmd[64];
    strncpy(cmd, cmd_in, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = 0;
    normalize_command(cmd);

    if (cmd[0] == 0)
        return;

    if ((strcmp(cmd, "h") == 0) || (strcmp(cmd, "help") == 0))
    {
        print_help();
        return;
    }

    if ((strcmp(cmd, "s") == 0) || (strcmp(cmd, "status") == 0))
    {
        print_status();
        return;
    }

    if ((cmd[0] == 's' && cmd[1] != 0) || starts_with(cmd, "status "))
    {
        int v = 0;
        if (cmd[0] == 's' && cmd[1] != 0) v = atoi(cmd + 1);
        else v = atoi(cmd + 7);

        if (v < 0) v = 0;
        g_status_period_ms = (uint32_t)v;
        g_last_status_ms = HAL_GetTick();

        uart_log("STATUS AUTO %lu ms\r\n", (unsigned long)g_status_period_ms);
        return;
    }

    if ((strcmp(cmd, "m") == 0) || (strcmp(cmd, "mute") == 0))
    {
        request_mute(g_fade_ms);
        uart_log("MUTE %lu ms\r\n", (unsigned long)g_fade_ms);
        return;
    }

    if ((strcmp(cmd, "u") == 0) || (strcmp(cmd, "unmute") == 0))
    {
        request_unmute(DEFAULT_UNMUTE_FADE_MS);
        uart_log("UNMUTE %lu ms\r\n", (unsigned long)DEFAULT_UNMUTE_FADE_MS);
        return;
    }

    if ((cmd[0] == 'g' && cmd[1] != 0) || starts_with(cmd, "gain "))
    {
        int v = 0;
        if (cmd[0] == 'g' && cmd[1] != 0) v = atoi(cmd + 1);
        else v = atoi(cmd + 5);

        if (v < 0)   v = 0;
        if (v > 200) v = 200;

        g_user_gain_q16 = (int32_t)(((int64_t)v * 65536LL) / 100LL);

        if (!g_soft_muted)
        {
            start_gain_ramp(g_user_gain_q16, g_fade_ms);
        }

        uart_log("GAIN %d%%\r\n", v);
        return;
    }

    if ((cmd[0] == 'f' && cmd[1] != 0) || starts_with(cmd, "fade "))
    {
        int v = 0;
        if (cmd[0] == 'f' && cmd[1] != 0) v = atoi(cmd + 1);
        else v = atoi(cmd + 5);

        if (v < 1)    v = 1;
        if (v > 1000) v = 1000;

        g_fade_ms = (uint32_t)v;
        uart_log("FADE %lu ms\r\n", (unsigned long)g_fade_ms);
        return;
    }

    if ((cmd[0] == 'a' && cmd[1] != 0) || starts_with(cmd, "auto "))
    {
        int v = 0;
        if (cmd[0] == 'a' && cmd[1] != 0) v = atoi(cmd + 1);
        else v = atoi(cmd + 5);

        g_src_auto = (v != 0) ? 1u : 0u;
        uart_log("AUTO %lu\r\n", (unsigned long)g_src_auto);

        if (g_src_auto)
        {
            apply_source_state(g_src_raw_stable);
        }
        else
        {
            request_unmute(DEFAULT_UNMUTE_FADE_MS);
        }
        return;
    }

    if ((cmd[0] == 'i' && cmd[1] != 0) || starts_with(cmd, "srcinv "))
    {
        int v = 0;
        if (cmd[0] == 'i' && cmd[1] != 0) v = atoi(cmd + 1);
        else v = atoi(cmd + 7);

        g_src_inv = (v != 0) ? 1u : 0u;
        uart_log("SRCINV %lu\r\n", (unsigned long)g_src_inv);

        if (g_src_auto)
        {
            g_src_interpreted_mute = 255u;
            apply_source_state(g_src_raw_stable);
        }
        return;
    }

    if ((cmd[0] == 'k' && cmd[1] != 0) || starts_with(cmd, "rate "))
    {
        uint32_t v = 0;
        if (cmd[0] == 'k' && cmd[1] != 0) v = (uint32_t)atoi(cmd + 1);
        else v = (uint32_t)atoi(cmd + 5);

        if (v == 44100U || v == 48000U)
        {
            g_pending_rate_hz = v;
            g_rate_change_req = 1;
            uart_log("RATE QUEUED %lu\r\n", (unsigned long)v);
        }
        else
        {
            uart_log("RATE BAD %lu\r\n", (unsigned long)v);
        }
        return;
    }

    if ((strcmp(cmd, "r") == 0) || (strcmp(cmd, "restart") == 0))
    {
        g_restart_req = 1;
        uart_log("RESTART REQ\r\n");
        return;
    }

    if ((strcmp(cmd, "z") == 0) || (strcmp(cmd, "zero") == 0))
    {
        g_rx_peak = 0;
        g_tx_peak = 0;
        g_cb_half_count = 0;
        g_cb_full_count = 0;
        g_i2s_err_count = 0;
        uart_log("ZERO\r\n");
        return;
    }

    uart_log("UNKNOWN %s\r\n", cmd);
}

static void poll_uart_commands(void)
{
    uint8_t ch;

    while (HAL_UART_Receive(&huart2, &ch, 1, 0) == HAL_OK)
    {
        if ((ch == '\r') || (ch == '\n'))
        {
            if (g_cmd_len > 0U)
            {
                g_cmd_buf[g_cmd_len] = 0;
                handle_command(g_cmd_buf);
                g_cmd_len = 0U;
            }
        }
        else if ((ch == 0x08U) || (ch == 0x7FU))
        {
            if (g_cmd_len > 0U)
            {
                g_cmd_len--;
            }
        }
        else if ((ch >= 32U) && (ch < 127U))
        {
            if (g_cmd_len < (sizeof(g_cmd_buf) - 1U))
            {
                g_cmd_buf[g_cmd_len++] = (char)ch;
            }
        }
    }
}

void HAL_I2SEx_TxRxHalfCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance == SPI3)
    {
        g_cb_half_count++;
        process_block(0U, HALF_WORDS);
    }
}

void HAL_I2SEx_TxRxCpltCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance == SPI3)
    {
        g_cb_full_count++;
        process_block(HALF_WORDS, HALF_WORDS);
    }
}

void HAL_I2S_ErrorCallback(I2S_HandleTypeDef *hi2s)
{
    if (hi2s->Instance == SPI3)
    {
        g_i2s_err_count++;
        g_restart_req = 1;
    }
}

static HAL_StatusTypeDef Clock_Config_HSE25(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
    RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM = 25;
    RCC_OscInitStruct.PLL.PLLN = 336;
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
    RCC_OscInitStruct.PLL.PLLQ = 7;

    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        return HAL_ERROR;
    }

    RCC_ClkInitStruct.ClockType =
        RCC_CLOCKTYPE_HCLK |
        RCC_CLOCKTYPE_SYSCLK |
        RCC_CLOCKTYPE_PCLK1 |
        RCC_CLOCKTYPE_PCLK2;

    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
    {
        return HAL_ERROR;
    }

    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_I2S;
    PeriphClkInitStruct.PLLI2S.PLLI2SN = 192;
    PeriphClkInitStruct.PLLI2S.PLLI2SR = 5;

    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
    {
        return HAL_ERROR;
    }

    g_clk_mode = 1;
    return HAL_OK;
}

static HAL_StatusTypeDef Clock_Config_HSI16(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
    RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    RCC_OscInitStruct.PLL.PLLM = 16;
    RCC_OscInitStruct.PLL.PLLN = 336;
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
    RCC_OscInitStruct.PLL.PLLQ = 7;

    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        return HAL_ERROR;
    }

    RCC_ClkInitStruct.ClockType =
        RCC_CLOCKTYPE_HCLK |
        RCC_CLOCKTYPE_SYSCLK |
        RCC_CLOCKTYPE_PCLK1 |
        RCC_CLOCKTYPE_PCLK2;

    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
    {
        return HAL_ERROR;
    }

    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_I2S;
    PeriphClkInitStruct.PLLI2S.PLLI2SN = 192;
    PeriphClkInitStruct.PLLI2S.PLLI2SR = 5;

    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
    {
        return HAL_ERROR;
    }

    g_clk_mode = 0;
    return HAL_OK;
}

void SystemClock_Config(void)
{
    if (Clock_Config_HSE25() == HAL_OK)
    {
        return;
    }

    HAL_RCC_DeInit();

    if (Clock_Config_HSI16() == HAL_OK)
    {
        return;
    }

    Error_Handler();
}

static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    GPIO_InitStruct.Pin = SRC_MUTE_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(SRC_MUTE_PORT, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = RATE_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;   // disconnected => 48k
    HAL_GPIO_Init(RATE_PORT, &GPIO_InitStruct);
}

static void MX_USART2_UART_Init(void)
{
    huart2.Instance = USART2;
    huart2.Init.BaudRate = 115200;
    huart2.Init.WordLength = UART_WORDLENGTH_8B;
    huart2.Init.StopBits = UART_STOPBITS_1;
    huart2.Init.Parity = UART_PARITY_NONE;
    huart2.Init.Mode = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&huart2) != HAL_OK)
    {
        Error_Handler();
    }
}

static void MX_I2S3_Init_Rate(uint32_t rate_hz)
{
    hi2s3.Instance = SPI3;
    hi2s3.Init.Mode = I2S_MODE_MASTER_TX;
    hi2s3.Init.Standard = I2S_STANDARD_PHILIPS;
    hi2s3.Init.DataFormat = I2S_DATAFORMAT_16B;
    hi2s3.Init.MCLKOutput = I2S_MCLKOUTPUT_DISABLE;
    hi2s3.Init.AudioFreq = (rate_hz == 44100U) ? I2S_AUDIOFREQ_44K : I2S_AUDIOFREQ_48K;
    hi2s3.Init.CPOL = I2S_CPOL_LOW;
    hi2s3.Init.ClockSource = I2S_CLOCK_PLL;
    hi2s3.Init.FullDuplexMode = I2S_FULLDUPLEXMODE_ENABLE;

    if (HAL_I2S_Init(&hi2s3) != HAL_OK)
    {
        Error_Handler();
    }
}

void Error_Handler(void)
{
    __disable_irq();
    while (1)
    {
    }
}
