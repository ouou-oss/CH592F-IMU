#include "sensor.h"
#include "bmi323.h"
#include <assert.h>
#include <stdio.h>

sensor_config_t sensor_config = {.node=1, .odr=13, .range=3, .bandwidth=1};
sensor_stats_t sensor_stats = {.streaming=1};
uint8_t sensor_task;
static uint32_t sensor_ticks = 800, rtc_base = 100000;
static uint8_t available, arrive_at_level;
static sample_stamp_t stamps[16];
static unsigned count, suspend_calls, fifo_irq_enabled;
static uint8_t motion_testing, motion_arm_ok;
static int16_t x_raw = 100;

uint32_t sensor_rtc(void)
{
    return (rtc_base + sensor_ticks * 32u / 25u) % PROTO_RTC_MODULUS;
}
uint32_t sensor_elapsed(uint32_t now, uint32_t then)
{
    return sample_rtc_elapsed(now, then);
}
uint8_t bmi_initialize(uint8_t odr, uint8_t range, uint8_t bandwidth)
{
    assert(odr == 13 && range == 3 && bandwidth == 1);
    return 1;
}
uint8_t bmi_suspend(void) { ++suspend_calls; return 1; }
uint8_t bmi_arm_motion(void) { return motion_arm_ok; }
void board_fifo_interrupt_enable(void) { fifo_irq_enabled = 1; }
void board_fifo_interrupt_disable(void) { fifo_irq_enabled = 0; }
uint16_t bmi_read(uint8_t address)
{
    if (address == 0x0a) return (uint16_t)sensor_ticks;
    assert(address == 0x15);
    /* A new sample can appear after a pre-FIFO SENSOR_TIME read. */
    if (available && arrive_at_level) {
        sensor_ticks += 8u;
        arrive_at_level = 0;
    }
    return available ? 4 : 0;
}
void bmi_fifo_read(uint8_t *data, uint16_t bytes)
{
    assert(available && bytes == 8);
    proto_put_u16(data, (uint16_t)x_raw); proto_put_u16(data + 2, 200);
    proto_put_u16(data + 4, 2048); proto_put_u16(data + 6, (uint16_t)sensor_ticks);
    available = 0;
}
void bmi_write(uint8_t address, uint16_t value) { (void)address; (void)value; }
uint32_t GPIOA_ReadPortPin(uint32_t pins) { assert(pins == GPIO_Pin_4); return 0; }
uint8_t tmos_set_event(uint8_t task, uint16_t events)
{
    (void)task; (void)events; return 0;
}
void packer_sample(int16_t x, int16_t y, int16_t z, sample_stamp_t stamp)
{
    if (motion_testing) return;
    assert(x == 100 && y == 200 && z == 2048 && count < 16);
    stamps[count++] = stamp;
}
void packer_flush(void) {}
void packer_expire(void) {}

static sample_stamp_t read_one(uint8_t late)
{
    available = 1; arrive_at_level = late;
    unsigned before = count;
    sampler_poll();
    assert(count == before + 1u);
    assert(stamps[before].rtc == sensor_rtc());
    return stamps[before];
}

static void snapshot_follows_fifo_read(void)
{
    sampler_init();
    sample_stamp_t a = read_one(0);
    assert(a.index == 0 && a.flags == SAMPLE_NEW_SEGMENT);
    sample_stamp_t b = read_one(1);
    assert(b.index == a.index + 1u && b.segment == a.segment && b.flags == 0);
    sensor_ticks += 8u;
    sample_stamp_t c = read_one(0);
    assert(c.index == b.index + 1u && c.segment == b.segment && c.flags == 0);
    assert(sensor_stats.fifo_gap_samples == 0);

    sensor_ticks += 24u;
    sample_stamp_t gap = read_one(0);
    assert(gap.index == c.index + 3u && gap.missing == 2 && gap.flags == SAMPLE_GAP);
    assert(sensor_stats.fifo_gap_samples == 2);
}

static void wraps_keep_continuity(void)
{
    sensor_ticks = 65528u;
    sampler_init();
    sample_stamp_t a = read_one(0), b = read_one(1);
    assert((uint16_t)sensor_ticks == 0);
    assert(b.index == a.index + 1u && b.segment == a.segment && b.flags == 0);

    sensor_ticks = 1000u;
    rtc_base = PROTO_RTC_MODULUS - sensor_ticks * 32u / 25u - 5u;
    sampler_init();
    a = read_one(0); b = read_one(1);
    assert(a.rtc == PROTO_RTC_MODULUS - 5u && b.rtc == 5u);
    assert(b.index == a.index + 1u && b.segment == a.segment && b.flags == 0);
    assert(sensor_stats.fifo_gap_samples == 2);
}

static void standby_suspends_imu_and_disables_irq(void)
{
    sensor_stats.streaming = 0;
    fifo_irq_enabled = 1;
    unsigned before = suspend_calls;
    sampler_init();
    assert(suspend_calls == before + 1u);
    available = 1;
    unsigned samples_before = count;
    sampler_poll();
    assert(count == samples_before);
    sensor_stats.streaming = 1;
    sampler_reconfigure();
    assert(fifo_irq_enabled == 1);
    sensor_stats.streaming = 0;
    sampler_reconfigure();
    assert(fifo_irq_enabled == 0 && suspend_calls == before + 2u);
}

int main(void)
{
    snapshot_follows_fifo_read(); wraps_keep_continuity(); standby_suspends_imu_and_disables_irq();
    motion_testing = 1;
    sensor_ticks = 1000u;
    rtc_base = PROTO_RTC_MODULUS - 2000u;
    sensor_stats.streaming = 1;
    sampler_reconfigure();
    assert(!sampler_inactive());
    for (unsigned i = 0; i < 2; ++i) {
        /* 3.2 kHz: 32,000 sample intervals in 10 seconds. */
        for (unsigned sample = i; sample < 32000; ++sample) {
            available = 1;
            sampler_poll();
            assert(!sampler_inactive());
            sensor_ticks += 8u;
        }
        available = 1;
        x_raw += i ? 0 : 64; /* First pass ends with motion exactly at the threshold. */
        sampler_poll();
        assert(sampler_inactive() == i);
        if (!i) sensor_ticks += 8u;
    }
    sensor_ticks += 3000u;
    assert(!sampler_inactive()); /* A stopped sensor does not prove inactivity. */
    assert(!sampler_arm_motion() && fifo_irq_enabled);
    motion_arm_ok = 1;
    assert(sampler_arm_motion() && !fifo_irq_enabled);
    puts("sensor sampler timestamp tests passed"); return 0;
}
