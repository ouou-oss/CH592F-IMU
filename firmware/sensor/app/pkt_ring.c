#include "pkt_ring.h"
#include "proto.h"
#include <string.h>

static packet_slot_t *oldest(packet_ring_t *ring, uint8_t state)
{
    packet_slot_t *result = 0;
    for (unsigned i = 0; i < SENSOR_RING_SLOTS; ++i) {
        packet_slot_t *slot = &ring->slots[i];
        if (!slot->state || (state && slot->state != state)) continue;
        if (!result || (int32_t)(proto_get_u32(slot->data + 6) -
                                  proto_get_u32(result->data + 6)) < 0) result = slot;
    }
    return result;
}

packet_slot_t *packet_ring_push(packet_ring_t *ring, const uint8_t *data, uint8_t len)
{
    packet_slot_t *slot = 0;
    uint8_t dropped = 0;
    if (len < 14 || len > 246) return 0;
    for (unsigned i = 0; i < SENSOR_RING_SLOTS; ++i) {
        if (ring->slots[i].state == SLOT_FREE) { slot = &ring->slots[i]; break; }
    }
    if (!slot) {
        slot = oldest(ring, 0); ring->drop_packets++;
        if ((slot->data[0] & 15u) == 1) ring->drop_samples += slot->data[18];
        dropped = 16;
    } else ring->used++;
    memcpy(slot->data, data, len);
    slot->data[2] |= dropped;
    proto_put_u32(slot->data + 6, ring->next_seq++);
    slot->len = len; slot->state = SLOT_QUEUED; slot->attempt = 0; slot->exchange = 0;
    if (ring->used > ring->max_used) ring->max_used = ring->used;
    return slot;
}

packet_slot_t *packet_ring_pick(packet_ring_t *ring) { return oldest(ring, SLOT_QUEUED); }

uint32_t packet_ring_oldest(const packet_ring_t *ring)
{
    uint32_t seq = ring->next_seq;
    for (unsigned i = 0; i < SENSOR_RING_SLOTS; ++i) {
        const packet_slot_t *s = &ring->slots[i];
        if (s->state && (int32_t)(proto_get_u32(s->data + 6) - seq) < 0)
            seq = proto_get_u32(s->data + 6);
    }
    return seq;
}

void packet_ring_confirm(packet_ring_t *ring, uint8_t node, uint16_t seq_lo)
{
    for (unsigned i = 0; i < SENSOR_RING_SLOTS; ++i) {
        packet_slot_t *s = &ring->slots[i];
        if (s->state && s->attempt && s->data[1] == node &&
            (uint16_t)proto_get_u32(s->data + 6) == seq_lo) {
            s->state = SLOT_FREE; ring->used--; return;
        }
    }
}

void packet_ring_age(packet_ring_t *ring, uint16_t exchange)
{
    for (unsigned i = 0; i < SENSOR_RING_SLOTS; ++i) {
        packet_slot_t *s = &ring->slots[i];
        if (s->state == SLOT_WAIT_CONF && (uint16_t)(exchange - s->exchange) >= 3)
            s->state = SLOT_QUEUED;
    }
}

uint8_t packet_ring_waiting(const packet_ring_t *ring)
{
    uint8_t n = 0;
    for (unsigned i = 0; i < SENSOR_RING_SLOTS; ++i)
        n += ring->slots[i].state == SLOT_WAIT_CONF;
    return n;
}

void packet_ring_drop_all(packet_ring_t *ring)
{
    for (unsigned i = 0; i < SENSOR_RING_SLOTS; ++i) {
        packet_slot_t *s = &ring->slots[i];
        if (!s->state) continue;
        ring->drop_packets++;
        if ((s->data[0] & 15u) == 1u) ring->drop_samples += s->data[18];
        s->state = SLOT_FREE;
    }
    ring->used = 0;
}
