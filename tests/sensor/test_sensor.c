#include "sample_clock.h"
#include "pkt_ring.h"
#include "proto.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void sample_clock_test(void)
{
    sample_clock_t c = {0};
    sample_clock_reset(&c, 0);
    sample_stamp_t s = sample_clock_next(&c, 65528, 100, 13);
    assert(s.index == 0 && s.flags == SAMPLE_NEW_SEGMENT);
    s = sample_clock_next(&c, 0, 110, 13);
    assert(s.index == 1 && s.flags == 0 && s.missing == 0);
    s = sample_clock_next(&c, 24, 141, 13);
    assert(s.index == 4 && s.flags == SAMPLE_GAP && s.missing == 2 && c.lost == 2);
    s = sample_clock_next(&c, 24, 142, 13);
    assert(s.index == 5 && s.segment == 1 && s.flags == SAMPLE_NEW_SEGMENT);
    sample_clock_reset(&c, SAMPLE_CFG_CHANGED);
    s = sample_clock_next(&c, 80, 200, 12);
    assert(s.index == 6 && s.segment == 2);
    assert(s.flags == (SAMPLE_CFG_CHANGED | SAMPLE_NEW_SEGMENT));
    sample_clock_reset(&c, SAMPLE_GAP | SAMPLE_FIFO_FULL);
    s = sample_clock_next(&c, 90, 210, 12);
    assert(s.index == 7 && s.missing == 0 && s.segment == 3);
    assert(s.flags == (SAMPLE_GAP | SAMPLE_FIFO_FULL | SAMPLE_NEW_SEGMENT));
    assert(sample_anchor(1000, 20, 12) == 990);
    assert(sample_anchor(5, 20, 12) == PROTO_RTC_MODULUS - 5);
    assert(sample_anchor(100000, 40000, 0) == 48800);
    assert(sample_rtc_elapsed(20, PROTO_RTC_MODULUS - 20) == 40);

    memset(&c, 0, sizeof(c));
    sample_clock_next(&c, 0, 0, 13);
    s = sample_clock_next(&c, (uint16_t)80000, 102400, 13);
    assert(s.index == 10000 && s.missing == 9999);
    s = sample_clock_next(&c, 0, 102400 + 61u * 32768u, 13);
    assert(s.flags & SAMPLE_NEW_SEGMENT);
}

static void ring_test(void)
{
    packet_ring_t r = {0}; uint8_t data[42] = {0x11, 1};
    data[18] = 1;
    packet_slot_t *first = packet_ring_push(&r, data, sizeof(data));
    assert(first && r.used == 1 && packet_ring_oldest(&r) == 0);
    packet_ring_confirm(&r, 1, 0);
    assert(r.used == 1); /* Unsent packets cannot be falsely confirmed. */
    first->attempt = 1; first->state = SLOT_WAIT_CONF; first->exchange = 4;
    packet_ring_confirm(&r, 2, 0); assert(r.used == 1);
    packet_ring_age(&r, 6); assert(first->state == SLOT_WAIT_CONF);
    packet_ring_age(&r, 7); assert(first->state == SLOT_QUEUED);
    packet_ring_confirm(&r, 1, 0); assert(r.used == 0);
    for (unsigned i = 0; i < SENSOR_RING_SLOTS; ++i) packet_ring_push(&r, data, sizeof(data));
    assert(r.used == SENSOR_RING_SLOTS && r.max_used == SENSOR_RING_SLOTS);
    packet_slot_t *added = packet_ring_push(&r, data, sizeof(data));
    assert(r.drop_packets == 1 && r.drop_samples == 1 && (added->data[2] & 16));
    assert(packet_ring_oldest(&r) == 2);
    assert(proto_get_u32(packet_ring_pick(&r)->data + 6) == 2);
    assert(!packet_ring_push(&r, data, 13));

    memset(&r, 0, sizeof(r)); r.next_seq = UINT32_MAX - 1;
    packet_ring_push(&r, data, sizeof(data)); packet_ring_push(&r, data, sizeof(data));
    packet_ring_push(&r, data, sizeof(data));
    assert(packet_ring_oldest(&r) == UINT32_MAX - 1);
    assert(proto_get_u32(packet_ring_pick(&r)->data + 6) == UINT32_MAX - 1);
}

int main(void)
{
    sample_clock_test(); ring_test(); puts("sensor clock/ring tests passed"); return 0;
}
