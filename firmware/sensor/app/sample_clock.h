#ifndef SENSOR_SAMPLE_CLOCK_H
#define SENSOR_SAMPLE_CLOCK_H
#include <stdint.h>

enum { SAMPLE_GAP = 1, SAMPLE_FIFO_FULL = 2, SAMPLE_CLIP = 4,
       SAMPLE_NEW_SEGMENT = 8, SAMPLE_RING_DROP = 16, SAMPLE_CFG_CHANGED = 64 };
typedef struct {
    uint32_t index, last_rtc, lost;
    uint16_t last_sensor;
    uint8_t segment, valid, pending_flags;
} sample_clock_t;
typedef struct { uint32_t index, rtc, missing; uint8_t segment, flags; } sample_stamp_t;

void sample_clock_reset(sample_clock_t *clock, uint8_t flags);
sample_stamp_t sample_clock_next(sample_clock_t *clock, uint16_t sensor,
                                uint32_t rtc, uint8_t odr);
uint32_t sample_rtc_elapsed(uint32_t now, uint32_t then);
uint32_t sample_anchor(uint32_t read_rtc, uint16_t sensor_now, uint16_t sensor);
#endif
