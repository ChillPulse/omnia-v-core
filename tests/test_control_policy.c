#include "omnia_control_policy.h"
#include <assert.h>
#include <stdio.h>
static float from_bits(uint32_t b) { float f; memcpy(&f, &b, sizeof f); return f; }
int main(void)
{
    omnia_cmd_mute_t mute = { .ramp_ms = 25, .hard_xsmt = 1 }, decoded;
    assert(omnia_policy_decode_mute(&mute, sizeof mute, &decoded));
    assert(decoded.ramp_ms == 25 && decoded.hard_xsmt == 1);
    assert(omnia_policy_decode_mute(NULL, 0, &decoded) && decoded.ramp_ms == 25);
    assert(!omnia_policy_decode_mute(&mute, 1, &decoded));
    assert(!omnia_policy_decode_mute(&mute, sizeof mute, NULL));
    mute.hard_xsmt = 2; assert(!omnia_policy_decode_mute(&mute, sizeof mute, &decoded));
    mute.hard_xsmt = 1; mute.ramp_ms = 5001; assert(!omnia_policy_decode_mute(&mute, sizeof mute, &decoded));
    omnia_cmd_source_t s = { .source = OMNIA_SRC_INTERNAL_TONE, .tone_freq_hz = 1000.f, .tone_level_dbfs = -24.f };
    assert(omnia_policy_source(&s, true, false, false) == OMNIA_STATUS_OK);
    assert(omnia_policy_source(&s, false, false, false) == OMNIA_STATUS_ERR_NOT_MUTED);
    s.tone_freq_hz = from_bits(0x7fc00000U);
    assert(omnia_policy_source(&s, true, false, false) == OMNIA_STATUS_ERR_INVALID_ARG);
    s.tone_freq_hz = 1000.f; s.tone_level_dbfs = from_bits(0x7f800000U);
    assert(omnia_policy_source(&s, true, false, false) == OMNIA_STATUS_ERR_INVALID_ARG);
    s.tone_level_dbfs = 0.f;
    assert(omnia_policy_source(&s, true, false, false) == OMNIA_STATUS_ERR_INVALID_ARG);
    s.tone_level_dbfs = -97.f;
    assert(omnia_policy_source(&s, true, false, false) == OMNIA_STATUS_ERR_INVALID_ARG);
    s.source = OMNIA_SRC_BT_I2S3_ASRC;
    assert(omnia_policy_source(&s, true, false, false) == OMNIA_STATUS_ERR_UNSUPPORTED);
    s.source = OMNIA_SRC_USB_DAC_UAC1;
    assert(omnia_policy_source(&s, true, false, false) == OMNIA_STATUS_ERR_UNSUPPORTED);
    s.source = 255;
    assert(omnia_policy_source(&s, true, true, true) == OMNIA_STATUS_ERR_UNSUPPORTED);
    assert(omnia_policy_source(NULL, true, false, false) == OMNIA_STATUS_ERR_INVALID_ARG);
    omnia_cmd_volume_t volume = { .volume_db = -24.f, .replaygain_db = 0.f, .balance = 0.f };
    assert(omnia_policy_volume(&volume));
    volume.volume_db = from_bits(0x7fc00000U); assert(!omnia_policy_volume(&volume));
    volume.volume_db = -24.f; volume.balance = 1.1f; assert(!omnia_policy_volume(&volume));
    volume.balance = 0.f; volume.replaygain_db = from_bits(0xff800000U); assert(!omnia_policy_volume(&volume));
    assert(!omnia_policy_volume(NULL));
    assert(omnia_policy_word_bits(16) && omnia_policy_word_bits(24) && omnia_policy_word_bits(32));
    assert(!omnia_policy_word_bits(0) && !omnia_policy_word_bits(8));
    assert(omnia_policy_sample_rate(44100) && omnia_policy_sample_rate(48000));
    assert(!omnia_policy_sample_rate(96000) && !omnia_policy_sample_rate(88200));
    puts("[OK] source/mute/tone/sample-rate policy");
}
