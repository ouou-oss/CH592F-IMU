#include "sensor.h"
#include <string.h>

static uint8_t current[246];
static uint8_t count;
static uint32_t started, last_index;

void packet_header(uint8_t *data, uint8_t type)
{
    data[0] = 0x10u | type; data[1] = sensor_config.node; data[2] = 0;
    data[3] = 0x80u | sensor_config.odr | (sensor_config.range << 4) |
              (sensor_config.bandwidth << 6);
    proto_put_u16(data + 4, sensor_config.boot);
    proto_put_u32(data + 6, sensor_ring.next_seq);
    proto_put_u32(data + 10, packet_ring_oldest(&sensor_ring));
}

void packer_flush(void)
{
    if (!count) return;
    current[18] = count;
    packet_ring_push(&sensor_ring, current, (uint8_t)(36u + count * 6u));
    count = 0;
    radio_request_tx();
}

void packer_sample(int16_t x, int16_t y, int16_t z, sample_stamp_t stamp)
{
    if (count && (stamp.index != last_index + 1u || stamp.segment != current[19] ||
                  (stamp.flags & (SAMPLE_GAP | SAMPLE_NEW_SEGMENT | SAMPLE_CFG_CHANGED))))
        packer_flush();
    if (!count) {
        memset(current, 0, 36);
        packet_header(current, 1);
        proto_put_u32(current + 14, stamp.index);
        current[19] = stamp.segment; started = sensor_rtc();
    }
    current[2] |= stamp.flags;
    proto_put_u32(current + 22, stamp.index);
    proto_put_u32(current + 26, stamp.rtc);
    unsigned pos = 36u + count * 6u;
    proto_put_u16(current + pos, (uint16_t)x);
    proto_put_u16(current + pos + 2, (uint16_t)y);
    proto_put_u16(current + pos + 4, (uint16_t)z);
    count++; last_index = stamp.index;
    if (count == 35) packer_flush();
}

void packer_expire(void)
{
    if (count && sensor_elapsed(sensor_rtc(), started) >= 3277u) packer_flush();
}
