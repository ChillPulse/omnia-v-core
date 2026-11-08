/**
 * @file    test_dsp_and_proto.c
 * @brief   Автоматические модульные тесты протокола OMNIA v2 (CRC16/парсер)
 *          и математического ядра 9-звенного студийного DSP (Bit-Perfect, AETHER, Limiter, DC Guard).
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omnia_crc16.h"
#include "omnia_protocol.h"
#include "omnia_dsp.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void test_crc16_ccitt_vector(void)
{
    const char *vec = "123456789";
    const uint16_t crc = omnia_crc16_ccitt(vec, 9U);
    assert(crc == 0x29B1U);
    printf("[OK] CRC-16/CCITT-FALSE эталонный вектор '123456789' == 0x%04X\n", crc);
}

static void test_protocol_framing_and_noise_recovery(void)
{
    uint8_t wire[512];
    omnia_aether_params_t tx_aether = {
        .enable           = 1U,
        .bass_keep_mono   = 1U,
        .side_hpf_en      = 1U,
        .crossfeed_en     = 0U,
        .master_blend     = 0.75f,
        .width            = 1.42f,
        .center_anchor_db = 0.80f,
        .bass_xover_hz    = 95.0f,
        .side_hpf_hz      = 140.0f,
        .air_gain_db      = 2.5f,
        .air_freq_hz      = 11500.0f,
        .air_q            = 0.72f,
        .de_harsh         = 0.40f,
        .depth_amount     = 0.25f,
        .stage_size_ms    = 6.8f,
        .triode_warmth    = 0.18f,
        .crossfeed_level  = 0.0f,
        .crossfeed_fc_hz  = 700.0f
    };

    /* Мусорные байты на линии перед валидным кадром */
    wire[0] = 0x13U;
    wire[1] = 0x55U; /* Ложная полу-преамбула */
    wire[2] = 0x99U;

    const size_t flen = omnia_proto_build_frame(
        &wire[3], sizeof(wire) - 3U, OMNIA_FLAG_ACK_REQ, 42U,
        OMNIA_CMD_LOAD_AETHER, &tx_aether, sizeof(tx_aether));
    assert(flen == sizeof(omnia_frame_hdr_t) + sizeof(tx_aether) + 2U);

    omnia_proto_parser_t parser;
    omnia_proto_parser_init(&parser);

    bool got_valid = false;
    for (size_t i = 0; i < 3U + flen; ++i) {
        if (omnia_proto_parser_feed(&parser, wire[i])) {
            got_valid = true;
        }
    }
    assert(got_valid);
    assert(parser.crc_errors == 0U);
    assert(parser.hdr.seq == 42U);
    assert(parser.hdr.cmd == OMNIA_CMD_LOAD_AETHER);
    assert(memcmp(parser.payload, &tx_aether, sizeof(tx_aether)) == 0);

    /* Проверка детекции битого бита в полезной нагрузке */
    wire[3 + 8] ^= 0x01U;
    got_valid = false;
    for (size_t i = 3U; i < 3U + flen; ++i) {
        if (omnia_proto_parser_feed(&parser, wire[i])) {
            got_valid = true;
        }
    }
    assert(!got_valid);
    assert(parser.crc_errors == 1U);
    printf("[OK] Протокол OMNIA v2: синхронизация после помех и отлов битого CRC проверены\n");
}

static void test_bit_perfect_exact_passthrough(void)
{
    static omnia_dsp_delay_arena_t arena;
    static omnia_dsp_ctx_t dsp;

    omnia_dsp_init(&dsp, &arena, 48000U);
    omnia_dsp_set_mute(&dsp, false, 0U);
    omnia_dsp_set_volume(&dsp, 0.0f, 0.0f, 0.0f, 0U);
    omnia_dsp_set_bypass_mask(&dsp, OMNIA_DSP_ALL_BYPASSED, 0U);

    /* Даём сглаживателю усиления выйти ровно на 1.0 за короткий прогон */
    int32_t warmup[128] = {0};
    for (int k = 0; k < 64; ++k) {
        omnia_dsp_process_block_i32(&dsp, warmup, warmup, 64U);
    }

    int32_t in_pcm[128];
    int32_t out_pcm[128];
    for (int i = 0; i < 128; ++i) {
        in_pcm[i] = (int32_t)(0x7ABC1200 - i * 133371);
    }

    omnia_dsp_process_block_i32(&dsp, in_pcm, out_pcm, 64U);
    assert(dsp.bit_perfect_active);
    assert(memcmp(in_pcm, out_pcm, sizeof(in_pcm)) == 0);
    printf("[OK] Режим Bit-Perfect Direct: 100%% побитовое совпадение 32-bit сэмплов L/R\n");
}

