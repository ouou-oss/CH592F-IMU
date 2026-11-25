#include "proto.h"

PROTO_RAM_CODE bool proto_rf_valid(const uint8_t *packet, size_t length)
{
    uint8_t type;
    if (!packet || !length || (packet[0] >> 4) != PROTO_VERSION)
        return false;
    type = packet[0] & 15;
    if (type == PROTO_ACK) {
        return length >= PROTO_ACK_HEADER && packet[5] <= PROTO_CMD_MAX &&
               packet[14] <= PROTO_CONF_MAX &&
               length == PROTO_ACK_HEADER + (size_t)packet[14] * 3;
    }
    if (length < 14 || packet[1] == 0 || packet[1] == 255)
        return false;
    if (type == PROTO_DATA) {
        return length >= PROTO_DATA_HEADER && packet[18] != 0 && packet[18] <= PROTO_SAMPLES_MAX &&
               length == PROTO_DATA_HEADER + (size_t)packet[18] * PROTO_SAMPLE_SIZE;
    }
    if (type == PROTO_STATUS)
        return length == PROTO_STATUS_SIZE;
    if (type == PROTO_POLL)
        return length == PROTO_POLL_SIZE;
    return false;
}
