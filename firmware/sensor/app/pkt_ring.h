#ifndef SENSOR_PKT_RING_H
#define SENSOR_PKT_RING_H
#include <stdint.h>
#ifndef SENSOR_RING_SLOTS
#define SENSOR_RING_SLOTS 24
#endif
enum { SLOT_FREE, SLOT_QUEUED, SLOT_WAIT_CONF };
typedef struct {
    uint8_t data[246];
    uint16_t exchange;
    uint8_t len, state, attempt;
} packet_slot_t;
typedef struct {
    packet_slot_t slots[SENSOR_RING_SLOTS];
    uint32_t next_seq, drop_packets, drop_samples;
    uint8_t used, max_used;
} packet_ring_t;
packet_slot_t *packet_ring_push(packet_ring_t *ring, const uint8_t *data, uint8_t len);
packet_slot_t *packet_ring_pick(packet_ring_t *ring);
uint32_t packet_ring_oldest(const packet_ring_t *ring);
void packet_ring_confirm(packet_ring_t *ring, uint8_t node, uint16_t seq_lo);
void packet_ring_age(packet_ring_t *ring, uint16_t exchange);
uint8_t packet_ring_waiting(const packet_ring_t *ring);
void packet_ring_drop_all(packet_ring_t *ring);
#endif