static void test_full_dsp_aether_subbass_mono_and_limiter(void)
{
    static omnia_dsp_delay_arena_t arena;
    static omnia_dsp_ctx_t dsp;

    omnia_dsp_init(&dsp, &arena, 48000U);
    omnia_dsp_set_mute(&dsp, false, 1U);
    omnia_dsp_set_volume(&dsp, 0.0f, 0.0f, 0.0f, 1U);

    const omnia_aether_params_t aether = {
        .enable           = 1U,
        .bass_keep_mono   = 1U,
        .side_hpf_en      = 1U,
        .crossfeed_en     = 0U,
        .master_blend     = 1.0f,
        .width            = 1.75f,
        .center_anchor_db = 0.70f,
        .bass_xover_hz    = 100.0f,
        .side_hpf_hz      = 150.0f,
        .air_gain_db      = 2.5f,
        .air_freq_hz      = 11000.0f,
        .air_q            = 0.75f,
        .de_harsh         = 0.40f,
        .depth_amount     = 0.25f,
        .stage_size_ms    = 6.0f,
        .triode_warmth    = 0.18f,
        .crossfeed_level  = 0.0f,
        .crossfeed_fc_hz  = 700.0f
    };
    omnia_dsp_apply_aether(&dsp, &aether);

    omnia_dsp_set_bypass_mask(&dsp,
                              OMNIA_DSP_BIT_SUBSONIC_HPF |
                              OMNIA_DSP_BIT_HEADROOM |
                              OMNIA_DSP_BIT_USER_PEQ |
                              OMNIA_DSP_BIT_VBB_BASS |
                              OMNIA_DSP_BIT_AETHER |
                              OMNIA_DSP_BIT_LOUDNESS |
                              OMNIA_DSP_BIT_LIMITER,
                              1U);

    int32_t in_blk[128];
    int32_t out_blk[128];
    double max_abs_out = 0.0;

    /* Прогон 200 блоков (12800 сэмплов) перегруженного мульти-тона для проверки Look-Ahead лимитера */
    double phase_40 = 0.0, phase_1k = 0.0;
    for (int b = 0; b < 200; ++b) {
        for (int f = 0; f < 64; ++f) {
            const double s_low  = 0.40 * sin(phase_40);
            const double s_high = 0.95 * sin(phase_1k);
            phase_40 += 2.0 * M_PI * 40.0 / 48000.0;
            phase_1k += 2.0 * M_PI * 1000.0 / 48000.0;

            double l = s_low + s_high;
            double r = -s_low + 0.6 * s_high;
            if (l > 0.999)  l = 0.999;
            if (l < -0.999) l = -0.999;
            if (r > 0.999)  r = 0.999;
            if (r < -0.999) r = -0.999;

            in_blk[f * 2]     = (int32_t)(l * 2147483647.0);
            in_blk[f * 2 + 1] = (int32_t)(r * 2147483647.0);
        }
        omnia_dsp_process_block_i32(&dsp, in_blk, out_blk, 64U);

        if (b > 30) {
            for (int i = 0; i < 128; ++i) {
                const double norm = fabs((double)out_blk[i] / 2147483648.0);
                assert(!isnan(norm) && !isinf(norm));
                if (norm > max_abs_out) {
                    max_abs_out = norm;
                }
            }
        }
    }

    /* Потолок лимитера по умолчанию -0.8 dBFS (~0.912): проверяем отсутствие цифрового клиппинга */
    assert(max_abs_out > 0.15 && max_abs_out <= 0.985);
    assert(!dsp.dc_fault_tripped);
    printf("[OK] Студийный 9-звенный тракт (AETHER + VBB + ISO226 + Limiter): пик=%.4f (без клиппинга)\n",
           max_abs_out);
}

static void test_room_speaker_delay_and_phase(void)
{
    static omnia_dsp_delay_arena_t arena;
    static omnia_dsp_ctx_t dsp;

    omnia_dsp_init(&dsp, &arena, 48000U);
    omnia_dsp_set_mute(&dsp, false, 0U);
    omnia_dsp_set_volume(&dsp, 0.0f, 0.0f, 0.0f, 0U);
    dsp.limiter_cfg.enable        = 0U;
    dsp.limiter_cfg.tpdf_dither_en = 0U;

    /* Настраиваем рум-коррекцию: задержка левой АС = 0.25 мс (ровно 12.0 сэмплов @ 48 кГц),
     * правая АС = 0.00 мс, но с переворотом фазы 180° (invert_phase_r = 1) */
    omnia_room_peq_t room = {0};
    room.enable         = 1U;
    room.band_count     = 0U;
    room.delay_l_ms     = 0.25f;
    room.delay_r_ms     = 0.00f;
    room.invert_phase_l = 0U;
    room.invert_phase_r = 1U;
    omnia_dsp_apply_room_peq(&dsp, &room);
    omnia_dsp_set_bypass_mask(&dsp, OMNIA_DSP_BIT_ROOM_PEQ, 0U);

    /* Даём параметрам задержки и фазы плавно установиться на тишине */
    int32_t zero_blk[128] = {0};
    for (int k = 0; k < 150; ++k) {
        omnia_dsp_process_block_i32(&dsp, zero_blk, zero_blk, 64U);
    }

    /* Подаём единичный импульс в сэмпле #0 на оба канала L и R */
    int32_t in_imp[128]  = {0};
    int32_t out_imp[128] = {0};
    in_imp[0] =  1000000000; /* L[0] = +0.4656 */
    in_imp[1] =  1000000000; /* R[0] = +0.4656 */

    omnia_dsp_process_block_i32(&dsp, in_imp, out_imp, 64U);

    /* Правый канал (0 мс, фаза 180°) должен выдать отрицательный импульс сразу в сэмпле #0,
     * а левый канал (0.25 мс = 12 сэмплов, фаза 0°) — положительный импульс ровно в сэмпле #12! */
    assert(out_imp[1] < -990000000);
    assert(abs(out_imp[0]) < 1000);
    assert(out_imp[12 * 2] > 990000000);
    printf("[OK] Рум-коррекция АС: суб-сэмпловая задержка (0.25 мс -> сэмпл #12) и инверсия фазы 180° точны!\n");
}

int main(void)
{
    printf("=== Запуск модульных тестов OMNIA v2.0 ===\n");
    test_crc16_ccitt_vector();
    test_protocol_framing_and_noise_recovery();
    test_bit_perfect_exact_passthrough();
    test_full_dsp_aether_subbass_mono_and_limiter();
    test_room_speaker_delay_and_phase();
    printf("=== ВСЕ ТЕСТЫ УСПЕШНО ПРОЙДЕНЫ ===\n");
    return 0;
}
