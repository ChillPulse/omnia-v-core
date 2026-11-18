/* Deterministic host-only UART framing stress; not a DMA/timing test. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "omnia_protocol.h"

static uint32_t rng = 0x5EED1234U;
static uint32_t next_random(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

int main(void)
{
    omnia_proto_parser_t parser;
    uint8_t payload[OMNIA_PROTO_MAX_PAYLOAD];
    uint8_t frame[OMNIA_PROTO_MAX_FRAME_SIZE];
    omnia_proto_parser_init(&parser);
    unsigned accepted = 0, corrupted = 0;
    for (unsigned round = 0; round < 4000; ++round) {
        const uint16_t len = (round < 545U) ? (uint16_t)round :
                             (uint16_t)(next_random() % (OMNIA_PROTO_MAX_PAYLOAD + 1U));
        for (uint16_t i = 0; i < len; ++i) payload[i] = (uint8_t)next_random();
        /* Exercise sync bytes inside payload, which must not restart parsing. */
        if (len >= 2U) { payload[0] = 0x55U; payload[1] = 0xAAU; }
        const uint8_t seq = (uint8_t)round;
        const size_t n = omnia_proto_build_frame(frame, sizeof(frame), OMNIA_FLAG_ACK_REQ,
                                                  seq, OMNIA_CMD_PING, payload, len);
        assert(n == (size_t)len + OMNIA_PROTO_HEADER_SIZE + OMNIA_PROTO_CRC_SIZE);
        if (round % 7U == 0U) {
            /* Flip one CRC bit only: deliberately preserve declared length. */
            frame[n - 1U] ^= 0x01U;
            ++corrupted;
        }
        /* Simulated chunks with no processing gap: frames are back-to-back. */
        for (size_t pos = 0; pos < n;) {
            size_t chunk = 1U + next_random() % 31U;
            if (chunk > n - pos) chunk = n - pos;
            for (size_t j = 0; j < chunk; ++j) {
                const bool ready = omnia_proto_parser_feed(&parser, frame[pos + j]);
                if (ready) {
                    assert(round % 7U != 0U);
                    assert(pos + j == n - 1U);
                    assert(parser.hdr.seq == seq && parser.hdr.cmd == OMNIA_CMD_PING);
                    assert(parser.hdr.payload_len == len);
                    assert(memcmp(parser.payload, payload, len) == 0);
                    ++accepted;
                }
            }
            pos += chunk;
        }
        assert(parser.state == OMNIA_RX_SYNC_0);
    }
    assert(accepted + corrupted == 4000U);
    assert(parser.frames_ok == accepted && parser.crc_errors == corrupted);

    /* A valid-looking length can consume a following frame: no magic recovery
     * without an idle gap. Explicitly expire the partial frame before retry. */
    const uint8_t partial[] = {0x55, 0xAA, 100, 0, 0x20, 1, OMNIA_CMD_PING, 0x42};
    for (size_t i = 0; i < sizeof(partial); ++i) assert(!omnia_proto_parser_feed(&parser, partial[i]));
    assert(!omnia_proto_parser_expire(&parser, 249U, 0U, 250U));
    assert(omnia_proto_parser_expire(&parser, 250U, 0U, 250U));
    size_t n = omnia_proto_build_frame(frame, sizeof(frame), 0, 99, OMNIA_CMD_PING, NULL, 0);
    bool got = false;
    for (size_t i = 0; i < n; ++i) got |= omnia_proto_parser_feed(&parser, frame[i]);
    assert(got && parser.hdr.seq == 99U);
    printf("protocol stream OK: %u valid, %u CRC corrupt, timeout reset\n", accepted, corrupted);
    return 0;
}
