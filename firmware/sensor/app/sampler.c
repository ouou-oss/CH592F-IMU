#include "sensor.h"
#include "bmi323.h"

static sample_clock_t clock_state;
static uint8_t fifo[48 * 8];
static uint32_t last_sample_rtc;
static uint8_t ready, bad_id;
#if SENSOR_MOTION_SLEEP
static int16_t quiet_x, quiet_y, quiet_z;
static uint32_t quiet_since, last_motion_sample;

static uint8_t axis_moved(int16_t sample, int16_t reference, int32_t threshold)
{
    int32_t delta = (int32_t)sample - reference;
    return delta >= threshold || delta <= -threshold;
}

static void observe_motion(int16_t x, int16_t y, int16_t z, sample_stamp_t stamp)
{
    /* 1/32 g at any range; a fixed reference also catches gradual movement. */
    int32_t threshold = 512 >> sensor_config.range;
    if ((stamp.flags & (SAMPLE_NEW_SEGMENT | SAMPLE_GAP)) ||
        axis_moved(x, quiet_x, threshold) || axis_moved(y, quiet_y, threshold) ||
        axis_moved(z, quiet_z, threshold)) {
        quiet_x = x;
        quiet_y = y;
        quiet_z = z;
        quiet_since = stamp.rtc;
    }
    last_motion_sample = stamp.rtc;
}

uint8_t sampler_inactive(void)
{
    return ready && clock_state.valid &&
           sensor_elapsed(sensor_rtc(), last_sample_rtc) < 3277u &&
           sensor_elapsed(last_motion_sample, quiet_since) >= SENSOR_INACTIVE_TICKS;
}

uint8_t sampler_arm_motion(void)
{
    board_fifo_interrupt_disable();
    if (!bmi_arm_motion()) {
        sensor_stats.reinit++;
        sampler_reconfigure();
        return 0;
    }
    packer_flush();
    sample_clock_reset(&clock_state, 0);
    ready = 0;
    return 1;
}
#endif
#ifndef SENSOR_TEST_FIFO_PAUSE_MS
#define SENSOR_TEST_FIFO_PAUSE_MS 0
#endif
#if SENSOR_TEST_FIFO_PAUSE_MS
static uint32_t test_started;
static uint8_t test_paused;
#endif

static uint8_t parse_frame(const uint8_t *p, uint32_t rtc, uint16_t now, uint8_t flags)
{
    int16_t x = (int16_t)proto_get_u16(p), y = (int16_t)proto_get_u16(p + 2);
    int16_t z = (int16_t)proto_get_u16(p + 4);
    if ((uint16_t)x == 0x7f01) return 1;
    if (x == INT16_MIN && y == INT16_MIN && z == INT16_MIN) return 0;
    uint16_t time = proto_get_u16(p + 6);
    sample_stamp_t stamp = sample_clock_next(&clock_state, time,
                            sample_anchor(rtc, now, time), sensor_config.odr);
    stamp.flags |= flags;
    if (x >= 32767 || x <= -32767 || y >= 32767 || y <= -32767 ||
        z >= 32767 || z <= -32767) stamp.flags |= SAMPLE_CLIP;
    sensor_stats.fifo_gap_samples += stamp.missing;
    sensor_stats.samples++;
    last_sample_rtc = sensor_rtc();
#if SENSOR_MOTION_SLEEP
    observe_motion(x, y, z, stamp);
#endif
    packer_sample(x, y, z, stamp);
    return 1;
}

void sampler_init(void)
{
    sample_clock_reset(&clock_state, 0);
    if (sensor_stats.streaming)
        ready = bmi_initialize(sensor_config.odr, sensor_config.range, sensor_config.bandwidth);
    else {
        ready = 0;
        (void)bmi_suspend();
    }
    last_sample_rtc = sensor_rtc();
#if SENSOR_TEST_FIFO_PAUSE_MS
    test_started = sensor_rtc(); test_paused = 0;
#endif
}

