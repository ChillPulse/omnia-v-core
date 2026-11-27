#include "omnia_dsp_validation.h"
#include "omnia_control_policy.h"
#include "omnia_limits.h"
#include <string.h>

static bool in(float x, float lo, float hi)
{
    return omnia_policy_finite(x) && x >= lo && x <= hi;
}
static bool flag(uint8_t x) { return x <= 1U; }

static bool band_ok(omnia_biquad_band_t b)
{
    if (b.type > OMNIA_BIQUAD_ALL_PASS || !omnia_policy_finite(b.f0_hz) ||
        !omnia_policy_finite(b.q) || !omnia_policy_finite(b.gain_db)) return false;
    return b.type == OMNIA_BIQUAD_OFF ||
           (in(b.f0_hz, OMNIA_BAND_F0_MIN_HZ, OMNIA_BAND_F0_MAX_HZ) && in(b.q, OMNIA_BAND_Q_MIN, OMNIA_BAND_Q_MAX) && in(b.gain_db, OMNIA_BAND_GAIN_MIN_DB, OMNIA_BAND_GAIN_MAX_DB));
}

#define READ(type, variable) type variable = {0}; if (len != sizeof(variable)) return false; memcpy(&variable, data, sizeof(variable))
#define FINITE(x) omnia_policy_finite(x)

