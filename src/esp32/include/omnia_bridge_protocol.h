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
#endif
