#ifndef APP_CRC16_H
#define APP_CRC16_H

#include "proto.h"

PROTO_RAM_CODE uint16_t crc16_ccitt(const uint8_t *data, size_t length);

#endif
