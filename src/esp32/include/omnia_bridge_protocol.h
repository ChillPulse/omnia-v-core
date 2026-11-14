/* Pure response validation for the ESP32 UART bridge; CRC is checked by parser. */
#ifndef OMNIA_BRIDGE_PROTOCOL_H
#define OMNIA_BRIDGE_PROTOCOL_H
#include "omnia_protocol.h"

static inline bool omnia_bridge_decode_ack(const omnia_frame_hdr_t *hdr, const uint8_t *payload,
                                           uint8_t seq, uint8_t cmd, uint8_t *result)
{
    if (!hdr || !payload || !result || hdr->cmd != OMNIA_EVT_ACK_NACK ||
        hdr->seq != seq || hdr->payload_len != sizeof(omnia_ack_payload_t) ||
        payload[0] != cmd || OMNIA_GET_VERSION(hdr->ver_flags) != OMNIA_PROTO_VERSION)
        return false;
    const uint8_t flags = OMNIA_GET_FLAGS(hdr->ver_flags);
    if (flags == OMNIA_FLAG_IS_ACK && payload[1] == OMNIA_STATUS_OK) {
        *result = OMNIA_STATUS_OK;
        return true;
    }
    if (flags == OMNIA_FLAG_IS_NACK) {
        /* NACK with an invalid zero error code must never become success. */
        *result = payload[1] ? payload[1] : OMNIA_STATUS_ERR_INVALID_ARG;
        return true;
    }
    return false;
}

/* Caller serializes EVERY access, including signalling and cancellation.
 * This state machine is not a session ID or duplicate suppression protocol. */
typedef enum {
    OMNIA_BRIDGE_IDLE = 0,
    OMNIA_BRIDGE_WAIT_ACK,
    OMNIA_BRIDGE_WAIT_INFO,
    OMNIA_BRIDGE_COMPLETE_ACK,
    OMNIA_BRIDGE_COMPLETE_INFO
} omnia_bridge_phase_t;

typedef struct {
    omnia_bridge_phase_t phase;
    uint8_t seq;
    uint8_t command;
    uint8_t ack_status;
    omnia_sys_info_t info;
} omnia_bridge_transaction_t;

static inline void omnia_bridge_begin(omnia_bridge_transaction_t *t, omnia_bridge_phase_t phase,
                                      uint8_t seq, uint8_t command)
{
    t->phase = phase;
    t->seq = seq;
    t->command = command;
    t->ack_status = OMNIA_STATUS_ERR_CRC;
}

/* Returns true exactly once for each armed operation. The frame parser has
 * already checked CRC; this function still validates all response fields. */
static inline bool omnia_bridge_accept(omnia_bridge_transaction_t *t, const omnia_frame_hdr_t *hdr,
                                       const uint8_t *payload)
{
    if (!t || !hdr || !payload) return false;
    if (t->phase == OMNIA_BRIDGE_WAIT_ACK) {
        uint8_t code;
        if (!omnia_bridge_decode_ack(hdr, payload, t->seq, t->command, &code)) return false;
        t->ack_status = code;
        t->phase = OMNIA_BRIDGE_COMPLETE_ACK;
        return true;
    }
    if (t->phase == OMNIA_BRIDGE_WAIT_INFO && hdr->seq == t->seq &&
        hdr->cmd == OMNIA_EVT_SYS_INFO && hdr->payload_len == sizeof(t->info) &&
        OMNIA_GET_VERSION(hdr->ver_flags) == OMNIA_PROTO_VERSION &&
        OMNIA_GET_FLAGS(hdr->ver_flags) == OMNIA_FLAG_IS_ACK) {
        memcpy(&t->info, payload, sizeof(t->info));
        t->phase = OMNIA_BRIDGE_COMPLETE_INFO;
        return true;
    }
    return false;
}

static inline void omnia_bridge_cancel(omnia_bridge_transaction_t *t)
{
    t->phase = OMNIA_BRIDGE_IDLE;
}
#endif
