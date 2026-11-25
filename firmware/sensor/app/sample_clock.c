#include "sample_clock.h"

#define RTC_MODULUS UINT32_C(0xA8C00000)

uint32_t sample_rtc_elapsed(uint32_t now, uint32_t then)
{
    return now >= then ? now - then : RTC_MODULUS - then + now;
}

uint32_t sample_anchor(uint32_t read_rtc, uint16_t sensor_now, uint16_t sensor)
{
    /* Unsigned subtraction is essential when FIFO data is older than 1.28 s. */
    uint32_t age = (uint32_t)(uint16_t)(sensor_now - sensor) * 32u / 25u;
    return read_rtc >= age ? read_rtc - age : RTC_MODULUS - age + read_rtc;
}

void sample_clock_reset(sample_clock_t *clock, uint8_t flags)
{
    if (clock->valid) { clock->index++; clock->segment++; }
    clock->valid = 0;
    clock->pending_flags |= flags | SAMPLE_NEW_SEGMENT;
}

sample_stamp_t sample_clock_next(sample_clock_t *c, uint16_t sensor,
                                uint32_t rtc, uint8_t odr)
{
    sample_stamp_t out = {c->index, rtc, 0, c->segment, c->pending_flags};
    uint32_t period = 1u << (16u - odr);
    uint32_t dt = (uint16_t)(sensor - c->last_sensor);
    uint32_t elapsed = sample_rtc_elapsed(rtc, c->last_rtc);
    if (c->valid && elapsed > 2u * 32768u && elapsed <= 60u * 32768u) {
        uint32_t estimate = (uint32_t)((uint64_t)elapsed * 25u / 32u);
        if (estimate > dt + 32768u) dt += ((estimate - dt + 32768u) / 65536u) * 65536u;
    }
    if (c->valid) {
        if (elapsed > 60u * 32768u || dt == 0 || dt % period) {
            c->index++; c->segment++; out.flags |= SAMPLE_NEW_SEGMENT;
        } else {
            c->index += dt / period;
            out.missing = dt / period - 1u;
            if (out.missing) out.flags |= SAMPLE_GAP;
            c->lost += out.missing;
        }
    }
    c->last_sensor = sensor; c->last_rtc = rtc; c->valid = 1; c->pending_flags = 0;
    out.index = c->index; out.segment = c->segment;
    return out;
}