void sampler_reconfigure(void)
{
    packer_flush();
    board_fifo_interrupt_disable();
    sample_clock_reset(&clock_state, SAMPLE_CFG_CHANGED);
    ready = 0;
    if (sensor_stats.streaming) {
        ready = bmi_initialize(sensor_config.odr, sensor_config.range, sensor_config.bandwidth);
        if (ready) board_fifo_interrupt_enable();
    } else {
        (void)bmi_suspend();
    }
    last_sample_rtc = sensor_rtc();
}

void sampler_poll(void)
{
    if (!ready || !sensor_stats.streaming) return;
#if SENSOR_TEST_FIFO_PAUSE_MS
    /* One pause after 5 s, watchdog still fed by the 20 ms event. */
    uint32_t elapsed = sensor_elapsed(sensor_rtc(), test_started);
    if (!test_paused && elapsed >= 5u * 32768u) test_paused = 1;
    if (test_paused == 1 && elapsed < 5u * 32768u +
        (uint32_t)SENSOR_TEST_FIFO_PAUSE_MS * 32768u / 1000u) return;
    if (test_paused == 1) test_paused = 2;
#endif
    for (unsigned round = 0; round < 3; ++round) {
        uint32_t read_rtc = sensor_rtc();
        uint16_t fill = bmi_read(0x15) & 0x7ffu;
        unsigned frames = fill / 4u;
        uint8_t flags = fill >= 1016u ? SAMPLE_FIFO_FULL : 0;
        if (!frames) break;
        if (flags && sensor_elapsed(read_rtc, last_sample_rtc) > 2u * 32768u) {
            /* The retained FIFO can be several 16-bit sensor-time wraps old.
             * Its exact lost-sample count is unknowable: preserve known indices,
             * discard the ambiguous FIFO and explicitly start a discontinuity. */
            packer_flush(); sample_clock_reset(&clock_state, SAMPLE_GAP | SAMPLE_FIFO_FULL);
            sensor_stats.fifo_unknown++; sensor_stats.reinit++;
            ready = bmi_initialize(sensor_config.odr, sensor_config.range, sensor_config.bandwidth);
            last_sample_rtc = sensor_rtc(); bad_id = 0;
            return;
        }
        if (flags) sensor_stats.fifo_full++;
        if (frames > 48) frames = 48;
        bmi_fifo_read(fifo, (uint16_t)(frames * 8u));
        /* Take the time pair after the burst: a pre-FIFO snapshot can precede
         * its newest frame, turning a few future ticks into a 2.56 s old age. */
        read_rtc = sensor_rtc();
        uint16_t now = bmi_read(0x0a);
        for (unsigned i = 0; i < frames; ++i) {
            if (!parse_frame(fifo + i * 8u, read_rtc, now, flags)) break;
        }
    }
    packer_expire();
    /* Level can stay asserted after the bounded drain; do not wait for a new edge. */
    if (GPIOA_ReadPortPin(GPIO_Pin_4)) tmos_set_event(sensor_task, FIFO_EVT);
}

void sampler_health(void)
{
    if (!sensor_stats.streaming) return;
    if ((bmi_read(0) & 0xffu) != 0x43) bad_id++;
    else bad_id = 0;
    if (bad_id < 5 && ready && sensor_elapsed(sensor_rtc(), last_sample_rtc) < 32768u) return;
    packer_flush();
    sample_clock_reset(&clock_state, 0);
    sensor_stats.reinit++; bad_id = 0;
    ready = bmi_initialize(sensor_config.odr, sensor_config.range, sensor_config.bandwidth);
    last_sample_rtc = sensor_rtc();
}

/* STATUS 使用已产生样本的右开界，启动尚无样本时为 0。 */
uint32_t sampler_index(void) { return clock_state.index + (clock_state.valid ? 1u : 0u); }
