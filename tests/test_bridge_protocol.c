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
    /* Portable state-machine interleavings; the FreeRTOS lock itself requires
     * target/integration testing. An event can complete an operation only once. */
    omnia_bridge_transaction_t t = {0};
    h.ver_flags = OMNIA_MAKE_VER_FLAGS(OMNIA_FLAG_IS_ACK);
    h.seq = 9; h.cmd = OMNIA_EVT_ACK_NACK; h.payload_len = 2;
    p[0] = OMNIA_CMD_MUTE; p[1] = OMNIA_STATUS_OK;
    assert(!omnia_bridge_accept(&t, &h, p)); /* no pending request */
    omnia_bridge_begin(&t, OMNIA_BRIDGE_WAIT_ACK, 10, OMNIA_CMD_MUTE);
    assert(!omnia_bridge_accept(&t, &h, p)); /* late response for previous seq */
    h.seq = 10; p[0] = OMNIA_CMD_UNMUTE;
    assert(!omnia_bridge_accept(&t, &h, p)); /* wrong command */
    p[0] = OMNIA_CMD_MUTE;
    assert(omnia_bridge_accept(&t, &h, p));
    assert(t.phase == OMNIA_BRIDGE_COMPLETE_ACK && t.ack_status == OMNIA_STATUS_OK);
    assert(!omnia_bridge_accept(&t, &h, p)); /* duplicate */
    omnia_bridge_cancel(&t);
    assert(!omnia_bridge_accept(&t, &h, p)); /* response after timeout/cancel */

    omnia_sys_info_t info = { .fw_major = 2, .sram_size_kb = 224, .sysclk_hz = 240000000 };
    omnia_bridge_begin(&t, OMNIA_BRIDGE_WAIT_INFO, 11, OMNIA_CMD_GET_SYS_INFO);
    assert(!omnia_bridge_accept(&t, &h, p)); /* ACK is not SYS_INFO */
    h.seq = 11; h.cmd = OMNIA_EVT_SYS_INFO; h.payload_len = sizeof(info);
    h.ver_flags = OMNIA_MAKE_VER_FLAGS(0);
    assert(!omnia_bridge_accept(&t, &h, (const uint8_t *)&info));
    h.ver_flags = OMNIA_MAKE_VER_FLAGS(OMNIA_FLAG_IS_ACK);
    assert(omnia_bridge_accept(&t, &h, (const uint8_t *)&info));
    assert(t.phase == OMNIA_BRIDGE_COMPLETE_INFO && t.info.sram_size_kb == 224);
    assert(!omnia_bridge_accept(&t, &h, (const uint8_t *)&info));
    omnia_bridge_cancel(&t);
    assert(t.phase == OMNIA_BRIDGE_IDLE);
    puts("[OK] strict ACK/NACK flags, status, sequence, command and length");
}
