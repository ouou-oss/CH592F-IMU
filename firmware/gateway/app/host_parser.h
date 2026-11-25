#ifndef GATEWAY_HOST_PARSER_H
#define GATEWAY_HOST_PARSER_H

#include "gateway_core.h"
#include "proto.h"

enum { GW_HOST_RX_PAYLOAD_MAX = 88, GW_HOST_RX_FRAME_MAX = GW_HOST_RX_PAYLOAD_MAX + 8 };

typedef struct {
    uint8_t bytes[GW_HOST_RX_FRAME_MAX];
    uint16_t used;
    uint32_t crc_errors;
    uint32_t length_errors;
} gw_host_parser_t;

typedef void (*gw_host_handler_t)(void *context, uint8_t type,
                                  const uint8_t *payload, uint16_t len);

/* 逐字节输入；坏 CRC/过长长度后重找完整帧头，保留跨调用的半个帧头。 */
GW_RAM void gw_host_feed(gw_host_parser_t *parser, uint8_t byte,
                         gw_host_handler_t handler, void *context);

#endif
