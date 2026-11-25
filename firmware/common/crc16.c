#include "crc16.h"

/* CRC-16/CCITT-FALSE，ASCII 123456789 的校验值为 0x29B1。 */
PROTO_RAM_CODE uint16_t crc16_ccitt(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xffff;
    size_t i;
    unsigned bit;
    for (i = 0; i < length; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (bit = 0; bit < 8; ++bit)
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}
