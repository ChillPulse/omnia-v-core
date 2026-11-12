#ifndef OMNIA_DSP_VALIDATION_H
#define OMNIA_DSP_VALIDATION_H
#include "omnia_protocol.h"

/* Reject malformed payloads before mutating the realtime DSP. This does not
 * provide atomic DSP reconfiguration or replace hardware safety measures. */
bool omnia_dsp_validate_payload(uint8_t cmd, const void *data, size_t len);
#endif
