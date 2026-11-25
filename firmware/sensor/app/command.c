#include "sensor.h"

static uint32_t identify_started, identify_duration;
static uint8_t identify_divider;

uint8_t command_identify_active(void)
{
    return identify_duration != 0;
}

void command_poll(void)
{
    if (!identify_duration) return;
    if (sensor_elapsed(sensor_rtc(), identify_started) >= identify_duration) {
        GPIOA_ResetBits(GPIO_Pin_6); identify_duration = 0;
        sensor_sync_timers();
        return;
    }
    if (++identify_divider == 10) {
        identify_divider = 0; GPIOA_InverseBits(GPIO_Pin_6);
    }
}

void command_apply(const uint8_t *ack)
{
    if (ack[2] != sensor_config.node || ack[3] == sensor_stats.cmd_ack) return;
    uint8_t done = 0;
    switch (ack[4]) {
    case 1:
        if (ack[5] != 3 || ack[6] < 12 || ack[6] > 14 || ack[7] > 3 || ack[8] > 1) break;
        packer_flush();
        sensor_config.odr = ack[6]; sensor_config.range = ack[7]; sensor_config.bandwidth = ack[8];
        sampler_reconfigure(); done = 1; break;
    case 2:
        if (ack[5] != 1 || ack[6] > 1) break;
        sensor_stats.cmd_ack = ack[3];
        sensor_apply_streaming_mode(ack[6]);
        return;
    case 3:
        if (ack[5] != 1) break;
        if (!LL_SetTxPowerLevel(ack[6])) { sensor_config.power = ack[6]; done = 1; }
        break;
    case 4:
        if (ack[5] >= 1 && ack[6] == 1) done = 1;
        break;
    case 5:
        /* V1 cannot report cmd_ack durably before reboot; reject stale REBOOT. */
        break;
    case 6:
        if (ack[5] != 1) break;
        identify_started = sensor_rtc(); identify_duration = (uint32_t)ack[6] * 32768u;
        identify_divider = 0; done = 1;
        sensor_sync_timers();
        break;
    default: break;
    }
    if (done) sensor_stats.cmd_ack = ack[3];
}
