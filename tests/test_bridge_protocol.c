#include "omnia_bridge_protocol.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    omnia_frame_hdr_t h = { .sync = OMNIA_PROTO_SYNC_WORD, .payload_len = 2,
        .ver_flags = OMNIA_MAKE_VER_FLAGS(OMNIA_FLAG_IS_ACK), .seq = 9, .cmd = OMNIA_EVT_ACK_NACK };
    uint8_t p[2] = { OMNIA_CMD_MUTE, OMNIA_STATUS_OK };
    uint8_t result = 99;
    assert(omnia_bridge_decode_ack(&h, p, 9, OMNIA_CMD_MUTE, &result) && result == OMNIA_STATUS_OK);
    assert(!omnia_bridge_decode_ack(&h, p, 8, OMNIA_CMD_MUTE, &result));
    assert(!omnia_bridge_decode_ack(&h, p, 9, OMNIA_CMD_UNMUTE, &result));
    h.payload_len = 1; assert(!omnia_bridge_decode_ack(&h, p, 9, OMNIA_CMD_MUTE, &result));
    h.payload_len = 3; assert(!omnia_bridge_decode_ack(&h, p, 9, OMNIA_CMD_MUTE, &result));
    h.payload_len = 2; h.ver_flags = OMNIA_MAKE_VER_FLAGS(0); assert(!omnia_bridge_decode_ack(&h, p, 9, OMNIA_CMD_MUTE, &result));
    h.ver_flags = OMNIA_MAKE_VER_FLAGS(OMNIA_FLAG_IS_ACK | OMNIA_FLAG_IS_NACK);
    assert(!omnia_bridge_decode_ack(&h, p, 9, OMNIA_CMD_MUTE, &result));
    h.ver_flags = OMNIA_MAKE_VER_FLAGS(OMNIA_FLAG_IS_ACK); p[1] = OMNIA_STATUS_ERR_INVALID_ARG;
    assert(!omnia_bridge_decode_ack(&h, p, 9, OMNIA_CMD_MUTE, &result));
    h.ver_flags = OMNIA_MAKE_VER_FLAGS(OMNIA_FLAG_IS_NACK);
    assert(omnia_bridge_decode_ack(&h, p, 9, OMNIA_CMD_MUTE, &result) && result == OMNIA_STATUS_ERR_INVALID_ARG);
    p[1] = 0; assert(omnia_bridge_decode_ack(&h, p, 9, OMNIA_CMD_MUTE, &result) && result != OMNIA_STATUS_OK);
    h.ver_flags = 0x34; assert(!omnia_bridge_decode_ack(&h, p, 9, OMNIA_CMD_MUTE, &result));
    assert(!omnia_bridge_decode_ack(NULL, p, 9, OMNIA_CMD_MUTE, &result));
    assert(!omnia_bridge_decode_ack(&h, NULL, 9, OMNIA_CMD_MUTE, &result));
    puts("[OK] strict ACK/NACK flags, status, sequence, command and length");
}