bool omnia_dsp_validate_payload(uint8_t cmd, const void *data, size_t len)
{
    if (!data) return false;
    switch (cmd) {
    case OMNIA_CMD_LOAD_HPF_HEADROOM: {
        READ(omnia_hpf_headroom_t, v);
        return flag(v.hpf_enable) && flag(v.auto_headroom_en) &&
            FINITE(v.hpf_freq_hz) && FINITE(v.hpf_q) &&
            (!v.hpf_enable || (in(v.hpf_freq_hz, OMNIA_HPF_FREQ_MIN_HZ, OMNIA_HPF_FREQ_MAX_HZ) && in(v.hpf_q, OMNIA_HPF_Q_MIN, OMNIA_HPF_Q_MAX))) &&
            in(v.manual_atten_db, OMNIA_HPF_MANUAL_ATTEN_MIN_DB, OMNIA_HPF_MANUAL_ATTEN_MAX_DB);
    }
    case OMNIA_CMD_LOAD_ROOM_PEQ: {
        READ(omnia_room_peq_t, v);
        if (!flag(v.enable) || !flag(v.invert_phase_l) || !flag(v.invert_phase_r) ||
            v.band_count > OMNIA_MAX_ROOM_PEQ_BANDS ||
            !in(v.delay_l_ms, OMNIA_ROOM_DELAY_MIN_MS, OMNIA_ROOM_DELAY_MAX_MS) ||
            !in(v.delay_r_ms, OMNIA_ROOM_DELAY_MIN_MS, OMNIA_ROOM_DELAY_MAX_MS) ||
            !in(v.trim_l_db, OMNIA_ROOM_TRIM_MIN_DB, OMNIA_ROOM_TRIM_MAX_DB) ||
            !in(v.trim_r_db, OMNIA_ROOM_TRIM_MIN_DB, OMNIA_ROOM_TRIM_MAX_DB)) return false;
        for (unsigned i = 0; i < v.band_count; ++i) if (!band_ok(v.bands[i])) return false;
        return true;
    }
    case OMNIA_CMD_LOAD_USER_PEQ: {
        READ(omnia_user_peq_t, v);
        if (!flag(v.enable) || v.band_count > OMNIA_MAX_USER_PEQ_BANDS) return false;
        for (unsigned i = 0; i < v.band_count; ++i) if (!band_ok(v.bands[i])) return false;
        return true;
    }
    case OMNIA_CMD_LOAD_ANTI_AD: {
        READ(omnia_anti_ad_t, v);
        if (v.mode > 2 || !FINITE(v.threshold_lin) || !FINITE(v.attack_coeff) ||
            !FINITE(v.release_coeff) || !FINITE(v.makeup_db)) return false;
        return v.mode == 0 || (in(v.threshold_lin, OMNIA_ANTI_AD_THRESH_MIN, OMNIA_ANTI_AD_THRESH_MAX) &&
               in(v.attack_coeff, OMNIA_ANTI_AD_ATTACK_MIN, OMNIA_ANTI_AD_ATTACK_MAX) &&
               in(v.release_coeff, OMNIA_ANTI_AD_RELEASE_MIN, OMNIA_ANTI_AD_RELEASE_MAX) &&
               in(v.makeup_db, OMNIA_ANTI_AD_MAKEUP_MIN_DB, OMNIA_ANTI_AD_MAKEUP_MAX_DB));
    }
    case OMNIA_CMD_LOAD_VBB: {
        READ(omnia_vbb_params_t, v);
        if (!flag(v.enable) || v.mode > 2 || !FINITE(v.amount) || !FINITE(v.freq_hz) ||
            !FINITE(v.drive) || !FINITE(v.lowpass_hz) || !FINITE(v.mix)) return false;
        return !v.enable || (in(v.amount, OMNIA_VBB_AMOUNT_MIN, OMNIA_VBB_AMOUNT_MAX) &&
               in(v.freq_hz, OMNIA_VBB_FREQ_MIN_HZ, OMNIA_VBB_FREQ_MAX_HZ) &&
               in(v.drive, OMNIA_VBB_DRIVE_MIN, OMNIA_VBB_DRIVE_MAX) &&
               in(v.lowpass_hz, OMNIA_VBB_LP_MIN_HZ, OMNIA_VBB_LP_MAX_HZ) &&
               in(v.mix, OMNIA_VBB_MIX_MIN, OMNIA_VBB_MIX_MAX));
    }
    case OMNIA_CMD_LOAD_AETHER: {
        READ(omnia_aether_params_t, v);
        if (!flag(v.enable) || !flag(v.bass_keep_mono) || !flag(v.side_hpf_en) || !flag(v.crossfeed_en) ||
            !FINITE(v.master_blend) || !FINITE(v.width) || !FINITE(v.center_anchor_db) ||
            !FINITE(v.bass_xover_hz) || !FINITE(v.side_hpf_hz) || !FINITE(v.air_gain_db) ||
            !FINITE(v.air_freq_hz) || !FINITE(v.air_q) || !FINITE(v.de_harsh) ||
            !FINITE(v.depth_amount) || !FINITE(v.stage_size_ms) || !FINITE(v.triode_warmth) ||
            !FINITE(v.crossfeed_level) || !FINITE(v.crossfeed_fc_hz)) return false;
        return !v.enable || (in(v.master_blend, OMNIA_AETHER_BLEND_MIN, OMNIA_AETHER_BLEND_MAX) &&
            in(v.width, OMNIA_AETHER_WIDTH_MIN, OMNIA_AETHER_WIDTH_MAX) &&
            in(v.center_anchor_db, OMNIA_AETHER_CENTER_MIN_DB, OMNIA_AETHER_CENTER_MAX_DB) &&
            in(v.bass_xover_hz, OMNIA_AETHER_BASS_XOVER_MIN, OMNIA_AETHER_BASS_XOVER_MAX) &&
            in(v.side_hpf_hz, OMNIA_AETHER_SIDE_HPF_MIN, OMNIA_AETHER_SIDE_HPF_MAX) &&
            in(v.air_gain_db, OMNIA_AETHER_AIR_GAIN_MIN_DB, OMNIA_AETHER_AIR_GAIN_MAX_DB) &&
            in(v.air_freq_hz, OMNIA_AETHER_AIR_FREQ_MIN, OMNIA_AETHER_AIR_FREQ_MAX) &&
            in(v.air_q, OMNIA_AETHER_AIR_Q_MIN, OMNIA_AETHER_AIR_Q_MAX) &&
            in(v.de_harsh, OMNIA_AETHER_DEHARSH_MIN, OMNIA_AETHER_DEHARSH_MAX) &&
            in(v.depth_amount, OMNIA_AETHER_DEPTH_MIN, OMNIA_AETHER_DEPTH_MAX) &&
            in(v.stage_size_ms, OMNIA_AETHER_STAGE_MIN_MS, OMNIA_AETHER_STAGE_MAX_MS) &&
            in(v.triode_warmth, OMNIA_AETHER_TRIODE_MIN, OMNIA_AETHER_TRIODE_MAX) &&
            in(v.crossfeed_level, OMNIA_AETHER_XFEED_MIN, OMNIA_AETHER_XFEED_MAX) &&
            in(v.crossfeed_fc_hz, OMNIA_AETHER_XFEED_FC_MIN, OMNIA_AETHER_XFEED_FC_MAX));
    }
    case OMNIA_CMD_LOAD_LOUDNESS: {
        READ(omnia_loudness_params_t, v);
        if (!flag(v.enable) || !FINITE(v.ref_volume_db) || !FINITE(v.max_bass_boost_db) ||
            !FINITE(v.max_high_boost_db)) return false;
        return !v.enable || (in(v.ref_volume_db, OMNIA_LOUD_REF_MIN_DB, OMNIA_LOUD_REF_MAX_DB) &&
               in(v.max_bass_boost_db, OMNIA_LOUD_MAX_BASS_MIN_DB, OMNIA_LOUD_MAX_BASS_MAX_DB) &&
               in(v.max_high_boost_db, OMNIA_LOUD_MAX_HIGH_MIN_DB, OMNIA_LOUD_MAX_HIGH_MAX_DB));
    }
    case OMNIA_CMD_LOAD_LIMITER: {
        READ(omnia_limiter_params_t, v);
        if (!flag(v.enable) || !flag(v.tpdf_dither_en) || !FINITE(v.ceiling_dbfs) ||
            !FINITE(v.lookahead_ms) || !FINITE(v.release_ms)) return false;
        return !v.enable || (in(v.ceiling_dbfs, OMNIA_LIM_CEILING_MIN_DBFS, OMNIA_LIM_CEILING_MAX_DBFS) &&
               in(v.lookahead_ms, OMNIA_LIM_LOOKAHEAD_MIN_MS, OMNIA_LIM_LOOKAHEAD_MAX_MS) &&
               in(v.release_ms, OMNIA_LIM_RELEASE_MIN_MS, OMNIA_LIM_RELEASE_MAX_MS));
    }
    case OMNIA_CMD_RESET_DC_FAULT:
        return len == 0U; /* payload не требуется; идемпотентная сервисная команда */
    case OMNIA_CMD_LOAD_FACTORY_AETHER: {
        READ(omnia_factory_aether_t, v);
        return v.slot <= 6U;
    }
    default: return false;
    }
}
