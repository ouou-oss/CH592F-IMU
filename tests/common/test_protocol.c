#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "crc16.h"
#include "framing.h"

static void test_framing(void)
{
    const uint8_t get_info[] = {0xa5, 0x5a, 0x83, 0, 0, 0, 0x24, 0xc2};
    const uint8_t time_req[] = {0xa5, 0x5a, 0x81, 1, 4, 0, 1, 0, 0, 0, 0x18, 0x43};
    uint8_t out[HOST_FRAME_MAX];
    uint8_t request[4] = {1, 0, 0, 0};
    assert(host_frame_encode(out, sizeof out, HOST_GET_INFO, 0, NULL, 0) == sizeof get_info);
    assert(memcmp(out, get_info, sizeof get_info) == 0);
    assert(host_frame_encode(out, sizeof out, HOST_TIME_REQ, 1, request, 4) == sizeof time_req);
    assert(memcmp(out, time_req, sizeof time_req) == 0);
    assert(host_frame_encode(out, 7, HOST_GET_INFO, 0, NULL, 0) == 0);
    assert(host_frame_encode(out, sizeof out, HOST_TIME_REQ, 0, NULL, 4) == 0);
    assert(host_frame_encode(out, sizeof out, HOST_TIME_REQ, 0, out, HOST_PAYLOAD_MAX + 1) == 0);
}

static void test_validation(void)
{
    uint8_t p[PROTO_RF_MAX] = {0x11, 1};
    p[18] = 35;
    assert(proto_rf_valid(p, sizeof p));
    assert(!proto_rf_valid(p, sizeof p - 1));
    p[18] = 0;
    assert(!proto_rf_valid(p, PROTO_DATA_HEADER));
    p[18] = 1;
    assert(proto_rf_valid(p, PROTO_DATA_HEADER + PROTO_SAMPLE_SIZE));
    p[18] = PROTO_SAMPLES_MAX + 1;
    assert(!proto_rf_valid(p, PROTO_DATA_HEADER));
    p[0] = 0x21;
    assert(!proto_rf_valid(p, sizeof p));
    p[0] = 0x12;
    assert(proto_rf_valid(p, PROTO_STATUS_SIZE));
    p[1] = 255;
    assert(!proto_rf_valid(p, PROTO_STATUS_SIZE));
    p[0] = 0x18;
    p[5] = 8;
    p[14] = 8;
    assert(proto_rf_valid(p, PROTO_ACK_MAX));
    p[14] = 9;
    assert(!proto_rf_valid(p, PROTO_ACK_MAX));
    assert(!proto_rf_valid(NULL, 0));
}

int main(void)
{
    uint8_t unaligned[7];
    proto_put_u16(unaligned + 1, 0x1234);
    assert(proto_get_u16(unaligned + 1) == 0x1234);
    proto_put_u32(unaligned + 1, UINT32_C(0xfedcba98));
    assert(proto_get_u32(unaligned + 1) == UINT32_C(0xfedcba98));
    assert(crc16_ccitt((const uint8_t *)"123456789", 9) == 0x29b1);
    assert(crc16_ccitt(NULL, 0) == 0xffff);
    test_framing();
    test_validation();
    puts("common: CRC, wire vectors, unaligned endian and RF validation passed");
    return 0;
}
