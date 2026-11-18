#include "omnia_dsp_validation.h"
#include "omnia_control_policy.h"
#include <string.h>

static bool in(float x, float lo, float hi)
{
    return omnia_policy_finite(x) && x >= lo && x <= hi;
}
static bool flag(uint8_t x) { return x <= 1U; }

static bool band_ok(const omnia_biquad_band_t *b)
{
    if (b->type > OMNIA_BIQUAD_ALL_PASS || !omnia_policy_finite(b->f0_hz) ||
        !omnia_policy_finite(b->q) || !omnia_policy_finite(b->gain_db)) return false;
    return b->type == OMNIA_BIQUAD_OFF ||
           (in(b->f0_hz, 15.f, 22000.f) && in(b->q, .1f, 18.f) && in(b->gain_db, -18.f, 18.f));
}

#define READ(type, variable) type variable; if (len != sizeof(variable)) return false; memcpy(&variable, data, sizeof(variable))
#define FINITE(x) omnia_policy_finite(x)

bool omnia_dsp_validate_payload(uint8_t cmd, const void *data, size_t len)
{
    if (!data) return false;
    switch (cmd) {
    case OMNIA_CMD_LOAD_HPF_HEADROOM: {
        READ(omnia_hpf_headroom_t, v);
        return flag(v.hpf_enable) && flag(v.auto_headroom_en) &&
            FINITE(v.hpf_freq_hz) && FINITE(v.hpf_q) &&
            (!v.hpf_enable || (in(v.hpf_freq_hz, 15.f, 40.f) && in(v.hpf_q, .1f, 18.f))) &&
            in(v.manual_atten_db, -18.f, 0.f);
    }
    case OMNIA_CMD_LOAD_ROOM_PEQ: {
        READ(omnia_room_peq_t, v);
        if (!flag(v.enable) || !flag(v.invert_phase_l) || !flag(v.invert_phase_r) ||
            v.band_count > OMNIA_MAX_ROOM_PEQ_BANDS ||
            !in(v.delay_l_ms, 0.f, 20.f) || !in(v.delay_r_ms, 0.f, 20.f) ||
            !in(v.trim_l_db, -12.f, 6.f) || !in(v.trim_r_db, -12.f, 6.f)) return false;
        for (unsigned i = 0; i < v.band_count; ++i) if (!band_ok(&v.bands[i])) return false;
        return true;
    }
    case OMNIA_CMD_LOAD_USER_PEQ: {
        READ(omnia_user_peq_t, v);
        if (!flag(v.enable) || v.band_count > OMNIA_MAX_USER_PEQ_BANDS) return false;
        for (unsigned i = 0; i < v.band_count; ++i) if (!band_ok(&v.bands[i])) return false;
        return true;
    }
    case OMNIA_CMD_LOAD_ANTI_AD: {
        READ(omnia_anti_ad_t, v);
        if (v.mode > 2 || !FINITE(v.threshold_lin) || !FINITE(v.attack_coeff) ||
            !FINITE(v.release_coeff) || !FINITE(v.makeup_db)) return false;
        return v.mode == 0 || (in(v.threshold_lin, .25f, .9f) &&
               in(v.attack_coeff, 0.f, 1.f) && in(v.release_coeff, 0.f, 1.f) &&
               in(v.makeup_db, 0.f, 6.f));
    }
    case OMNIA_CMD_LOAD_VBB: {
        READ(omnia_vbb_params_t, v);
        if (!flag(v.enable) || v.mode > 2 || !FINITE(v.amount) || !FINITE(v.freq_hz) ||
            !FINITE(v.drive) || !FINITE(v.lowpass_hz) || !FINITE(v.mix)) return false;
        return !v.enable || (in(v.amount, 0.f, 1.f) && in(v.freq_hz, 40.f, 120.f) &&
               in(v.drive, 0.f, 1.f) && in(v.lowpass_hz, 90.f, 240.f) && in(v.mix, 0.f, 1.f));
    }
    case OMNIA_CMD_LOAD_AETHER: {
        READ(omnia_aether_params_t, v);
        if (!flag(v.enable) || !flag(v.bass_keep_mono) || !flag(v.side_hpf_en) || !flag(v.crossfeed_en) ||
            !FINITE(v.master_blend) || !FINITE(v.width) || !FINITE(v.center_anchor_db) ||
            !FINITE(v.bass_xover_hz) || !FINITE(v.side_hpf_hz) || !FINITE(v.air_gain_db) ||
            !FINITE(v.air_freq_hz) || !FINITE(v.air_q) || !FINITE(v.de_harsh) ||
            !FINITE(v.depth_amount) || !FINITE(v.stage_size_ms) || !FINITE(v.triode_warmth) ||
            !FINITE(v.crossfeed_level) || !FINITE(v.crossfeed_fc_hz)) return false;
        return !v.enable || (in(v.master_blend, 0.f, 1.f) && in(v.width, 0.f, 2.f) &&
            in(v.center_anchor_db, -6.f, 6.f) && in(v.bass_xover_hz, 40.f, 250.f) &&
            in(v.side_hpf_hz, 20.f, 180.f) && in(v.air_gain_db, 0.f, 8.f) &&
            in(v.air_freq_hz, 5500.f, 16000.f) && in(v.air_q, .4f, 1.6f) &&
            in(v.de_harsh, 0.f, 1.f) && in(v.depth_amount, 0.f, .45f) &&
            in(v.stage_size_ms, .5f, 12.f) && in(v.triode_warmth, 0.f, .30f) &&
            in(v.crossfeed_level, 0.f, 1.f) && in(v.crossfeed_fc_hz, 500.f, 900.f));
    }
    case OMNIA_CMD_LOAD_LOUDNESS: {
        READ(omnia_loudness_params_t, v);
        if (!flag(v.enable) || !FINITE(v.ref_volume_db) || !FINITE(v.max_bass_boost_db) ||
            !FINITE(v.max_high_boost_db)) return false;
        return !v.enable || (in(v.ref_volume_db, -96.f, 0.f) &&
               in(v.max_bass_boost_db, 0.f, 12.f) && in(v.max_high_boost_db, 0.f, 6.f));
    }
    case OMNIA_CMD_LOAD_LIMITER: {
        READ(omnia_limiter_params_t, v);
        if (!flag(v.enable) || !flag(v.tpdf_dither_en) || !FINITE(v.ceiling_dbfs) ||
            !FINITE(v.lookahead_ms) || !FINITE(v.release_ms)) return false;
        return !v.enable || (in(v.ceiling_dbfs, -3.f, -.2f) &&
               in(v.lookahead_ms, .5f, 3.f) && in(v.release_ms, 30.f, 500.f));
    }
    default: return false;
    }
}
