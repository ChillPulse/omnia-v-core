#include "omnia_dsp_validation.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* All payloads are packed. Inject IEEE-754 bit patterns via memcpy, also when
 * compiled with -ffast-math; never depend on isfinite() in the test. */
static void bad_float(uint8_t cmd, const void *good, size_t size, size_t offset)
{
    unsigned char buf[sizeof(omnia_user_peq_t)];
    const uint32_t bits[] = { 0x7fc00000U, 0x7f800000U, 0xff800000U };
    assert(size <= sizeof(buf) && offset + 4 <= size);
    for (unsigned i = 0; i < sizeof(bits) / sizeof(bits[0]); ++i) {
        memcpy(buf, good, size); memcpy(buf + offset, &bits[i], 4);
        assert(!omnia_dsp_validate_payload(cmd, buf, size));
    }
}
#define CHECK(cmd, v) do { \
    assert(omnia_dsp_validate_payload(cmd, &(v), sizeof(v))); \
    assert(!omnia_dsp_validate_payload(cmd, &(v), sizeof(v) - 1)); \
    assert(!omnia_dsp_validate_payload(cmd, NULL, sizeof(v))); \
} while (0)
#define BAD(cmd, v, type, field) bad_float(cmd, &(v), sizeof(v), offsetof(type, field))
int main(void)
{
    omnia_hpf_headroom_t h = {1, 1, 20.f, .707f, -6.f};
    CHECK(OMNIA_CMD_LOAD_HPF_HEADROOM, h);
    BAD(OMNIA_CMD_LOAD_HPF_HEADROOM, h, omnia_hpf_headroom_t, hpf_q);
    h.hpf_enable = 2; assert(!omnia_dsp_validate_payload(OMNIA_CMD_LOAD_HPF_HEADROOM, &h, sizeof h));
    h.hpf_enable = 0; h.hpf_freq_hz = h.hpf_q = 0; CHECK(OMNIA_CMD_LOAD_HPF_HEADROOM, h);
    omnia_room_peq_t room = {0}; room.enable = 1; room.band_count = 1;
    room.bands[0] = (omnia_biquad_band_t){.type = OMNIA_BIQUAD_PEAK, .f0_hz = 1000.f, .q = 1.f, .gain_db = 3.f};
    CHECK(OMNIA_CMD_LOAD_ROOM_PEQ, room);
    BAD(OMNIA_CMD_LOAD_ROOM_PEQ, room, omnia_room_peq_t, delay_l_ms);
    bad_float(OMNIA_CMD_LOAD_ROOM_PEQ, &room, sizeof room,
              offsetof(omnia_room_peq_t, bands) + offsetof(omnia_biquad_band_t, gain_db));
    room.band_count = 6; assert(!omnia_dsp_validate_payload(OMNIA_CMD_LOAD_ROOM_PEQ, &room, sizeof room));
    omnia_user_peq_t user = {0}; user.enable = 1; user.band_count = 1;
    user.bands[0] = (omnia_biquad_band_t){.type = OMNIA_BIQUAD_HIGH_SHELF, .f0_hz = 8000.f, .q = .7f, .gain_db = -2.f};
    CHECK(OMNIA_CMD_LOAD_USER_PEQ, user);
    user.bands[0].type = 255; assert(!omnia_dsp_validate_payload(OMNIA_CMD_LOAD_USER_PEQ, &user, sizeof user));
    user.bands[0].type = OMNIA_BIQUAD_HIGH_SHELF;
    bad_float(OMNIA_CMD_LOAD_USER_PEQ, &user, sizeof user,
              offsetof(omnia_user_peq_t, bands) + offsetof(omnia_biquad_band_t, f0_hz));
    omnia_anti_ad_t anti = {1, .65f, .992f, .0002f, 0.f}; CHECK(OMNIA_CMD_LOAD_ANTI_AD, anti);
    BAD(OMNIA_CMD_LOAD_ANTI_AD, anti, omnia_anti_ad_t, attack_coeff);
    omnia_vbb_params_t vbb = {1, 1, .35f, 65.f, .3f, 140.f, .5f}; CHECK(OMNIA_CMD_LOAD_VBB, vbb);
    BAD(OMNIA_CMD_LOAD_VBB, vbb, omnia_vbb_params_t, mix);
    omnia_aether_params_t a = {
        .enable=1, .bass_keep_mono=1, .side_hpf_en=1, .crossfeed_en=0,
        .master_blend=.55f, .width=1.28f, .center_anchor_db=.8f, .bass_xover_hz=95.f,
        .side_hpf_hz=135.f, .air_gain_db=2.2f, .air_freq_hz=11200.f, .air_q=.72f,
        .de_harsh=.35f, .depth_amount=.22f, .stage_size_ms=6.4f, .triode_warmth=.14f,
        .crossfeed_level=0.f, .crossfeed_fc_hz=700.f
    };
    CHECK(OMNIA_CMD_LOAD_AETHER, a);
    /* Each float in the packed AETHER request must reject NaN and infinities. */
    const size_t aether_fields[] = {
        offsetof(omnia_aether_params_t, master_blend), offsetof(omnia_aether_params_t, width),
        offsetof(omnia_aether_params_t, center_anchor_db), offsetof(omnia_aether_params_t, bass_xover_hz),
        offsetof(omnia_aether_params_t, side_hpf_hz), offsetof(omnia_aether_params_t, air_gain_db),
        offsetof(omnia_aether_params_t, air_freq_hz), offsetof(omnia_aether_params_t, air_q),
        offsetof(omnia_aether_params_t, de_harsh), offsetof(omnia_aether_params_t, depth_amount),
        offsetof(omnia_aether_params_t, stage_size_ms), offsetof(omnia_aether_params_t, triode_warmth),
        offsetof(omnia_aether_params_t, crossfeed_level), offsetof(omnia_aether_params_t, crossfeed_fc_hz)
    };
    for (size_t i = 0; i < sizeof(aether_fields) / sizeof(aether_fields[0]); ++i)
        bad_float(OMNIA_CMD_LOAD_AETHER, &a, sizeof a, aether_fields[i]);
    a.stage_size_ms = 13.f; assert(!omnia_dsp_validate_payload(OMNIA_CMD_LOAD_AETHER, &a, sizeof a));
    a.enable = 0; CHECK(OMNIA_CMD_LOAD_AETHER, a);
    BAD(OMNIA_CMD_LOAD_AETHER, a, omnia_aether_params_t, stage_size_ms);
    omnia_loudness_params_t loud = {1, -18.f, 6.5f, 3.f}; CHECK(OMNIA_CMD_LOAD_LOUDNESS, loud);
    BAD(OMNIA_CMD_LOAD_LOUDNESS, loud, omnia_loudness_params_t, max_bass_boost_db);
    omnia_limiter_params_t lim = {1, 1, -.8f, 1.8f, 120.f}; CHECK(OMNIA_CMD_LOAD_LIMITER, lim);
    BAD(OMNIA_CMD_LOAD_LIMITER, lim, omnia_limiter_params_t, lookahead_ms);
    lim.release_ms = 0; assert(!omnia_dsp_validate_payload(OMNIA_CMD_LOAD_LIMITER, &lim, sizeof lim));
    assert(!omnia_dsp_validate_payload(0xff, &h, sizeof h));
    puts("[OK] DSP payload ranges, NaN/Inf, flags, sizes and band counts");
    return 0;
}
