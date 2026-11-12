/* Pure command gates, independently testable on the host; no hardware guarantees. */
#ifndef OMNIA_CONTROL_POLICY_H
#define OMNIA_CONTROL_POLICY_H
#include "omnia_protocol.h"
#include <string.h>

_Static_assert(sizeof(float) == sizeof(uint32_t), "protocol requires 32-bit float");
static inline bool omnia_policy_finite(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    /* Bit check survives -ffast-math, unlike isfinite(). */
    return (bits & 0x7f800000U) != 0x7f800000U;
}
static inline omnia_err_code_t omnia_policy_source(const omnia_cmd_source_t *s, bool muted,
                                                     bool bt_enabled, bool usb_enabled)
{
    if (!s) return OMNIA_STATUS_ERR_INVALID_ARG;
    if (s->source > OMNIA_SRC_USB_DAC_UAC1 ||
        (s->source == OMNIA_SRC_BT_I2S3_ASRC && !bt_enabled) ||
        (s->source == OMNIA_SRC_USB_DAC_UAC1 && !usb_enabled))
        return OMNIA_STATUS_ERR_UNSUPPORTED;
    if (!muted) return OMNIA_STATUS_ERR_NOT_MUTED;
    if (s->source == OMNIA_SRC_INTERNAL_TONE &&
        (!omnia_policy_finite(s->tone_freq_hz) || !omnia_policy_finite(s->tone_level_dbfs) ||
         s->tone_freq_hz < 20.0f || s->tone_freq_hz > 20000.0f ||
         s->tone_level_dbfs < -96.0f || s->tone_level_dbfs > -1.0f))
        return OMNIA_STATUS_ERR_INVALID_ARG;
    return OMNIA_STATUS_OK;
}
static inline bool omnia_policy_volume(const omnia_cmd_volume_t *v)
{
    return v && omnia_policy_finite(v->volume_db) && omnia_policy_finite(v->replaygain_db) &&
           omnia_policy_finite(v->balance) && v->volume_db >= -96.0f && v->volume_db <= 0.0f &&
           v->replaygain_db >= -18.0f && v->replaygain_db <= 6.0f &&
           v->balance >= -1.0f && v->balance <= 1.0f;
}
static inline bool omnia_policy_word_bits(uint8_t bits)
{
    return bits == 16U || bits == 24U || bits == 32U;
}
static inline bool omnia_policy_sample_rate(uint32_t hz)
{
    /* Native 96 kHz remains a future feature, not a selectable mode yet. */
    return hz == 44100U || hz == 48000U;
}
#endif
