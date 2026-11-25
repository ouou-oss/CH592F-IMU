#ifndef APP_FRAMING_H
#define APP_FRAMING_H

#include "proto.h"

PROTO_RAM_CODE size_t host_frame_encode(uint8_t *out, size_t capacity,
                                       uint8_t type, uint8_t sequence,
                                       const uint8_t *payload, uint16_t length);

#endif
