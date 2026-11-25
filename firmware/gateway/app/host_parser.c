#include "host_parser.h"
#include "crc16.h"

static GW_RAM void discard(gw_host_parser_t *p, uint16_t count)
{
    p->used = (uint16_t)(p->used - count);
    for (uint16_t i = 0; i < p->used; ++i) p->bytes[i] = p->bytes[i + count];
}

GW_RAM void gw_host_feed(gw_host_parser_t *p, uint8_t byte,
                         gw_host_handler_t handler, void *context)
{
    if (p->used == GW_HOST_RX_FRAME_MAX) discard(p, 1);
    p->bytes[p->used++] = byte;
    while (p->used >= 2u) {
        if (p->bytes[0] != 0xA5 || p->bytes[1] != 0x5A) {
            discard(p, 1);
            continue;
        }
        if (p->used < 6u) return;
        uint16_t len = proto_get_u16(p->bytes + 4);
        if (len > GW_HOST_RX_PAYLOAD_MAX) {
            ++p->length_errors;
            discard(p, 1);
            continue;
        }
        uint16_t total = (uint16_t)(len + 8u);
        if (p->used < total) return;
        uint16_t crc = crc16_ccitt(p->bytes + 2, (size_t)len + 4u);
        if (crc != proto_get_u16(p->bytes + 6u + len)) {
            ++p->crc_errors;
            discard(p, 1);
            continue;
        }
        handler(context, p->bytes[2], p->bytes + 6, len);
        discard(p, total);
    }
}
