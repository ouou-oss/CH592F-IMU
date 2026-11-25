#include "framing.h"
#include "crc16.h"

PROTO_RAM_CODE size_t host_frame_encode(uint8_t *out, size_t capacity,
                                       uint8_t type, uint8_t sequence,
                                       const uint8_t *payload, uint16_t length)
{
    size_t i;
    if (!out || length > HOST_PAYLOAD_MAX || capacity < (size_t)length + 8 ||
        (length && !payload))
        return 0;
    out[0] = 0xa5;
    out[1] = 0x5a;
    out[2] = type;
    out[3] = sequence;
    proto_put_u16(out + 4, length);
    for (i = 0; i < length; ++i)
        out[6 + i] = payload[i];
    proto_put_u16(out + 6 + length, crc16_ccitt(out + 2, (size_t)length + 4));
    return (size_t)length + 8;
}
